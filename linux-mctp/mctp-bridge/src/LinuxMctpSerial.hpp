/**
 * @file LinuxMctpSerial.hpp
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
#pragma once
#include <string>
#include <cstring>
#include <vector>
extern "C" {
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/ioctl.h>
    #include <linux/tty.h>
    #include <dirent.h>
}

class LinuxMctpSerial {
public:
    LinuxMctpSerial();
    ~LinuxMctpSerial();

    // Initializes the MCTP serial link and returns the associated mctpserial device name
    std::string initialize(const std::string& tty_path, const std::string& mctp_if_name); 
 
    // Closes the TTY and waits for the mctpserial device to disappear
    void close();

    // Returns the name of the active mctpserial device
    std::string getMctpIfName() const;

private:
    int fd_;
    std::string mctp_if_name;

    // Helper to list all current mctpserial devices
    std::vector<std::string> list_mctp_devices();
};
