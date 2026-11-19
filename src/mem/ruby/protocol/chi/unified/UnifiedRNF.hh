/*
 * UnifiedRNF: one core's L1I + L1D + L2 as a single CHI request node.
 *
 * Architecture
 *   - LineTable: one entry per line holding the joint L1/L2 state, so a
 *     single lookup decides any access. L2 is inclusive of both L1s.
 *   - Tag arrays (L1I, L1D, L2): ways plus gem5 replacement policies; used
 *     only to choose victims, never to decide hit or miss.
 *   - Pools: L1D/L1I TBEs, L2 request TBEs (MSHRs to L3), replacement TBEs,
 *     snoop TBEs, with time-weighted occupancy.
 *   - CHI channels: the eight Ruby message buffers of CHIGenericController.
 *     The HN-F sees an ordinary RN-F.
 *
 * Time
 *   gem5's event queue is the only clock. Hits create no events: the CPU
 *   gets the completion tick back from fastMemAccess(). Misses, writebacks
 *   and snoops schedule the message sends Ruby's L2 would make, at the
 *   ticks it would make them (constant latencies from the parameters).
 */

#ifndef __MEM_RUBY_PROTOCOL_CHI_UNIFIED_UNIFIEDRNF_HH__
#define __MEM_RUBY_PROTOCOL_CHI_UNIFIED_UNIFIEDRNF_HH__

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>

#include "base/output.hh"
#include "base/statistics.hh"
#include "cpu/interval/fast_mem.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/ruby/protocol/CHI/CHIDataType.hh"
#include "mem/ruby/protocol/CHI/CHIRequestType.hh"
#include "mem/ruby/protocol/CHI/CHIResponseType.hh"
#include "mem/ruby/protocol/chi/generic/CHIGenericController.hh"
#include "params/UnifiedRNF.hh"
#include "sim/eventq.hh"

namespace gem5
{
namespace ruby
{

class UnifiedRNF : public CHIGenericController, public FastMem
{
  public:
    PARAMS(UnifiedRNF);
    UnifiedRNF(const Params &p);

    void init() override;
    void startup() override;
    void collateStats() override;
    void resetStats() override;

    // FastMem (CPU side)
    void setFastMemClient(FastMemClient *c) override { client = c; }
    Tick fastMemAccess(PacketPtr pkt) override;

  protected:
    bool recvRequestMsg(const CHIRequestMsg *msg) override;
    bool recvSnoopMsg(const CHIRequestMsg *msg) override;
    bool recvResponseMsg(const CHIResponseMsg *msg) override;
    bool recvDataMsg(const CHIDataMsg *msg) override;

  private:
    using ReqType = CHI::CHIRequestType;
    using RspType = CHI::CHIResponseType;
    using DatType = CHI::CHIDataType;

    // ---- line state -----------------------------------------------------
    enum class Chi : uint8_t { I, SC, SD, UC, UD };   ///< as the HN-F sees it
    /** L1D copy. Sd: shared, but the L1 owns the dirty data (Ruby's L2
     *  passes dirty on an L1 ReadShared: UD_RSD). */
    enum class L1d : uint8_t { None, S, Sd, Uc, Ud };
    /** Transaction with the HN-F (or waiting for a pool to issue one). */
    enum class Txn : uint8_t
    {
        None,
        WaitMshr,   ///< read/upgrade needed, no request TBE free
        RS,         ///< ReadShared / ReadNotSharedDirty
        RU,         ///< ReadUnique
        CU,         ///< CleanUnique
        WaitRepl,   ///< eviction needed, no replacement TBE free
        WB,         ///< WriteBackFull
        WE,         ///< WriteEvictFull
        EV          ///< Evict
    };
    enum class Acc : uint8_t { Load, Store, Fetch };

    struct Access
    {
        PacketPtr pkt;
        Acc type;
    };

