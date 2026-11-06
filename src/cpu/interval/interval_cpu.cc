/*
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cpu/interval/interval_cpu.hh"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/IntervalCPU.hh"
#include "proto/protoio.hh"
#include "proto/sync_event.pb.h"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5
{

int IntervalCPU::numActive = 0;

// DefaultX86FUPool, by op-class code: IntAlu, IntMult, IntDiv (x86: a 1-cycle
// non-pipelined microcode step), FloatAdd, FloatCmp, FloatCvt, FloatMult,
// FloatMultAcc, FloatMisc, FloatDiv, FloatSqrt, Simd* (all latency 1).
const IntervalCPU::OpClassDesc
IntervalCPU::opClassDesc[IntervalCPU::NumOpClasses] = {
    {FuInt, 1, true},      {FuMulDiv, 3, true},   {FuMulDiv, 1, false},
    {FuFpAlu, 2, true},    {FuFpAlu, 2, true},    {FuFpAlu, 2, true},
    {FuFpMulDiv, 4, true}, {FuFpMulDiv, 5, true}, {FuFpMulDiv, 3, true},
    {FuFpMulDiv, 12, false}, {FuFpMulDiv, 24, false}, {FuSimd, 1, true},
    {FuInt, 1, true},      // barrier: no work; see nonSpec in issue()
};
// IntALU 6, X86IntMultDiv 2, FP_ALU 4, FP_MultDiv 2, SIMD_Unit 4, RdWrPort 4
const unsigned IntervalCPU::fuCount[IntervalCPU::NumFuGroups] =
    {6, 2, 4, 2, 4, 4};

IntervalCPU::IntervalCPU(const IntervalCPUParams &p)
    : ClockedObject(p),
      instRequestorId(p.system->getRequestorId(this, "inst")),
      dataRequestorId(p.system->getRequestorId(this, "data")),
      lineSize(p.system->cacheLineSize()),
      system(p.system),
      dispatchWidth(p.dispatchWidth),
      issueWidth(p.issueWidth),
      commitWidth(p.commitWidth),
      robSize(p.robSize),
      iqSize(p.iqSize),
      lqSize(p.lqSize),
      sqSize(p.sqSize),
      compLatency(p.compLatency),
      storeLatency(p.storeLatency),
      fuModel(p.fuModel),
      forwardLatency(p.forwardLatency),
      loadWakeNextEdge(p.loadWakeNextEdge),
      loadIssueDelay(p.loadIssueDelay),
      commitDelay(p.commitDelay),
      storeWriteDelay(p.storeWriteDelay),
      dispatchToIssueDelay(p.dispatchToIssueDelay),
      staleOccupancy(p.staleOccupancy),
      storeHoldsIq(p.storeHoldsIq),
      branchPred(p.branchPred),
      branchRedirectDelay(p.branchRedirectDelay),
      decodeRedirectDelay(p.decodeRedirectDelay),
      squashWidth(p.squashWidth),
      squashUnblockDelay(p.squashUnblockDelay),
      skidModel(p.skidModel),
      skidSize(p.skidSize),
      tsoStores(p.tsoStores),
      fetchEnabled(p.fetchEnabled),
      fetchWidth(p.fetchWidth),
      fetchQueueSize(p.fetchQueueSize),
      frontendDepth(p.frontendDepth),
      lookaheadSize(p.lookahead),
      memDepPred(p.memDepPred),
      depCheckShift(p.depCheckShift),
      storeSetClearPeriod(p.storeSetClearPeriod),
      lfstSize(p.LFSTSize),
      squashPenalty(p.squashPenalty),
      icachePort(this),
      dcachePort(this),
      trace(p.dataTraceFile, name()),
      rob(p.robSize),
      arbiter(p.arbiter),
      threadId(p.threadId),
      // CPU_Tick_Pri, as O3: the core ticks after the cache and Ruby events
      // of the same tick. At default priority a request sent on an edge
      // caught the Ruby L1 controller that edge (a 1-cycle hit where O3
      // sees 2), and a classic response landing on the edge was seen a
      // cycle late, which had hidden the missing issue-to-execute cycle.
      tickEvent([this]{ tick(); }, name() + ".tick", false,
                Event::CPU_Tick_Pri),
      stats(this)
{
    fatal_if(!dispatchWidth || !issueWidth || !commitWidth,
             "%s: widths must be non-zero", name());
    fatal_if(!robSize || !iqSize || !lqSize || !sqSize,
             "%s: window sizes must be non-zero", name());
    fatal_if(!fetchWidth || !fetchQueueSize || lookaheadSize < fetchWidth,
             "%s: fetchWidth and fetchQueueSize must be non-zero and "
             "lookahead at least fetchWidth", name());

    fatal_if(memDepPred && !lfstSize, "%s: LFSTSize must be non-zero",
             name());
    lfst.assign(lfstSize, 0);
    lfstValid.assign(lfstSize, 0);

    for (unsigned g = 0; g < NumFuGroups; ++g)
        fuBusyUntil[g].assign(fuCount[g], 0);

    iq.reserve(iqSize);
    ++numActive;
}

void
IntervalCPU::init()
{
    ClockedObject::init();
    const auto &p = static_cast<const IntervalCPUParams &>(params());
    if (arbiter)
        arbiter->registerThread(threadId, this);
    if (!p.syncTraceFile.empty())
        loadSyncTrace(p.syncTraceFile);
}

void
IntervalCPU::loadSyncTrace(const std::string &filename)
{
    fatal_if(!arbiter, "%s: syncTraceFile is set but no arbiter was given",
             name());
    ProtoInputStream in(filename);
    ProtoMessage::SyncEventHeader header;
    fatal_if(!in.read(header), "%s: no header in sync trace %s", name(),
             filename);
    ProtoMessage::SyncEvent ev;
    while (in.read(ev)) {
        SyncPoint sp{ev.num_insts(), ev.type(), ev.is_end() != 0,
                     (uint32_t)ev.tid()};
        if (sp.type == SyncSpawn)
            arbiter->expectSpawn(sp.child);   // before any startup()
        else if (sp.type == SyncParallel || sp.type == SyncBarrier)
            barrierMember = countsAtBarriers = true;
        else if (sp.type == SyncCritical)
            barrierMember = true;
        syncPoints.push_back(sp);
    }
    syncEnabled = !syncPoints.empty();
    // Threads with barrier or critical events get an arbiter id; only those
    // with barrier/parallel events count at barriers, otherwise a thread that
    // never reaches one (main, parked at join) would hold them all.
    if (barrierMember)
        arbiterId = arbiter->registerCpu(this, countsAtBarriers);
    inform("%s: thread %u, %u sync points from %s", name(), threadId,
           (unsigned)syncPoints.size(), filename);
}

bool
IntervalCPU::syncGate() const
{
    return syncEnabled && nextSync < syncPoints.size() &&
           dispatchedOps >= syncPoints[nextSync].at;
}

void
IntervalCPU::processSyncPoints()
{
    // Dispatch stops at a sync point, so once everything before it has
    // committed the pipeline has drained up to it.
    while (!parked && syncEnabled && nextSync < syncPoints.size() &&
           committedOps >= syncPoints[nextSync].at) {
        const SyncPoint &sp = syncPoints[nextSync++];
        ++stats.syncPointsReached;
        switch (sp.type) {
          case SyncCritical:
            if (!sp.isEnd) {
                if (!arbiter->acquire(arbiterId))
                    parked = true;
            } else {
                arbiter->releaseLock(arbiterId);
            }
            break;
          case SyncBarrier:
            if (!sp.isEnd) {
                parked = true;              // before arrive(): it may
                arbiter->arrive(arbiterId); // release us immediately
            }
            break;
          case SyncSpawn:
            arbiter->spawn(sp.child);
            break;
          case SyncJoin:
            if (!arbiter->join(sp.child, this))
                parked = true;
            break;
          case SyncParallel:
          default:
            if (sp.isEnd) {
                parked = true;
                arbiter->arrive(arbiterId);
            }
            break;
        }
        if (parked)
            ++stats.syncParks;
    }
    if (barrierMember && syncEnabled && nextSync == syncPoints.size() &&
        !parked) {
        arbiter->retire(arbiterId);
        barrierMember = false;
    }
}

void
IntervalCPU::releaseFromSync()
{
    if (!parked)
        return;
    parked = false;
    wakeAt(clockEdge());
}

void
IntervalCPU::startup()
{
    // A thread created by another starts parked until its spawn point.
    if (arbiter && arbiter->spawnPending(threadId))
        parked = true;
    lastTickCycle = curCycle();
    lastTickTick = curTick();
    schedule(tickEvent, clockEdge());
}

Port &
IntervalCPU::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "icache_port")
        return icachePort;
    if (if_name == "dcache_port")
        return dcachePort;
    return ClockedObject::getPort(if_name, idx);
}

// ---------------------------------------------------------------------------
// Trace reader
// ---------------------------------------------------------------------------

IntervalCPU::TraceReader::TraceReader(const std::string &filename,
                                      const std::string &owner)
    : buf(1 << 20), name(owner)
{
    // gzread passes uncompressed files through, as ProtoInputStream does
    // for names not ending in .gz.
    gzFile f = gzopen(filename.c_str(), "rb");
    fatal_if(!f, "%s: cannot open trace %s", name, filename);
    gzbuffer(f, 1 << 18);
    file = f;

    // LE32 magic, then the InstDepRecordHeader message, which this model
    // does not need (window size and tick frequency describe the capture).
    fatal_if(!fill(4), "%s: %s is empty", name, filename);
    const uint32_t magic = buf[pos] | (buf[pos + 1] << 8) |
                           (buf[pos + 2] << 16) | (uint32_t(buf[pos + 3]) << 24);
    fatal_if(magic != 0x356d6567, "%s: %s is not a gem5 proto stream",
             name, filename);
    pos += 4;
    uint64_t len;
    fatal_if(!varint(len) || !fill(len),
             "%s: no InstDepRecord header in %s", name, filename);
    pos += len;
}

IntervalCPU::TraceReader::~TraceReader()
{
    gzclose(static_cast<gzFile>(file));
}

bool
IntervalCPU::TraceReader::fill(size_t need)
{
    if (end - pos >= need)
        return true;
    if (pos) {
        std::memmove(buf.data(), buf.data() + pos, end - pos);
        end -= pos;
        pos = 0;
    }
    if (need > buf.size())
        buf.resize(need);
    while (!eof && end - pos < need) {
        const int n = gzread(static_cast<gzFile>(file), buf.data() + end,
                             buf.size() - end);
        fatal_if(n < 0, "%s: trace read error", name);
        if (n == 0)
            eof = true;
        end += n;
    }
    return end - pos >= need;
}

bool
IntervalCPU::TraceReader::varint(uint64_t &v)
{
    v = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (pos == end && !fill(1))
            return false;
        const uint8_t b = buf[pos++];
        v |= uint64_t(b & 0x7f) << shift;
        if (!(b & 0x80))
            return true;
    }
    panic("%s: malformed varint in trace", name);
}

bool
IntervalCPU::TraceReader::next(Record &r)
{
    uint64_t len;
    if (!varint(len))
        return false;                 // clean end of stream
    fatal_if(!fill(len), "%s: truncated record in trace", name);

    // Decode one InstDepRecord in place (proto2 field numbers from
    // src/proto/inst_dep_record.proto). Repeated fields are accepted packed
    // or unpacked; unknown fields are skipped by wire type.
    const uint8_t *p = buf.data() + pos;
    const uint8_t *const e = p + len;
    pos += len;
    auto vi = [&](uint64_t &v) {
        v = 0;
        for (unsigned shift = 0; p < e; shift += 7) {
            const uint8_t b = *p++;
            v |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80))
                return;
        }
        panic("%s: malformed record in trace", name);
    };
    r = Record();
    while (p < e) {
        uint64_t key, v;
        vi(key);
        const unsigned field = key >> 3, wire = key & 7;
        if (wire == 0) {
            vi(v);
            switch (field) {
              case 1: r.seq = v; break;
              case 2: r.type = v; break;
              case 3: r.paddr = v; break;
              case 4: r.size = v; break;
              case 5: r.flags = v; break;
              case 8:
                if (r.ndeps < MaxDeps) r.deps[r.ndeps++] = v;
                else r.overflow = true;
                break;
              case 9: r.weight = v; break;
              case 10: r.pc = v; break;
              default: break;          // rob_dep, comp_delay, v_addr, asid
            }
        } else if (wire == 2) {
            uint64_t n;
            vi(n);
            const uint8_t *const sub = p + n;
            panic_if(sub > e, "%s: malformed record in trace", name);
            if (field == 8) {           // packed reg_dep
                while (p < sub) {
                    vi(v);
                    if (r.ndeps < MaxDeps) r.deps[r.ndeps++] = v;
                    else r.overflow = true;
                }
            }
            p = sub;
        } else if (wire == 1) {
            p += 8;
        } else if (wire == 5) {
            p += 4;
        } else {
            panic("%s: unsupported wire type %u in trace", name, wire);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Trace input
// ---------------------------------------------------------------------------

void
IntervalCPU::pushUop(Uop &&u)
{
    // An instruction that crosses a line boundary cannot be decoded until
    // the line holding its last byte is fetched; O3 delivers it with that
    // line (synth-like loops where one instruction straddles two lines take
    // two line fetches per iteration).
    unsigned len = u.brLen;
    if (len)
        pcLen[u.pc] = len;
    else if (auto it = pcLen.find(u.pc); it != pcLen.end())
        len = it->second;
    u.fetchLine = (u.pc + (len ? len - 1 : 0)) & ~Addr(lineSize - 1);
    const Addr line = u.pc & ~(lineSize - 1);
    // isMemAddr walks the system's range map (and copies AddrRanges); its
    // answer only changes with the line, so ask once per line.
    if (line != lastCheckedLine) {
        lastCheckedLine = line;
        lastLineIsMem = system->isMemAddr(line);
    }
    u.needsFetch = fetchEnabled && lastLineIsMem;
    pending.push_back(std::move(u));
}

void
IntervalCPU::fillLookahead()
{
    while (!traceDone && pending.size() < lookaheadSize) {
        TraceReader::Record r;
        if (!trace.next(r)) {
            traceDone = true;
            break;
        }
        if (r.overflow)
            ++stats.numDepOverflows;

        // ElasticTrace drops COMPs that nothing depends on and counts them
        // in the next record's weight. They still used dispatch, ROB and
        // commit slots, so put them back as independent COMPs.
        for (uint32_t w = 0; w < r.weight; ++w) {
            Uop f;
            f.pc = r.pc;
            pushUop(std::move(f));
        }

        Uop u;
        u.seq = r.seq;
        u.pc = r.pc;
        switch (r.type) {
          case 1:                 // InstDepRecord::LOAD
            u.type = UopType::Load;
            break;
          case 2:                 // InstDepRecord::STORE
            u.type = UopType::Store;
            break;
          default:
            u.type = UopType::Comp;
            break;
        }
        if (u.type != UopType::Comp) {
            u.paddr = r.paddr;
            u.size = r.size;
            u.flags = r.flags;
        }
        // Register dependencies only. rob_dep and comp_delay describe the
        // capturing core's schedule, which is what this model recomputes.
        u.deps = r.deps;
        u.ndeps = r.ndeps;
        // etrace -uoptable: a COMP's op class rides in flags
        if (u.type == UopType::Comp) {
            if ((r.flags & 0xff) < NumOpClasses)
                u.opClass = r.flags & 0xff;
            // etrace_to_elastic: bits 8-10 branch kind, 11 taken, 12-15
            // instruction length, on a branch's last micro-op.
            u.brKind = (r.flags >> 8) & 7;
            u.brTaken = (r.flags >> 11) & 1;
            u.brLen = (r.flags >> 12) & 15;
        }
        pushUop(std::move(u));
    }
}

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

bool
IntervalCPU::strictlyOrdered(const RobEntry &e) const
{
    return (e.flags & Request::STRICT_ORDER) != 0;
}

bool
IntervalCPU::depsReady(const RobEntry &e) const
{
    for (unsigned i = 0; i < e.ndeps; ++i) {
        const auto &[s, seq] = e.deps[i];
        const RobEntry &p = rob[s];
        // A slot that no longer holds the producer means it has committed.
        if (p.seq == seq && p.completeTick > curTick())
            return false;
    }
    return true;
}

PacketPtr
IntervalCPU::makeDataPacket(const RobEntry &e, bool is_read, Addr addr,
                            unsigned size)
{
    // As TraceCPU: no exclusive monitor is modelled, and a locked/LLSC
    // request would otherwise be treated as an atomic by the caches.
    Request::FlagsType flags = e.flags;
    flags &= ~(Request::LOCKED_RMW | Request::LLSC);
    // etrace_to_elastic marks the load of a read-modify-write (gem5's
    // ldst/ldstl) with bit 31 of the record's 32-bit flags (a load never
    // carries gem5's ATOMIC_NO_RETURN_OP); gem5 sends it READ_MODIFY_WRITE,
    // which Ruby issues as an RMW_Read with store-type permission.
    if (is_read && (flags & RmwMarker)) {
        flags &= ~Request::FlagsType(RmwMarker | NonSpecMarker);
        flags |= Request::READ_MODIFY_WRITE;
    }

    auto req = std::make_shared<Request>(addr, size, flags, dataRequestorId);
    req->setReqInstSeqNum(e.seq);
    req->setContext(ContextID(0));
    req->setPC(e.pc);

    PacketPtr pkt = is_read ? Packet::createRead(req)
                            : Packet::createWrite(req);
    uint8_t *data = new uint8_t[size];
    if (!is_read)
        std::memset(data, 0xA, size);
    pkt->dataDynamic(data);
    return pkt;
}

unsigned
IntervalCPU::sendData(const RobEntry &e, bool is_read)
{
    // An access crossing a line is two requests, as O3's split LSQ
    // requests; it completes when both have.
    const unsigned size = e.size ? e.size : 1;
    const Addr offset = e.paddr & (lineSize - 1);
    if (offset + size <= lineSize) {
        queueDcache(makeDataPacket(e, is_read, e.paddr, size));
        return 1;
    }
    ++stats.numSplitReqs;
    // Pin reports some instructions (xsave/xrstor, string ops) as one
    // access of hundreds of bytes; gem5 splits into at most two requests,
    // so the second covers at most the next line.
    const unsigned first = lineSize - offset;
    const unsigned second = std::min<unsigned>(size - first, lineSize);
    queueDcache(makeDataPacket(e, is_read, e.paddr, first));
    queueDcache(makeDataPacket(e, is_read, e.paddr + first, second));
    return 2;
}

void
IntervalCPU::queueDcache(PacketPtr pkt)
{
    if (dcacheRetryPkt) {
        dcacheBacklog.push_back(pkt);
        return;
    }
    if (!dcachePort.sendTimingReq(pkt))
        dcacheRetryPkt = pkt;
}

unsigned
IntervalCPU::commit()
{
    unsigned n = 0;
    while (n < commitWidth && robCount) {
        RobEntry &e = rob[robHead];
        // O3 commits an instruction iewToCommitDelay after writeback and
        // retires it the cycle after that: at least 2 cycles from
        // completion in a blackscholes O3PipeView.
        if (!e.issued || e.completeTick == MaxTick ||
            e.completeTick + commitDelay * clockPeriod() > curTick())
            break;

        if (e.brKind && e.seq)
            bpred.commit(e.seq);
        if (e.type == UopType::Store) {
            if (strictlyOrdered(e)) {
                --sqCount;
                ++stats.numStrictlyOrdered;
            } else {
                // Committed: the write drains from the store queue later
                // (drainStores); the SQ entry is held until it is
                // acknowledged, and the data can forward until it is sent.
                // O3's IEW writes it back commitToIEWDelay later.
                storeWriteQ.push_back({e.seq, e.pc, e.paddr, e.size,
                                       e.flags,
                                       clockEdge(storeWriteDelay)});
            }
            ++stats.numStores;
            if (!sqOrder.empty() && sqOrder.front() == robHead)
                sqOrder.pop_front();
        } else if (e.type == UopType::Load) {
            --lqCount;
            ++stats.numLoads;
        }

        if (e.seq) {
            auto it = inflight.find(e.seq);
            if (it != inflight.end() && it->second == robHead)
                inflight.erase(it);
        }
        DPRINTF(IntervalCPU, "commit seq %llu slot %u\n", e.seq, robHead);
        e.seq = 0;
        robHead = (robHead + 1) % robSize;
        --robCount;
        ++stats.numOps;
        ++committedOps;
        ++n;
    }
    return n;
}

unsigned
IntervalCPU::sendLoads()
{
    unsigned sent = 0;
    while (!loadSendQ.empty() && loadSendQ.front().first <= curTick() &&
           !dcacheRetryPkt) {
        RobEntry &e = rob[loadSendQ.front().second];
        loadSendQ.pop_front();
        e.respPending = sendData(e, true);
        ++sent;
    }
    return sent;
}

unsigned
IntervalCPU::drainStores()
{
    unsigned sent = 0;
    while (!storeWriteQ.empty() && !dcacheRetryPkt &&
           storeWriteQ.front().ready <= curTick() &&
           (!tsoStores || storesInFlight == 0)) {
        const PendingWrite &w = storeWriteQ.front();
        RobEntry tmp;
        tmp.seq = w.seq;
        tmp.pc = w.pc;
        tmp.paddr = w.paddr;
        tmp.size = w.size;
        tmp.flags = w.flags;
        storeWriteQ.pop_front();
        lastStoreSendTick = curTick();
        DPRINTF(IntervalCPU, "store write seq %llu pc %#x (queued %u)\n",
                tmp.seq, tmp.pc, (unsigned)storeWriteQ.size());
        const unsigned parts = sendData(tmp, false);
        storesInFlight += parts;
        if (parts > 1)
            storeParts[tmp.seq] = parts;
        ++sent;
    }
    return sent;
}

IntervalCPU::Fwd
IntervalCPU::forwardCheck(unsigned load_slot) const
{
    // Youngest older store that overlaps decides, as in O3's LSQUnit::read:
    // full coverage with data ready forwards; data not ready, or a partial
    // overlap, makes the load wait. O3 searches only stores not yet sent to
    // the cache (storeWBIt onwards): committed stores still queued can
    // forward, but once a store's write is in flight a load to the same
    // bytes goes to the cache -- where Ruby's Sequencer holds it behind the
    // store's same-line request -- and a partially overlapping load is
    // replayed only when the store is sent (storePostSend). Forwarding from
    // sent stores too gave a blackscholes main thread 2.4x O3's forwarded
    // loads and a load-to-use of 2.2 cycles against O3's 4.7 on Ruby.
    const RobEntry &ld = rob[load_slot];
    const Addr lo = ld.paddr, hi = ld.paddr + (ld.size ? ld.size : 1);
    const unsigned ld_age = (load_slot + robSize - robHead) % robSize;
    for (auto it = sqOrder.rbegin(); it != sqOrder.rend(); ++it) {
        const unsigned ss = *it;
        if ((ss + robSize - robHead) % robSize >= ld_age)
            continue;                         // younger than the load
        const RobEntry &st = rob[ss];
        // A store that has not executed has no known address yet: O3's
        // load cannot see it and speculates past it. If they do overlap,
        // the store's issue detects the violation and trains the store
        // sets (memDepStoreIssued). Waiting here instead used the trace's
        // oracle address, avoided every violation and left the predictor
        // untrained: 7.9M predicted loads per blackscholes worker against
        // O3's 23.4M.
        if (!st.issued)
            continue;
        const Addr slo = st.paddr, shi = st.paddr + (st.size ? st.size : 1);
        if (shi <= lo || hi <= slo)
            continue;
        if (slo <= lo && hi <= shi)
            return st.completeTick <= curTick() ? Fwd::Forward : Fwd::Wait;
        return Fwd::Partial;
    }
    for (auto it = storeWriteQ.rbegin(); it != storeWriteQ.rend(); ++it) {
        const Addr slo = it->paddr, shi = it->paddr + (it->size ? it->size : 1);
        if (shi <= lo || hi <= slo)
            continue;
        return (slo <= lo && hi <= shi) ? Fwd::Forward : Fwd::Partial;
    }
    return Fwd::None;
}

unsigned
IntervalCPU::issue()
{
    unsigned n = 0;
    size_t keep = 0;
    fuUsed.fill(0);
    while (!iqReleaseQ.empty() && iqReleaseQ.front() <= curTick()) {
        iqReleaseQ.pop_front();
        --iqHeld;
    }
    // Claim a functional unit of group g for this cycle; a non-pipelined op
    // also holds one of the group's units for its whole latency.
    auto claim = [&](FuGroup g, unsigned lat, bool pipelined) {
        if (!fuModel)
            return true;
        if (fuUsed[g] >= fuCount[g]) {
            ++stats.fuBusy;
            return false;
        }
        if (!pipelined) {
            auto &units = fuBusyUntil[g];
            auto free = std::find_if(units.begin(), units.end(),
                [&](Tick t) { return t <= curTick(); });
            if (free == units.end()) {
                ++stats.fuBusy;
                return false;
            }
            *free = clockEdge(Cycles(lat));
        }
        ++fuUsed[g];
        return true;
    };
    for (size_t i = 0; i < iq.size(); ++i) {
        const unsigned s = iq[i];
        RobEntry &e = rob[s];
        bool issued_now = false;

        // Renamed instructions reach the IQ renameToIEWDelay later.
        // A non-speculative micro-op executes once it reaches the ROB head
        // and every older store has been written back; commit then signals
        // IEW (commitToIEWDelay). Commit::commitHead, hasStoresToWB.
        bool nonspec_ok = true;
        if (e.nonSpec) {
            if (s != robHead || !storeWriteQ.empty() || storesInFlight) {
                nonspec_ok = false;
            } else if (!e.nonSpecReady) {
                e.nonSpecReady = clockEdge(Cycles(1));
                ++stats.nonSpecStalls;
                nonspec_ok = false;
            } else {
                nonspec_ok = e.nonSpecReady <= curTick();
            }
        }
        if (n < issueWidth && nonspec_ok &&
            e.dispatchTick + dispatchToIssueDelay * clockPeriod() <=
                curTick() &&
            depsReady(e)) {
            switch (e.type) {
              case UopType::Comp: {
                const OpClassDesc &oc = opClassDesc[e.opClass];
                if (!claim(oc.group, oc.latency, oc.pipelined))
                    break;
                e.completeTick = clockEdge(fuModel ? Cycles(oc.latency)
                                                   : compLatency);
                issued_now = true;
                // A mispredicted branch is found when it executes; the
                // squash reaches fetch through commit (iewToCommitDelay +
                // commitToFetchDelay).
                if (e.mispredict && e.seq == brBlockSeq && !brRedirectTick) {
                    brRedirectTick = e.completeTick +
                        branchRedirectDelay * clockPeriod();
                    // Wrong-path micro-ops renamed before the branch
                    // executes reach the ROB (2 cycles later) before commit
                    // starts squashing, the cycle after execute.
                    wrongPathUntil = e.completeTick;
                    brCompleteTick = e.completeTick;
                }
                break;
              }
              case UopType::Store:
                if (!claim(FuMem, 1, true))
                    break;
                e.completeTick = clockEdge(storeLatency);
                issued_now = true;
                if (memDepPred)
                    memDepStoreIssued(s);
                if (storeHoldsIq) {
                    // A memory instruction frees its IQ entry when it
                    // writes back (InstructionQueue::wakeDependents), a
                    // store the cycle after it issues.
                    ++iqHeld;
                    iqReleaseQ.push_back(e.completeTick);
                }
                break;
              case UopType::Load:
                if (!strictlyOrdered(e)) {
                    const Fwd f = forwardCheck(s);
                    if (!e.firstTryTick)
                        e.firstTryTick = curTick();
                    // A load held by a partial overlap is replayed when the
                    // store is sent (LSQUnit::storePostSend), after this
                    // cycle's issue has run (IEW::tick: scheduleReadyInsts
                    // before writebackStores): it issues the cycle after.
                    if (f == Fwd::None && (e.waitSeen & 1) &&
                        lastStoreSendTick >= curTick()) {
                        ++stats.loadsWaitedOnStore;
                        break;
                    }
                    if (f == Fwd::Wait || f == Fwd::Partial) {
                        ++stats.loadsWaitedOnStore;
                        const uint8_t bit = f == Fwd::Partial ? 1 : 2;
                        if (!(e.waitSeen & bit)) {
                            e.waitSeen |= bit;
                            if (bit == 1)
                                ++stats.loadsPartialStall;
                            else
                                ++stats.loadsDataWait;
                        }
                        break;
                    }
                    if (f == Fwd::Forward) {
                        if (!claim(FuMem, 1, true))
                            break;
                        e.completeTick = clockEdge(forwardLatency);
                        ++stats.loadsForwarded;
                        DPRINTF(IntervalCPU, "forward seq %llu pc %#x\n",
                                e.seq, e.pc);
                        stats.loadToUseCycles += forwardLatency +
                            (curTick() - e.firstTryTick) / clockPeriod();
                        issued_now = true;
                        break;
                    }
                }
                if (!claim(FuMem, 1, true))
                    break;
                if (strictlyOrdered(e)) {
                    // As TraceCPU: not sent; treated as complete.
                    e.completeTick = clockEdge(Cycles(1));
                    ++stats.numStrictlyOrdered;
                    issued_now = true;
                } else if (loadIssueDelay > 0 || !dcacheRetryPkt) {
                    // O3 executes, and so sends, a load issueToExecuteDelay
                    // cycles after it issues (sendLoads).
                    ++loadsInFlight;
                    e.holdsIq = true;       // until the data returns
                    ++iqHeld;
                    if (loadIssueDelay > 0) {
                        loadSendQ.push_back({clockEdge(loadIssueDelay), s});
                    } else {
                        e.respPending = sendData(e, true);
                    }
                    issued_now = true;   // completes on the response
                }
                break;
            }
        }

        if (issued_now) {
            e.issued = true;
            e.issueTick = curTick();
            DPRINTF(IntervalCPU, "issue seq %llu type %d oc %d pc %#x "
                    "wait %llu\n", e.seq, (int)e.type, (int)e.opClass, e.pc,
                    (curTick() - e.dispatchTick) / clockPeriod());
            ++n;
        } else {
            iq[keep++] = s;
        }
    }
    iq.resize(keep);
    return n;
}

unsigned
IntervalCPU::dispatch()
{
    unsigned n = 0;
    unsigned dispLoads = 0, dispStores = 0;
    dispStall = DispStall::None;

    if (curTick() < squashUntil || curTick() < robSquashUntil) {
        // Refetching after a memory-order violation, or commit is still
        // removing a mispredicted branch's wrong path from the ROB (IEW
        // stalls rename while robSquashing).
        dispStall = DispStall::Squash;
        return 0;
    }

    // How many micro-ops rename can take this cycle under the skid-buffer
    // protocol.
    size_t allow = dispatchWidth;
    if (skidModel) {
        if (skidState == Skid::Blocked) {
            allow = std::min(allow, skidRename);
        } else if (skidState == Skid::Refill) {
            if (curTick() >= skidRefillAt)
                skidState = Skid::Run;
            else if (curTick() >= skidDecodeAt)
                allow = std::min(allow, skidDecode);
            else
                allow = 0;
        }
    }

    while (n < dispatchWidth && !fetched.empty()) {
        if (parked || syncGate()) {
            dispStall = DispStall::Sync;
            break;
        }
        const Uop &u = fetched.front();

        if (u.readyTick > curTick()) {
            dispStall = DispStall::Frontend;
            break;
        }
        // O3's rename sees the free ROB/IQ/LQ/SQ entries IEW and commit
        // reported last cycle, less what it has renamed since
        // (Rename::calcFreeIQEntries and instsInProgress): an entry freed
        // this cycle is reusable from the next one.
        const bool stale = staleOccupancy;
        if ((stale ? occRob0 + n : robCount) >= robSize) {
            dispStall = DispStall::Rob;
            break;
        }
        if ((stale ? occIq0 + n : iq.size() + iqHeld) >= iqSize) {
            dispStall = DispStall::Iq;
            break;
        }
        if (u.type == UopType::Load &&
            (stale ? occLq0 + dispLoads : lqCount) >= lqSize) {
            dispStall = DispStall::Lq;
            break;
        }
        if (u.type == UopType::Store &&
            (stale ? occSq0 + dispStores : sqCount) >= sqSize) {
            dispStall = DispStall::Sq;
            break;
        }
        if (n >= allow) {
            // Rename has room but nothing to rename: its skid buffer is
            // drained and decode has not resumed yet.
            dispStall = DispStall::Unblock;
            break;
        }
        if (u.type == UopType::Load)
            ++dispLoads;
        else if (u.type == UopType::Store)
            ++dispStores;

        const unsigned s = slot(robCount);
        RobEntry &e = rob[s];
        e.seq = u.seq;
        e.type = u.type;
        e.pc = u.pc;
        e.paddr = u.paddr;
        e.size = u.size;
        e.flags = u.flags;
        e.opClass = u.opClass;
        e.brKind = u.brKind;
        e.mispredict = u.mispredict;
        if (mispredDispatchTick) {
            stats.mispredictDispatchGap +=
                (curTick() - mispredDispatchTick) / clockPeriod();
            mispredDispatchTick = 0;
        }
        if (u.mispredict) {
            mispredDispatchTick = curTick();
            if (u.seq == brBlockSeq)
                brDispatchTick = curTick();
        }
        e.issued = false;
        e.holdsIq = false;
        e.firstTryTick = 0;
        e.waitSeen = 0;
        e.dispatchTick = curTick();
        e.completeTick = MaxTick;
        e.ndeps = 0;
        for (unsigned i = 0; i < u.ndeps; ++i) {
            const uint64_t d = u.deps[i];
            auto it = inflight.find(d);
            // Not in the window: committed, filtered out of the trace, or
            // beyond the dependency window. All three mean complete.
            if (it != inflight.end())
                e.deps[e.ndeps++] = {it->second, d};
        }
        if (memDepPred && e.type != UopType::Comp)
            memDepDispatch(e);
        // Non-speculative micro-ops and memory barriers (gem5 x86 mfence,
        // and ldstl, the load of a LOCK-prefixed instruction).
        e.nonSpec = (e.type == UopType::Comp && e.opClass == OpBarrier) ||
                    (e.type == UopType::Load && (e.flags & NonSpecMarker));
        e.nonSpecReady = 0;
        if (e.type != UopType::Comp && lastBarrierSeq) {
            auto b = inflight.find(lastBarrierSeq);
            if (b != inflight.end() &&
                rob[b->second].completeTick > curTick()) {
                // O3's conflictingLoads/Stores count barrier waits too
                // (MemDepUnit::insert: a producer found), once per op.
                const unsigned before = e.ndeps;
                bool counted = false;
                for (unsigned k = 0; k < before; ++k) {
                    const RobEntry &p = rob[e.deps[k].first];
                    if (p.type == UopType::Store && p.seq == e.deps[k].second)
                        counted = true;     // store-set dep already counted
                }
                e.deps[e.ndeps++] = {b->second, lastBarrierSeq};
                if (!counted) {
                    if (e.type == UopType::Load)
                        ++stats.memDepLoads;
                    else
                        ++stats.memDepStores;
                }
            }
        }
        if (e.type == UopType::Comp && e.opClass == OpBarrier && u.seq)
            lastBarrierSeq = u.seq;

        if (u.seq)
            inflight[u.seq] = s;
        else
            ++stats.numFillerOps;
        if (u.type == UopType::Load) {
            ++lqCount;
        } else if (u.type == UopType::Store) {
            ++sqCount;
            sqOrder.push_back(s);
        }

        ++robCount;
        iq.push_back(s);
        fetched.pop_front();
        ++dispatchedOps;
        ++n;
    }

    if (skidModel)
        skidUpdate(n);

    if (n)
        dispStall = DispStall::None;
    else if (fetched.empty() && dispStall == DispStall::None)
        dispStall = DispStall::Frontend;
    return n;
}

void
IntervalCPU::skidUpdate(unsigned n)
{
    const bool backend = dispStall == DispStall::Rob ||
                         dispStall == DispStall::Iq ||
                         dispStall == DispStall::Lq ||
                         dispStall == DispStall::Sq;
    if (skidState == Skid::Blocked) {
        skidRename -= std::min<size_t>(skidRename, n);
        if (!backend && skidRename == 0) {
            // Skid buffer drained with room to spare: rename unblocks
            // decode (renameToDecodeDelay), whose skid buffer reaches
            // rename a cycle later; fetch's output follows two cycles
            // after that (decode had stalled fetch).
            skidState = Skid::Refill;
            skidDecodeAt = clockEdge(Cycles(2));
            skidRefillAt = clockEdge(Cycles(4));
        }
        return;
    }
    if (skidState == Skid::Refill && curTick() >= skidDecodeAt)
        skidDecode -= std::min<size_t>(skidDecode, n);
    if (backend) {
        // Rename blocks. What decode had already sent sits in rename's
        // skid buffer, what fetch had sent in decode's; both hold
        // skidSize micro-ops ((delay + 1) x width in gem5).
        ++stats.renameBlocks;
        size_t at_rename = 0, at_decode = 0;
        const Tick next = clockEdge(Cycles(1));
        for (const Uop &u : fetched) {
            if (u.readyTick <= curTick() && at_rename < skidSize)
                ++at_rename;
            else if (u.readyTick <= next && at_decode < skidSize)
                ++at_decode;
            else
                break;
        }
        if (skidState == Skid::Refill && curTick() < skidDecodeAt)
            at_rename = 0;          // decode had not resumed yet
        skidState = Skid::Blocked;
        skidRename = at_rename;
        skidDecode = at_decode;
    }
}

// ---------------------------------------------------------------------------
// Memory dependence prediction: O3's StoreSet + MemDepUnit
// (src/cpu/o3/store_set.cc, mem_dep_unit.cc)
// ---------------------------------------------------------------------------

void
IntervalCPU::memDepDispatch(RobEntry &e)
{
    // StoreSet::checkClear runs from insertStore only: MemDepUnit::insert
    // never calls insertLoad, so the period counts stores, not all memops.
    if (e.type == UopType::Store && ++memOpsPred > storeSetClearPeriod) {
        memOpsPred = 0;
        ssit.clear();
        std::fill(lfstValid.begin(), lfstValid.end(), 0);
        ++stats.storeSetClears;
    }

    auto it = ssit.find(e.pc);
    if (it == ssit.end())
        return;
    const unsigned set = it->second;

    // MemDepUnit::insert -> StoreSet::checkInst: wait for the last store
    // dispatched in this set, if it has not executed yet. The wait ends
    // when that store completes (MemDepUnit::completeInst), which is what
    // a normal dependency on its completeTick expresses.
    if (lfstValid[set]) {
        auto f = inflight.find(lfst[set]);
        if (f != inflight.end() && rob[f->second].completeTick > curTick()) {
            e.deps[e.ndeps++] = {f->second, lfst[set]};
            DPRINTF(IntervalCPU, "memdep seq %llu pc %#x waits on store "
                    "%llu (set %u)\n", e.seq, e.pc, lfst[set], set);
            if (e.type == UopType::Load)
                ++stats.memDepLoads;
            else
                ++stats.memDepStores;
        }
    }

    // StoreSet::insertStore, after the check: this store is now the set's
    // last fetched store.
    if (e.type == UopType::Store) {
        lfst[set] = e.seq;
        lfstValid[set] = 1;
    }
}

void
IntervalCPU::memDepStoreIssued(unsigned store_slot)
{
    const RobEntry &st = rob[store_slot];

    // StoreSet::issued: a store that issues stops being the set's last
    // fetched store, so loads dispatched from now on do not wait for it.
    auto it = ssit.find(st.pc);
    if (it != ssit.end() && lfstValid[it->second] &&
        lfst[it->second] == st.seq)
        lfstValid[it->second] = 0;

    // LSQUnit::checkViolations: a younger load that already went to memory
    // with an overlapping address block read stale data. O3 squashes from
    // the oldest such load and trains the predictor.
    const unsigned st_age = (store_slot + robSize - robHead) % robSize;
    const Addr st_lo = st.paddr >> depCheckShift;
    const Addr st_hi = (st.paddr + std::max(st.size, 1u) - 1) >> depCheckShift;
    for (unsigned k = st_age + 1; k < robCount; ++k) {
        const RobEntry &ld = rob[slot(k)];
        if (ld.type != UopType::Load || !ld.issued || strictlyOrdered(ld))
            continue;
        const Addr ld_lo = ld.paddr >> depCheckShift;
        const Addr ld_hi =
            (ld.paddr + std::max(ld.size, 1u) - 1) >> depCheckShift;
        if (ld_lo <= st_hi && st_lo <= ld_hi) {
            ++stats.memOrderViolations;
            storeSetViolation(st.pc, ld.pc);
            // Timing of the squash is approximated by a dispatch bubble;
            // the loads and their consumers are not re-executed. Measured
            // O3 has 48 violations in 13.3M instructions, so this is noise.
            squashUntil = std::max(squashUntil, clockEdge(squashPenalty));
            break;
        }
    }
}

void
IntervalCPU::storeSetViolation(Addr store_pc, Addr load_pc)
{
    // StoreSet::violation, with an unbounded SSIT.
    auto ld = ssit.find(load_pc);
    auto st = ssit.find(store_pc);
    if (ld == ssit.end() && st == ssit.end()) {
        const unsigned set = calcSSID(load_pc);
        ssit[load_pc] = set;
        ssit[store_pc] = set;
    } else if (st == ssit.end()) {
        ssit[store_pc] = ld->second;
    } else if (ld == ssit.end()) {
        ssit[load_pc] = st->second;
    } else if (st->second > ld->second) {
        st->second = ld->second;     // the lower-numbered set wins
    } else {
        ld->second = st->second;
    }
}

unsigned
IntervalCPU::fetch()
{
    // O3's Fetch::fetch for one thread. Fetch reads micro-ops out of a
    // single line buffer (O3's 64-byte fetch buffer, no next-line
    // prefetch), at most fetchWidth per cycle, and stops after a taken
    // branch or when its queue is full. When the next micro-op is in
    // another line, the request for it goes out at the end of the same
    // cycle (Fetch::pipelineIcacheAccesses) and fetch resumes once it
    // returns: 3 cycles per new line on Ruby, in an O3 Fetch trace.
    // Measured on blackscholes over Ruby: O3 stalls ~2.0 cycles per line
    // (17.0M of the main thread's 37.4M cycles), which two lines in flight
    // and a 256-micro-op lookahead had hidden almost entirely.
    if (curTick() < squashUntil)
        return 0;                  // refetching after a violation
    if (brBlockSeq) {
        // Waiting at a mispredicted branch (the wrong path is not in the
        // trace, so fetch delivers nothing until the redirect).
        if (!brRedirectTick || curTick() < brRedirectTick) {
            ++stats.fetchBranchStallCycles;
            return 0;
        }
        brBlockSeq = 0;
        brRedirectTick = 0;
        if (wrongPathOn) {
            // The wrong path O3 renamed behind the branch until it
            // executed: the scheduled wrong-path fetch groups that reach
            // rename after the branch did and before it completed, bounded
            // by the ROB space left.
            for (const auto &g : wrongPathSched) {
                if (g.first > brDispatchTick && g.first <= brCompleteTick)
                    wrongPathRob += g.second;
            }
            const size_t room = robCount < robSize ? robSize - robCount : 0;
            DPRINTF(IntervalCPU, "wrong path: branch dispatched %lld, done "
                    "%lld (cycles from now), sched %u groups first %lld, "
                    "counted %u, rob room %u, upb %.2f\n",
                    (long long)(brDispatchTick - curTick()) /
                        (long long)clockPeriod(),
                    (long long)(brCompleteTick - curTick()) /
                        (long long)clockPeriod(),
                    (unsigned)wrongPathSched.size(),
                    wrongPathSched.empty() ? 0LL :
                    (long long)(wrongPathSched[0].first - curTick()) /
                        (long long)clockPeriod(),
                    (unsigned)wrongPathRob, (unsigned)room, uopsPerByte);
            wrongPathRob = std::min(wrongPathRob, room);
            stats.wrongPathUops += wrongPathRob;
            // Commit squashes the ROB from the cycle after execute,
            // squashWidth a cycle; IEW stalls rename meanwhile, and the
            // stall takes squashUnblockDelay to clear through rename and
            // decode (6 cycles from the end of the squash to the first
            // rename, in O3 traces of swaptions and branchk br50).
            const Cycles sq((wrongPathRob + squashWidth - 1) / squashWidth);
            robSquashUntil = std::max(robSquashUntil, brCompleteTick +
                (Cycles(1) + sq + squashUnblockDelay) * clockPeriod());
            stats.robSquashCycles += sq;
            DPRINTF(IntervalCPU, "redirect: branch done %llu, wrong path "
                    "%u in ROB, dispatch blocked until %llu\n",
                    (brCompleteTick - curTick()) / clockPeriod(),
                    (unsigned)wrongPathRob,
                    (robSquashUntil - curTick()) / clockPeriod());
            wrongPathRob = 0;
            // The wrong path moved fetch to other code: the correct path's
            // line is requested again (complete-to-refetch is 6 cycles in
            // 78% of O3 mispredicts, 3 cycles otherwise).
            fetchBufValid = false;
            fetchBufLine = MaxAddr;
        }
    }
    fillLookahead();
    const size_t cap = fetchQueueSize + frontendDepth * fetchWidth;
    const Tick ready = clockEdge(frontendDepth);
    unsigned n = 0;
    Addr need = 0;                 // line fetch found it needs, if any
    while (n < fetchWidth && fetched.size() < cap && !pending.empty()) {
        Uop &u = pending.front();
        if (u.needsFetch && !fetchBufValid)
            break;                 // a line is on its way: wait for it
        if (u.needsFetch) {
            const Addr start = u.pc & ~Addr(lineSize - 1);
            const bool have_start = fetchBufValid && fetchBufLine == start;
            if (start != u.fetchLine) {
                // Crosses a line: decoding starts from the buffered first
                // line, then needs the second; the instruction comes out
                // with the second line (O3 decoder + one-line buffer).
                const bool have_end = fetchBufValid &&
                                      fetchBufLine == u.fetchLine &&
                                      straddlePc == u.pc;
                if (!have_end) {
                    if (have_start)
                        straddlePc = u.pc;
                    need = have_start ? u.fetchLine : start;
                    break;
                }
            } else if (!have_start) {
                need = start;
                break;
            }
        }
        // A new instruction that does not follow on from the last one
        // (x86 instructions are at most 15 bytes) was reached by a taken
        // branch: O3 stops fetching for the cycle after predicting one.
        if (n && u.pc != lastFetchedPc &&
            (u.pc < lastFetchedPc || u.pc > lastFetchedPc + 15)) {
            ++stats.fetchTakenBreaks;
            lastFetchedPc = u.pc;  // so the next cycle starts here
            uopsThisInst = 0;
            break;
        }
        if (u.pc != lastFetchedPc) {
            // A new instruction: learn micro-ops per byte of code from
            // straight-line sequences (stands in for the wrong path's).
            auto it = pcLen.find(lastFetchedPc);
            if (it != pcLen.end() && u.pc == lastFetchedPc + it->second &&
                uopsThisInst)
                uopsPerByte += (double(uopsThisInst) / it->second -
                                uopsPerByte) / 256.0;
            uopsThisInst = 0;
        }
        ++uopsThisInst;
        lastFetchedPc = u.pc;
        u.readyTick = ready;
        const bool is_branch = branchPred && u.brKind && u.seq;
        fetched.push_back(std::move(u));
        pending.pop_front();
        if (pending.empty())
            fillLookahead();
        ++n;
        if (is_branch && predictBranch(fetched.back()))
            break;                 // fetch stops at a mispredicted branch
    }

    // Micro-ops fetched per cycle, averaged (stands in for the wrong
    // path's fetch rate).
    if (n || fetched.size() < cap)       // not merely backpressure
        fetchRate += (double(n) - fetchRate) / 64.0;

    // Request the line fetch needs next, unless it is buffered or on its
    // way: the one it ran into, or, if it stopped for another reason (width,
    // taken branch, full queue), the line the next instruction starts in.
    if (!pending.empty() && pending.front().needsFetch &&
        (fetchBufValid || !fetchesInFlight)) {
        const Addr line = need ? need
                               : pending.front().pc & ~Addr(lineSize - 1);
        if (fetchBufLine != line || !fetchBufValid) {
            auto req = std::make_shared<Request>(line, lineSize,
                                                 Request::INST_FETCH,
                                                 instRequestorId);
            req->setContext(ContextID(0));
            req->setPC(line);
            PacketPtr pkt = Packet::createRead(req);
            pkt->allocate();
            DPRINTF(IntervalCPU, "fetch request line %#x (fetched %u)\n",
                    line, n);
            fetchBufLine = line;
            fetchBufValid = false;
            fetchReqTick = curTick();
            ++fetchesInFlight;
            ++stats.numFetches;
            if (!icachePort.sendTimingReq(pkt))
                icacheRetryPkt = pkt;
        }
    }
    return n;
}

bool
IntervalCPU::predictBranch(Uop &u)
{
    // The trace gives the outcome; the next micro-op's PC is the target.
    const Addr target = pending.empty() ? u.pc + u.brLen : pending.front().pc;
    const auto kind = static_cast<BranchModel::Kind>(u.brKind);
    const BranchModel::Result r =
        bpred.predict(u.seq, u.pc, u.brLen, kind, u.brTaken, target);
    ++stats.branches;
    if (!r.mispredict)
        return false;
    u.mispredict = true;
    stats.branchMispredicts[u.brKind]++;
    DPRINTF(IntervalCPU, "mispredict seq %llu pc %#x kind %d%s\n", u.seq,
            u.pc, u.brKind, r.decodeFix ? " (decode)" : "");
    if (r.btbMiss)
        ++stats.btbMissMispredicts;
    brBlockSeq = u.seq;
    brRedirectTick = 0;
    brDispatchTick = MaxTick;
    wrongPathRob = 0;
    wrongPathCredit = 0;
    wrongPathOn = !r.decodeFix;
    if (wrongPathOn) {
        // O3 keeps fetching down the predicted path. Its code is not in
        // the trace, so fetch it as straight-line code at this program's
        // micro-ops per byte, under the front end's own rules: fetchWidth
        // a cycle, one line buffer, 3 cycles (request at the end of the
        // cycle, data, fetch) per new line. Each fetched group can enter
        // the ROB frontendDepth later (dispatch() counts them in, bounded
        // by ROB space, until the branch executes).
        wrongPathSched.clear();
        Addr p = r.predNext;
        Addr line = fetchBufValid ? fetchBufLine : MaxAddr;
        Cycles t(1);
        const double upb = std::max(0.1, uopsPerByte);
        unsigned total = 0;
        while (total < robSize && t < Cycles(200)) {
            const Addr pl = p & ~Addr(lineSize - 1);
            if (pl != line) {
                line = pl;
                t += Cycles(3);
                continue;
            }
            const double bytes_left = double(pl + lineSize - p);
            unsigned k = std::min<unsigned>(fetchWidth,
                std::max(1.0, bytes_left * upb));
            wrongPathSched.push_back(
                {clockEdge(t + frontendDepth), k});
            total += k;
            p += Addr(std::ceil(k / upb));
            t += Cycles(1);
        }
        wrongPathPos = 0;
    }
    // The wrong path follows the branch into rename frontendDepth later.
    wrongPathFrom = clockEdge(frontendDepth);
    wrongPathUntil = MaxTick;
    if (r.decodeFix) {
        // Decode computes PC-relative targets and squashes fetch itself,
        // before the wrong path reaches rename.
        ++stats.decodeRedirects;
        brRedirectTick = clockEdge(decodeRedirectDelay);
    }
    return true;
}

IntervalCPU::Idle
IntervalCPU::classifyIdle() const
{
    if (!robCount)
        return Idle::RobEmpty;
    const RobEntry &h = rob[robHead];
    if (h.type == UopType::Load)
        return Idle::HeadLoad;
    if (h.type == UopType::Store && h.issued && h.completeTick <= curTick())
        return Idle::HeadStore;
    return Idle::HeadExec;
}

void
IntervalCPU::account(Idle idle, DispStall stall, Cycles n)
{
    stats.numCycles += n;
    switch (idle) {
      case Idle::None:      stats.commitCycles += n; break;
      case Idle::RobEmpty:  stats.idleRobEmpty += n; break;
      case Idle::HeadLoad:  stats.idleHeadLoad += n; break;
      case Idle::HeadStore: stats.idleHeadStore += n; break;
      case Idle::HeadExec:  stats.idleHeadExec += n; break;
    }
    switch (stall) {
      case DispStall::None:     break;
      case DispStall::Frontend: stats.dispatchStallFrontend += n; break;
      case DispStall::Rob:      stats.dispatchStallRob += n; break;
      case DispStall::Iq:       stats.dispatchStallIq += n; break;
      case DispStall::Lq:       stats.dispatchStallLq += n; break;
      case DispStall::Sq:       stats.dispatchStallSq += n; break;
      case DispStall::Squash:   stats.dispatchStallSquash += n; break;
      case DispStall::Sync:     stats.dispatchStallSync += n; break;
      case DispStall::Unblock:  stats.dispatchStallUnblock += n; break;
    }
}

bool
IntervalCPU::finished() const
{
    return !parked && traceDone && pending.empty() && fetched.empty() &&
           !robCount &&
           !storesInFlight && storeWriteQ.empty() &&
           !loadsInFlight && !fetchesInFlight && !dcacheRetryPkt &&
           dcacheBacklog.empty() &&
           !icacheRetryPkt;
}

void
IntervalCPU::wakeAt(Tick when)
{
    if (done)
        return;
    // Never run twice in one cycle.
    when = std::max(when, lastTickTick + clockPeriod());
    if (!tickEvent.scheduled())
        schedule(tickEvent, when);
    else if (tickEvent.when() > when)
        reschedule(tickEvent, when);
}

void
IntervalCPU::tick()
{
    // Cycles skipped while asleep looked exactly like the last one: nothing
    // in the core changed, it was waiting for the memory system.
    const Cycles now = curCycle();
    if (now > lastTickCycle + Cycles(1))
        account(lastIdle, lastDispStall, now - lastTickCycle - Cycles(1));
    lastTickCycle = now;
    lastTickTick = curTick();

    // What rename will see this cycle: occupancy at the end of the last.
    occRob0 = robCount;
    occIq0 = iq.size() + iqHeld;
    occLq0 = lqCount;
    occSq0 = sqCount;

    // Reverse pipeline order, so a stage sees what the stage after it freed.
    const unsigned c = commit() + drainStores() + sendLoads();
    const uint64_t sync_before = nextSync;
    processSyncPoints();
    const bool s = nextSync != sync_before;
    const unsigned i = issue();
    const unsigned d = dispatch();
    const unsigned f = fetch();

    lastIdle = c ? Idle::None : classifyIdle();
    lastDispStall = dispStall;
    account(lastIdle, lastDispStall, Cycles(1));

    if (finished()) {
        done = true;
        stats.lastTick = curTick();
        if (arbiter) {
            if (barrierMember)
                arbiter->retire(arbiterId);
            arbiter->threadExit(threadId);
        }
        inform("%s: Execution complete.", name());
        if (--numActive == 0)
            exitSimLoop("end of all traces reached.");
        return;
    }

    // wakeAt, not schedule: the arbiter may already have rescheduled us
    // from inside this tick (a barrier this CPU completed releases it too).
    if (c || s || i || d || f) {
        wakeAt(clockEdge(Cycles(1)));
        return;
    }

    // Nothing moved. Sleep until the earliest in-flight completion, or
    // until a response or retry wakes us.
    Tick next = MaxTick;
    for (unsigned k = 0; k < robCount; ++k) {
        const RobEntry &e = rob[slot(k)];
        if (e.issued && e.completeTick != MaxTick) {
            if (e.completeTick > curTick())
                next = std::min(next, e.completeTick);
            const Tick c = e.completeTick + commitDelay * clockPeriod();
            if (k == 0 && c > curTick())
                next = std::min(next, c);
        }
    }
    if (!storeWriteQ.empty() && storeWriteQ.front().ready > curTick())
        next = std::min(next, storeWriteQ.front().ready);
    if (!iqReleaseQ.empty() && iqReleaseQ.front() > curTick())
        next = std::min(next, iqReleaseQ.front());
    if (brBlockSeq && brRedirectTick > curTick())
        next = std::min(next, brRedirectTick);
    if (robSquashUntil > curTick())
        next = std::min(next, robSquashUntil);
    if (robCount && rob[robHead].nonSpec && !rob[robHead].issued &&
        rob[robHead].nonSpecReady > curTick())
        next = std::min(next, rob[robHead].nonSpecReady);
    if (skidState == Skid::Refill) {
        if (skidDecodeAt > curTick())
            next = std::min(next, skidDecodeAt);
        else if (skidRefillAt > curTick())
            next = std::min(next, skidRefillAt);
    }
    for (unsigned s : iq) {
        const Tick t = rob[s].dispatchTick +
            dispatchToIssueDelay * clockPeriod();
        if (t > curTick()) {
            next = std::min(next, t);
            break;                     // program order: the earliest
        }
    }
    if (squashUntil > curTick())
        next = std::min(next, squashUntil);
    if (!fetched.empty() && fetched.front().readyTick > curTick())
        next = std::min(next, fetched.front().readyTick);
    if (!loadSendQ.empty() && !dcacheRetryPkt)
        next = std::min(next, std::max(loadSendQ.front().first,
                                       clockEdge(Cycles(1))));
    // A ready micro-op may be waiting for a non-pipelined unit to free up.
    for (const auto &units : fuBusyUntil)
        for (Tick t : units)
            if (t > curTick())
                next = std::min(next, t);
    if (next != MaxTick) {
        wakeAt(clockEdge(Cycles(divCeil(next - curTick(), clockPeriod()))));
        return;
    }
    // Parked: the arbiter will call releaseFromSync().
    panic_if(!parked && !loadsInFlight && !storesInFlight &&
             !fetchesInFlight &&
             !dcacheRetryPkt && !icacheRetryPkt,
             "%s: no progress and nothing to wait for (rob %u, iq %zu, "
             "pending %zu, lq %u, sq %u)", name(), robCount, iq.size(),
             pending.size(), lqCount, sqCount);
}

// ---------------------------------------------------------------------------
// Memory interface
// ---------------------------------------------------------------------------

void
IntervalCPU::recvIcacheResp(PacketPtr pkt)
{
    if ((pkt->getAddr() & ~Addr(lineSize - 1)) == fetchBufLine) {
        fetchBufValid = true;
        stats.icacheStallCycles +=
            divCeil(curTick() - fetchReqTick, clockPeriod());
    }
    --fetchesInFlight;
    delete pkt;
    wakeAt(clockEdge());
}

void
IntervalCPU::recvDcacheResp(PacketPtr pkt)
{
    if (pkt->isRead()) {
        auto it = inflight.find(pkt->req->getReqInstSeqNum());
        if (it != inflight.end() && rob[it->second].respPending > 1) {
            --rob[it->second].respPending;     // first half of a split
            delete pkt;
            return;
        }
        if (it != inflight.end()) {
            RobEntry &e = rob[it->second];
            e.respPending = 0;
            if (e.type == UopType::Load && e.completeTick == MaxTick) {
                // Dependents may issue from the first edge at or after the
                // response (the core ticks after the memory system's events
                // of the same tick, as O3). loadWakeNextEdge would push a
                // response landing exactly on an edge to the next one;
                // Ruby responses arrive mid-cycle, so it is off.
                const bool on_edge = curTick() == clockEdge();
                if (on_edge)
                    ++stats.loadRespOnEdge;
                e.completeTick = (loadWakeNextEdge && on_edge)
                    ? clockEdge(Cycles(1)) : clockEdge();
                // From the first issue attempt, as O3's firstIssue: a load
                // held back by an overlapping store counts its wait.
                stats.loadToUseCycles += divCeil(e.completeTick -
                    (e.firstTryTick ? e.firstTryTick : e.issueTick),
                    clockPeriod());
            }
            if (e.holdsIq) {
                e.holdsIq = false;
                --iqHeld;
            }
        }
        --loadsInFlight;
    } else {
        --storesInFlight;
        auto p = storeParts.find(pkt->req->getReqInstSeqNum());
        if (p != storeParts.end() && --p->second) {
            // the other half of a split store is still outstanding
        } else {
            if (p != storeParts.end())
                storeParts.erase(p);
            --sqCount;
        }
    }
    delete pkt;
    wakeAt(clockEdge());
}

void
IntervalCPU::icacheRetry()
{
    if (icacheRetryPkt && icachePort.sendTimingReq(icacheRetryPkt))
        icacheRetryPkt = nullptr;
    wakeAt(clockEdge());
}

void
IntervalCPU::dcacheRetry()
{
    if (dcacheRetryPkt && dcachePort.sendTimingReq(dcacheRetryPkt)) {
        dcacheRetryPkt = nullptr;
        while (!dcacheBacklog.empty()) {
            PacketPtr p = dcacheBacklog.front();
            dcacheBacklog.pop_front();
            if (!dcachePort.sendTimingReq(p)) {
                dcacheRetryPkt = p;
                break;
            }
        }
    }
    wakeAt(clockEdge());
}

bool
IntervalCPU::IcachePort::recvTimingResp(PacketPtr pkt)
{
    owner->recvIcacheResp(pkt);
    return true;
}

void
IntervalCPU::IcachePort::recvReqRetry()
{
    owner->icacheRetry();
}

bool
IntervalCPU::DcachePort::recvTimingResp(PacketPtr pkt)
{
    owner->recvDcacheResp(pkt);
    return true;
}

void
IntervalCPU::DcachePort::recvReqRetry()
{
    owner->dcacheRetry();
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

IntervalCPU::IntervalCPUStats::IntervalCPUStats(IntervalCPU *cpu)
    : statistics::Group(cpu),
      ADD_STAT(numCycles, statistics::units::Cycle::get(),
               "Cycles from start until this core finished"),
      ADD_STAT(numOps, statistics::units::Count::get(),
               "Micro-ops committed, including filler COMPs"),
      ADD_STAT(numLoads, statistics::units::Count::get(),
               "Loads committed"),
      ADD_STAT(numStores, statistics::units::Count::get(),
               "Stores committed"),
      ADD_STAT(numFillerOps, statistics::units::Count::get(),
               "Filler COMPs re-inserted from record weights"),
      ADD_STAT(numFetches, statistics::units::Count::get(),
               "Instruction line fetches sent"),
      ADD_STAT(icacheStallCycles, statistics::units::Cycle::get(),
               "Cycles fetch waited for a code line (O3's "
               "fetch.icacheStallCycles)"),
      ADD_STAT(fetchTakenBreaks, statistics::units::Count::get(),
               "Fetch cycles ended by a taken branch"),
      ADD_STAT(loadRespOnEdge, statistics::units::Count::get(),
               "Load responses arriving exactly on a clock edge"),
      ADD_STAT(branches, statistics::units::Count::get(),
               "Branches predicted (last micro-op of a control instruction)"),
      ADD_STAT(branchMispredicts, statistics::units::Count::get(),
               "Mispredicted branches by kind"),
      ADD_STAT(btbMissMispredicts, statistics::units::Count::get(),
               "Mispredicts of taken branches the BTB had no entry for"),
      ADD_STAT(decodeRedirects, statistics::units::Count::get(),
               "Mispredicts redirected from decode (direct targets)"),
      ADD_STAT(fetchBranchStallCycles, statistics::units::Cycle::get(),
               "Fetch cycles waiting for a mispredicted branch to resolve"),
      ADD_STAT(mispredictDispatchGap, statistics::units::Cycle::get(),
               "Cycles from dispatching a mispredicted branch to dispatching "
               "the next micro-op (O3: rename gap after a squash)"),
      ADD_STAT(wrongPathUops, statistics::units::Count::get(),
               "Wrong-path micro-ops O3 would have renamed into the ROB"),
      ADD_STAT(robSquashCycles, statistics::units::Cycle::get(),
               "Cycles commit spends squashing them (squashWidth/cycle)"),
      ADD_STAT(numSplitReqs, statistics::units::Count::get(),
               "Accesses crossing a line, sent as two requests"),
      ADD_STAT(numStrictlyOrdered, statistics::units::Count::get(),
               "Strictly ordered accesses, completed without memory"),
      ADD_STAT(numDepOverflows, statistics::units::Count::get(),
               "Records with more than MaxDeps register dependencies; the "
               "excess was dropped"),
      ADD_STAT(lastTick, statistics::units::Tick::get(),
               "Tick at which this core finished"),
      ADD_STAT(commitCycles, statistics::units::Cycle::get(),
               "Cycles that committed at least one micro-op"),
      ADD_STAT(idleRobEmpty, statistics::units::Cycle::get(),
               "Cycles committing nothing with an empty ROB (front end)"),
      ADD_STAT(idleHeadLoad, statistics::units::Cycle::get(),
               "Cycles committing nothing with a load at the ROB head"),
      ADD_STAT(idleHeadStore, statistics::units::Cycle::get(),
               "Cycles committing nothing with a store blocked at the head"),
      ADD_STAT(idleHeadExec, statistics::units::Cycle::get(),
               "Cycles committing nothing with a COMP or unissued store "
               "at the head"),
      ADD_STAT(dispatchStallFrontend, statistics::units::Cycle::get(),
               "Cycles dispatching nothing: instruction line not fetched"),
      ADD_STAT(dispatchStallRob, statistics::units::Cycle::get(),
               "Cycles dispatching nothing: ROB full"),
      ADD_STAT(dispatchStallIq, statistics::units::Cycle::get(),
               "Cycles dispatching nothing: issue queue full"),
      ADD_STAT(dispatchStallLq, statistics::units::Cycle::get(),
               "Cycles dispatching nothing: load queue full"),
      ADD_STAT(dispatchStallSq, statistics::units::Cycle::get(),
               "Cycles dispatching nothing: store queue full"),
      ADD_STAT(dispatchStallSquash, statistics::units::Cycle::get(),
               "Cycles dispatching nothing: refetch after a memory-order "
               "violation"),
      ADD_STAT(dispatchStallSync, statistics::units::Cycle::get(),
               "Cycles dispatching nothing: at a sync point or parked"),
      ADD_STAT(dispatchStallUnblock, statistics::units::Cycle::get(),
               "Cycles dispatching nothing: rename's skid buffer drained, "
               "decode not resumed yet (O3 block/unblock)"),
      ADD_STAT(nonSpecStalls, statistics::units::Count::get(),
               "Non-speculative micro-ops executed at the ROB head (O3's "
               "commit.commitNonSpecStalls)"),
      ADD_STAT(renameBlocks, statistics::units::Count::get(),
               "Times rename blocked on a full ROB/IQ/LQ/SQ (O3's "
               "rename.*FullEvents)"),
      ADD_STAT(fuBusy, statistics::units::Count::get(),
               "Ready micro-ops held back because no functional unit or "
               "memory port was free"),
      ADD_STAT(loadsForwarded, statistics::units::Count::get(),
               "Loads completed from an older store's data (O3's forwLoads)"),
      ADD_STAT(loadToUseCycles, statistics::units::Cycle::get(),
               "Sum over loads of issue-to-data cycles"),
      ADD_STAT(loadToUse, statistics::units::Rate<
                   statistics::units::Cycle, statistics::units::Count>::get(),
               "Mean load-to-use latency, issue to data (O3's "
               "lsq.loadToUse mean)", loadToUseCycles / numLoads),
      ADD_STAT(loadsWaitedOnStore, statistics::units::Count::get(),
               "Load issue attempts deferred behind an overlapping older "
               "store (data not ready, or partial overlap)"),
      ADD_STAT(loadsPartialStall, statistics::units::Count::get(),
               "Loads held until an older, partially overlapping store was "
               "sent to the cache (O3 counts each retry: rescheduledLoads)"),
      ADD_STAT(loadsDataWait, statistics::units::Count::get(),
               "Loads that waited for a covering store's data"),
      ADD_STAT(syncPointsReached, statistics::units::Count::get(),
               "Synchronisation points acted on"),
      ADD_STAT(syncParks, statistics::units::Count::get(),
               "Times parked waiting for the arbiter (barrier, queued "
               "lock, join)"),
      ADD_STAT(memDepLoads, statistics::units::Count::get(),
               "Loads made to wait on a predicted store (O3's "
               "conflictingLoads)"),
      ADD_STAT(memDepStores, statistics::units::Count::get(),
               "Stores made to wait on a predicted store (O3's "
               "conflictingStores)"),
      ADD_STAT(memOrderViolations, statistics::units::Count::get(),
               "Loads that read before an older overlapping store"),
      ADD_STAT(storeSetClears, statistics::units::Count::get(),
               "Periodic store-set predictor clears"),
      ADD_STAT(cpi, statistics::units::Rate<
                   statistics::units::Cycle, statistics::units::Count>::get(),
               "Cycles per micro-op", numCycles / numOps),
      ADD_STAT(opsPerCycle, statistics::units::Rate<
                   statistics::units::Count, statistics::units::Cycle>::get(),
               "Micro-ops committed per cycle", numOps / numCycles)
{
    branchMispredicts.init(7);
    const char *kinds[] = {"none", "cond", "jump", "call", "indCall",
                           "return", "indJump"};
    for (int k = 0; k < 7; ++k)
        branchMispredicts.subname(k, kinds[k]);
    cpi.precision(6);
    opsPerCycle.precision(6);
}

} // namespace gem5
