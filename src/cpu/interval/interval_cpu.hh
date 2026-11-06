/*
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CPU_INTERVAL_INTERVAL_CPU_HH__
#define __CPU_INTERVAL_INTERVAL_CPU_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/statistics.hh"
#include "cpu/interval/branch_model.hh"
#include "cpu/trace/sync_arbiter.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "mem/request.hh"
#include "params/IntervalCPU.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{

class System;

/**
 * Out-of-order core model driven by a micro-op stream instead of a recorded
 * schedule.
 *
 * TraceCPU replays an elastic trace: each node issues comp_delay after its
 * recorded dependencies resolve, so the core's timing is whatever the
 * capturing core did, encoded in delays and order edges. That only works when
 * delays and edges come from the same schedule, which a Pin-derived trace does
 * not have. IntervalCPU keeps the program (types, addresses, register
 * dependencies) and recomputes the timing:
 *
 *   fetch    as O3: one line buffer, one icache request at a time, sent
 *            when fetch reaches a new line; fetchWidth micro-ops per cycle,
 *            stopping after a taken branch; fetched micro-ops reach
 *            dispatch frontendDepth cycles later through a bounded queue
 *   dispatch in order, dispatchWidth per cycle, into ROB and IQ; loads and
 *            stores also take an LQ/SQ entry
 *   issue    out of order, oldest first, issueWidth per cycle, once every
 *            register producer has completed
 *              COMP  completes compLatency cycles later
 *              LOAD  sends a read; completes when the data returns
 *              STORE completes storeLatency cycles later; writes at commit
 *   commit   in order, commitWidth per cycle; a store's SQ entry is held
 *            until its write is acknowledged (store buffer)
 *
 * Interval behaviour falls out of the window: a long-latency load reaches
 * the ROB head and stalls commit until the ROB fills, while independent loads
 * behind it overlap (MLP). Cycles that commit nothing are attributed to the
 * ROB head's state, giving a measured CPI stack instead of an analytical one.
 *
 * Loads and stores also wait on O3's store-set memory dependence predictor:
 * a violation (younger load issued before an older store to the same
 * 2^depCheckShift-byte block) puts both PCs in one set, and from then on
 * each waits for the set's last dispatched store to execute. On synth l1
 * that false dependence is what holds O3 to 3.0 micro-ops/cycle.
 *
 * Records with weight > 0 (COMPs that ElasticTrace filtered out because
 * nothing depended on them) are re-inserted as independent filler COMPs so
 * they still occupy dispatch, ROB and commit bandwidth.
 */
class IntervalCPU : public ClockedObject, public SyncParticipant
{
  public:
    IntervalCPU(const IntervalCPUParams &params);

    void init() override;
    void startup() override;
    /** Called by the arbiter when this CPU may continue. */
    void releaseFromSync() override;
    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;

  private:
    /** Register dependencies kept per micro-op (etrace caps at 16). */
    static constexpr unsigned MaxDeps = 16;

    /**
     * Functional units, after O3's DefaultX86FUPool (src/arch/x86/
     * X86CPU.py, src/cpu/o3/FuncUnitConfig.py). Op-class codes are the ones
     * gen_gem5_uops.py writes into COMP records' flags.
     */
    enum FuGroup : uint8_t
    { FuInt, FuMulDiv, FuFpAlu, FuFpMulDiv, FuSimd, FuMem, NumFuGroups };
    struct OpClassDesc { FuGroup group; uint8_t latency; bool pipelined; };
    static constexpr unsigned NumOpClasses = 13;
    /** Op-class code of a memory barrier (gem5 mfence/lfence/sfence). */
    static constexpr unsigned OpBarrier = 12;
    static const OpClassDesc opClassDesc[NumOpClasses];
    static const unsigned fuCount[NumFuGroups];

    enum class UopType : uint8_t { Load, Store, Comp };