    /**
     * Ruby Sequencer behaviour, per line and per side (data / fetch): one
     * access to a line is in the cache at a time; later ones wait for it.
     * When it completes, the waiting ones complete with it, except a store
     * behind a load, which is issued then (Sequencer::readCallback).
     */
    struct SeqSide
    {
        Tick until = 0;            ///< in-flight hit completes at this tick
        Acc last = Acc::Load;      ///< type of the access in flight / last
        bool pending = false;      ///< in-flight access completes later
        std::deque<Access> q;      ///< accesses waiting behind it
    };

    struct Entry
    {
        Addr addr = 0;
        SeqSide side[2];           ///< [0] data, [1] fetch
        Chi chi = Chi::I;
        L1d l1d = L1d::None;
        bool l1i = false;
        int l2Way = -1, l1dWay = -1, l1iWay = -1;

        Txn txn = Txn::None;
        std::deque<Access> waiters;   ///< front = primary of a read txn
        uint8_t holdsL1 = 0;          ///< 1: L1D TBE, 2: L1I TBE
        // read/upgrade bookkeeping
        bool sent = false;            ///< REQ has left (outstanding)
        bool dataDone = false;        ///< installed, waiting for finish
        unsigned beats = 0;
        bool sepData = false, gotSepResp = false;
        Chi granted = Chi::I;
        bool cuLost = false;          ///< invalidated while CU pending
        bool retryAck = false, retryCredit = false;
        ReqType pendReq = CHI::CHIRequestType_NUM;
        Tick reqTick = 0;
    };

    bool busy(const Entry &e) const { return e.txn != Txn::None; }
    /** The L1D owns the line: answers snoops and evicts with data. */
    static bool l1dOwns(const Entry &e)
    { return e.l1d == L1d::Sd || e.l1d == L1d::Uc || e.l1d == L1d::Ud; }
    static bool unique(Chi c) { return c == Chi::UC || c == Chi::UD; }
    static bool dirty(Chi c) { return c == Chi::UD || c == Chi::SD; }

    // ---- tag arrays -----------------------------------------------------
    struct Way : public ReplaceableEntry
    {
        Entry *e = nullptr;
    };

    class TagArray
    {
      public:
        void init(const std::string &name, uint64_t bytes, unsigned assoc,
                  unsigned line_bits, replacement_policy::Base *rp);
        unsigned setOf(Addr a) const { return (a >> lineBits) & setMask; }
        void touch(int w) { rp->touch(ways[w].replacementData); }
        /** Way to fill for @a; *victim = its occupant (nullptr if free). */
        int pick(Addr a, Entry **victim,
                 const std::function<bool(const Entry &)> &busy);
        void place(int w, Entry *e);
        void clear(int w);
        Entry *at(int w) const { return ways[w].e; }

      private:
        std::string name;
        unsigned assoc = 0, lineBits = 0;
        Addr setMask = 0;
        replacement_policy::Base *rp = nullptr;
        std::vector<Way> ways;          ///< set-major
    };

    // ---- resource pools --------------------------------------------------
    class Pool
    {
      public:
        void init(unsigned cap) { capacity = cap; occTicks.assign(cap + 1, 0); }
        unsigned occ() const { return held + timed.size(); }
        bool full(Tick now) { advance(now); return occ() >= capacity; }
        void hold(Tick now) { advance(now); ++held; }
        void release(Tick now) { advance(now); assert(held); --held; }
        void holdUntil(Tick now, Tick until)
        { advance(now); timed.push(until); }
        Tick nextRelease() const { return timed.empty() ? MaxTick : timed.top(); }
        /** Account occupancy up to @now and free expired timed holds. */
        void advance(Tick now);
        void resetStats(Tick now);
        double periodAvg(Tick now);   ///< mean since the last call

        unsigned capacity = 0;
        std::vector<double> occTicks;   ///< ticks spent at each occupancy
        double area = 0;                ///< sum occupancy * ticks

