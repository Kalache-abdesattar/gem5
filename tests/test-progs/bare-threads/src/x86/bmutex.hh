// mutex.h
#pragma once

#include <atomic>

class BareMetalMutex {
private:
    std::atomic_flag locked = ATOMIC_FLAG_INIT;

public:
    BareMetalMutex() = default;
    ~BareMetalMutex() = default;

    // Delete copy semantics
    BareMetalMutex(const BareMetalMutex&) = delete;
    BareMetalMutex& operator=(const BareMetalMutex&) = delete;

    void lock() {
        while (locked.test_and_set(std::memory_order_acquire)) {
            // Busy-wait (spin)
            // Optionally insert pause/yield instruction here
        }
    }

    void unlock() {
        locked.clear(std::memory_order_release);
    }

    bool try_lock() {
        return !locked.test_and_set(std::memory_order_acquire);
    }
};