    /** A decoded micro-op waiting in the front end. */
    struct Uop
    {
        uint64_t seq = 0;          ///< 0 for filler COMPs
        UopType type = UopType::Comp;
        Addr pc = 0;
        Addr paddr = 0;
        unsigned size = 0;
        Request::FlagsType flags = 0;
        std::array<uint64_t, MaxDeps> deps; ///< producer sequence numbers
        uint8_t ndeps = 0;
        uint8_t opClass = 0;        ///< COMP only, see OpClassCode
        bool needsFetch = false;    ///< its code line is fetched via icache
        Addr fetchLine = 0;         ///< line holding the instruction's end
        Tick readyTick = 0;         ///< fetched: dispatchable from this tick
        uint8_t brKind = 0;         ///< BranchModel::Kind, last micro-op
        bool brTaken = false;
        uint8_t brLen = 0;          ///< instruction length
        bool mispredict = false;    ///< set when fetched
    };

    /** A micro-op in the window. Lives in a ring buffer of robSize slots. */
    struct RobEntry
    {
        uint64_t seq = 0;
        UopType type = UopType::Comp;
        Addr pc = 0;
        Addr paddr = 0;
        unsigned size = 0;
        Request::FlagsType flags = 0;
        /** Producers in flight at dispatch, as (slot, seq). A producer whose
         *  slot no longer holds its seq has committed, hence completed. */
        std::array<std::pair<unsigned, uint64_t>, MaxDeps + 2> deps;
        uint8_t ndeps = 0;
        uint8_t opClass = 0;
        uint8_t brKind = 0;
        bool mispredict = false;    ///< branch: fetch waits for it to resolve
        bool issued = false;
        bool holdsIq = false;   ///< issued load still occupying an IQ entry
        Tick dispatchTick = 0;
        Tick issueTick = 0;
        Tick firstTryTick = 0;      ///< load: first issue attempt
        uint8_t waitSeen = 0;       ///< load: 1 partial overlap, 2 data wait
        Tick completeTick = MaxTick;
        uint8_t respPending = 0;    ///< load: responses still to come
        /** O3 executes it non-speculatively, at the ROB head (barriers,
         *  locked loads); nonSpecReady is when commit's signal arrives. */
        bool nonSpec = false;
        Tick nonSpecReady = 0;
    };

    /** Why a cycle committed nothing, read off the ROB head. */
    enum class Idle : uint8_t
    {
        None,       ///< something committed
        RobEmpty,   ///< front end could not supply micro-ops
        HeadLoad,   ///< oldest micro-op is a load waiting for data
        HeadStore,  ///< oldest micro-op is a store blocked on the port
        HeadExec,   ///< oldest micro-op is waiting on operands or executing
    };

    /** What stopped dispatch in a cycle that dispatched nothing. */
    enum class DispStall : uint8_t
    { None, Frontend, Rob, Iq, Lq, Sq, Squash, Sync, Unblock };

    class IcachePort : public RequestPort
    {
      public:
        IcachePort(IntervalCPU *cpu)
            : RequestPort(cpu->name() + ".icache_port"), owner(cpu) {}
      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
      private:
        IntervalCPU *owner;
    };

    class DcachePort : public RequestPort
    {
      public:
        DcachePort(IntervalCPU *cpu)
            : RequestPort(cpu->name() + ".dcache_port"), owner(cpu) {}
      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
      private:
        IntervalCPU *owner;
    };

    // ---- pipeline ---------------------------------------------------------
    void tick();
    unsigned commit();
    unsigned issue();
    unsigned dispatch();
    unsigned fetch();
    /** Advance the rename skid-buffer state after dispatching n. */
    void skidUpdate(unsigned n);
    /** Predict the branch just fetched; true if mispredicted. */
    bool predictBranch(Uop &u);

    bool depsReady(const RobEntry &e) const;
    void fillLookahead();
    void pushUop(Uop &&u);
    void account(Idle idle, DispStall stall, Cycles n);
    PacketPtr makeDataPacket(const RobEntry &e, bool is_read, Addr addr,
                             unsigned size);
    /** Send a load or store, as two requests if it crosses a line;
     *  returns the number sent. */
    unsigned sendData(const RobEntry &e, bool is_read);
    void queueDcache(PacketPtr pkt);
    /** Packets waiting behind dcacheRetryPkt (second half of a split). */
    std::deque<PacketPtr> dcacheBacklog;
    /** Split stores' outstanding halves, by sequence number. */
    std::unordered_map<uint64_t, unsigned> storeParts;
    static constexpr Request::FlagsType RmwMarker = 0x80000000;
    static constexpr Request::FlagsType NonSpecMarker = 0x40000000;
    /** The youngest memory barrier dispatched (younger loads and stores
     *  wait for it, as in O3's MemDepUnit::insertBarrierSN). */
    uint64_t lastBarrierSeq = 0;
    Tick lastStoreSendTick = 0;
    bool strictlyOrdered(const RobEntry &e) const;
    Idle classifyIdle() const;
    bool finished() const;
    void wakeAt(Tick when);

