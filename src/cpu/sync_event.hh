#ifndef __CPU_SYNC_EVENT_HH__
#define __CPU_SYNC_EVENT_HH__

#include <cstdint>

namespace gem5
{

/**
 * Payload of the "SyncEvent" probe point notified from the workbegin/workend
 * pseudo-instructions. ElasticTrace listens for these to build the
 * synchronisation trace described in ElasticSimMATE (Nocua et al., 2017).
 *
 * The workload encodes the event class in the low bits of workid:
 *   0 = parallel region, 1 = critical section, 2 = barrier.
 */
struct SyncEventInfo
{
    uint64_t workid;
    uint64_t threadid;
    bool isEnd;
};

} // namespace gem5

#endif // __CPU_SYNC_EVENT_HH__
