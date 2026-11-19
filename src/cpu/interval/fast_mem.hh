/*
 * Direct-call memory interface between a CPU model and a cache model that
 * resolves hits in one pass (no packets through ports, no events per hit).
 *
 * The CPU calls fastMemAccess() at the tick the access is issued. A hit
 * returns its completion tick at once. A miss returns MaxTick and the model
 * later calls fastMemDone() with the completion tick; that tick may still be
 * in the future, and the CPU delivers the response when it reaches it.
 *
 * Protocol-neutral, so the CPU does not depend on the Ruby protocol build.
 */

#ifndef __CPU_INTERVAL_FAST_MEM_HH__
#define __CPU_INTERVAL_FAST_MEM_HH__

#include "base/types.hh"
#include "mem/packet.hh"

namespace gem5
{

class FastMemClient
{
  public:
    virtual ~FastMemClient() = default;
    /** A pending access completes at @when (>= curTick()). */
    virtual void fastMemDone(PacketPtr pkt, Tick when) = 0;
};

class FastMem
{
  public:
    virtual ~FastMem() = default;
    virtual void setFastMemClient(FastMemClient *client) = 0;
    /** Completion tick of a hit, or MaxTick if fastMemDone() follows. */
    virtual Tick fastMemAccess(PacketPtr pkt) = 0;
};

} // namespace gem5

#endif // __CPU_INTERVAL_FAST_MEM_HH__