      private:
        void account(Tick t);
        unsigned held = 0;
        std::priority_queue<Tick, std::vector<Tick>, std::greater<Tick>>
            timed;
        Tick last = 0;
        double periodArea = 0;
        Tick periodStart = 0;
    };

    // ---- L1 <-> L2 links (Ruby routes them through the NoC) ---------------
    /**
     * One link direction as a calendar of busy intervals. A message takes
     * the earliest free slot at or after the time it reaches the link and
     * holds it for its flit time; like Ruby's Throttle, only the messages
     * behind it wait, the message itself is not delayed by its own size.
     */
    class LinkCal
    {
      public:
        /** Earliest slot of @occ at or after @t. Returns when the message
         *  leaves, as Ruby's throttle sees it: in the cycle the slot starts
         *  (a message starts in the cycle the previous one ends, if that
         *  cycle has bandwidth left). */
        Tick reserve(Tick t, Tick occ);
        void prune(Tick before);
        Tick period = 1;                  ///< the link's cycle
        uint64_t msgs = 0;
        double busy = 0, wait = 0;        ///< ticks
      private:
        std::map<Tick, Tick> iv;          ///< start -> end, disjoint
    };
    enum Link { DUp, DDown, IUp, IDown, L2In, NumLinks };
    LinkCal links[NumLinks];
    Tick edgeAtOrAfter(Tick t) const;
    /** L1 -> L2 control/data; returns arrival at the L2. */
    Tick upPath(bool fetch, Tick t, bool data);
    /** L2 -> L1: one control message or a line (all beats); arrival. */
    Tick downPath(bool fetch, Tick t, bool data);
    /** Messages after an L1 fill: CompAck and the L1 victim's eviction. */
    void l1AfterFill(bool fetch, Tick t, int victim_kind);
    /** Fill the L1 (victim choice, insertion) at @when, when the data
     *  reaches it. */
    void fillL1At(Addr line, Acc t, Tick when);
    /** Message from the L3 entering the L2: when Ruby's L2 would see it. */
    Tick inboundAt(int ch, bool data);
    Tick lastInbound[4] = {};

    // ---- core-side paths -------------------------------------------------
    Acc accType(PacketPtr pkt) const;
    /** Sequencer layer: completion tick or MaxTick (pending). */
    Tick seqAccept(const Access &a);
    void seqDone(Addr line, bool fetch, Acc type, bool release_primary);
    void retrySeqFull();
    Pool &seqPool(bool fetch) { return fetch ? iseqPool : dseqPool; }
    /** Completion tick, or MaxTick when the access is queued. */
    Tick tryAccess(const Access &a, bool rerun = false);
    void deliver(const Access &a, Tick when);
    void rerun(std::deque<Access> &&accs);
    bool l1Hit(const Entry &e, Acc t) const;
    /** Returns the L1 victim's eviction: 0 none, 1 Evict, 2 with data. */
    int fillL1(Entry &e, Acc t);
    int evictL1(Entry &v, bool icache);
    void insertL2(Entry &e);
    void evictL2(Entry &v);
    void touchL2(Entry &e) { if (e.l2Way >= 0) l2.touch(e.l2Way); }
    void dropFromArrays(Entry &e);
    void maybeErase(Addr line);
    Entry *find(Addr line);

    // ---- transactions ----------------------------------------------------
    void startRead(Entry &e, bool from_core = true);
    bool handleData(const CHIDataMsg *msg);
    bool handleResponse(const CHIResponseMsg *msg);
    void readComplete(Entry &e);
    void finishRead(Addr line);
    void maybeReadDone(Entry &e);
    void maybeResend(Entry &e);
    void issueEviction(Entry &v, Tick when);
    void sendEviction(Addr line);
    void finishEviction(Addr line);
    void startWaiters();
    void retryStalled();
    void scheduleStallRetry(Pool &p);

