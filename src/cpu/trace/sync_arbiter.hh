#ifndef __CPU_TRACE_SYNC_ARBITER_HH__
#define __CPU_TRACE_SYNC_ARBITER_HH__

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

    /** A TraceCPU has reached a synchronisation point. */
    void arrive(unsigned id);

    /** A TraceCPU's trace is exhausted; stop expecting it at barriers. */
    void retire(unsigned id);

    unsigned numRegistered() const { return participants.size(); }

  private:
    void releaseAll();

    std::vector<TraceCPU *> participants;
    std::vector<bool> arrived;
    std::vector<bool> active;
    unsigned expected;     /**< from params; 0 = all registered */
    unsigned numArrived;
    unsigned numActive;
    uint64_t barriersReleased;
};

} // namespace gem5

#endif // __CPU_TRACE_SYNC_ARBITER_HH__
