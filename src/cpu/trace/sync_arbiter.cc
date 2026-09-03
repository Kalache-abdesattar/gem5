#include "cpu/trace/sync_arbiter.hh"

#include "base/trace.hh"
#include "cpu/trace/trace_cpu.hh"
#include "debug/TraceCPUData.hh"

namespace gem5
{

SyncArbiter::SyncArbiter(const Params &params)
    : SimObject(params),
      expected(params.num_cpus),
      numArrived(0),
      numActive(0),
      barriersReleased(0)
{
}

unsigned
SyncArbiter::registerCpu(TraceCPU *cpu)
{
    const unsigned id = participants.size();
    participants.push_back(cpu);
    arrived.push_back(false);
    active.push_back(true);
    ++numActive;
    return id;
}

void
SyncArbiter::arrive(unsigned id)
{
    assert(id < participants.size());
    if (arrived[id])
        return;
    arrived[id] = true;
    ++numArrived;

    const unsigned target = expected ? expected : numActive;
    DPRINTF(TraceCPUData, "SyncArbiter: cpu %u arrived (%u/%u)\n",
            id, numArrived, target);

    if (numArrived >= target)
        releaseAll();
}

void
SyncArbiter::retire(unsigned id)
{
    assert(id < participants.size());
    if (!active[id])
        return;
    active[id] = false;
    --numActive;
    // A retiring CPU must not hold the barrier: if it had already arrived,
    // drop its vote, then re-evaluate in case it was the last one outstanding.
    if (arrived[id]) {
        arrived[id] = false;
        --numArrived;
    }
    DPRINTF(TraceCPUData, "SyncArbiter: cpu %u retired, %u active\n",
            id, numActive);
    const unsigned target = expected ? expected : numActive;
    if (numActive > 0 && numArrived >= target)
        releaseAll();
}

void
SyncArbiter::releaseAll()
{
    ++barriersReleased;
    DPRINTF(TraceCPUData, "SyncArbiter: releasing barrier %llu\n",
            barriersReleased);
    for (unsigned i = 0; i < participants.size(); ++i) {
        arrived[i] = false;
    }
    numArrived = 0;
    for (unsigned i = 0; i < participants.size(); ++i) {
        if (active[i])
            participants[i]->releaseFromSync();
    }
}

} // namespace gem5
