#define _GNU_SOURCE
#include <iostream>
#include <vector>
#include <thread>
#include <sched.h>
#include <pthread.h>
#include "bmutex.hh"

constexpr int NUM_THREADS = 4;
constexpr int INCREMENTS_PER_THREAD = 10;

BareMetalMutex mutex;
volatile int counter = 0;

void worker(int thread_id) {
    // Set thread affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(thread_id % NUM_THREADS, &cpuset); // core id must be < num simulated CPUs

    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    for (int i = 0; i < INCREMENTS_PER_THREAD; i++) {
        mutex.lock();
        ++counter;
        mutex.unlock();
    }

    std::cout << "Thread " << thread_id << " ran on CPU " << sched_getcpu() << std::endl;
}

int main() {
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "Final counter value: " << counter << std::endl;
    return 0;
}