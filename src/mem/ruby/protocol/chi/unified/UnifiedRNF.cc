#include "mem/ruby/protocol/chi/unified/UnifiedRNF.hh"

#include <algorithm>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/UnifiedRNF.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/system/RubySystem.hh"

namespace gem5
{
namespace ruby
{

using namespace CHI;

namespace
{

const char *
chiName(int c)
{
    static const char *n[] = {"I", "SC", "SD", "UC", "UD"};
    return n[c];
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Tag arrays and pools
// ---------------------------------------------------------------------------

void
UnifiedRNF::TagArray::init(const std::string &n, uint64_t bytes,
                           unsigned a, unsigned line_bits,
                           replacement_policy::Base *policy)
{
    name = n;
    assoc = a;
    lineBits = line_bits;
    rp = policy;
    const uint64_t lines = bytes >> line_bits;
    fatal_if(lines % assoc, "%s: size not a multiple of assoc", name);
    const uint64_t nsets = lines / assoc;
    fatal_if(!isPowerOf2(nsets), "%s: set count must be a power of 2", name);
    setMask = nsets - 1;
    ways.resize(nsets * assoc);
    // Set-major order, as CacheMemory: TreePLRU shares one tree per set.
    for (uint64_t s = 0; s < nsets; ++s) {
        for (unsigned w = 0; w < assoc; ++w) {
            Way &way = ways[s * assoc + w];
            way.setPosition(s, w);
            way.replacementData = rp->instantiateEntry();
        }
    }
}

int
UnifiedRNF::TagArray::pick(Addr a, Entry **victim,
                           const std::function<bool(const Entry &)> &busy)
{
    const unsigned base = setOf(a) * assoc;
    *victim = nullptr;
    for (unsigned w = 0; w < assoc; ++w)
        if (!ways[base + w].e)
            return base + w;
    ReplacementCandidates cands;
    for (unsigned w = 0; w < assoc; ++w)
        if (!busy || !busy(*ways[base + w].e))
            cands.push_back(&ways[base + w]);
    if (cands.empty())
        return -1;
    ReplaceableEntry *v = rp->getVictim(cands);
    const int idx = base + v->getWay();
    *victim = ways[idx].e;
    return idx;
}

void
UnifiedRNF::TagArray::place(int w, Entry *e)
{
    ways[w].e = e;
    rp->reset(ways[w].replacementData);
}

void
UnifiedRNF::TagArray::clear(int w)
{
    ways[w].e = nullptr;
    rp->invalidate(ways[w].replacementData);
}

void
UnifiedRNF::Pool::account(Tick t)
{
    if (t <= last)
        return;
    const double d = t - last;
    const unsigned o = occ();
    occTicks[std::min(o, capacity)] += d;
    area += o * d;
    periodArea += o * d;
    last = t;
}

void
UnifiedRNF::Pool::advance(Tick now)
{
    while (!timed.empty() && timed.top() <= now) {
        account(timed.top());
        timed.pop();
    }
    account(now);
}

void
UnifiedRNF::Pool::resetStats(Tick now)
{
    advance(now);
    std::fill(occTicks.begin(), occTicks.end(), 0);
    area = 0;
}

double
UnifiedRNF::Pool::periodAvg(Tick now)
{
    advance(now);
    const double avg = now > periodStart ? periodArea / (now - periodStart)
                                         : 0.0;
    periodArea = 0;
    periodStart = now;
    return avg;
}

Tick
UnifiedRNF::LinkCal::reserve(Tick t, Tick occ)
{
    const Tick asked = t;
    auto it = iv.upper_bound(t);
    if (it != iv.begin()) {
        auto p = std::prev(it);
        if (p->second > t)
            t = p->second;
    }
    while (it != iv.end() && it->first < t + occ) {
        t = std::max(t, it->second);
        ++it;
    }
    // insert [t, t + occ), merging with touching neighbours
    Tick start = t, end = t + occ;
    auto nx = iv.lower_bound(start);
    if (nx != iv.begin()) {
        auto p = std::prev(nx);
        if (p->second == start) {
            start = p->first;
            iv.erase(p);
        }
    }
    nx = iv.find(end);
    if (nx != iv.end()) {
        end = nx->second;
        iv.erase(nx);
    }
    iv[start] = end;
    ++msgs;
    busy += occ;
    wait += t - asked;
    return std::max(asked, t / period * period);
}

void
UnifiedRNF::LinkCal::prune(Tick before)
{
    while (!iv.empty() && iv.begin()->second < before)
        iv.erase(iv.begin());
}

Tick
UnifiedRNF::edgeAtOrAfter(Tick t) const
{
    const Tick e = clockEdge();
    return t <= e ? e : e + divCeil(t - e, clockPeriod()) * clockPeriod();
}

Tick
UnifiedRNF::upPath(bool fetch, Tick t, bool data)
{
    LinkCal &own = links[fetch ? IUp : DUp];
    LinkCal &in = links[L2In];
    const Tick occ = data ? dataOcc : ctrlOcc;
    const int n = data ? dataMsgsPerLine : 1;
    Tick s = t + cyclesToTicks(upPre), last = 0;
    for (int b = 0; b < n; ++b) {
        s = own.reserve(s, occ);
        last = in.reserve(std::max(last, s + cyclesToTicks(upMid)), occ);
    }
    const Tick now = curTick();
    if ((own.msgs & 1023) == 0) {
        own.prune(now > 1000000 ? now - 1000000 : 0);
        in.prune(now > 1000000 ? now - 1000000 : 0);
    }
    return edgeAtOrAfter(last + cyclesToTicks(upPost));
}

Tick
UnifiedRNF::downPath(bool fetch, Tick t, bool data)
{
    LinkCal &own = links[fetch ? IDown : DDown];
    const Tick occ = data ? dataOcc : ctrlOcc;
    const int n = data ? dataMsgsPerLine : 1;
    Tick s = t + cyclesToTicks(downPre);
    for (int b = 0; b < n; ++b)
        s = own.reserve(s, occ);
    if ((own.msgs & 1023) == 0) {
        const Tick now = curTick();
        own.prune(now > 1000000 ? now - 1000000 : 0);
    }
    return edgeAtOrAfter(s + cyclesToTicks(downPost));
}

void
UnifiedRNF::l1AfterFill(bool fetch, Tick t, int victim_kind)
{
    upPath(fetch, t, false);                     // CompAck to the L2
    if (!victim_kind)
        return;
    // Ruby's L1 evicts its victim when the new line fills
    const Tick at_l2 = upPath(fetch, t + cyclesToTicks(l1SendLat), false);
    const Tick resp = downPath(fetch, at_l2 + cyclesToTicks(l1EvictProc),
                               false);           // CompDBIDResp / Comp_I
    if (victim_kind == 2)                        // WriteBack/WriteEvict data
        upPath(fetch, resp + cyclesToTicks(l1WbDataLat), true);
}

Tick
UnifiedRNF::inboundAt(int ch, bool data)
{
    // The message reached us over our own input link; in Ruby it would have
    // shared the L2's input link with traffic from the L1s.
    const Tick now = curTick();
    if (!linksOn)
        return now;
    const Tick post = cyclesToTicks(upPost);
    const Tick occ = data ? dataOcc : ctrlOcc;
    const Tick s = links[L2In].reserve(now > post ? now - post : 0, occ);
    Tick t = std::max(edgeAtOrAfter(s + post), now);
    t = std::max(t, lastInbound[ch]);            // keep each channel FIFO
    lastInbound[ch] = t;
    return t;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

UnifiedRNF::UnifiedRNF(const Params &p)
    : CHIGenericController(p),
      timerEvent([this] { runTimers(); }, name() + ".timer"),
      sampleEvent([this] { sample(); }, name() + ".sample"),
      allowSD(p.allow_SD), hitsUseTbe(p.hits_use_tbe),
      linksOn(p.model_l1l2_links), l1SendLat(p.l1_send_latency),
      upPre(p.up_pre_latency), upMid(p.up_mid_latency),
      upPost(p.up_post_latency), downPre(p.down_pre_latency),
      downPost(p.down_post_latency), 
      ctrlOcc(clockPeriod() * p.ctrl_msg_bytes / p.link_bytes_per_cycle),
      dataOcc(clockPeriod() * p.data_msg_bytes / p.link_bytes_per_cycle),
      l2HitProc(p.l2_hit_proc_latency),
      l2UpgradeProc(p.l2_upgrade_proc_latency),
      l2MissProc(p.l2_miss_proc_latency), l2FwdProc(p.l2_fwd_latency),
      l1EvictProc(p.l1_evict_proc_latency), l1WbDataLat(p.l1_wbdata_latency),
      l1dHitLat(p.l1d_hit_latency), l1iHitLat(p.l1i_hit_latency),
      l2HitLat(p.l2_hit_latency), l2UpgradeLat(p.l2_upgrade_latency),
      missIssueLat(p.miss_issue_latency), ackLat(p.ack_latency),
      fillLat(p.fill_latency), respLat(p.resp_latency),
      victimStartLat(p.victim_start_latency),
      victimEvictLat(p.victim_evict_latency),
      victimWbLat(p.victim_wb_latency),
      backInvSendLat(p.backinv_send_latency),
      backInvDataLat(p.backinv_data_latency), wbDataLat(p.wb_data_latency),
      snpLat(p.snp_latency), snpInvLat(p.snp_inv_latency),
      snpL1Lat(p.snp_l1_latency), snpL1InvLat(p.snp_l1_inv_latency),
      snpRespLat(p.snp_resp_latency),
      samplePeriod(p.sample_period), sampleFile(p.sample_file),
      rnfStats(this)
{
    l1d.init(name() + ".l1d", p.l1d_size, p.l1d_assoc, cacheLineBits,
             p.l1d_replacement_policy);
    l1i.init(name() + ".l1i", p.l1i_size, p.l1i_assoc, cacheLineBits,
             p.l1i_replacement_policy);
    l2.init(name() + ".l2", p.l2_size, p.l2_assoc, cacheLineBits,
            p.l2_replacement_policy);
    l1dTbe.init(p.num_l1d_tbes);
    l1iTbe.init(p.num_l1i_tbes);
    reqTbe.init(p.num_req_tbes);
    replTbe.init(p.num_repl_tbes);
    snpTbe.init(p.num_snp_tbes);
    outstanding.init(p.num_req_tbes + p.num_repl_tbes);
    dseqPool.init(p.max_outstanding_requests);
    iseqPool.init(p.max_outstanding_requests);
    rnfStats.occL1d.init(l1dTbe.capacity + 1);
    rnfStats.occL1i.init(l1iTbe.capacity + 1);
    rnfStats.occReq.init(reqTbe.capacity + 1);
    rnfStats.occRepl.init(replTbe.capacity + 1);
    rnfStats.occSnp.init(snpTbe.capacity + 1);
    fatal_if(cacheLineSize % dataChannelSize,
             "line size must be a multiple of the DAT channel width");
    for (LinkCal &l : links)
        l.period = clockPeriod();
    // AbstractController registers the stats-dump callback only for
    // version 0 (which collates a whole SLICC machine type)
    if (m_version != 0)
        statistics::registerDumpCallback([this] { collateStats(); });
}

void
UnifiedRNF::init()
{
    CHIGenericController::init();
    for (int i = 0; i < CHIRequestType_NUM; ++i) {
        const auto n = CHIRequestType_to_string(CHIRequestType(i));
        rnfStats.reqOut.subname(i, n);
        rnfStats.snpIn.subname(i, n);
    }
    for (int i = 0; i < CHIResponseType_NUM; ++i) {
        const auto n = CHIResponseType_to_string(CHIResponseType(i));
        rnfStats.rspOut.subname(i, n);
        rnfStats.rspIn.subname(i, n);
    }
    for (int i = 0; i < CHIDataType_NUM; ++i) {
        const auto n = CHIDataType_to_string(CHIDataType(i));
        rnfStats.datOut.subname(i, n);
        rnfStats.datIn.subname(i, n);
    }
    const char *ch[] = {"REQ", "SNP", "RSP", "DAT"};
    for (int i = 0; i < 4; ++i) {
        rnfStats.flitsOut.subname(i, ch[i]);
        rnfStats.flitsIn.subname(i, ch[i]);
    }
}

void
UnifiedRNF::startup()
{
    CHIGenericController::startup();
    panic_if(!client, "%s: no CPU registered (IntervalCPU.fast_mem)",
             name());
    if (samplePeriod > 0) {
        const std::string f = sampleFile.empty()
            ? name() + ".occupancy.csv" : sampleFile;
        sampleOut = simout.create(f, false);
        *sampleOut->stream()
            << "tick,l1d_tbe,l1i_tbe,req_tbe,repl_tbe,snp_tbe,outstanding,"
               "secondary,req_out,rsp_out,dat_out,snp_in,rsp_in,dat_in\n";
        schedule(sampleEvent, clockEdge(samplePeriod));
    }
}

UnifiedRNF::Entry *
UnifiedRNF::find(Addr line)
{
    auto it = table.find(line);
    return it == table.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// Core side: one lookup per access
// ---------------------------------------------------------------------------

UnifiedRNF::Acc
UnifiedRNF::accType(PacketPtr pkt) const
{
    if (pkt->req->isInstFetch())
        return Acc::Fetch;
    // An RMW's load needs write permission, as Ruby's RMW_Read.
    if (pkt->isWrite() || pkt->req->isReadModifyWrite())
        return Acc::Store;
    return Acc::Load;
}

Tick
UnifiedRNF::fastMemAccess(PacketPtr pkt)
{
    const Access a{pkt, accType(pkt)};
    const bool fetch = a.type == Acc::Fetch;
    std::deque<Access> &refused = fetch ? iseqFull : dseqFull;
    Pool &pool = seqPool(fetch);
    // Ruby's sequencer refuses requests beyond max_outstanding_requests and
    // the CPU resends them in order when one completes.
    // full() also expires timed holds, so nextRelease() is in the future
    const bool full = pool.full(curTick());
    if (!refused.empty() || full) {
        refused.push_back(a);
        ++rnfStats.seqFull;
        if (pool.nextRelease() != MaxTick)
            at(pool.nextRelease(), [this] { retrySeqFull(); });
        return MaxTick;
    }
    return seqAccept(a);
}

Tick
UnifiedRNF::seqAccept(const Access &a)
{
    const Tick now = curTick();
    const Addr line = a.pkt->getAddr() & ~Addr(cacheLineSize - 1);
    const bool fetch = a.type == Acc::Fetch;
    Pool &pool = seqPool(fetch);

    Entry *e = find(line);
    if (e) {
        SeqSide &sd = e->side[fetch];
        if (sd.pending) {                       // wait behind it
            sd.q.push_back(a);
            pool.hold(now);
            ++rnfStats.coalesced;
            return MaxTick;
        }
        if (now < sd.until) {                   // a hit still in flight
            ++rnfStats.coalesced;
            if (sd.last == Acc::Store || a.type != Acc::Store) {
                pool.holdUntil(now, sd.until);  // completes with it
                return sd.until;
            }
            // a store behind a load is issued when the load completes
            sd.q.push_back(a);
            sd.pending = true;
            pool.hold(now);
            const Acc last = sd.last;
            at(sd.until, [this, line, fetch, last] {
                seqDone(line, fetch, last, false);
            });
            return MaxTick;
        }
    } else {
        e = &table[line];
        e->addr = line;
    }

    const Tick done = tryAccess(a);
    e = find(line);
    SeqSide &sd = e->side[fetch];
    if (done != MaxTick) {
        sd.until = done;
        sd.last = a.type;
        pool.holdUntil(now, done);
        maybeErase(line);
        return done;
    }
    sd.pending = true;
    sd.last = a.type;
    pool.hold(now);
    return MaxTick;
}

void
UnifiedRNF::deliver(const Access &a, Tick when)
{
    // Completion of an access that was pending in the sequencer layer.
    client->fastMemDone(a.pkt, when);
    const Addr line = a.pkt->getAddr() & ~Addr(cacheLineSize - 1);
    const bool fetch = a.type == Acc::Fetch;
    const Acc type = a.type;
    at(when, [this, line, fetch, type] { seqDone(line, fetch, type, true); });
}

void
UnifiedRNF::seqDone(Addr line, bool fetch, Acc type, bool release_primary)
{
    const Tick now = curTick();
    Pool &pool = seqPool(fetch);
    Entry *e = find(line);
    assert(e);
    SeqSide &sd = e->side[fetch];
    if (release_primary)
        pool.release(now);
    sd.pending = false;
    sd.until = now;
    sd.last = type;
    while (!sd.q.empty()) {
        const Access b = sd.q.front();
        sd.q.pop_front();
        pool.release(now);
        if (sd.last == Acc::Store || b.type != Acc::Store) {
            const Tick t = std::max(sd.until, now);
            if (t > now)
                pool.holdUntil(now, t);
            client->fastMemDone(b.pkt, t);
            continue;
        }
        // store behind a load: issued now (Sequencer::readCallback)
        const Tick done = tryAccess(b);
        e = find(line);
        if (done != MaxTick) {
            pool.holdUntil(now, done);
            e->side[fetch].until = done;
            e->side[fetch].last = Acc::Store;
            client->fastMemDone(b.pkt, done);
            continue;
        }
        pool.hold(now);
        e->side[fetch].pending = true;
        e->side[fetch].last = Acc::Store;
        break;
    }
    maybeErase(line);
    retrySeqFull();
}

void
UnifiedRNF::retrySeqFull()
{
    const Tick now = curTick();
    for (int f = 0; f < 2; ++f) {
        std::deque<Access> &q = f ? iseqFull : dseqFull;
        Pool &pool = seqPool(f);
        while (!q.empty() && !pool.full(now)) {
            const Access a = q.front();
            q.pop_front();
            const Tick done = seqAccept(a);
            if (done != MaxTick)
                client->fastMemDone(a.pkt, done);
        }
        if (!q.empty() && pool.nextRelease() != MaxTick)
            at(pool.nextRelease(), [this] { retrySeqFull(); });
    }
}

void
UnifiedRNF::rerun(std::deque<Access> &&accs)
{
    for (const Access &a : accs) {
        const Tick done = tryAccess(a, true);
        if (done != MaxTick)
            deliver(a, done);
    }
}

bool
UnifiedRNF::l1Hit(const Entry &e, Acc t) const
{
    switch (t) {
      case Acc::Load:  return e.l1d != L1d::None;
      case Acc::Store: return e.l1d == L1d::Uc || e.l1d == L1d::Ud;
      case Acc::Fetch: return e.l1i;
    }
    return false;
}

Tick
UnifiedRNF::tryAccess(const Access &a, bool rerun)
{
    const Tick now = curTick();
    const Addr line = a.pkt->getAddr() & ~Addr(cacheLineSize - 1);
    const bool fetch = a.type == Acc::Fetch;
    std::deque<Access> &stallQ = fetch ? l1iStall : l1dStall;
    Pool &l1Tbe = fetch ? l1iTbe : l1dTbe;

    // Ruby's mandatory queue is FIFO: once one access waits for an L1 TBE,
    // later ones (hits too) wait behind it.
    if (!rerun && !stallQ.empty()) {
        stallQ.push_back(a);
        return MaxTick;
    }

    Entry *e = find(line);
    if (e && busy(*e)) {                 // secondary: merge with the txn
        e->waiters.push_back(a);
        ++rnfStats.secondary;
        ++perSecondary;
        return MaxTick;
    }

    const bool hit = e && l1Hit(*e, a.type);

    // gem5's CHI L1 allocates a TBE for every access, hits included
    if ((!hit || hitsUseTbe) && l1Tbe.full(now)) {
        if (rerun)
            stallQ.push_front(a);
        else
            stallQ.push_back(a);
        ++rnfStats.l1TbeStalls;
        scheduleStallRetry(l1Tbe);
        return MaxTick;
    }

    if (hit) {
        Tick done;
        if (fetch) {
            l1i.touch(e->l1iWay);
            ++rnfStats.l1iHits;
            done = clockEdge(l1iHitLat);
        } else {
            l1d.touch(e->l1dWay);
            if (a.type == Acc::Store) {
                e->l1d = L1d::Ud;
                e->chi = Chi::UD;
            }
            ++rnfStats.l1dHits;
            done = clockEdge(l1dHitLat);
        }
        if (hitsUseTbe)
            l1Tbe.holdUntil(now, done);
        return done;
    }

    if (e && e->chi != Chi::I &&
        (a.type != Acc::Store || unique(e->chi))) {
        // served inside the RN-F: L2 hit or L1D upgrade from L2
        touchL2(*e);
        const bool upgrade = a.type == Acc::Store && e->l1d != L1d::None;
        if (upgrade) {
            ++rnfStats.l2Upgrades;
            e->l1d = L1d::Ud;
        } else {
            ++rnfStats.l2Hits;
        }
        if (a.type == Acc::Store)
            e->chi = Chi::UD;
        Tick done;
        if (linksOn) {
            const Tick at_l2 = upPath(fetch, now + cyclesToTicks(l1SendLat),
                                      false);
            done = upgrade
                ? downPath(fetch, at_l2 + cyclesToTicks(l2UpgradeProc), false)
                : downPath(fetch, at_l2 + cyclesToTicks(l2HitProc), true);
        } else {
            done = clockEdge(upgrade ? l2UpgradeLat : l2HitLat);
        }
        if (upgrade) {
            if (linksOn)
                upPath(fetch, done, false);      // CompAck
        } else {
            fillL1At(line, a.type, done);
        }
        l1Tbe.holdUntil(now, done);
        return done;
    }

    // needs the HN-F: this access becomes the primary of a transaction
    if (!e) {
        e = &table[line];
        e->addr = line;
    }
    l1Tbe.hold(now);
    e->holdsL1 = fetch ? 2 : 1;
    e->waiters.push_back(a);
    if (reqTbe.full(now) || !mshrWait.empty()) {
        e->txn = Txn::WaitMshr;
        mshrWait.push_back(line);
        ++rnfStats.mshrStalls;
        return MaxTick;
    }
    startRead(*e);
    return MaxTick;
}

void
UnifiedRNF::fillL1At(Addr line, Acc t, Tick when)
{
    // As Ruby's L1: the victim is chosen and the line inserted when the
    // data reaches the L1 (CheckCacheFill), after whatever the core did to
    // the set meanwhile, not when the L1 asked for it.
    at(when, [this, line, t] {
        Entry *e = find(line);
        if (!e || e->chi == Chi::I)      // snooped away / evicted from L2
            return;
        const int victim = fillL1(*e, t);
        if (t == Acc::Store && unique(e->chi))
            e->l1d = L1d::Ud;
        if (linksOn)
            l1AfterFill(t == Acc::Fetch, curTick(), victim);
    });
}

int
UnifiedRNF::fillL1(Entry &e, Acc t)
{
    int kind = 0;
    if (t == Acc::Fetch) {
        if (e.l1iWay >= 0) {
            l1i.touch(e.l1iWay);
            return 0;
        }
        Entry *v;
        const int w = l1i.pick(e.addr, &v, nullptr);
        if (v)
            kind = evictL1(*v, true);
        l1i.place(w, &e);
        e.l1iWay = w;
        e.l1i = true;
        return kind;
    }
    if (e.l1dWay >= 0) {
        l1d.touch(e.l1dWay);
        return 0;
    }
    Entry *v;
    const int w = l1d.pick(e.addr, &v, nullptr);
    if (v)
        kind = evictL1(*v, false);
    l1d.place(w, &e);
    e.l1dWay = w;
    // As Ruby's L2 (fwd_unique_on_readshared is off): an L1 ReadShared
    // never gets the line unique, so a later store costs a CleanUnique to
    // the L2. A dirty line is passed dirty (SD) unless another upstream
    // cache (the L1I) may already own it.
    e.l1d = (dirty(e.chi) && !e.l1i) ? L1d::Sd : L1d::S;   // store: fillL1At
    return kind;
}

int
UnifiedRNF::evictL1(Entry &v, bool icache)
{
    // No CHI message to the HN-F: the L2 holds the line and its dirtiness
    // (chi). Ruby's L1 sends WriteBack/WriteEvict (with data) for a unique
    // or dirty-shared copy and Evict for a clean shared one, to the L2.
    const int kind = (!icache && l1dOwns(v)) ? 2 : 1;
    if (icache) {
        l1i.clear(v.l1iWay);
        v.l1iWay = -1;
        v.l1i = false;
    } else {
        l1d.clear(v.l1dWay);
        v.l1dWay = -1;
        v.l1d = L1d::None;
    }
    // Ruby's L1 sends Evict/WriteBack to the L2, which touches its entry.
    touchL2(v);
    ++rnfStats.l1Evictions;
    return kind;
}

void
UnifiedRNF::dropFromArrays(Entry &e)
{
    if (e.l1dWay >= 0)
        l1d.clear(e.l1dWay);
    if (e.l1iWay >= 0)
        l1i.clear(e.l1iWay);
    if (e.l2Way >= 0)
        l2.clear(e.l2Way);
    e.l1dWay = e.l1iWay = e.l2Way = -1;
    e.l1d = L1d::None;
    e.l1i = false;
}

void
UnifiedRNF::insertL2(Entry &e)
{
    Entry *v;
    const int w = l2.pick(e.addr, &v,
                          [](const Entry &x) { return x.txn != Txn::None; });
    panic_if(w < 0, "%s: every L2 way of set %#x is busy", name(),
             l2.setOf(e.addr));
    if (v)
        evictL2(*v);
    l2.place(w, &e);
    e.l2Way = w;
}

void
UnifiedRNF::evictL2(Entry &v)
{
    const bool in_l1[2] = {v.l1dWay >= 0, v.l1iWay >= 0};
    const bool l1d_owns = l1dOwns(v);
    dropFromArrays(v);
    ++rnfStats.evictions;
    const Tick start = clockEdge(victimStartLat);
    Tick send = start + cyclesToTicks(v.chi == Chi::SC ? victimEvictLat
                                                       : victimWbLat);
    if (in_l1[0] || in_l1[1]) {
        // back-invalidate each L1 copy; the REQ waits for the answers
        ++rnfStats.backInvs;
        send = start;
        for (int f = 0; f < 2; ++f) {
            if (!in_l1[f])
                continue;
            const bool data = !f && l1d_owns;
            const Tick s = start + cyclesToTicks(backInvSendLat);
            Tick back;
            if (linksOn) {
                const Tick at_l1 = downPath(f, s, false);
                back = upPath(f, at_l1 + cyclesToTicks(snpL1InvLat), data);
            } else {
                back = s + cyclesToTicks(Cycles(5 + snpL1InvLat + 5 +
                                                (data ? 4 : 0)));
            }
            send = std::max(send, back + (data ? cyclesToTicks(backInvDataLat)
                                               : Tick(0)));
        }
    }
    issueEviction(v, send);
}

void
UnifiedRNF::maybeErase(Addr line)
{
    auto it = table.find(line);
    if (it == table.end())
        return;
    const Entry &e = it->second;
    if (!busy(e) && e.chi == Chi::I && e.waiters.empty() && !e.holdsL1 &&
        e.l2Way < 0 && e.l1dWay < 0 && e.l1iWay < 0 &&
        !e.side[0].pending && !e.side[1].pending &&
        e.side[0].q.empty() && e.side[1].q.empty())
        table.erase(it);
}

// ---------------------------------------------------------------------------
// Reads and upgrades: ReadShared/ReadNotSharedDirty, ReadUnique, CleanUnique
// ---------------------------------------------------------------------------

void
UnifiedRNF::startRead(Entry &e, bool from_core)
{
    const Tick now = curTick();
    reqTbe.hold(now);
    const Acc primary = e.waiters.front().type;
    ReqType t;
    if (e.chi == Chi::I) {
        e.txn = primary == Acc::Store ? Txn::RU : Txn::RS;
        t = primary == Acc::Store ? CHIRequestType_ReadUnique
            : allowSD ? CHIRequestType_ReadShared
                      : CHIRequestType_ReadNotSharedDirty;
    } else {
        assert(primary == Acc::Store && !unique(e.chi));
        e.txn = Txn::CU;
        t = CHIRequestType_CleanUnique;
    }
    e.pendReq = t;
    e.beats = 0;
    e.sepData = e.gotSepResp = false;
    e.cuLost = false;
    e.dataDone = false;
    e.retryAck = e.retryCredit = false;
    ++rnfStats.l3Reads;
    const Addr line = e.addr;
    Tick send;
    if (!linksOn) {
        send = clockEdge(missIssueLat);
    } else if (from_core) {
        const bool fetch = primary == Acc::Fetch;
        send = upPath(fetch, now + cyclesToTicks(l1SendLat), false) +
               cyclesToTicks(l2MissProc);
    } else {        // the request has been waiting at the L2 for a TBE
        send = clockEdge(l2MissProc);
    }
    at(send, [this, line, t] {
        Entry *x = find(line);
        assert(x);
        DPRINTF(UnifiedRNF, "%#x send %s\n", line,
                CHIRequestType_to_string(t));
        sendReq(line, t, true);
        outstanding.hold(curTick());
        x->sent = true;
        x->reqTick = curTick();
    });
}

void
UnifiedRNF::maybeReadDone(Entry &e)
{
    if (e.beats < unsigned(dataMsgsPerLine) || (e.sepData && !e.gotSepResp))
        return;
    outstanding.release(curTick());
    e.sent = false;
    readComplete(e);
}

void
UnifiedRNF::readComplete(Entry &e)
{
    const Tick now = curTick();
    const Addr line = e.addr;
    rnfStats.readLatency.sample((now - e.reqTick) / clockPeriod());
    e.dataDone = true;
    e.chi = e.granted;
    if (e.l2Way < 0)
        insertL2(e);
    const Access p = e.waiters.front();
    e.waiters.pop_front();
    const bool fetch = p.type == Acc::Fetch;
    // A CleanUnique the L1 already holds data for is answered with Comp
    const bool comp_only = e.txn == Txn::CU && e.l1dWay >= 0;
    if (p.type == Acc::Store) {
        panic_if(!unique(e.chi), "%s: store got %s for %#x", name(),
                 chiName(int(e.chi)), line);
        e.chi = Chi::UD;
    }
    DPRINTF(UnifiedRNF, "%#x read done: %s\n", line, chiName(int(e.chi)));
    Tick resp;
    if (linksOn) {
        resp = downPath(fetch, now + cyclesToTicks(l2FwdProc), !comp_only);
    } else {
        resp = clockEdge(respLat);
    }
    fillL1At(line, p.type, resp);     // before deliver(): same-tick order
    deliver(p, resp);
    // the L1 TBE is freed when the core gets the data
    if (e.holdsL1) {
        Pool &l1 = e.holdsL1 == 2 ? l1iTbe : l1dTbe;
        l1.release(now);
        l1.holdUntil(now, resp);
        e.holdsL1 = 0;
    }
    at(clockEdge(fillLat), [this, line] { finishRead(line); });
}

void
UnifiedRNF::finishRead(Addr line)
{
    const Tick now = curTick();
    Entry *e = find(line);
    assert(e && e->dataDone);
    reqTbe.release(now);
    if (e->holdsL1)
        (e->holdsL1 == 2 ? l1iTbe : l1dTbe).release(now);
    e->holdsL1 = 0;
    e->txn = Txn::None;
    e->dataDone = false;
    std::deque<Access> w;
    w.swap(e->waiters);
    rerun(std::move(w));
    maybeErase(line);
    startWaiters();
    retryStalled();
}

// ---------------------------------------------------------------------------
// Evictions: Evict, WriteEvictFull, WriteBackFull
// ---------------------------------------------------------------------------

void
UnifiedRNF::issueEviction(Entry &v, Tick when)
{
    if (v.chi == Chi::I)
        return;
    const Addr line = v.addr;
    if (replTbe.full(curTick()) || !replWait.empty()) {
        v.txn = Txn::WaitRepl;
        replWait.push_back(line);
        ++rnfStats.replStalls;
        return;
    }
    replTbe.hold(curTick());
    v.txn = Txn::WB;            // refined from the state when sent
    at(when, [this, line] { sendEviction(line); });
}

void
UnifiedRNF::sendEviction(Addr line)
{
    Entry *e = find(line);
    assert(e);
    if (e->chi == Chi::I) {     // a snoop took the line before we sent
        finishEviction(line);
        return;
    }
    ReqType t;
    switch (e->chi) {
      case Chi::SC: t = CHIRequestType_Evict; e->txn = Txn::EV; break;
      case Chi::UC: t = CHIRequestType_WriteEvictFull; e->txn = Txn::WE; break;
      default:      t = CHIRequestType_WriteBackFull; e->txn = Txn::WB; break;
    }
    e->pendReq = t;
    e->retryAck = e->retryCredit = false;
    DPRINTF(UnifiedRNF, "%#x send %s\n", line, CHIRequestType_to_string(t));
    sendReq(line, t, true);
    outstanding.hold(curTick());
    e->sent = true;
}

void
UnifiedRNF::finishEviction(Addr line)
{
    const Tick now = curTick();
    Entry *e = find(line);
    assert(e);
    replTbe.release(now);
    if (e->sent)
        outstanding.release(now);
    e->sent = false;
    e->chi = Chi::I;
    e->txn = Txn::None;
    std::deque<Access> w;
    w.swap(e->waiters);
    rerun(std::move(w));
    maybeErase(line);
    startWaiters();
    retryStalled();
}

// ---------------------------------------------------------------------------
// Pools: requests waiting for a TBE
// ---------------------------------------------------------------------------

void
UnifiedRNF::startWaiters()
{
    const Tick now = curTick();
    while (!mshrWait.empty() && !reqTbe.full(now)) {
        const Addr line = mshrWait.front();
        mshrWait.pop_front();
        Entry *e = find(line);
        if (e && e->txn == Txn::WaitMshr)
            startRead(*e, false);
    }
    while (!replWait.empty() && !replTbe.full(now)) {
        const Addr line = replWait.front();
        replWait.pop_front();
        Entry *e = find(line);
        if (!e || e->txn != Txn::WaitRepl)
            continue;
        if (e->chi == Chi::I) {           // snooped away meanwhile
            e->txn = Txn::None;
            std::deque<Access> w;
            w.swap(e->waiters);
            rerun(std::move(w));
            maybeErase(line);
            continue;
        }
        replTbe.hold(now);
        e->txn = Txn::WB;
        at(clockEdge(e->chi == Chi::SC ? victimEvictLat : victimWbLat),
           [this, line] { sendEviction(line); });
    }
}

void
UnifiedRNF::retryStalled()
{
    const Tick now = curTick();
    for (int k = 0; k < 2; ++k) {
        std::deque<Access> &q = k ? l1iStall : l1dStall;
        Pool &p = k ? l1iTbe : l1dTbe;
        while (!q.empty() && !p.full(now)) {
            const Access a = q.front();
            q.pop_front();
            const Tick done = tryAccess(a, true);
            if (done != MaxTick)
                deliver(a, done);
        }
        if (!q.empty())
            scheduleStallRetry(p);
    }
}

void
UnifiedRNF::scheduleStallRetry(Pool &p)
{
    const Tick t = p.nextRelease();
    if (t != MaxTick)
        at(std::max(t, curTick()), [this] { retryStalled(); });
    // otherwise a held TBE frees in finishRead, which retries
}

// ---------------------------------------------------------------------------
// Incoming messages
// ---------------------------------------------------------------------------

bool
UnifiedRNF::recvRequestMsg(const CHIRequestMsg *msg)
{
    panic("%s: an RN-F does not receive requests (%s)", name(),
          CHIRequestType_to_string(msg->gettype()));
}

bool
UnifiedRNF::recvDataMsg(const CHIDataMsg *msg)
{
    ++rnfStats.datIn[msg->gettype()];
    ++rnfStats.flitsIn[CHI_DAT];
    ++perFlits[1][CHI_DAT];
    const Tick t = inboundAt(CHI_DAT, true);
    if (t > curTick()) {
        auto m = std::static_pointer_cast<CHIDataMsg>(msg->clone());
        at(t, [this, m] { handleData(m.get()); });
        return true;
    }
    return handleData(msg);
}

bool
UnifiedRNF::handleData(const CHIDataMsg *msg)
{
    const Addr line = msg->getaddr();
    Entry *e = find(line);
    panic_if(!e || !(e->txn == Txn::RS || e->txn == Txn::RU) || e->dataDone,
             "%s: unexpected %s for %#x", name(),
             CHIDataType_to_string(msg->gettype()), line);
    switch (msg->gettype()) {
      case CHIDataType_CompData_UC:    e->granted = Chi::UC; break;
      case CHIDataType_CompData_SC:    e->granted = Chi::SC; break;
      case CHIDataType_CompData_UD_PD: e->granted = Chi::UD; break;
      case CHIDataType_CompData_SD_PD: e->granted = Chi::SD; break;
      case CHIDataType_DataSepResp_UC:
        e->granted = Chi::UC;
        e->sepData = true;
        break;
      default:
        panic("%s: unexpected %s", name(),
              CHIDataType_to_string(msg->gettype()));
    }
    if (++e->beats == 1)    // Ruby's L2 acks on the first beat
        at(clockEdge(ackLat), [this, line] { sendCompAck(line); });
    maybeReadDone(*e);
    return true;
}

void
UnifiedRNF::maybeResend(Entry &e)
{
    if (!(e.retryAck && e.retryCredit))
        return;
    e.retryAck = e.retryCredit = false;
    DPRINTF(UnifiedRNF, "%#x resend %s\n", e.addr,
            CHIRequestType_to_string(e.pendReq));
    sendReq(e.addr, e.pendReq, false);
}

bool
UnifiedRNF::recvResponseMsg(const CHIResponseMsg *msg)
{
    ++rnfStats.rspIn[msg->gettype()];
    ++rnfStats.flitsIn[CHI_RSP];
    ++perFlits[1][CHI_RSP];
    const Tick t = inboundAt(CHI_RSP, false);
    if (t > curTick()) {
        auto m = std::static_pointer_cast<CHIResponseMsg>(msg->clone());
        at(t, [this, m] { handleResponse(m.get()); });
        return true;
    }
    return handleResponse(msg);
}

bool
UnifiedRNF::handleResponse(const CHIResponseMsg *msg)
{
    const Tick now = curTick();
    const Addr line = msg->getaddr();
    Entry *e = find(line);
    panic_if(!e, "%s: %s for unknown line %#x", name(),
             CHIResponseType_to_string(msg->gettype()), line);

    switch (msg->gettype()) {
      case CHIResponseType_Comp_UC:
        panic_if(e->txn != Txn::CU, "%s: Comp_UC without CleanUnique",
                 name());
        outstanding.release(now);
        e->sent = false;
        if (e->cuLost) {
            // A snoop invalidated the line while the CleanUnique was out:
            // ack it and fetch the data with a ReadUnique (Ruby's L2 does
            // the same instead of using the UCE state).
            ++rnfStats.cuLost;
            sendCompAck(line);
            e->txn = Txn::RU;
            e->pendReq = CHIRequestType_ReadUnique;
            e->beats = 0;
            e->cuLost = false;
            at(clockEdge(ackLat), [this, line] {
                Entry *x = find(line);
                sendReq(line, CHIRequestType_ReadUnique, true);
                outstanding.hold(curTick());
                x->sent = true;
                x->reqTick = curTick();
            });
        } else {
            at(clockEdge(ackLat), [this, line] { sendCompAck(line); });
            e->granted = Chi::UC;
            readComplete(*e);
        }
        break;

      case CHIResponseType_RespSepData:
        e->gotSepResp = true;
        maybeReadDone(*e);
        break;

      case CHIResponseType_Comp_I:
        panic_if(e->txn != Txn::EV, "%s: Comp_I without Evict", name());
        finishEviction(line);
        break;

      case CHIResponseType_CompDBIDResp: {
        panic_if(e->txn != Txn::WB && e->txn != Txn::WE,
                 "%s: CompDBIDResp without a write", name());
        const MachineID dest = msg->getresponder();
        at(clockEdge(wbDataLat), [this, line, dest] {
            Entry *x = find(line);
            // the state at send time: a snoop may have changed it
            DatType t;
            switch (x->chi) {
              case Chi::UD: t = CHIDataType_CBWrData_UD_PD; break;
              case Chi::SD: t = CHIDataType_CBWrData_SD_PD; break;
              case Chi::UC: t = CHIDataType_CBWrData_UC; break;
              case Chi::SC: t = CHIDataType_CBWrData_SC; break;
              default:      t = CHIDataType_CBWrData_I; break;
            }
            sendData(line, t, dest);
            finishEviction(line);
        });
        break;
      }

      case CHIResponseType_RetryAck:
        ++rnfStats.retryAcks;
        e->retryAck = true;
        maybeResend(*e);
        break;

      case CHIResponseType_PCrdGrant:
        e->retryCredit = true;
        maybeResend(*e);
        break;

      default:
        panic("%s: unexpected %s", name(),
              CHIResponseType_to_string(msg->gettype()));
    }
    return true;
}

bool
UnifiedRNF::recvSnoopMsg(const CHIRequestMsg *msg)
{
    const Tick now = curTick();
    if (snpTbe.full(now)) {
        ++rnfStats.snpStalls;
        return false;           // stays in snpIn; the base retries
    }
    const ReqType t = msg->gettype();
    ++rnfStats.snpIn[t];
    ++rnfStats.flitsIn[CHI_SNP];
    ++perFlits[1][CHI_SNP];
    const Tick seen = inboundAt(CHI_SNP, false);

    const bool inv = t == CHIRequestType_SnpUnique ||
                     t == CHIRequestType_SnpCleanInvalid;
    const bool shr = t == CHIRequestType_SnpShared;
    const bool once = t == CHIRequestType_SnpOnce;
    panic_if(!(inv || shr || once),
             "%s: %s not modelled (DCT and DVM are off)", name(),
             CHIRequestType_to_string(t));

    const Addr line = msg->getaddr();
    Entry *e = find(line);
    // A read not yet answered holds no data: its line is I.
    const Chi st = e ? e->chi : Chi::I;
    const bool ret = msg->getretToSrc();

    bool data = false, via_l1 = false;
    // which L1s the snoop must visit, and whether the L1D answers with data
    // (it owns the line: unique, or shared dirty); the L1I only ever
    // answers SnpResp_I
    bool to_l1d = false, to_l1i = false;
    const bool l1d_owns = e && l1dOwns(*e);
    RspType rt = CHIResponseType_SnpResp_I;
    DatType dt = CHIDataType_SnpRespData_I;
    if (st == Chi::I) {
        // SnpResp_I
    } else if (inv) {
        data = dirty(st) || ret ||
               (unique(st) && t == CHIRequestType_SnpUnique);
        dt = dirty(st) ? CHIDataType_SnpRespData_I_PD
                       : CHIDataType_SnpRespData_I;
        to_l1d = e->l1dWay >= 0;
        to_l1i = e->l1iWay >= 0;
    } else if (shr) {
        data = dirty(st) || unique(st) || ret;
        dt = dirty(st) ? CHIDataType_SnpRespData_SC_PD
                       : CHIDataType_SnpRespData_SC;
        rt = CHIResponseType_SnpResp_SC;
        to_l1d = l1d_owns;          // Ruby: only if an upstream owner exists
    } else {
        data = true;
        dt = st == Chi::UD ? CHIDataType_SnpRespData_UD
           : st == Chi::SD ? CHIDataType_SnpRespData_SD
           : st == Chi::UC ? CHIDataType_SnpRespData_UC
                           : CHIDataType_SnpRespData_SC;
        to_l1d = e->l1d == L1d::Ud;
    }
    via_l1 = to_l1d || to_l1i;

    // State changes at once; the L1 round trip only delays the response.
    if (e && st != Chi::I) {
        if (inv) {
            e->chi = Chi::I;
            dropFromArrays(*e);
            if (e->txn == Txn::CU)
                e->cuLost = true;
        } else {
            if (shr) {
                e->chi = Chi::SC;
                if (l1dOwns(*e))
                    e->l1d = L1d::S;
            }
            touchL2(*e);    // Ruby's snoop TBE sets the line MRU
        }
    }
    DPRINTF(UnifiedRNF, "%#x %s in %s -> %s\n", line,
            CHIRequestType_to_string(t), chiName(int(st)),
            data ? CHIDataType_to_string(dt) : CHIResponseType_to_string(rt));

    if (via_l1)
        ++rnfStats.snoopsViaL1;
    // L2 pipeline, then the round trip to each L1 that must see it
    const Tick piped = edgeAtOrAfter(seen) +
                       cyclesToTicks(inv ? snpInvLat : snpLat);
    Tick when = piped;
    const Cycles l1_proc = inv ? snpL1InvLat : snpL1Lat;
    for (int f = 0; f < 2; ++f) {
        if (!(f ? to_l1i : to_l1d))
            continue;
        const bool l1_data = !f && l1d_owns;
        Tick back;
        if (linksOn) {
            const Tick at_l1 = downPath(f, piped, false);
            back = upPath(f, at_l1 + cyclesToTicks(l1_proc), l1_data);
        } else {
            back = piped + cyclesToTicks(Cycles(5 + l1_proc + 5 +
                                                (l1_data ? 4 : 0)));
        }
        when = std::max(when, back + cyclesToTicks(snpRespLat));
    }
    snpTbe.holdUntil(now, when);
    rnfStats.snoopLatency.sample((when - now) / clockPeriod());
    const MachineID dest = msg->getrequestor();
    at(when, [this, line, data, dt, rt, dest] {
        if (data)
            sendData(line, dt, dest);
        else
            sendRsp(line, rt, dest);
    });
    if (e)
        maybeErase(line);
    return true;
}

// ---------------------------------------------------------------------------
// Outgoing messages
// ---------------------------------------------------------------------------

void
UnifiedRNF::sendReq(Addr line, ReqType t, bool allow_retry)
{
    auto msg = std::make_shared<CHIRequestMsg>(curTick(), cacheLineSize,
                                               m_ruby_system);
    msg->setaddr(line);
    msg->setaccAddr(line);
    msg->setaccSize(cacheLineSize);
    msg->settype(t);
    msg->setrequestor(m_machineID);
    msg->setfwdRequestor(m_machineID);
    msg->setdataToFwdRequestor(false);
    msg->setretToSrc(false);
    msg->setallowRetry(allow_retry);
    msg->setisSeqReqValid(false);
    msg->getDestination().add(mapAddressToDownstreamMachine(line));
    ++rnfStats.reqOut[t];
    sendOut(CHI_REQ, msg);
}

void
UnifiedRNF::sendRsp(Addr line, RspType t, MachineID dest)
{
    auto msg = std::make_shared<CHIResponseMsg>(curTick(), cacheLineSize,
                                                m_ruby_system);
    msg->setaddr(line);
    msg->settype(t);
    msg->setresponder(m_machineID);
    msg->getDestination().add(dest);
    ++rnfStats.rspOut[t];
    sendOut(CHI_RSP, msg);
}

void
UnifiedRNF::sendCompAck(Addr line)
{
    sendRsp(line, CHIResponseType_CompAck,
            mapAddressToDownstreamMachine(line));
}

void
UnifiedRNF::sendData(Addr line, DatType t, MachineID dest)
{
    ++rnfStats.datOut[t];
    for (int b = 0; b < dataMsgsPerLine; ++b) {
        auto msg = std::make_shared<CHIDataMsg>(curTick(), cacheLineSize,
                                                m_ruby_system);
        msg->setaddr(line);
        msg->settype(t);
        msg->setresponder(m_machineID);
        msg->getDestination().add(dest);
        msg->getbitMask().setMask(b * dataChannelSize, dataChannelSize);
        sendOut(CHI_DAT, msg);
    }
}

bool
UnifiedRNF::trySend(int ch, const std::shared_ptr<Message> &m)
{
    switch (ch) {
      case CHI_REQ:
        return sendRequestMsg(std::static_pointer_cast<CHIRequestMsg>(m));
      case CHI_RSP:
        return sendResponseMsg(std::static_pointer_cast<CHIResponseMsg>(m));
      case CHI_DAT:
        return sendDataMsg(std::static_pointer_cast<CHIDataMsg>(m));
      default:
        panic("%s: RN-F sends nothing on channel %d", name(), ch);
    }
}

void
UnifiedRNF::sendOut(int ch, std::shared_ptr<Message> msg)
{
    ++rnfStats.flitsOut[ch];
    ++perFlits[0][ch];
    if (blocked[ch].empty() && trySend(ch, msg))
        return;
    blocked[ch].push_back(std::move(msg));
    ++rnfStats.blockedSends;
    if (!retryScheduled) {
        retryScheduled = true;
        at(clockEdge(Cycles(1)), [this] { retryBlocked(); });
    }
}

void
UnifiedRNF::retryBlocked()
{
    retryScheduled = false;
    bool left = false;
    for (int ch = 0; ch < 4; ++ch) {
        while (!blocked[ch].empty() && trySend(ch, blocked[ch].front()))
            blocked[ch].pop_front();
        left = left || !blocked[ch].empty();
    }
    if (left) {
        retryScheduled = true;
        at(clockEdge(Cycles(1)), [this] { retryBlocked(); });
    }
}

// ---------------------------------------------------------------------------
// Deferred work
// ---------------------------------------------------------------------------

void
UnifiedRNF::at(Tick when, std::function<void()> fn)
{
    assert(when >= curTick());
    timers.push({when, timerSeq++, std::move(fn)});
    if (!timerEvent.scheduled())
        schedule(timerEvent, when);
    else if (timerEvent.when() > when)
        reschedule(timerEvent, when);
}

void
UnifiedRNF::runTimers()
{
    while (!timers.empty() && timers.top().when <= curTick()) {
        std::function<void()> fn =
            std::move(const_cast<Timed &>(timers.top()).fn);
        timers.pop();
        fn();
    }
    if (!timers.empty() && !timerEvent.scheduled())
        schedule(timerEvent, timers.top().when);
}

// ---------------------------------------------------------------------------
// Occupancy: statistics and sampler
// ---------------------------------------------------------------------------

void
UnifiedRNF::sample()
{
    const Tick now = curTick();
    auto &os = *sampleOut->stream();
    os << now << ',' << l1dTbe.periodAvg(now) << ','
       << l1iTbe.periodAvg(now) << ',' << reqTbe.periodAvg(now) << ','
       << replTbe.periodAvg(now) << ',' << snpTbe.periodAvg(now) << ','
       << outstanding.periodAvg(now) << ',' << perSecondary << ','
       << perFlits[0][CHI_REQ] << ',' << perFlits[0][CHI_RSP] << ','
       << perFlits[0][CHI_DAT] << ',' << perFlits[1][CHI_SNP] << ','
       << perFlits[1][CHI_RSP] << ',' << perFlits[1][CHI_DAT] << '\n';
    perSecondary = 0;
    std::fill(&perFlits[0][0], &perFlits[0][0] + 8, 0);
    schedule(sampleEvent, clockEdge(samplePeriod));
}

void
UnifiedRNF::collateStats()
{
    const Tick now = curTick();
    struct Out
    {
        Pool &pool;
        statistics::Vector &hist;
        statistics::Scalar &avg;
    };
    Out outs[] = {
        {l1dTbe, rnfStats.occL1d, rnfStats.avgL1d},
        {l1iTbe, rnfStats.occL1i, rnfStats.avgL1i},
        {reqTbe, rnfStats.occReq, rnfStats.avgReq},
        {replTbe, rnfStats.occRepl, rnfStats.avgRepl},
        {snpTbe, rnfStats.occSnp, rnfStats.avgSnp},
    };
    for (Out &o : outs) {
        o.pool.advance(now);
        double total = 0;
        for (size_t i = 0; i < o.pool.occTicks.size(); ++i) {
            o.hist[i] = o.pool.occTicks[i] / clockPeriod();
            total += o.pool.occTicks[i];
        }
        o.avg = total ? o.pool.area / total : 0;
    }
    for (int i = 0; i < NumLinks; ++i) {
        rnfStats.linkMsgs[i] = links[i].msgs;
        rnfStats.linkBusy[i] = links[i].busy / clockPeriod();
        rnfStats.linkWait[i] = links[i].wait / clockPeriod();
    }
    outstanding.advance(now);
    double total = 0;
    for (double t : outstanding.occTicks)
        total += t;
    rnfStats.avgOutstanding = total ? outstanding.area / total : 0;
}

void
UnifiedRNF::resetStats()
{
    CHIGenericController::resetStats();
    const Tick now = curTick();
    for (Pool *p : {&l1dTbe, &l1iTbe, &reqTbe, &replTbe, &snpTbe,
                    &outstanding})
        p->resetStats(now);
}

UnifiedRNF::RnfStats::RnfStats(statistics::Group *parent)
    : statistics::Group(parent, "unified"),
      ADD_STAT(l1dHits, statistics::units::Count::get(), "L1D hits"),
      ADD_STAT(l1iHits, statistics::units::Count::get(), "L1I hits"),
      ADD_STAT(l2Hits, statistics::units::Count::get(),
               "L1 misses served by the L2"),
      ADD_STAT(l2Upgrades, statistics::units::Count::get(),
               "Stores upgraded from L1D shared to L2 unique"),
      ADD_STAT(l3Reads, statistics::units::Count::get(),
               "Read/upgrade transactions sent to the HN-F"),
      ADD_STAT(secondary, statistics::units::Count::get(),
               "Accesses waiting on a transaction of the other side"),
      ADD_STAT(coalesced, statistics::units::Count::get(),
               "Accesses queued behind one to the same line (sequencer)"),
      ADD_STAT(seqFull, statistics::units::Count::get(),
               "Accesses refused: max_outstanding_requests reached"),
      ADD_STAT(l1TbeStalls, statistics::units::Count::get(),
               "Accesses that waited for an L1 TBE"),
      ADD_STAT(mshrStalls, statistics::units::Count::get(),
               "Transactions that waited for a request TBE"),
      ADD_STAT(replStalls, statistics::units::Count::get(),
               "Evictions that waited for a replacement TBE"),
      ADD_STAT(snpStalls, statistics::units::Count::get(),
               "Cycles a snoop waited for a snoop TBE"),
      ADD_STAT(retryAcks, statistics::units::Count::get(),
               "RetryAcks received"),
      ADD_STAT(cuLost, statistics::units::Count::get(),
               "CleanUniques overtaken by an invalidating snoop"),
      ADD_STAT(evictions, statistics::units::Count::get(), "L2 evictions"),
      ADD_STAT(l1Evictions, statistics::units::Count::get(),
               "L1 victims (each sends Evict or WriteBack/WriteEvict to the L2)"),
      ADD_STAT(backInvs, statistics::units::Count::get(),
               "L2 victims back-invalidated from an L1 first"),
      ADD_STAT(snoopsViaL1, statistics::units::Count::get(),
               "Snoops that had to reach an L1 copy"),
      ADD_STAT(blockedSends, statistics::units::Count::get(),
               "Messages that found their output buffer full"),
      ADD_STAT(reqOut, statistics::units::Count::get(), "REQ sent"),
      ADD_STAT(rspOut, statistics::units::Count::get(), "RSP sent"),
      ADD_STAT(datOut, statistics::units::Count::get(),
               "DAT sent (per line)"),
      ADD_STAT(snpIn, statistics::units::Count::get(), "SNP received"),
      ADD_STAT(rspIn, statistics::units::Count::get(), "RSP received"),
      ADD_STAT(datIn, statistics::units::Count::get(),
               "DAT received (per flit)"),
      ADD_STAT(flitsOut, statistics::units::Count::get(),
               "Flits sent per channel"),
      ADD_STAT(flitsIn, statistics::units::Count::get(),
               "Flits received per channel"),
      ADD_STAT(occL1d, statistics::units::Cycle::get(),
               "Cycles at each L1D TBE occupancy"),
      ADD_STAT(occL1i, statistics::units::Cycle::get(),
               "Cycles at each L1I TBE occupancy"),
      ADD_STAT(occReq, statistics::units::Cycle::get(),
               "Cycles at each request TBE (MSHR) occupancy"),
      ADD_STAT(occRepl, statistics::units::Cycle::get(),
               "Cycles at each replacement TBE occupancy"),
      ADD_STAT(occSnp, statistics::units::Cycle::get(),
               "Cycles at each snoop TBE occupancy"),
      ADD_STAT(avgL1d, statistics::units::Count::get(),
               "Mean L1D TBE occupancy"),
      ADD_STAT(avgL1i, statistics::units::Count::get(),
               "Mean L1I TBE occupancy"),
      ADD_STAT(avgReq, statistics::units::Count::get(),
               "Mean request TBE (MSHR) occupancy"),
      ADD_STAT(avgRepl, statistics::units::Count::get(),
               "Mean replacement TBE occupancy"),
      ADD_STAT(avgSnp, statistics::units::Count::get(),
               "Mean snoop TBE occupancy"),
      ADD_STAT(avgOutstanding, statistics::units::Count::get(),
               "Mean REQ transactions outstanding at the HN-F"),
      ADD_STAT(readLatency, statistics::units::Cycle::get(),
               "Read/upgrade REQ sent -> data/Comp received"),
      ADD_STAT(snoopLatency, statistics::units::Cycle::get(),
               "Snoop received -> response sent"),
      ADD_STAT(linkMsgs, statistics::units::Count::get(),
               "Messages per modelled L1<->L2 link"),
      ADD_STAT(linkBusy, statistics::units::Cycle::get(),
               "Busy cycles per modelled L1<->L2 link"),
      ADD_STAT(linkWait, statistics::units::Cycle::get(),
               "Cycles messages waited for each link"),
      ADD_STAT(linkAvgWait, statistics::units::Cycle::get(),
               "Mean wait per message per link")
{
    reqOut.init(CHIRequestType_NUM);
    rspOut.init(CHIResponseType_NUM);
    datOut.init(CHIDataType_NUM);
    snpIn.init(CHIRequestType_NUM);
    rspIn.init(CHIResponseType_NUM);
    datIn.init(CHIDataType_NUM);
    flitsOut.init(4);
    flitsIn.init(4);
    linkMsgs.init(NumLinks);
    linkBusy.init(NumLinks);
    linkWait.init(NumLinks);
    const char *ln[] = {"dUp", "dDown", "iUp", "iDown", "l2In"};
    for (int i = 0; i < NumLinks; ++i) {
        linkMsgs.subname(i, ln[i]);
        linkBusy.subname(i, ln[i]);
        linkWait.subname(i, ln[i]);
    }
    linkAvgWait = linkWait / linkMsgs;
    readLatency.init(32);
    snoopLatency.init(16);
    for (statistics::Vector *v : {&reqOut, &rspOut, &datOut, &snpIn,
                                  &rspIn, &datIn})
        v->flags(statistics::nozero);
}

} // namespace ruby
} // namespace gem5
