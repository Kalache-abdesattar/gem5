#ifndef __CPU_TRACE_SYNC_ARBITER_HH__
#define __CPU_TRACE_SYNC_ARBITER_HH__

#include <deque>
#include <set>
#include <unordered_map>
#include <vector>

#include "params/SyncArbiter.hh"
#include "sim/sim_object.hh"

namespace gem5
{

/**
 * A CPU the arbiter can park and release: TraceCPU and IntervalCPU.
 */
class SyncParticipant
{
  public:
    virtual ~SyncParticipant() = default;
    /** Called by the arbiter when this CPU may continue. */
    virtual void releaseFromSync() = 0;
};

/**
 * Barrier arbiter for trace replay.
 *
 * Each CPU registers once at init. When a TraceCPU reaches a recorded
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

    /**
     * Called once per CPU at init. Returns the participant id. A CPU whose
     * trace has no barrier or parallel-region events passes barriers=false:
     * it may still take the critical-section lock, but barriers do not wait
     * for it. Otherwise a thread with only lock events (a main thread parked
     * at join while its workers meet at barriers) would hold every barrier.
     */
    unsigned registerCpu(SyncParticipant *cpu, bool barriers = true);

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

    /*
     * Thread creation and join, keyed by trace (thread) number rather than
     * participant id, because a thread can take part in spawn/join without
     * ever reaching a barrier. A child whose spawn appears in some parent's
     * sync trace starts parked and is released when the parent reaches it;
     * a parent at join is released when the child's trace is exhausted.
     */
    void registerThread(unsigned tid, SyncParticipant *cpu);
    /** A parent's trace spawns `child`; call at init, before startup. */
    void expectSpawn(unsigned child);
    /** True while `tid` waits for its spawn. */
    bool spawnPending(unsigned tid) const { return gated.count(tid) != 0; }
    void spawn(unsigned child);
    /** True if `child` has already exited; otherwise `waiter` is parked. */
    bool join(unsigned child, SyncParticipant *waiter);
    void threadExit(unsigned tid);

    unsigned numRegistered() const { return participants.size(); }

    uint64_t numBarriers() const { return barriersReleased; }
    uint64_t numCriticalGrants() const { return criticalGrants; }

  private:
    void releaseAll();

    std::vector<SyncParticipant *> participants;
    std::vector<bool> arrived;
    std::vector<bool> active;      ///< counts at barriers and not retired
    std::vector<bool> retired;
    unsigned expected;     /**< from params; 0 = all registered */
    unsigned numArrived;
    unsigned numActive;
    uint64_t barriersReleased;

    /* Critical-section lock: -1 when free, else the holding participant. */
    int lockHolder;
    std::deque<unsigned> lockQueue;
    uint64_t criticalGrants;

    std::unordered_map<unsigned, SyncParticipant *> threads;
    std::set<unsigned> gated;     ///< spawn expected, not yet reached
    std::set<unsigned> exited;
    std::unordered_map<unsigned, std::vector<SyncParticipant *>> joinWaiters;
};

} // namespace gem5

#endif // __CPU_TRACE_SYNC_ARBITER_HH__