    // ---- messages --------------------------------------------------------
    void sendReq(Addr line, ReqType t, bool allow_retry);
    void sendRsp(Addr line, RspType t, MachineID dest);
    void sendData(Addr line, DatType t, MachineID dest);
    void sendCompAck(Addr line);
    void sendOut(int ch, std::shared_ptr<Message> msg);
    bool trySend(int ch, const std::shared_ptr<Message> &msg);
    void retryBlocked();

    // ---- own timer queue (one gem5 event for all deferred work) ---------
    void at(Tick when, std::function<void()> fn);
    void runTimers();

    struct Timed
    {
        Tick when;
        uint64_t seq;
        std::function<void()> fn;
        bool operator>(const Timed &o) const
        { return when != o.when ? when > o.when : seq > o.seq; }
    };
    std::priority_queue<Timed, std::vector<Timed>, std::greater<Timed>>
        timers;
    uint64_t timerSeq = 0;
    EventFunctionWrapper timerEvent;

    // ---- sampler ---------------------------------------------------------
    void sample();
    EventFunctionWrapper sampleEvent;
    OutputStream *sampleOut = nullptr;

    // ---- state -----------------------------------------------------------
    FastMemClient *client = nullptr;
    std::unordered_map<Addr, Entry> table;
    TagArray l1d, l1i, l2;
    Pool l1dTbe, l1iTbe, reqTbe, replTbe, snpTbe, outstanding;
    Pool dseqPool, iseqPool;                 ///< Sequencer outstanding limit
    std::deque<Access> dseqFull, iseqFull;   ///< refused: sequencer full
    std::deque<Access> l1dStall, l1iStall;   ///< no L1 TBE
    std::deque<Addr> mshrWait, replWait;     ///< no request / repl TBE
    std::deque<std::shared_ptr<Message>> blocked[4];
    bool retryScheduled = false;

    const bool allowSD, hitsUseTbe, linksOn;
    const Cycles l1SendLat, upPre, upMid, upPost, downPre, downPost;
    const Tick ctrlOcc, dataOcc;             ///< link time per message
    const Cycles l2HitProc, l2UpgradeProc, l2MissProc, l2FwdProc;
    const Cycles l1EvictProc, l1WbDataLat;
    const Cycles l1dHitLat, l1iHitLat, l2HitLat, l2UpgradeLat;
    const Cycles missIssueLat, ackLat, fillLat, respLat;
    const Cycles victimStartLat, victimEvictLat, victimWbLat;
    const Cycles backInvSendLat, backInvDataLat, wbDataLat;
    const Cycles snpLat, snpInvLat, snpL1Lat, snpL1InvLat, snpRespLat;
    const Cycles samplePeriod;
    const std::string sampleFile;

    // per-sample-period counters
    uint64_t perSecondary = 0;
    uint64_t perFlits[2][4] = {};   ///< [out=0/in=1][CHI channel]

    struct RnfStats : public statistics::Group
    {
        RnfStats(statistics::Group *parent);
        statistics::Scalar l1dHits, l1iHits, l2Hits, l2Upgrades;
        statistics::Scalar l3Reads, secondary, coalesced, seqFull;
        statistics::Scalar l1TbeStalls, mshrStalls, replStalls, snpStalls;
        statistics::Scalar retryAcks, cuLost, evictions, l1Evictions;
        statistics::Scalar backInvs;
        statistics::Scalar snoopsViaL1, blockedSends;
        statistics::Vector reqOut, rspOut, datOut;
        statistics::Vector snpIn, rspIn, datIn;
        statistics::Vector flitsOut, flitsIn;
        statistics::Vector occL1d, occL1i, occReq, occRepl, occSnp;
        statistics::Scalar avgL1d, avgL1i, avgReq, avgRepl, avgSnp;
        statistics::Scalar avgOutstanding;
        statistics::Histogram readLatency, snoopLatency;
        statistics::Vector linkMsgs, linkBusy, linkWait;
        statistics::Formula linkAvgWait;
    } rnfStats;
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_PROTOCOL_CHI_UNIFIED_UNIFIEDRNF_HH__
