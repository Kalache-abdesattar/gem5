#ifndef __CPU_TRACE_SYNC_ARBITER_HH__
#define __CPU_TRACE_SYNC_ARBITER_HH__

#include <deque>
#include <vector>

#include "params/SyncArbiter.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class TraceCPU;

/**
 * Barrier arbiter for trace replay.
 *
 * Each TraceCPU registers once at init. When a TraceCPU reaches a recorded
 * synchronisation point it calls arrive(); when every registered participant
 * has arrived, the arbiter releases them all and resets for the next region.
 *
 * A TraceCPU whose trace has run out calls retire() so it stops being counted,
 * otherwise the remaining CPUs would deadlock waiting for a finished one.
 */
class SyncArbiter : public SimObject
{
  public:
    PARAMS(SyncArbiter);
    SyncArbiter(const Params &params);

    /** Called once per TraceCPU at init. Returns the participant id. */
    unsigned registerCpu(TraceCPU *cpu);

    /** A TraceCPU has reached a barrier. Blocks until all participants do. */
    void arrive(unsigned id);

    /**
     * Enter a critical section. Returns true if the lock was free and the
     * caller may proceed immediately; false if it was queued, in which case
     * the caller must stall and will be released via releaseFromSync().
     *
     * A critical section is mutual exclusion, not a rendezvous: treating it as
     * a barrier would serialise every thread at every lock and systematically
     * over-serialise lock-heavy workloads.
     */
    bool acquire(unsigned id);

    /** Leave a critical section, handing the lock to the next waiter. */
    void releaseLock(unsigned id);

    /** A TraceCPU's trace is exhausted; stop expecting it at barriers. */
    void retire(unsigned id);

    unsigned numRegistered() const { return participants.size(); }

    uint64_t numBarriers() const { return barriersReleased; }
    uint64_t numCriticalGrants() const { return criticalGrants; }

  private:
    void releaseAll();

    std::vector<TraceCPU *> participants;
    std::vector<bool> arrived;
    std::vector<bool> active;
    unsigned expected;     /**< from params; 0 = all registered */
    unsigned numArrived;
    unsigned numActive;
    uint64_t barriersReleased;

    /* Critical-section lock: -1 when free, else the holding participant. */
    int lockHolder;
    std::deque<unsigned> lockQueue;
    uint64_t criticalGrants;
};

} // namespace gem5

#endif // __CPU_TRACE_SYNC_ARBITER_HH__
