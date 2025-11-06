/**
 * @file MctpSerial.cpp
 * @brief Implementation of the MCTP serial driver and receive FSM.
 *
 * Implements a state machine that receives a byte stream from a serial
 * device (or file descriptor) and assembles/validates MCTP frames. The file
 * contains code to configure the serial port, run a background receive
 * thread, and provide send/receive helpers for higher-level code.
 *
 * Portions of this code are based on the Management Component Transport
 * Protocol (MCTP) specifications from the Distributed Management Task Force
 * (DMTF). This implementation was adapted from original PICMG source and
 * re-released under the MIT No Attribution (MIT-0) license below.
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

#include "MctpSerial.hpp"
#include "ReceiveBuffer.hpp"
#include <string>
#include <iostream>
extern "C" {
    #include <sys/select.h>     // for select(), fd_set, timeval
    #include <unistd.h>         // for read(), write(), close()
    #include <fcntl.h>          // for open(), O_RDWR, O_NONBLOCK
    #include <termios.h>        // for termios configuration
    #include <errno.h>          // for errno values like EAGAIN
    #include <unistd.h>
}

/**
 * Mctp()
 * Default constructor.
 */
MctpSerial::MctpSerial()
{
    rxState = MCTPSER_WAITING_FOR_SYNC;
    byte_count = 0;
    running = false;
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;    
    serial_fd = -1;
}

/**
 * @brief Destructor
 *
 * Stops the receive thread and closes the underlying serial file descriptor.
 */
MctpSerial::~MctpSerial()
{
    close();
}

/**
 * @brief Open and configure a serial device by path.
 *
 * This configures the device for non-blocking I/O, sets the baud rate and
 * standard 8N1 settings, and starts the receive thread. A wake pipe is
 * created if necessary and the read-end fd is returned for use with select().
 *
 * @param dev_path Path to the serial device (e.g. /dev/ttyS0)
 * @param baud_rate POSIX speed constant (e.g. B115200)
 * @return read-end file descriptor of internal pipe on success, -1 on error
 */
int MctpSerial::open(std::string dev_path, int baud_rate) {
    struct termios tty;
    if (serial_fd>0) return -1;

    // create the communications pipe (if required)
    if (pipe_fds[0]<0) {
        if (pipe(pipe_fds)==-1) {
            return -1;
        }
    }

    serial_fd = ::open(dev_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serial_fd < 0) {
        return false;
    }

    if (tcgetattr(serial_fd, &tty) != 0) {
        ::close(serial_fd);
        serial_fd = -1;
        return -1;
    }

    // Set baud rate
    cfsetospeed(&tty, baud_rate); // Output baud rate
    cfsetispeed(&tty, baud_rate); // Input baud rate

    // Input flags - clear all processing
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);

    // Output flags - disable post-processing
    tty.c_oflag &= ~OPOST;

    // Control flags - set 8N1 (8-bit, no parity, 1 stop bit)
    tty.c_cflag &= ~(CSIZE | PARENB);
    tty.c_cflag |= CS8;

    // Local flags - disable echo and canonical mode
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

    if (tcsetattr(serial_fd, TCSANOW, &tty) != 0) {
        ::close(serial_fd);
        serial_fd = -1;
        return -1;
    }

    running = true;
    // Start the reception thread
    recv_thread = std::thread(&MctpSerial::run, this);
    return pipe_fds[0];
}

/**
 * @brief Attach to an already-open serial file descriptor.
 *
 * The provided file descriptor will be configured for non-blocking I/O and
 * termios settings applied consistent with the open(path, ...) variant.
 *
 * @param fd Already-open serial file descriptor
 * @return read-end file descriptor of internal pipe on success, -1 on error
 */
int MctpSerial::open(int fd) {
    struct termios tty;
    if (serial_fd>0) return -1;
    if (fd<0) return -1;
    serial_fd = fd;

    // create the communications pipe (if required)
    if (pipe_fds[0]<0) {
        if (pipe(pipe_fds)==-1) {
            return -1;
        }
    }

    if (serial_fd<0) return -1;

    // create the communications pipe (if required)
    if (pipe_fds[0]<0) {
        if (pipe(pipe_fds)==-1) {
            return -1;
        }
    }

    int flags = fcntl(serial_fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return false;
    }

    if (fcntl(serial_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        return false;
    }

    if (tcgetattr(serial_fd, &tty) != 0) {
        ::close(serial_fd);
        serial_fd = -1;
        return -1;
    }

    // Input flags - clear all processing
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);

    // Output flags - disable post-processing
    tty.c_oflag &= ~OPOST;

    // Control flags - set 8N1 (8-bit, no parity, 1 stop bit)
    tty.c_cflag &= ~(CSIZE | PARENB);
    tty.c_cflag |= CS8;

    // Local flags - disable echo and canonical mode
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

    if (tcsetattr(serial_fd, TCSANOW, &tty) != 0) {
        ::close(serial_fd);
        serial_fd = -1;
        return -1;
    }

    running = true;
    // Start the reception thread
    recv_thread = std::thread(&MctpSerial::run, this);
    return pipe_fds[0];
}

