/**
 * @file SerialInspector.cpp
 * @brief Serial inspector: PTY <> real-serial bridge and debugging helpers.
 *
 * Implements a small utility that creates a pseudo-terminal pair and bridges
 * data between a master PTY and a real serial device. The bridge runs in a
 * background thread and can optionally emit hex dumps of transmitted data for
 * debugging and analysis.
 *
 * Typical usage:
 *  - Construct a SerialInspector instance
 *  - Call setTty() to attach a real serial device
 *  - Use getVirtualName() to obtain the slave PTY for connecting test tools
 *  - Optionally enable debug output via setDebug(true)
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

#include "SerialInspector.hpp"
#include <iostream>
#include <iomanip>

/**
 * @brief Construct a SerialInspector instance.
 *
 * Allocates a pseudo-terminal master/slave pair, initializes internal state,
 * and starts the background bridge thread.
 */
SerialInspector::SerialInspector() : debug_traffic(false) {
    master_fd = createPty(slave_name);
    running = true;
    bridge_thread = std::thread(&SerialInspector::bridge, this);
}

/**
 * @brief Destroy the SerialInspector instance.
 *
 * Stops the bridge thread and closes open file descriptors.
 */
SerialInspector::~SerialInspector() {
    running = false;
    if (bridge_thread.joinable()) {
        bridge_thread.join();
    }
    ::close(master_fd);
    ::close(real_fd);
}

/**
 * @brief Open and configure the real serial TTY for the inspector.
 *
 * Opens the device at `tty` (non-blocking), configures raw mode and the
 * specified baud rate, and enables/disables RTS/CTS per `hw_flow_control`.
 * If a previous device was open it will be closed first.
 *
 * @param[in] tty Path to the serial device (e.g. "/dev/ttyUSB0").
 * @param[in] baud Baud rate value from the `BaudRate` enum.
 * @param[in] hw_flow_control true to enable RTS/CTS, false to disable.
 * @return void
 * @throws std::runtime_error on failure to open or configure the device.
 */
void SerialInspector::setTty(std::string tty, BaudRate baud, bool hw_flow_control) {
    {
        std::lock_guard<std::mutex> g(fd_mtx);
        if (real_fd != -1) {
            ::close(real_fd);
            real_fd = -1;
        }
        // open real_fd
        real_fd = ::open(tty.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (real_fd < 0) {
            throw std::runtime_error("Failed to open TTY: " + tty);
        }
    }

    // Configure raw mode and disable flow control
    struct termios tio;
    if (tcgetattr(real_fd, &tio) < 0) {
        ::close(real_fd);
        real_fd = -1;
        throw std::runtime_error("Failed to get TTY attributes");
    }

    cfmakeraw(&tio);  // Set raw mode

    // set the baud rate
    cfsetispeed(&tio, static_cast<speed_t>(baud));
    cfsetospeed(&tio, static_cast<speed_t>(baud));

    // Disable software flow control
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);

    // Disable hardware flow control
    if (!hw_flow_control) {
        tio.c_cflag &= ~CRTSCTS;
    } else {
        // Enable RTS/CTS hardware flow control
        tio.c_cflag |= CRTSCTS;
    }

    if (tcsetattr(real_fd, TCSANOW, &tio) < 0) {
        ::close(real_fd);
        real_fd = -1;
        throw std::runtime_error("Failed to set TTY attributes");
    }
}

/**
 * @brief Retrieve the virtual (slave) PTY device name.
 *
 * @return std::string The slave device path (e.g. "/dev/pts/N").
 */
std::string SerialInspector::getVirtualName() const {
    return slave_name;
}

/**
 * @brief Retrieve the currently configured real serial device name.
 *
 * @return std::string The real device path or an empty string if none.
 */
std::string SerialInspector::getRealName() const {
    return real_name;
}

/**
 * @brief Enable or disable debug traffic printing.
 *
 * When enabled, `bridge()` prints hex dumps of observed traffic.
 *
 * @param[in] enable true to enable debug printing, false to disable.
 * @return void
 */
void SerialInspector::setDebug(bool enable) {
    debug_traffic = enable;
}

/**
 * @brief Create and prepare a pseudo-terminal pair.
 *
 * Allocates a PTY master and prepares the slave device. On success the slave
 * device path is written to `slave_name`.
 *
 * @param[out] slave_name Receives the slave device path string.
 * @return int File descriptor for the PTY master (>=0) on success.
 */
int SerialInspector::createPty(std::string& slave_name) {
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
 * @param[in] direction Label indicating direction/context (e.g. "master -> real").
 * @return void
 */
void SerialInspector::printHex(const std::vector<uint8_t>& data, const std::string& direction) {
    std::cout << direction << ": ";
    for (uint8_t byte : data) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte) << " ";
    }
    std::cout << std::dec << std::endl;
}

/**
 * @brief Background loop that bridges data between master PTY and real device.
 *
 * Waits for readability on both endpoints, copies data between them, and
 * optionally logs hex dumps when `debug_traffic` is enabled. The loop
 * continues while `running` is true.
 *
 * @return void
 */
void SerialInspector::bridge() {
    const size_t buf_size = 256;
    uint8_t buf[buf_size];
    std::vector<uint8_t> buffer1, buffer2;

    while (running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(master_fd, &read_fds);
        FD_SET(real_fd, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;  // 0.5 seconds
        int max_fd = std::max(master_fd, real_fd) + 1;
        int ready = select(max_fd, &read_fds, nullptr, nullptr, &timeout);
        if (ready < 0) {
            perror("SerialInspector select");
            break;
        }
        if (ready == 0) {
            continue;  // timeout occurred, no data ready
        }

        if (FD_ISSET(master_fd, &read_fds)) {
            ssize_t n = read(master_fd, buf, buf_size);
            if (n > 0) {
                ssize_t w = ::write(real_fd, buf, n);
                if (debug_traffic) {
                    for (ssize_t i = 0; i < n; ++i) {
                        buffer1.push_back(buf[i]);
                        if (buf[i] == 0x7e) {
                            printHex(buffer1, "master → real");
                            buffer1.clear();
                        }
                    }
                }
            }
        }

        if (FD_ISSET(real_fd, &read_fds)) {
            ssize_t n = read(real_fd, buf, buf_size);
            if (n > 0) {
                ssize_t w = ::write(master_fd, buf, n);
                if (w != n) {
                    perror("SerialInspector: write to master_fd failed");
                }
                if (debug_traffic) {
                    for (ssize_t i = 0; i < n; ++i) {
                        buffer2.push_back(buf[i]);
                        if (buf[i] == 0x7e) {
                            printHex(buffer2, "real → master");
                            buffer2.clear();
                        }
                    }
                }
            }
        }
    }
}