    // ---- memory dependence prediction (O3 StoreSet + MemDepUnit) ----------
    unsigned calcSSID(Addr pc) const { return (pc ^ (pc >> 10)) % lfstSize; }
    void memDepDispatch(RobEntry &e);
    void memDepStoreIssued(unsigned store_slot);
    void storeSetViolation(Addr store_pc, Addr load_pc);

    // ---- synchronisation (SyncArbiter) ------------------------------------
    enum SyncType : uint32_t
    { SyncParallel = 0, SyncCritical = 1, SyncBarrier = 2, SyncSpawn = 3,
      SyncJoin = 4 };
    struct SyncPoint
    {
        uint64_t at;      ///< micro-op count at which it applies
        uint32_t type;
        bool isEnd;
        uint32_t child;   ///< spawn/join only
    };
    void loadSyncTrace(const std::string &filename);
    /** True while dispatch must wait at the next sync point. */
    bool syncGate() const;
    /** Act on every sync point the committed count has reached. */
    void processSyncPoints();

    void recvIcacheResp(PacketPtr pkt);
    void recvDcacheResp(PacketPtr pkt);
    void icacheRetry();
    void dcacheRetry();

    unsigned slot(unsigned i) const { return (robHead + i) % robSize; }

    // ---- configuration ----------------------------------------------------
    const RequestorID instRequestorId;
    const RequestorID dataRequestorId;
    const Addr lineSize;
    System *system;
    const unsigned dispatchWidth;
    const unsigned issueWidth;
    const unsigned commitWidth;
    const unsigned robSize;
    const unsigned iqSize;
    const unsigned lqSize;
    const unsigned sqSize;
    const Cycles compLatency;
    const Cycles storeLatency;
    const bool fuModel;
    const Cycles forwardLatency;
    const bool loadWakeNextEdge;
    const Cycles loadIssueDelay;
    const Cycles commitDelay;
    const Cycles storeWriteDelay;
    const Cycles dispatchToIssueDelay;
    const bool staleOccupancy;
    const bool storeHoldsIq;
    const bool branchPred;
    const Cycles branchRedirectDelay;
    const Cycles decodeRedirectDelay;
    const unsigned squashWidth;
    const Cycles squashUnblockDelay;
    const bool skidModel;
    const unsigned skidSize;

    /** O3's rename/decode block-unblock protocol (Rename/Decode skid
     *  buffers): after a backend stall rename can rename only what reached
     *  its skid buffer; decode resumes when that is drained. */
    enum class Skid : uint8_t { Run, Blocked, Refill };
    Skid skidState = Skid::Run;
    size_t skidRename = 0, skidDecode = 0;
    Tick skidDecodeAt = 0, skidRefillAt = 0;

    // ---- branch prediction (O3's TournamentBP, BTB, RAS, indirect) -------
    BranchModel bpred;
    /** Fetch stopped at a mispredicted branch: its seq, and the tick fetch
     *  resumes (0 until the branch has issued and its resolution is known). */
    uint64_t brBlockSeq = 0;
    Tick brRedirectTick = 0;
    /** The wrong path O3 renames while fetch is held at a mispredict. */
    bool wrongPathOn = false;
    Tick wrongPathFrom = 0, wrongPathUntil = 0;
    size_t wrongPathRob = 0;
    double wrongPathCredit = 0;
    std::vector<std::pair<Tick, unsigned>> wrongPathSched;
    size_t wrongPathPos = 0;
    double uopsPerByte = 0.5;  ///< micro-ops per byte of code (average)
    unsigned uopsThisInst = 0;
    double fetchRate = 4.0;    ///< micro-ops fetched per cycle (average)
    Tick brCompleteTick = 0;
    Tick brDispatchTick = 0;
    Tick robSquashUntil = 0;   ///< dispatch blocked while the ROB squashes
    Tick mispredDispatchTick = 0;
    const bool tsoStores;
    const bool fetchEnabled;
    const unsigned fetchWidth;
    const unsigned fetchQueueSize;
    const Cycles frontendDepth;
    const unsigned lookaheadSize;
    const bool memDepPred;
    const unsigned depCheckShift;
    const uint64_t storeSetClearPeriod;
    const unsigned lfstSize;
    const Cycles squashPenalty;

