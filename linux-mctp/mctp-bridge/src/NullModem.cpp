/**
 * @file NullModem.cpp
 * @brief Null-modem PTY bridge implementation.
 *
 * Implements a simple null-modem style bridge that allocates two pseudo-terminals
 * and copies data between their master endpoints. Useful for testing serial
 * communication and higher-layer protocol implementations without physical
 * wiring.
 *
 * Typical usage:
 *  - Construct a NullModem instance
 *  - Use getName1()/getName2() to obtain slave PTY paths for test tools
 *  - Optionally enable debug hex dumps via setDebug(true)
 *
 * @author Doug Sandy (doug@picmg.org)
 * @license MIT No Attribution (MIT-0)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
 */
#include "NullModem.hpp"
#include <iostream>
#include <iomanip>

/**
 * @brief Construct a NullModem instance.
 *
 * Creates two PTY master/slave pairs and starts the background bridge thread
 * that copies data between the two masters.
 */
NullModem::NullModem() : debug_traffic(false) {
    master1_fd = createPty(slave1_name);
    master2_fd = createPty(slave2_name);
    running = true;
    bridge_thread = std::thread(&NullModem::bridge, this);
}

/**
 * @brief Destructor for NullModem.
 *
 * Stops the bridge thread, waits for it to terminate, and closes the PTY
 * master file descriptors.
 */
NullModem::~NullModem() {
    running = false;
    if (bridge_thread.joinable()) {
        bridge_thread.join();
    }
    close(master1_fd);
    close(master2_fd);
    usleep(100000);  // 0.1 seconds
}

/**
 * @brief Get the slave path for the first PTY.
 *
 * @return std::string The path of the first slave PTY (e.g. "/dev/pts/N").
 */
std::string NullModem::getName1() const {
    return slave1_name;
}

/**
 * @brief Get the slave path for the second PTY.
 *
 * @return std::string The path of the second slave PTY (e.g. "/dev/pts/N").
 */
std::string NullModem::getName2() const {
    return slave2_name;
}

/**
 * @brief Enable or disable debug hex dumps of bridged traffic.
 *
 * @param[in] enable true to enable debug printing, false to disable.
 * @return void
 */
void NullModem::setDebug(bool enable) {
    debug_traffic = enable;
}

/**
 * @brief Create a PTY master and return its file descriptor.
 *
 * Allocates a PTY master, prepares the slave device, and writes the slave
 * path into the output parameter.
 *
 * @param[out] slave_name Receives the slave device path string.
 * @return int File descriptor for the PTY master (>=0) on success. On fatal
 *         errors this function exits the process.
 */
int NullModem::createPty(std::string& slave_name) {
    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0) {
        perror("posix_openpt");
        exit(1);
    }

    if (grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
        perror("grantpt/unlockpt");
        exit(1);
    }

    char* name = ptsname(master_fd);
    if (!name) {
        perror("ptsname");
        exit(1);
    }

    slave_name = name;
    return master_fd;
}

/**
 * @brief Print a hex dump of the provided byte buffer with a direction label.
 *
 * @param[in] data Byte buffer to print.
 * @param[in] direction Label indicating the direction/context (e.g. "master1 -> master2").
 * @return void
 */
void NullModem::printHex(const std::vector<uint8_t>& data, const std::string& direction) {
    std::cout << direction << ": ";
    for (uint8_t byte : data) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte) << " ";
    }
    std::cout << std::dec << std::endl;
}

/**
 * @brief Background loop that bridges data between the two PTY masters.
 *
 * Waits for readability on either master, copies available bytes from one
 * master to the other, and optionally emits hex dumps when debug is enabled.
 * The loop runs while `running` is true.
 *
 * @return void
 */
void NullModem::bridge() {
    const size_t buf_size = 256;
    uint8_t buf[buf_size];
    std::vector<uint8_t> buffer1, buffer2;

    // Main loop runs while the bridge is active.
    while (running) {
        // Prepare the fd_set for select(): monitor both PTY masters for readability.
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(master1_fd, &read_fds);   // master1: data from endpoint 1
        FD_SET(master2_fd, &read_fds);   // master2: data from endpoint 2

        // Timeout for select() so we wake periodically and can check `running`.
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;  // 0.5 seconds

        // Compute the maximum fd+1 for select(). Using std::max keeps it simple.
        int max_fd = std::max(master1_fd, master2_fd) + 1;

        // Wait for data on either master (or timeout). select() blocks up to timeout.
        int ready = select(max_fd, &read_fds, nullptr, nullptr, &timeout);
        if (ready < 0) {
            // select() error: print and break out to allow thread to terminate.
            // Common causes: EBADF (invalid FD), EINTR (signal) etc.
            perror("NullModem select");
            break;
        }

        // No FDs ready: loop again to check `running` and avoid busy-looping.
        if (ready == 0) {
            continue;
        }

        // If master1 has data available, read and forward to master2.
        if (FD_ISSET(master1_fd, &read_fds)) {
            // read() returns number of bytes read or <=0 on error/EOF
            ssize_t n = read(master1_fd, buf, buf_size);
            if (n > 0) {
                // Forward the bytes to the other endpoint.
                ssize_t w = ::write(master2_fd, buf, n);

                // If debug printing is enabled, accumulate bytes until we see
                // a frame delimiter (0x7e) then print a hex dump.
                if (debug_traffic) {
                    for (ssize_t i = 0; i < n; ++i) {
                        buffer1.push_back(buf[i]);
                        if (buf[i] == 0x7e) {
                            printHex(buffer1, "master1 → master2");
                            buffer1.clear();
                        }
                    }
                }
            }
        }

        // If master2 has data available, read and forward to master1.
        if (FD_ISSET(master2_fd, &read_fds)) {
            ssize_t n = read(master2_fd, buf, buf_size);
            if (n > 0) {
                ssize_t w = ::write(master1_fd, buf, n);
                if (w != n) {
                    perror("NullModem: write to master1 failed");
                }

                if (debug_traffic) {
                    for (ssize_t i = 0; i < n; ++i) {
                        buffer2.push_back(buf[i]);
                        if (buf[i] == 0x7e) {
                            printHex(buffer2, "master2 → master1");
                            buffer2.clear();
                        }
                    }
                }
            }
        }
    }
}