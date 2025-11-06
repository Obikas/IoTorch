/** 
 * @file MctpBridge.hpp
 * @brief Class for creating an MCTP bridge object.
 *
 * This class provides functionality to initialize and tear down MCTP serial links
 * by configuring a TTY device with the MCTP line discipline, monitoring the creation
 * and removal of associated mctpserial network interfaces, and managing terminal settings.
 *
 * In addition, a persistent broadcast device is created that allows socket listenders to 
 * receive broadcast events from the downstream serial port, and upstream senders to send
 * broadcast messages to the downstream device.
 *
 * This class requires that the linux kernel be built with MCTP core functions enabled and 
 * the MCTP serial module intalled and running.
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
#include "LinuxMctpSerial.hpp"
#include "MctpSerial.hpp"
#include "NullModem.hpp"
#include "SerialInspector.hpp"
#include "BcastMessenger.hpp"
extern "C" {
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/ioctl.h>
    #include <linux/tty.h>
    #include <dirent.h>
    #include <termios.h>
}

class MctpBridge {
public:
    MctpBridge();
    ~MctpBridge();

    // Initializes the MCTP serial link and returns the associated mctpserial device name
    bool open(const std::string& tty_path, std::string &mctp_dev_name, BaudRate baud, bool hw_flow_control);

    // Closes the TTY and waits for the mctpserial device to disappear
    void close();

private:
    LinuxMctpSerial linuxMctp;   // this is the representation of the linux facing mctp interface
    NullModem nullmodem;         // this is the nullmodem virtual interface between the linux kernel and internal router
                                 // both sides are virtual and initially closed, 
    MctpSerial linuxSerial;      // a serial encoder/framer on the internal linux-facing interface
    MctpSerial mctpSerial;       // a serial encoder/framer on the internal tty-facing interface
    SerialInspector ttyInternal; // a virtual link that connects between the external tty and our internal encoder.
                                 // The tty-facing side is opened and configured.
                                 // The virtual side is closed but a device is created for it.
    BcastMessenger bcast;        // stateful sockets-based messenger for broadcast interception.
    
    int linux_fd;                // the virtual serial file descriptor to the linux-side mctp-interface receiver
    int tty_fd;                  // the link to the mctp serial's file descriptor receiver
    std::string bcast_name;      // bradcast device name for this object - persistent for the life of the object
    std::string mctp_name;       // mctpserial name for this object - persistent for the life of the object
    std::string tty_name;        // the name of the bound tty device - persistent for the life of the object
    std::atomic<bool> running;   // a flag that controls the state of the receive thread
    std::thread dispatch_thread; // a thread used for data reception

    bool setup_bcast(std::string name);
    void run();
};
