#define _GNU_SOURCE
// #include <iostream>
// #include <vector>
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

    worker();


    // thread join
    for(int i=0; i<100; i++){

    }

    // std::cout << "Final counter value: " << counter << std::endl;
    return 0;
}