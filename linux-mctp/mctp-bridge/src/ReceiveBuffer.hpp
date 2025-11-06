/**
 * @file ReceiveBuffer.hpp
 * @brief Header for a small thread-safe FIFO buffer (ReceiveBuffer).
 *
 * Provides a concurrency-safe message queue used to transfer byte vectors
 * between producer and consumer threads. The implementation uses a mutex and
 * condition_variable to support blocking and non-blocking pop operations.
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
#include <deque>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <cstdint>

class ReceiveBuffer {
public:
    /**
     * @brief Push a message into the buffer.
     *
     * Thread-safe: enqueues `data` and notifies one waiting consumer.
     * @param[in] data Byte vector to enqueue.
     */
    void push(const std::vector<uint8_t>& data);

    /**
     * @brief Pop a message from the buffer, blocking until available.
     * @return std::vector<uint8_t> The popped message.
     */
    std::vector<uint8_t> pop();

    /**
     * @brief Try to pop a message without blocking.
     * @return std::vector<uint8_t> The popped message or an empty vector if none available.
     */
    std::vector<uint8_t> tryPop();

    /**
     * @brief Check whether the buffer is empty.
     * @return true if empty, false otherwise.
     */
    bool empty() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::deque<std::vector<uint8_t>> buffer_;
};