/**
 * @brief Close the serial interface and stop the receive thread.
 *
 * Waits for the receive thread to exit and closes the serial fd. Safe to
 * call multiple times.
 *
 * @return true always (placeholder for future error reporting)
 */
bool MctpSerial::close() {
    bool wait_for_join = running;
    running = false;
    if ((wait_for_join)&&(recv_thread.joinable())) {
        recv_thread.join();
    }

    if (serial_fd>=0) 
        ::close(serial_fd);
    serial_fd = -1;

    return true;
 }

 /**
  * @brief Background receive thread main loop.
  *
  * Blocks on select() for data on the serial fd and calls updateRxFSM() to
  * process incoming bytes. The loop exits when `running` is cleared.
  */
 void MctpSerial::run() {
    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(serial_fd, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ret = select(serial_fd + 1, &readfds, nullptr, nullptr, &timeout);
        if (ret < 0) {
            break;
        }
        if (ret == 0) continue;  // Timeout, check running again

        if (FD_ISSET(serial_fd, &readfds)) {
            updateRxFSM();           
        }
    }
}


/**
 * @brief Update the Frame Check Sequence (FCS) over a buffer.
 *
 * The algorithm follows the CRC table in fcstab[] and updates the running
 * FCS value for `len` bytes starting at `cp`.
 *
 * @param fcs Initial FCS value
 * @param cp Pointer to the data bytes to include in the FCS
 * @param len Number of bytes to process
 * @return Updated FCS value
 */
uint16_t MctpSerial::calcFcs(uint16_t fcs, uint8_t *cp, int len)
{
    for(int i = 0; i<len; i++)
        fcs = 0x0ffff&((fcs >> 8) ^ fcstab[(fcs ^ (((int)cp[i])&0x0ff)) & 0xff]);
    return (fcs);
}

/**
 * transmitFrame()
 * This function sends the mctp frame, adding sync escapes as required.
 * @param msg - the mctp message, Framing flags and FCS;
 */
void MctpSerial::transmitFrame(std::vector<uint8_t> msg) {
    for (int i=0;i<msg.size();i++) {
        uint8_t data = msg[i];
        if ((i==0)||(i==msg.size()-1)) {
            // don't escape the first and last characters - they are framing marks
            ssize_t w = ::write(serial_fd, &data, 1);
        } else if ((data == FRAME_CHAR) || (data == ESCAPE_CHAR)) {
            ssize_t w = ::write(serial_fd, &ESCAPE_CHAR, 1);
            data = data - (uint8_t)0x20;
            w = ::write(serial_fd, &data, 1);
        } else {
            // write the character
            ssize_t w = ::write(serial_fd, &data, 1);
        }
    }
}

/**
 * send()
 * This function sends a MCTP packet without waiting for response.
 * Message encoding for serial transmission will occur during the transmission process
 * 
 * @param msg - the message being transmitted, inluding the transport-independent header, but
 *              not including serial transport header, framing flags and FCS
 * @param message_type - the message type: 0 for command, 1 for PLDM
 */
void MctpSerial::send(std::vector<uint8_t> msg){
    // create the full message, starting with the framing mark and sertial header
    std::vector<uint8_t> full_msg = {FRAME_CHAR, 0x01, (uint8_t)msg.size()};
    full_msg.insert(full_msg.end(), msg.begin(), msg.end());
    
    // calculate the frame check sequence on the message    
    uint16_t fcs = calcFcs(INITFCS, full_msg.data()+1, full_msg.size()-1);
    
    // add the FCS to the message
    full_msg.push_back(fcs>>8);
    full_msg.push_back(fcs&0xff);

    // add the final frame mark to the message
    full_msg.push_back(FRAME_CHAR);

    // write the frame to the serial port
    transmitFrame(full_msg);
}

/**
 * rxEmpty()
 * This is a helper function that returns the the state of the rx buffer
 * @return - true if empty, otherwise false
 */
bool MctpSerial::rxEmpty() {
    return 	rxBuffer.empty();
}

/**
 * Validate the rx frame that has been received.  It should include all characters from the 
 * start frame through the end frame.
 */