    IcachePort icachePort;
    DcachePort dcachePort;

    // ---- trace ------------------------------------------------------------
    /**
     * Reads the InstDepRecord stream (gem5's protobuf framing: optional
     * gzip, LE32 magic, then varint-length-prefixed messages) by decoding
     * the varints directly. Profiled on Ruby: going through libprotobuf
     * cost 24% of an IntervalCPU run, more than the core model.
     */
    class TraceReader
    {
      public:
        struct Record
        {
            uint64_t seq = 0, paddr = 0, pc = 0;
            uint32_t type = 0, size = 0, flags = 0, weight = 0;
            std::array<uint64_t, MaxDeps> deps;
            uint8_t ndeps = 0;
            bool overflow = false;   ///< more than MaxDeps reg deps
        };
        TraceReader(const std::string &filename, const std::string &owner);
        ~TraceReader();
        bool next(Record &r);
      private:
        bool fill(size_t need);
        bool varint(uint64_t &v);
        void *file;                  ///< gzFile
        std::vector<uint8_t> buf;
        size_t pos = 0, end = 0;
        bool eof = false;
        std::string name;
    };

    TraceReader trace;
    bool traceDone = false;

    // ---- front end --------------------------------------------------------
    std::deque<Uop> pending;           ///< read from the trace, not fetched
    /** Instruction length by PC, learned from records that carry it. */
    std::unordered_map<Addr, uint8_t> pcLen;
    std::deque<Uop> fetched;           ///< fetched, on their way to dispatch
    /** O3's fetch buffer: the one code line fetch can read from. */
    Addr fetchBufLine = MaxAddr;
    bool fetchBufValid = false;
    Tick fetchReqTick = 0;
    Addr lastFetchedPc = 0;
    /** A line-crossing instruction whose first part has been decoded. */
    Addr straddlePc = 0;
    Addr lastCheckedLine = MaxAddr;
    bool lastLineIsMem = false;
    unsigned fetchesInFlight = 0;
    PacketPtr icacheRetryPkt = nullptr;

    // ---- window -----------------------------------------------------------
    std::vector<RobEntry> rob;
    unsigned robHead = 0;
    unsigned robCount = 0;
    std::vector<unsigned> iq;          ///< unissued slots, program order
    /** Issued loads still holding an IQ entry. As in O3 (inst_queue.cc,
     *  wakeDependents), a memory instruction frees its entry when it
     *  completes, not when it issues, so outstanding misses fill the IQ and
     *  bound memory-level parallelism. Measured on synth fp: without this the
     *  model ran out of L1 MSHRs 190x more often than O3 yet finished 19%
     *  sooner. */
    unsigned iqHeld = 0;
    /** Ticks at which issued stores give back their IQ entries. */
    std::deque<Tick> iqReleaseQ;
    /** Occupancy at the start of this tick, which dispatch sees. */
    size_t occRob0 = 0, occIq0 = 0, occLq0 = 0, occSq0 = 0;

    // ---- store-to-load forwarding -------------------------------------------
    /** Store slots in the window, program order. */
    std::deque<unsigned> sqOrder;
    /** Committed stores not yet sent to the cache, oldest first. They can
     *  still forward; once sent they cannot (O3's storeWBIt). */
    struct PendingWrite
    {
        uint64_t seq; Addr pc, paddr; unsigned size; Request::FlagsType flags;
        Tick ready;                ///< may be sent from this tick
    };
    std::deque<PendingWrite> storeWriteQ;
    /** Send committed stores, one in flight at a time under TSO. */
    unsigned drainStores();
    enum class Fwd { None, Forward, Wait, Partial };
    /** How an issuing load relates to older stores (O3 LSQUnit::read). */
    Fwd forwardCheck(unsigned load_slot) const;
    std::unordered_map<uint64_t, unsigned> inflight; ///< seq -> slot
    unsigned lqCount = 0;
    unsigned sqCount = 0;
    unsigned loadsInFlight = 0;
    unsigned storesInFlight = 0;   ///< committed writes not yet acknowledged
    PacketPtr dcacheRetryPkt = nullptr;
    /** Issued loads waiting to execute: (send tick, slot), in order. */
    std::deque<std::pair<Tick, unsigned>> loadSendQ;
    unsigned sendLoads();

