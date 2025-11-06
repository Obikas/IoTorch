/**
 * @file ReceiveBuffer.cpp
 * @brief Thread-safe FIFO for passing messages between threads.
 *
 * Simple wrapper around a std::deque<std::vector<uint8_t>> providing
 * thread-safe push/pop operations and a condition variable for blocking
 * waits. This is used by serial/bridge code to hand off received frames
 * between producer and consumer threads.
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

#include "ReceiveBuffer.hpp"

/**
 * @brief Push a message into the buffer.
 *
 * Thread-safe: acquires the internal mutex, appends the message, and notifies
 * one waiting consumer.
 *
 * @param[in] data Byte vector containing the message to enqueue.
 * @return void
 */
void ReceiveBuffer::push(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.push_back(data);
    cond_.notify_one();
}

/**
 * @brief Pop a message from the buffer, blocking until one is available.
 *
 * Waits on the internal condition variable until the queue is non-empty,
 * then removes and returns the front element.
 *
 * @return std::vector<uint8_t> The popped message.
 */
std::vector<uint8_t> ReceiveBuffer::pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [this] { return !buffer_.empty(); });

    std::vector<uint8_t> data = buffer_.front();
    buffer_.pop_front();
    return data;
}

/**
 * @brief Try to pop a message without blocking.
 *
 * If the buffer is empty an empty vector is returned immediately.
 *
 * @return std::vector<uint8_t> The popped message or an empty vector if none.
 */
std::vector<uint8_t> ReceiveBuffer::tryPop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (buffer_.empty()) return {};
    std::vector<uint8_t> data = buffer_.front();
    buffer_.pop_front();
    return data;
}

/**
 * @brief Check whether the buffer is empty.
 *
 * @return true if the buffer contains no messages, false otherwise.
 */
bool ReceiveBuffer::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_.empty();
}
