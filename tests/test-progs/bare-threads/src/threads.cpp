#include <iostream>
#include <thread>
#include <vector>
#include "bmutex.hh"

constexpr int NUM_THREADS = 4;
constexpr int INCREMENTS_PER_THREAD = 1000;

BareMetalMutex mutex;
volatile int counter = 0;

void worker() {
    for (int i = 0; i < INCREMENTS_PER_THREAD; ++i) {
        mutex.lock();
        ++counter;
        mutex.unlock();
    }
}

int main() {
    // std::vector<std::thread> threads;

    // for (int i = 0; i < NUM_THREADS; ++i) {
    //     threads.emplace_back(worker);
    // }

    // for (auto& t : threads) {
    //     t.join();
    // }

    worker();

    std::cout << "Final counter value: " << counter << std::endl;
    return 0;
}