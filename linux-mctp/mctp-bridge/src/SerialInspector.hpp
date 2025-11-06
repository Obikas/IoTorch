/**
 * @file SerialInspector.hpp
 * @brief Header for SerialInspector: PTY <> serial bridge helper.
 *
 * Defines the SerialInspector class which manages a pseudo-terminal pair and
 * bridges data between the master PTY and a real serial device. The class
 * supports non-blocking operation and optional debug hex dumps.
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
#include <mutex>

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>
}

enum class BaudRate : speed_t {
    BR_0        = B0,
    BR_50       = B50,
    BR_75       = B75,
    BR_110      = B110,
    BR_134      = B134,
    BR_150      = B150,
    BR_200      = B200,
    BR_300      = B300,
    BR_600      = B600,
    BR_1200     = B1200,
    BR_1800     = B1800,
    BR_2400     = B2400,
    BR_4800     = B4800,
    BR_9600     = B9600,
    BR_19200    = B19200,
    BR_38400    = B38400,
    BR_57600    = B57600,
    BR_115200   = B115200,
    BR_230400   = B230400
};

class SerialInspector {
public:
    SerialInspector();
    ~SerialInspector();

    std::string getVirtualName() const;
    std::string getRealName() const;
    void setDebug(bool enable);
    void setTty(std::string tty, BaudRate baud, bool hw_flow_control);

private:
    std::mutex fd_mtx;
    int master_fd;
    int real_fd;
    std::string slave_name;
    std::string real_name;
    std::atomic<bool> running;
    std::atomic<bool> debug_traffic;
    std::thread bridge_thread;

    int createPty(std::string& slave_name);
    void bridge();
    void printHex(const std::vector<uint8_t>& data, const std::string& direction);
};
