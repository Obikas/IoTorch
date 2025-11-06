/**
 * @file MctpBridge.cpp
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
#include "MctpBridge.hpp"
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <regex>
#include <algorithm>
extern "C" {
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/un.h>     // For struct sockaddr_un
    #include <netinet/in.h>
    #include <net/if.h>        // for if_nametoindex
    #include <linux/if_ether.h> // for ETH_P_ALL
    #include <unistd.h>
    #include <termio.h>
    #include <netpacket/packet.h>
    #include <pty.h>
    #include <sys/select.h>
}

/**
 * @brief Constructs a new MctpBridge object.
 * 
 * Initializes internal components including LinuxMctpSerial, NullModem, MctpSerial,
 * and sets up file descriptors and thread control flags. Actual values are assigned later.
 */
MctpBridge::MctpBridge()
    : linux_fd(-1),
      tty_fd(-1),
      running(false)
{}

/**
 * @brief Destructor for MctpBridge.
 *
 * Automatically calls close() to clean up resources.
 */
MctpBridge::~MctpBridge() {
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
 * @param mctp_name the resulting name of the public mctp device to be bound to.  If not empty on entrance,
 *                 this reflects the desired name for the inerface.  It must be unique across the system.
 * @param bcast_name the resulting name of the broadcast interface
 * @param baud the baud rate expressed 
 * @return true on success, otherwise failure.
 */
bool MctpBridge::open(const std::string& tty_path, std::string &mctp_name, BaudRate baud, bool hw_flow_control) {
    // close if already configured.  this also stops the routing thread
    close();

    // first, connect the slave end of the null-modem to an MCTP-serial converter.  This 
    // will remove framing and ethernet header from arriving packets from linux, and 
    // add them to packets bound for linux.  The linuxSerial object will open and configure its
    // side of the nullmodem connection
    linux_fd = linuxSerial.open(nullmodem.getName2(), B115200);

    // next, bind the linux side to it.  Thie linux side will also open and configure its
    // side of the nullmodem cable, including setting the line discipline to tty.
    linuxMctp.initialize(nullmodem.getName1(), mctp_name);
    
    // create the broadcast link
    if (!setup_bcast("bcast_"+linuxMctp.getMctpIfName())) {
        close();
        return false;
    }

    // connect to the physical interface
    ttyInternal.setTty(tty_path, baud, hw_flow_control);
    tty_fd = mctpSerial.open(ttyInternal.getVirtualName(), B115200);
    if (tty_fd < 0) {
        close();
    }

    running = true;

    this->mctp_name = linuxMctp.getMctpIfName();
    mctp_name = this->mctp_name;
    bcast_name = this->bcast_name;

    // Start the dispatch loop in a thread
    dispatch_thread = std::thread(&MctpBridge::run, this);
    return true;
}

/**
 * @brief Closes the active MCTPbridge and releases its resoures
 */
void MctpBridge::close() {
    // stop the dispatch thread
    running = false;
    if (dispatch_thread.joinable()) {
        dispatch_thread.join();
    }

    // close related devices
    linuxMctp.close();
    mctpSerial.close();
    linuxSerial.close();

    // close the file descriptors
    if (linux_fd>=0) {
        ::close(linux_fd);
        linux_fd = -1;
    }
    if (tty_fd>=0) {
        ::close(linux_fd);
        linux_fd = -1;
    }
}

/**
 * @brief Run the main bridging function until interrupted
 * 
 * This method runs within a separate thread and is started when
 * the bridge is successfully opened.  
 */
void MctpBridge::run() {
   while (running) {
        // Try to accept a new connection (non-blocking)
        if (!bcast.isConnected()) {
            bcast.acceptConnection();  
        }

        fd_set readfds;
        FD_ZERO(&readfds);

        // stateless sockets
        FD_SET(linux_fd, &readfds);
        FD_SET(tty_fd, &readfds);

        int max_fd = std::max(linux_fd, tty_fd);

        // Add bcast socket only if connected
        if (bcast.isConnected()) {
            FD_SET(bcast.getFd(), &readfds);
            max_fd = std::max(max_fd, bcast.getFd());
        }

        struct timeval timeout = {0, 500000};  // .5 second timeout
        int ret = select(max_fd + 1, &readfds, nullptr, nullptr, &timeout);
        if (ret <= 0) {
            continue;
        }
        
        if (bcast.isConnected() && FD_ISSET(bcast.getFd(), &readfds)) {
            std::vector<uint8_t> msg = bcast.recvMessage();
                if (msg.size() > 0) {
                    // TODO: validate that the data actually is an MCTP message
                    std::cout << "Received datagram: " << msg.size() << " bytes on bcast"<<std::flush<<std::endl;;
                
                    // need to route based on the destination eid
                    uint8_t dest_eid = msg[1];
                    if ((dest_eid == 0xff) || (dest_eid == 0x00)) {
                        // broadcast and no/any address are sent the serial port
                        std::cout << "    fwd to serial"<<std::flush<<std::endl;
                        mctpSerial.send(msg);                        
                } else {
                    // everything else gets dropped
                    std::cout << "    dropped"<<std::flush<<std::endl;
                } 
            }
        }

        if (FD_ISSET(linux_fd, &readfds)) {
            // All inbound traffic should be sent directly to the tty.
            char rxflag;
            ssize_t len = read(linux_fd, &rxflag, 1);
            std::vector<uint8_t> msg = linuxSerial.receive();
            if (msg.size() > 0) {
                std::cout << "Received from linux subsystem: " << msg.size() << " bytes\n";
                
                // Linux messages are alway sent to the serial device
                mctpSerial.send(msg);
            }
        }

        if (FD_ISSET(tty_fd, &readfds)) {
            char rxflag;
            ssize_t len = read(tty_fd, &rxflag, 1);
            std::vector<uint8_t> msg = mctpSerial.receive();
            if (msg.size() > 0) {
                std::cout << "Received from serial device: " << msg.size() << " bytes"<<std::flush<<std::endl;;
                
                // need to route based on the destination eid
                uint8_t dest_eid = msg[1];
                if ((dest_eid == 0xff) || (dest_eid == 0x00)) {
                    // broadcast and no/any address are sent to the broadcast port
                    std::cout << "    fwd to bcast"<<std::flush<<std::endl;
                    if (bcast.isConnected()) {
                        bcast.sendMessage(msg);
                    }                        
                } else {
                    // everything else goes to linux
                    linuxSerial.send(msg);
                } 
            }
        }
    }
}

/***
 * @brief sets up the broadcast interface and opens it for transmission
 * 
 * @param name - the desired name for the socket
 * @return true if no errors, otherwise false
 */
bool MctpBridge::setup_bcast(std::string name) {
    bcast_name = name;
    if (!bcast.setup("/tmp/"+bcast_name)) {
        bcast_name.clear();
        return false;
    }
    return true;
}
