/**
 * @file NullModem.hpp
 * @brief Header for NullModem: PTY pair bridge declarations.
 *
 * Declares the NullModem class which creates two PTY pairs and provides a
 * background bridge thread to copy data between them. Useful for simulating a
 * null-modem connection in software.
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
#include <thread>
#include <atomic>
#include <vector>

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>
}

class NullModem {
public:
    NullModem();
    ~NullModem();

    std::string getName1() const;
    std::string getName2() const;
    void setDebug(bool enable);

private:
    int master1_fd;
    int master2_fd;
    std::string slave1_name;
    std::string slave2_name;
    std::atomic<bool> running;
    std::atomic<bool> debug_traffic;
    std::thread bridge_thread;

    int createPty(std::string& slave_name);
    void bridge();
    void printHex(const std::vector<uint8_t>& data, const std::string& direction);
};