/**
 * @file LinuxMctpSerial.cpp
 * @brief Class for managing MCTP serial links over Linux TTY interfaces.
 *
 * This class provides functionality to initialize and tear down MCTP serial links
 * by configuring a TTY device with the MCTP line discipline, monitoring the creation
 * and removal of associated mctpserial network interfaces, and managing terminal settings.
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
#include "LinuxMctpSerial.hpp"
#include "MctpNetlink.hpp"
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <regex>
#include <algorithm>
extern "C" {
    #include <termio.h>
}

/**
 * @brief Constructor for LinuxMctpSerial.
 *
 * Initializes internal state and sets file descriptor to invalid.
 */
LinuxMctpSerial::LinuxMctpSerial() : fd_(-1) {}

/**
 * @brief Destructor for LinuxMctpSerial.
 *
 * Automatically calls close() to clean up resources.
 */
LinuxMctpSerial::~LinuxMctpSerial() {
    close();
}

/**
 * @brief Initializes the MCTP serial link on the specified TTY.
 *
 * Opens the TTY device, configures it for raw mode with no flow control,
 * sets the MCTP line discipline, and waits for a new mctpserial device to appear.
 * If a previous link was active, it will be closed first.
 *
 * @param tty_path Path to the TTY device (e.g., "/dev/pts/8").
 * @return std::string Name of the newly created mctpserial device (e.g., "mctpserial0").
 * @throws std::runtime_error if initialization fails or times out.
 */
std::string LinuxMctpSerial::initialize(const std::string& tty_path, const std::string& mctp_if_name) {
    if (fd_ != -1) {
        close();
    }

    std::vector<std::string> before = list_mctp_devices();

    fd_ = ::open(tty_path.c_str(), O_RDWR | O_NOCTTY);
    if (fd_ < 0) {
        throw std::runtime_error("Failed to open TTY: " + tty_path);
    }

    // Set non-blocking mode
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Failed to set non-blocking mode");
    }

    // Configure raw mode and disable flow control
    struct termios tio;
    if (tcgetattr(fd_, &tio) < 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Failed to get TTY attributes");
    }

    cfmakeraw(&tio);  // Set raw mode

    // Disable software flow control
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);

    // Disable hardware flow control
    tio.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd_, TCSANOW, &tio) < 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Failed to set TTY attributes");
    }

    // Set MCTP line discipline
    int ldisc = N_MCTP;
    if (ioctl(fd_, TIOCSETD, &ldisc) < 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Failed to set MCTP line discipline");
    }

    // Wait for a new mctpserial device to appear
    for (int i = 0; i < 50; ++i) {
        std::vector<std::string> after = list_mctp_devices();
        for (const auto& dev : after) {
            if (std::find(before.begin(), before.end(), dev) == before.end()) {
                this->mctp_if_name = dev;
                if (!mctp_if_name.empty()) {
                    if (!mctpnet::setMctpInterfaceName(dev, mctp_if_name)) {
                        ::close(fd_);
                        fd_ = -1;
                        throw std::runtime_error("Failed to set mctpserial interface name to " + mctp_if_name);
                    }
                    this->mctp_if_name = mctp_if_name;
                }
                return this->mctp_if_name;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    throw std::runtime_error("Timed out waiting for new mctpserial device");
}

/**
 * @brief Closes the active MCTP serial link.
 *
 * Resets the line discipline to N_TTY, closes the file descriptor,
 * and waits for the associated mctpserial device to disappear.
 * If no link is active, this function does nothing.
 */
void LinuxMctpSerial::close() {
    if (fd_ != -1) {
        // set the line discipline back
        int ldisc = N_TTY;
        if (ioctl(fd_, TIOCSETD, &ldisc) < 0) {
            perror("ioctl TIOCSETD failed");
        }

        ::close(fd_);
        fd_ = -1;

        // Wait for the associated mctpserial device to disappear
        for (int i = 0; i < 50; ++i) {
            std::vector<std::string> current = list_mctp_devices();
            if (std::find(current.begin(), current.end(), mctp_if_name) == current.end()) {
                mctp_if_name.clear();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cerr << "Warning: mctpserial device did not disappear after close()" << std::endl;
    }
}

/**
 * @brief Returns the name of the active mctpserial device.
 *
 * @return std::string Name of the mctpserial device (e.g., "mctpserial0"),
 *         or empty string if no device is active.
 */
std::string LinuxMctpSerial::getMctpIfName() const {
    return mctp_if_name;
}

/**
 * @brief Lists all currently available mctpserial devices.
 *
 * Scans /sys/class/net/ for entries matching the pattern "mctpserial[0-9]+".
 *
 * @return std::vector<std::string> List of mctpserial device names.
 */
std::vector<std::string> LinuxMctpSerial::list_mctp_devices() {
    const std::string net_path = "/sys/class/net/";
    DIR* dir = opendir(net_path.c_str());
    std::vector<std::string> devices;

    if (!dir) return devices;

    struct dirent* entry;
    std::regex pattern("^mctpserial[0-9]+$");

    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_LNK && std::regex_match(entry->d_name, pattern)) {
            devices.push_back(entry->d_name);
        }
    }

    closedir(dir);
    return devices;
}