    // ---- functional units -------------------------------------------------
    std::array<unsigned, NumFuGroups> fuUsed{};   ///< issued this cycle
    /** Non-pipelined units: tick each unit becomes free, per group. */
    std::array<std::vector<Tick>, NumFuGroups> fuBusyUntil;

    // ---- store sets -------------------------------------------------------
    std::unordered_map<Addr, unsigned> ssit;   ///< PC -> store set
    std::vector<uint64_t> lfst;                ///< set -> last fetched store
    std::vector<uint8_t> lfstValid;
    uint64_t memOpsPred = 0;
    Tick squashUntil = 0;

    // ---- synchronisation --------------------------------------------------
    SyncArbiter *arbiter;
    const unsigned threadId;
    std::vector<SyncPoint> syncPoints;
    size_t nextSync = 0;
    bool syncEnabled = false;
    bool barrierMember = false;     ///< registered for barriers/critical
    bool countsAtBarriers = false;  ///< has barrier/parallel events
    unsigned arbiterId = 0;
    bool parked = false;            ///< waiting for releaseFromSync()
    uint64_t dispatchedOps = 0;
    uint64_t committedOps = 0;

    // ---- timing -----------------------------------------------------------
    EventFunctionWrapper tickEvent;
    Cycles lastTickCycle;
    Tick lastTickTick = 0;
    Idle lastIdle = Idle::None;
    DispStall dispStall = DispStall::None;
    DispStall lastDispStall = DispStall::None;
    bool done = false;

    static int numActive;

    struct IntervalCPUStats : public statistics::Group
    {
        IntervalCPUStats(IntervalCPU *cpu);

        statistics::Scalar numCycles;
        statistics::Scalar numOps;
        statistics::Scalar numLoads;
        statistics::Scalar numStores;
        statistics::Scalar numFillerOps;
        statistics::Scalar numFetches;
        statistics::Scalar icacheStallCycles;
        statistics::Scalar fetchTakenBreaks;
        statistics::Scalar loadRespOnEdge;
        statistics::Scalar branches;
        statistics::Vector branchMispredicts;
        statistics::Scalar btbMissMispredicts;
        statistics::Scalar decodeRedirects;
        statistics::Scalar fetchBranchStallCycles;
        statistics::Scalar wrongPathUops;
        statistics::Scalar mispredictDispatchGap;
        statistics::Scalar robSquashCycles;
        statistics::Scalar numSplitReqs;
        statistics::Scalar numStrictlyOrdered;
        statistics::Scalar numDepOverflows;
        statistics::Scalar lastTick;

        statistics::Scalar commitCycles;
        statistics::Scalar idleRobEmpty;
        statistics::Scalar idleHeadLoad;
        statistics::Scalar idleHeadStore;
        statistics::Scalar idleHeadExec;

        statistics::Scalar dispatchStallFrontend;
        statistics::Scalar dispatchStallRob;
        statistics::Scalar dispatchStallIq;
        statistics::Scalar dispatchStallLq;
        statistics::Scalar dispatchStallSq;
        statistics::Scalar dispatchStallSquash;
        statistics::Scalar dispatchStallSync;
        statistics::Scalar dispatchStallUnblock;
        statistics::Scalar renameBlocks;
        statistics::Scalar nonSpecStalls;
        statistics::Scalar fuBusy;
        statistics::Scalar loadsForwarded;
        statistics::Scalar loadToUseCycles;   ///< sum, issue -> data
        statistics::Formula loadToUse;
        statistics::Scalar loadsWaitedOnStore;
        statistics::Scalar loadsPartialStall;
        statistics::Scalar loadsDataWait;
        statistics::Scalar syncPointsReached;
        statistics::Scalar syncParks;

        statistics::Scalar memDepLoads;
        statistics::Scalar memDepStores;
        statistics::Scalar memOrderViolations;
        statistics::Scalar storeSetClears;

        statistics::Formula cpi;
        statistics::Formula opsPerCycle;
    } stats;
};

} // namespace gem5

#endif // __CPU_INTERVAL_INTERVAL_CPU_HH__
