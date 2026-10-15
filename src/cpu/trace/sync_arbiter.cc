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
      barriersReleased(0),
      lockHolder(-1),
      criticalGrants(0)
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
    if (lockHolder == (int)id)
        releaseLock(id);
    DPRINTF(TraceCPUData, "SyncArbiter: cpu %u retired, %u active\n",
            id, numActive);
    const unsigned target = expected ? expected : numActive;
    if (numActive > 0 && numArrived >= target)
        releaseAll();
}

bool
SyncArbiter::acquire(unsigned id)
{
    assert(id < participants.size());
    if (lockHolder == (int)id) {
        // Already the holder. A trace derived from pthread calls can show a
        // second acquire without an intervening release, because
        // pthread_cond_wait releases and reacquires its mutex internally,
        // below the exported symbols Pin hooks. Queueing here would park the
        // CPU behind itself and deadlock. Grant it, and let the next release
        // free the lock -- no nesting depth, because an approximate trace can
        // also lose the matching release and would then leave it held
        // forever.
        DPRINTF(TraceCPUData, "SyncArbiter: cpu %u re-entered critical\n", id);
        return true;
    }
    if (lockHolder < 0) {
        lockHolder = (int)id;
        ++criticalGrants;
        DPRINTF(TraceCPUData, "SyncArbiter: cpu %u entered critical\n", id);
        return true;
    }
    DPRINTF(TraceCPUData, "SyncArbiter: cpu %u queued on critical (held by %d)\n",
            id, lockHolder);
    lockQueue.push_back(id);
    return false;
}

void
SyncArbiter::releaseLock(unsigned id)
{
    if (lockHolder != (int)id) {
        // Can happen if a CPU retires mid-section; nothing to hand over.
        return;
    }
    lockHolder = -1;
    while (!lockQueue.empty()) {
        const unsigned next = lockQueue.front();
        lockQueue.pop_front();
        if (!active[next])
            continue;
        lockHolder = (int)next;
        ++criticalGrants;
        DPRINTF(TraceCPUData, "SyncArbiter: cpu %u handed critical to %u\n",
                id, next);
        participants[next]->releaseFromSync();
        return;
    }
    DPRINTF(TraceCPUData, "SyncArbiter: cpu %u left critical, now free\n", id);
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
