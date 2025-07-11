#include <iostream>
#include <vector>
#include <thread>
#include "bmutex.hh"

constexpr int NUM_THREADS = 4;
constexpr int INCREMENTS_PER_THREAD = 10;

BareMetalMutex mutex;

volatile int counter = 0;

void worker() {
    for (int i = 0; i < INCREMENTS_PER_THREAD; i++) {
        mutex.lock();
        ++counter;
        mutex.unlock();
    }
}

int main() {
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(worker);
    }

    for (auto& t : threads) {
        t.join();
    }

    // worker();

    // wait for all threads to continue
    // TODO: implement a thread join..at some point
    // for(int i=0; i<10000; i++){}

    std::cout << "Final counter value: " << counter << std::endl;
    return 0;
}