bool MctpSerial::validateRx() {
    // a minimum message must include 6 bytes of header, 1 message type, 2 bytes of FCS, and 2 frame marks
    if (rx_in_progress.size()<11) return false;

    // byte count must be correct: total - 2 serial header - 2 fcs - 2 framing marks
    byte_count = rx_in_progress[2];
    if ((uint16_t)byte_count != (uint16_t)rx_in_progress.size()-6) return false;

    // frame check sequence must match  Note, there is an inconsistency between the linux
    // implmentations and the DMTF specification.  The DMTF spec states that that the 
    // frame check sequence shoudl include the first framimg mark.  However, no linux
    // implementation does this.  This code implements FCS in accordance with existing
    // implementations to promote interoperability.
    uint16_t fcs = calcFcs(INITFCS, rx_in_progress.data()+1, rx_in_progress.size()-4);
    uint16_t msg_fcs = rx_in_progress[rx_in_progress.size()-3];
    msg_fcs = msg_fcs<<8;
    msg_fcs += rx_in_progress[rx_in_progress.size()-2];
    if (msg_fcs!=fcs) return false;
    return true;
}

/**
 * updateRxFSM()
 * This is a finite state machine that takes in chars from the serial port
 * and builds them into a MCTP packet and validates the packet. This
 * should be called from the primary communications handler.
 * 
 * This FSM receives the packet byte-by-byte.  Encoding is specified in detail
 * in DMTF specification DSP0253 (MCTP over Serial Transport).  Byte ordering is as follows:
 * 
 * ## Serial Transport Header
 * 0: Framing flag (0x7F)
 * 1: Serial Protocol revision (0x01)
 * 2: Byte count (payload plus 5-byte mctp header)
 * ## MCTP Header
 * 3: Flags/MCTP Header Version
 * 4: Destination EID
 * 5: Source EID
 * 6: SOM/EOM/SEQ#/TO/Tag
 * 7: IC/Message Type
 * 8 - (N-1): Message
 * N-N+1: Frame Check Sequence, MSB first
 * N+1: Framing Flag (0x7E)
 * 
 * FCS is performed over bytes 0 - N-1
 * 
 */
void MctpSerial::updateRxFSM() {
    // reading in char from serial port
    uint8_t by;
    ssize_t result = read(serial_fd, &by, 1);
    if (result == 1) {
        switch (rxState) {
            case MCTPSER_WAITING_FOR_SYNC:
                // checking sync char for start of frame
                if (by == FRAME_CHAR) {
                    rxState = MCTPSER_BODY;
                    rx_in_progress.clear();
                    rx_in_progress.push_back(FRAME_CHAR);
                }
                break;
            case MCTPSER_BODY:
                // building data payload
                if (by == ESCAPE_CHAR) {
                    rxState = MCTPSER_ESCAPE;
                }
                else if (by == FRAME_CHAR) {
                    // this is either that start end of the message - perform checks,
                    // or the start of a new message due to sync issues.
                    rx_in_progress.push_back(FRAME_CHAR);
                    if (validateRx()) {
                        // remove serial preamble, Framing, and FCS
                        rx_in_progress.erase(rx_in_progress.begin(), rx_in_progress.begin() + 3);
                        rx_in_progress.erase(rx_in_progress.end()-3, rx_in_progress.end());
                        std::vector<uint8_t> packet = rx_in_progress;
                        rxBuffer.push(packet);

                        // notify that message is available
                        ssize_t _w = ::write(pipe_fds[1], "x", 1);
                        if (_w != 1) {
                            // pipe write failure is non-fatal for message delivery; log and continue
                            perror("MctpSerial: pipe write failed");
                        }
                        rxState = MCTPSER_WAITING_FOR_SYNC;
                    } else {
                        rxState = MCTPSER_BODY;
                        rx_in_progress.clear();
                        rx_in_progress.push_back(FRAME_CHAR);
                    }
                } else {
                    rx_in_progress.push_back(by);
                    if (rx_in_progress.size()>550) {
                        // too many characters
                        rxState = MCTPSER_WAITING_FOR_SYNC;
                    } 
                }
                break;
            case MCTPSER_ESCAPE:
                // if escaped sync or escaped escape are detected in data payload,
                // this either replaces it or drops the packet
                if ((by==(ESCAPE_CHAR-0x20))||(by==(FRAME_CHAR-0x20))) {
                    by = (uint8_t)(by+0x20);
                    rx_in_progress.push_back(by); 
                    rxState = MCTPSER_BODY;
                    if (rx_in_progress.size()>550) {
                        // too many characters
                        rxState = MCTPSER_WAITING_FOR_SYNC;
                    } 
                } else if (by == FRAME_CHAR) {
                    // possibly start of new frame after corruption
                    rx_in_progress.clear();
                    rx_in_progress.push_back(FRAME_CHAR);
                    rxState = MCTPSER_BODY;
                } else {
                    // some other error - flush until resync
                    rxState = MCTPSER_WAITING_FOR_SYNC;
                }
                break;
        }
    }
}

/**
 * receive()
 * Returns a packet from the buffer.
 * @return - the buffer's contents, usually a MCTP packet
 */
std::vector<uint8_t> MctpSerial::receive() {
    std::vector<uint8_t> empty;
    if (rxBuffer.empty())
        return empty;
    return rxBuffer.pop();
}
