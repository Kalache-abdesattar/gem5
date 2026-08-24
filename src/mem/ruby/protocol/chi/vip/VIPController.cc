/*
 * VIPController.cc — gem5 co-simulation bridge implementation.
 *
 * Data flows:
 *
 *   DUT --[shm q2g_req]--> makeReqMsg() --> reqOut (Ruby network)
 *   DUT --[shm q2g_rsp]--> makeRspMsg() --> rspOut
 *   DUT --[shm q2g_dat]--> makeDatMsg() --> datOut
 *
 *   rspIn (Ruby network) --> recvResponseMsg() --> packRsp() --> shm g2q_rsp --> DUT
 *   datIn                --> recvDataMsg()     --> packDat() --> shm g2q_dat --> DUT
 *   snpIn                --> recvSnoopMsg()    --> packSnp() --> shm g2q_snp --> DUT
 */

#include "mem/ruby/protocol/chi/vip/VIPController.hh"

#include <cstddef>
#include <cstring>
#include <unistd.h>   // usleep — time-quantum barrier spin

#include "base/logging.hh"
#include "sim/sim_exit.hh"
#include "debug/RubyCHIGeneric.hh"
#include "mem/ruby/protocol/CHI/CHIDataMsg.hh"
#include "mem/ruby/protocol/CHI/CHIRequestMsg.hh"
#include "mem/ruby/protocol/CHI/CHIResponseMsg.hh"

// Use CHI:: types that are the SLICC-generated names
using namespace gem5::ruby::CHI;

namespace gem5 {
namespace ruby {

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / init
// ─────────────────────────────────────────────────────────────────────────────

VIPController::VIPController(const Params& p)
    : CHIGenericController(p)
    // rnf_index==0 creates the segment (shm_unlink + placement-new).
    // Higher indices open the already-created segment immediately.
    , shm_(p.shm_name, /*create=*/(p.rnf_index == 0))
    , cacheLineSz_(p.ruby_system->getBlockSizeBytes())
    , rnf_index_(p.rnf_index)
    , rtl_src_id_(static_cast<uint16_t>(p.rtl_src_id))
    , rtl_hnf_nid_(static_cast<uint16_t>(p.rtl_hnf_nid))
    , quantum_ps_(static_cast<uint64_t>(p.quantum_ps))
{
    if (rnf_index_ > 0) {
        // Segment was created by VIPController-0 just before us; open it now.
        if (!shm_.tryOpen())
            fatal("VIPController[%d]: could not open shm '%s' created by index 0\n",
                  rnf_index_, p.shm_name.c_str());
    }
}

void
VIPController::init()
{
    CHIGenericController::init();

    // Publish this RN-F's RTL NodeID into the shm header so questa_top_ipc
    // can route incoming flits by src_id to the correct q2g slot.
    auto* shm = shm_.layout();
    if (shm) {
        shm->rnf_nid[rnf_index_] = rtl_src_id_;
        // Atomically advance num_rnf to at least rnf_index_+1.
        uint32_t expected = shm->num_rnf.load(std::memory_order_relaxed);
        uint32_t desired  = static_cast<uint32_t>(rnf_index_ + 1);
        while (expected < desired &&
               !shm->num_rnf.compare_exchange_weak(expected, desired,
                   std::memory_order_release, std::memory_order_relaxed))
            {}
        fprintf(stderr, "[VIP:%d] init: rtl_src_id=%u rtl_hnf_nid_=%u num_rnf=%u\n",
                rnf_index_, rtl_src_id_, rtl_hnf_nid_,
                shm->num_rnf.load(std::memory_order_relaxed));
        // One-time layout probe: if these numbers disagree between gem5 and
        // the Questa-side print, the shm marshalling is corrupt (byte offsets
        // of src_id/tgt_id/home_n_id land in different places on the two
        // sides).
        if (rnf_index_ == 0) {
            // VIPController-0 owns the quantum_ps field; publish it once so the
            // Questa side can read it at attach.  Other VIPs must not overwrite.
            shm->quantum_ps.store(quantum_ps_, std::memory_order_relaxed);
            fprintf(stderr, "[VIP:0] quantum_ps=%lu\n",
                    (unsigned long)quantum_ps_);
            fprintf(stderr,
                "[VIP:layout] sizeof(ChiIpcDat)=%zu"
                " off(src_id)=%zu off(tgt_id)=%zu off(home_n_id)=%zu\n",
                sizeof(chi_ipc::ChiIpcDat),
                offsetof(chi_ipc::ChiIpcDat, src_id),
                offsetof(chi_ipc::ChiIpcDat, tgt_id),
                offsetof(chi_ipc::ChiIpcDat, home_n_id));
        }
    }

    scheduleEvent(Cycles(1));
}

// ─────────────────────────────────────────────────────────────────────────────
// wakeup — called every tick while there is work to do
// ─────────────────────────────────────────────────────────────────────────────

void
VIPController::wakeup()
{
    if (!shm_.valid()) {
        scheduleEvent(Cycles(1));
        return;
    }
    auto* shm = shm_.layout();

    bool pending = false;
    Tick ct = curTick();
    Tick lat1 = cyclesToTicks(Cycles(1));  // minimum non-zero latency

    auto& my_q2g_rsp = shm->q2g_rsp[rnf_index_];
    auto& my_q2g_req = shm->q2g_req[rnf_index_];
    auto& my_q2g_dat = shm->q2g_dat[rnf_index_];

    // Drain q2g_rsp FIRST → rspOut
    // Must precede q2g_req: CompAck uses txnid_to_addr_ to recover the HNF
    // TBE address.  If q2g_req is drained first, a new request with the same
    // txnid overwrites txnid_to_addr_ before the CompAck can be routed to the
    // correct TBE, causing an Invalid Transition panic in the HNF.
    {
        chi_ipc::ChiIpcRsp ipc;
        while (my_q2g_rsp.pop(ipc)) {
            auto msg = makeRspMsg(ipc);
            if (msg && rspOut->areNSlotsAvailable(1, ct))
                rspOut->enqueue(msg, ct, lat1, false, false);
            pending = true;
        }
    }

    // Drain q2g_req → pendingReqs_ (never drop; always buffer)
    {
        chi_ipc::ChiIpcReq ipc;
        while (my_q2g_req.pop(ipc)) {
            if (ipc.opcode == 0) continue;  // ReqLCrdReturn, not a transaction
            // Detect the DUT touching one of the two termination cache
            // lines: 0x80040000 (tohost = PASS) or 0x80040040 (fromhost =
            // FAIL).  A plain `sd` to either line produces a ReadUnique on
            // the CHI wire before the store commits.  Using two disjoint
            // addresses means the wire access itself carries the PASS/FAIL
            // outcome — no need to wait for the value to reach gem5 via
            // WriteBackFull (which would only happen after eviction, but
            // the CPU immediately goes to `wfi` and nothing forces it).
            const uint64_t line_addr = static_cast<uint64_t>(ipc.addr) & ~0x3FULL;
            const bool is_pass = (line_addr == 0x80040000ULL);
            const bool is_fail = (line_addr == 0x80040040ULL);
            if (is_pass || is_fail) {
                fprintf(stderr,
                    "[VIP:%d] tohost %s request (op=0x%02x txn=0x%03x"
                    " addr=0x%lx) — signalling termination\n",
                    rnf_index_, is_pass ? "PASS" : "FAIL",
                    (unsigned)ipc.opcode,
                    (unsigned)ipc.txn_id, (unsigned long)ipc.addr);
                auto* shm = shm_.layout();
                if (shm) {
                    shm->sim_done.store(is_pass ? 1ULL
                                                : 0xFA11000000000001ULL,
                                        std::memory_order_release);
                }
                exitSimLoop(is_pass ? "tohost: PASS (request seen)"
                                    : "tohost: FAIL (request seen)",
                            is_pass ? 0 : 1);
                // Fall through — still enqueue the request so gem5 can
                // service the ReadUnique and CVA6 doesn't hang mid-commit
                // while gem5 tears down.
            }
            pendingReqs_.push(makeReqMsg(ipc));
            pending = true;
        }
    }
    // Flush pendingReqs_ → reqOut up to available slots
    while (!pendingReqs_.empty() && reqOut->areNSlotsAvailable(1, ct)) {
        reqOut->enqueue(pendingReqs_.front(), ct, lat1, false, false);
        pendingReqs_.pop();
        pending = true;
    }
    if (!pendingReqs_.empty()) pending = true;  // wake up again to drain remainder

    // Drain q2g_dat → datOut.
    // Each IPC DAT carries a full cache line (CHI_DATA_W_BYTES = dataChannelSize
    // when both sides use the same channel width). dataMsgsPerLine gives the
    // number of CHIDataMsgs the HN-F expects per cache line; split accordingly.
    {
        chi_ipc::ChiIpcDat ipc;
        while (my_q2g_dat.pop(ipc)) {
            // Check for tohost write: CVA6 test writes 1=PASS or FAIL code
            // to 0x80040000 to signal test completion.
            {
                auto it = txnid_to_addr_.find(ipc.txn_id);
                Addr waddr = (it != txnid_to_addr_.end())
                             ? it->second
                             : static_cast<Addr>(ipc.addr);
                if (waddr == 0x80040000ULL) {
                    uint64_t tohost_val = 0;
                    std::memcpy(&tohost_val, ipc.data, sizeof(tohost_val));
                    if (tohost_val != 0) {
                        // Publish the completion signal to shm BEFORE
                        // exitSimLoop tears down.  Questa polls sim_done in
                        // clk_posedge and calls sc_stop() so both sides
                        // terminate on the single tohost event.  sim_done
                        // lives at the tail of ChiShmLayout — see the
                        // static_assert in chi_ipc.hh.
                        auto* shm = shm_.layout();
                        if (shm) {
                            shm->sim_done.store(tohost_val,
                                std::memory_order_release);
                        }
                    }
                    if (tohost_val == 1ULL) {
                        fprintf(stderr, "[VIP] tohost=1 — PASS\n");
                        exitSimLoop("tohost: PASS", 0);
                    } else if (tohost_val != 0) {
                        fprintf(stderr, "[VIP] tohost=0x%016lx — FAIL\n",
                                (unsigned long)tohost_val);
                        exitSimLoop("tohost: FAIL", 1);
                    }
                }
            }
            for (int beat = 0; beat < dataMsgsPerLine; beat++) {
                auto msg = makeDatMsg(ipc, beat, dataChannelSize);
                if (datOut->areNSlotsAvailable(1, ct))
                    datOut->enqueue(msg, ct, lat1, false, false);
            }
            pending = true;
        }
    }

    // Drain inbound Ruby message buffers (rspIn / datIn / snpIn / reqIn).
    // CHIGenericController::wakeup() calls our virtual recvXxx overrides via
    // its private receiveAllRdyMessages template.  Calling the base wakeup()
    // here handles that entire path; we just suppress its reschedule by not
    // checking its return (pending already accounts for q2g traffic above).
    {
        Tick cur_tick = curTick();
        // Drain rspIn
        while (rspIn->isReady(cur_tick)) {
            const auto* m = dynamic_cast<const CHIResponseMsg*>(rspIn->peek());
            assert(m);
            if (recvResponseMsg(m)) { rspIn->dequeue(cur_tick); pending = true; }
            else                    { pending = true; break; }
        }
        // Drain datIn
        while (datIn->isReady(cur_tick)) {
            const auto* m = dynamic_cast<const CHIDataMsg*>(datIn->peek());
            assert(m);
            if (recvDataMsg(m)) { datIn->dequeue(cur_tick); pending = true; }
            else                { pending = true; break; }
        }
        // Drain snpIn
        while (snpIn->isReady(cur_tick)) {
            const auto* m = dynamic_cast<const CHIRequestMsg*>(snpIn->peek());
            assert(m);
            if (recvSnoopMsg(m)) { snpIn->dequeue(cur_tick); pending = true; }
            else                 { pending = true; break; }
        }
        // Drain reqIn (HN should not send REQ but handle anyway)
        while (reqIn->isReady(cur_tick)) {
            const auto* m = dynamic_cast<const CHIRequestMsg*>(reqIn->peek());
            assert(m);
            if (recvRequestMsg(m)) { reqIn->dequeue(cur_tick); pending = true; }
            else                   { pending = true; break; }
        }
    }

    // Update gem5 time marker for the Questa side.
    // gem5 Tick = 1 ps when using m5.ticks.setGlobalFrequency("1ps").
    // For other frequencies: divide curTick() by the number of ticks per ps.
    // Using curTick() directly in ps units (valid for 1 ps tick granularity).
    shm->gem5_grant_ps.store(static_cast<uint64_t>(ct),
                             std::memory_order_release);

    // ── Time-quantum barrier ─────────────────────────────────────────────────
    // Publish gem5's next quantum boundary, then real-time spin until Questa is
    // within one quantum of us.  Blocking HERE is what actually holds gem5's
    // sim-time back: wakeup() runs on gem5's single event-queue thread, so this
    // loop stalls sim-time until the separate Questa process catches up.
    // Rescheduling in sim-time does NOT work — with no other events gem5 just
    // fast-forwards its clock to the reschedule target and races ahead.  Only
    // VIPController-0 drives the barrier (all VIPs share one gem5 event queue,
    // so one is enough and avoids double-blocking).
    uint64_t Q = shm->quantum_ps.load(std::memory_order_relaxed);
    if (rnf_index_ == 0 && Q > 0) {  // Q == 0 → loose mode: no barrier
        uint64_t gem5_ps = static_cast<uint64_t>(ct);
        uint64_t gem5_q_end = (gem5_ps / Q + 1) * Q;
        shm->gem5_quantum_end_ps.store(gem5_q_end, std::memory_order_release);
        // Wake Questa if it is blocked waiting on gem5's progress.
        shm->gem5_seq.fetch_add(1, std::memory_order_release);
        chi_ipc::futex_wake(&shm->gem5_seq);
        // Block (not poll) until Questa is within one quantum, or the sim ends.
        // Sampling questa_seq *before* the range re-check makes wakes lossless:
        // if Questa bumps questa_seq between the check and the wait, futex_wait
        // returns immediately.  The 100 ms timeout re-checks sim_done so a
        // finished/dead Questa can't hang gem5 forever.
        while (shm->sim_done.load(std::memory_order_acquire) == 0) {
            uint32_t s = shm->questa_seq.load(std::memory_order_acquire);
            if (gem5_ps <=
                    shm->questa_quantum_end_ps.load(std::memory_order_acquire) + Q)
                break;
            chi_ipc::futex_wait(&shm->questa_seq, s);
        }
    }

    // Always reschedule: Questa may push into q2g at any time and there are no
    // CPUs to keep the event queue alive in standalone (Questa-only) mode.
    // Poll at 1-cycle granularity when there is pending work, otherwise back
    // off to 1000 cycles to reduce overhead during Questa inter-transaction gaps.
    bool q2g_nonempty = !my_q2g_req.empty() ||
                        !my_q2g_rsp.empty() ||
                        !my_q2g_dat.empty();
    scheduleEvent(pending || q2g_nonempty ? Cycles(1) : Cycles(1000));
}

// ─────────────────────────────────────────────────────────────────────────────
// Outbound: Ruby → DUT  (recvXxx methods push into g2q ring buffers)
// ─────────────────────────────────────────────────────────────────────────────

bool
VIPController::recvResponseMsg(const CHIResponseMsg* msg)
{
    auto* shm = shm_.layout();
    if (!shm) return true;

    auto ipc = packRsp(msg);
    fprintf(stderr, "[VIP:%d] recvResponseMsg type=%d txnId=%lu dbid=%lu stale=%d → ipc op=0x%02x txn=0x%03x\n",
            rnf_index_, (int)msg->gettype(), (unsigned long)msg->gettxnId(),
            (unsigned long)msg->getdbid(), (int)msg->getstale(),
            (unsigned)ipc.opcode, (unsigned)ipc.txn_id);
    if (!shm->g2q_rsp.push(ipc)) {
        fprintf(stderr, "[VIP:%d] recvResponseMsg: g2q_rsp FULL — retry op=0x%02x txn=0x%03x\n",
                rnf_index_, (unsigned)ipc.opcode, (unsigned)ipc.txn_id);
        return false;  // retry next cycle
    }
    return true;
}

bool
VIPController::recvDataMsg(const CHIDataMsg* msg)
{
    auto* shm = shm_.layout();
    if (!shm) return true;

    // The testbench's dat_rx_sm expects ONE IPC flit per cache line (with the
    // full 64 bytes) and splits it into two 32-byte RTL flits (DataID=00 and
    // DataID=10).  gem5 calls recvDataMsg once per beat; pack a full-cacheline
    // IPC flit on the first beat (offset==0) and silently drop subsequent beats
    // (their data is already present in beat 0's DataBlock).
    const WriteMask& bm = msg->getbitMask();
    int first_set = bm.firstBitSet(true);
    int offset = (first_set >= cacheLineSz_) ? 0 : first_set;
    if (offset != 0)
        return true;  // non-first beat: data already sent in the combined flit

    auto ipc = packDat(msg, cacheLineSz_);
    if (!shm->g2q_dat.push(ipc)) {
        DPRINTF(RubyCHIGeneric, "VIPController: g2q_dat full\n");
        return false;
    }
    return true;
}

bool
VIPController::recvSnoopMsg(const CHIRequestMsg* msg)
{
    auto* shm = shm_.layout();
    if (!shm) return true;

    auto ipc = packSnp(msg);
    if (!shm->g2q_snp.push(ipc)) {
        DPRINTF(RubyCHIGeneric, "VIPController: g2q_snp full\n");
        return false;
    }
    return true;
}

bool
VIPController::recvRequestMsg(const CHIRequestMsg* /*msg*/)
{
    // HN-F does not send REQ to RN-F in normal operation.
    warn("VIPController: unexpected recvRequestMsg — discarded\n");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Inbound message constructors (IPC → gem5)
// ─────────────────────────────────────────────────────────────────────────────

CHIGenericController::CHIRequestMsgPtr
VIPController::makeReqMsg(const chi_ipc::ChiIpcReq& m)
{
    Tick t = curTick();
    auto msg = std::make_shared<CHIRequestMsg>(t, cacheLineSz_, m_ruby_system);
    Addr addr = static_cast<Addr>(m.addr);
    Addr lineAddr = addr & ~(Addr)(cacheLineSz_ - 1);
    fprintf(stderr, "[VIP:%d] makeReqMsg op=0x%02x txn=0x%03x addr=0x%lx lineAddr=0x%lx src=%u\n",
            rnf_index_, (unsigned)m.opcode, (unsigned)m.txn_id,
            (unsigned long)addr, (unsigned long)lineAddr, (unsigned)m.src_id);
    msg->setaddr(lineAddr);
    msg->setaccAddr(addr);
    msg->setaccSize(cacheLineSz_);
    msg->settype(sccToGem5Req(m.opcode));
    if (m.opcode >= 0x28 && m.opcode <= 0x39)
        msg->setchiAtomicSubOp(static_cast<int>(m.opcode));
    msg->settxnId(static_cast<Addr>(m.txn_id));
    msg->setusesTxnId(false);  // HN-F requires usesTxnId=false for incoming REQs
    msg->setns(static_cast<bool>(m.ns));
    msg->setallowRetry(true);  // VIPController never manages PCrd credits; always use AllocRequest path

    // Track txnId→addr so makeDatMsg can set the correct address on
    // write-data messages (NCBWrData DAT flits carry no address field).
    txnid_to_addr_[m.txn_id] = lineAddr;

    // Track txnId→original byte address for CCID computation in packDat.
    // gem5 SLICC always emits CCID=0 in CHIDataMsg; we reconstruct the correct
    // CCID from the request address (bits[5:4] = 16-byte chunk within 64B line).
    txnid_to_acc_addr_[m.txn_id] = addr;

    // Track txnId→RTL SrcID so packDat/packRsp can set the correct TgtID.
    // In multi-core RTL designs the ICN routes by TgtID to the per-core channel.
    txnid_to_src_id_[m.txn_id] = static_cast<uint16_t>(m.src_id);

    // Always identify as this VIPController so responses route back here.
    msg->setrequestor(m_machineID);
    // Route to the HN-F responsible for the request address.
    // CHI gem5 uses MachineType_Cache for both RNF and HNF controllers.
    MachineID tgt = mapAddressToDownstreamMachine(lineAddr, MachineType_Cache);
    msg->getDestination().add(tgt);
    return msg;
}

CHIGenericController::CHIResponseMsgPtr
VIPController::makeRspMsg(const chi_ipc::ChiIpcRsp& m)
{
    fprintf(stderr, "[VIP] makeRspMsg op=0x%02x txn=0x%03x resp=0x%02x tgt=%u src=%u\n",
            (unsigned)m.opcode, (unsigned)m.txn_id, (unsigned)m.resp,
            (unsigned)m.tgt_id, (unsigned)m.src_id);
    Tick t = curTick();
    auto msg = std::make_shared<CHIResponseMsg>(t, cacheLineSz_, m_ruby_system);
    msg->settxnId(static_cast<Addr>(m.txn_id));
    msg->setusesTxnId(true);

    using Resp = uint8_t;
    switch (static_cast<Resp>(m.opcode)) {
    // RTL CHI-B (IHI0050B) opcode values from chie_defines.v
    case 0x01: // SnpResp
        msg->settype(m.resp == 1 ? CHIResponseType_SnpResp_SC
                                 : CHIResponseType_SnpResp_I);
        // HN-F dispatches SnpResp by cache-line address (req TBE), not txnId.
        // usesTxnId=true (default above) would route by txnId interpreted as
        // an address → no TBE at that address → state I → panic.
        msg->setusesTxnId(false);
        {
            auto it = snp_txnid_to_addr_.find(m.txn_id);
            if (it != snp_txnid_to_addr_.end()) {
                msg->setaddr(it->second);
                snp_txnid_to_addr_.erase(it);
                fprintf(stderr, "[VIP] SnpResp txn=0x%03x resp=0x%02x → addr=0x%lx\n",
                        (unsigned)m.txn_id, (unsigned)m.resp,
                        (unsigned long)msg->getaddr());
            } else {
                // CVA6 RTL dual-response bug: RTL sends both SnpRespData_I_PD
                // (TXDAT) and SnpResp_I (TXRSP) for the same snoop.  makeDatMsg
                // already consumed and erased the snp_txnid_to_addr_ entry.
                // Drop this stray RSP flit; wakeup() null-checks the return value.
                fprintf(stderr, "[VIP] SnpResp txn=0x%03x → stray (SnpRespData already processed), dropping\n",
                        (unsigned)m.txn_id);
                return nullptr;
            }
        }
        break;
    case 0x09: // SnpRespFwded — same routing fix as SnpResp
        msg->settype(CHIResponseType_SnpResp_I);
        msg->setusesTxnId(false);
        {
            auto it = snp_txnid_to_addr_.find(m.txn_id);
            if (it != snp_txnid_to_addr_.end()) {
                msg->setaddr(it->second);
                snp_txnid_to_addr_.erase(it);
            }
        }
        break;
    case 0x02: // CompAck
        msg->settype(CHIResponseType_CompAck);
        // Recover address so the HN-F routes CompAck to the right TBE.
        // usesTxnId must be false: if true the Cache_Controller dispatches by
        // txnId (=0x01) as address → hits state I → panic.
        msg->setusesTxnId(false);
        {
            auto it = txnid_to_addr_.find(m.txn_id);
            if (it != txnid_to_addr_.end()) {
                msg->setaddr(it->second);
                fprintf(stderr, "[VIP] CompAck txn=0x%03x → addr=0x%lx\n",
                        (unsigned)m.txn_id, (unsigned long)it->second);
            } else {
                fprintf(stderr, "[VIP] CompAck txn=0x%03x → NO ADDR in txnid_to_addr_ (size=%zu)\n",
                        (unsigned)m.txn_id, txnid_to_addr_.size());
            }
        }
        break;
    case 0x03: // RetryAck
        msg->settype(CHIResponseType_RetryAck); break;
    case 0x04: // Comp
        switch (m.resp) {
        case 0: msg->settype(CHIResponseType_Comp_I);     break;
        case 1: msg->settype(CHIResponseType_Comp_SC);    break;
        case 2: msg->settype(CHIResponseType_Comp_UC);    break;
        case 6: msg->settype(CHIResponseType_Comp_UD_PD); break;
        default:msg->settype(CHIResponseType_Comp);       break;
        }
        break;
    case 0x05: // CompDBIDResp
        msg->settype(CHIResponseType_CompDBIDResp); break;
    case 0x06: // DBIDResp
        msg->settype(CHIResponseType_DBIDResp); break;
    case 0x07: // PCrdGrant
        msg->settype(CHIResponseType_PCrdGrant); break;
    case 0x08: // ReadReceipt
        msg->settype(CHIResponseType_ReadReceipt); break;
    case 0x0B: // RespSepData
        msg->settype(CHIResponseType_RespSepData); break;
    default:
        msg->settype(CHIResponseType_Comp); break;
    }

    msg->setresponder(m_machineID);
    // RSP from DUT goes back to HN-F; use first downstream destination.
    MachineID tgt = allDownstreamDest().smallestElement();
    msg->getDestination().add(tgt);
    msg->setdbid(m.db_id);
    return msg;
}

CHIGenericController::CHIDataMsgPtr
VIPController::makeDatMsg(const chi_ipc::ChiIpcDat& m, int beat, int beatSz)
{
    Tick t = curTick();
    auto msg = std::make_shared<CHIDataMsg>(t, cacheLineSz_, m_ruby_system);

    // DAT flits carry no address.  For snoop responses use the snoop TxnId map;
    // for write responses use the write TxnId map.
    Addr addr;
    if (m.opcode == 0x1 || m.opcode == 0x5) {  // SnpRespData / SnpRespDataPtl
        auto sit = snp_txnid_to_addr_.find(m.txn_id);
        if (sit != snp_txnid_to_addr_.end()) {
            addr = sit->second;
            // One IPC SnpRespData is split into dataMsgsPerLine gem5 beats, each
            // calling makeDatMsg with the same TxnID.  Erase the address entry
            // only on the LAST beat — erasing on beat 0 made beats 1..N-1 miss
            // and fall back to addr 0 (SnpRespData_I_PD → addr 0 panic at any
            // width where dataMsgsPerLine > 1, e.g. 128b → 4 beats).
            if (beat == dataMsgsPerLine - 1)
                snp_txnid_to_addr_.erase(sit);
        } else {
            addr = static_cast<Addr>(m.addr);
            fprintf(stderr,
                "[VIP:%d] makeDatMsg: SnpRespData miss on snp_txnid_to_addr_"
                " op=0x%02x resp=0x%02x txn=0x%03x m.addr=0x%lx"
                " src=%u tgt=%u → falling back to addr=0x%lx"
                " (map has %zu entries)\n",
                rnf_index_, (unsigned)m.opcode, (unsigned)m.resp,
                (unsigned)m.txn_id, (unsigned long)m.addr,
                (unsigned)m.src_id, (unsigned)m.tgt_id,
                (unsigned long)addr, snp_txnid_to_addr_.size());
        }
    } else {
        auto it = txnid_to_addr_.find(m.txn_id);
        addr = (it != txnid_to_addr_.end()) ? it->second
                                            : static_cast<Addr>(m.addr);
    }
    msg->setaddr(addr);
    msg->settxnId(static_cast<Addr>(m.txn_id));
    msg->setusesTxnId(false);  // datInPort asserts !usesTxnId; routes by addr

    // Map dat opcode + resp to gem5 CHIDataType.
    // RTL CHI-B (IHI0050B) opcode values from chie_defines.v.
    uint8_t op   = m.opcode;
    uint8_t resp = m.resp;
    switch (op) {
    case 0x1: // SnpRespData
        switch (resp) {
        case 1: msg->settype(CHIDataType_SnpRespData_SC);   break;
        case 2: msg->settype(CHIDataType_SnpRespData_UC);   break;
        case 3: msg->settype(CHIDataType_SnpRespData_SD);   break;
        case 4: msg->settype(CHIDataType_SnpRespData_I_PD); break;
        case 5: msg->settype(CHIDataType_SnpRespData_SC_PD);break;
        case 6: msg->settype(CHIDataType_SnpRespData_SD);   break;
        default:msg->settype(CHIDataType_SnpRespData_I);    break;
        }
        break;
    case 0x2: // CopyBackWrData (CHI-B 0x2; was NonCopyBackWrData in CHI-E)
        // CHI-B RESP encoding: 000=I, 001=SC, 010=UC, 110=UD_PD, 111=SD_PD
        switch (resp) {
        case 0: msg->settype(CHIDataType_CBWrData_I);     break;
        case 1: msg->settype(CHIDataType_CBWrData_SC);    break;
        case 2: msg->settype(CHIDataType_CBWrData_UC);    break;
        case 6: msg->settype(CHIDataType_CBWrData_UD_PD); break;
        case 7: msg->settype(CHIDataType_CBWrData_SD_PD); break;
        default:msg->settype(CHIDataType_CBWrData_UC);    break;
        }
        break;
    case 0x3: // NonCopyBackWrData (CHI-B 0x3; was DataSepResp in CHI-E)
    case 0xC: // NCBWrDataCompAck
        msg->settype(CHIDataType_NCBWrData); break;
    case 0x4: // CompData
        switch (resp) {
        case 0: msg->settype(CHIDataType_CompData_I);     break;
        case 1: msg->settype(CHIDataType_CompData_SC);    break;
        case 2: msg->settype(CHIDataType_CompData_UC);    break;
        case 6: msg->settype(CHIDataType_CompData_UD_PD); break;
        case 7: msg->settype(CHIDataType_CompData_SD_PD); break;
        default:msg->settype(CHIDataType_CompData_I);     break;
        }
        break;
    case 0x5: // SnpRespDataPtl — no exact gem5 type; map to SnpRespData_I
        switch (resp) {
        case 4: msg->settype(CHIDataType_SnpRespData_I_PD);     break;
        case 2: msg->settype(CHIDataType_SnpRespData_UD);     break;
        default:msg->settype(CHIDataType_SnpRespData_I);     break;
        }
        break;
    case 0x6: // SnpRespDataFwded
        msg->settype(CHIDataType_SnpRespData_I_Fwded_SC); break;
    case 0xB: // DataSepResp (CHI-B 0xB; was 0x3 in CHI-E)
        msg->settype(CHIDataType_DataSepResp_UC); break;
    default:
        msg->settype(CHIDataType_NCBWrData); break;
    }

    // Copy the beat-sized slice of IPC data into the DataBlock at the correct
    // byte offset so that copyPartial() in the HN-F merges beats correctly.
    int offset = beat * beatSz;
    int n = std::min(beatSz, cacheLineSz_ - offset);
    if (n > 0) {
        msg->getdataBlk().setData(m.data + offset, offset, n);
        fprintf(stderr,
                "[VIP:%d] makeDatMsg op=0x%02x resp=0x%02x txn=0x%03x"
                " addr=0x%lx beat=%d off=%d"
                " d[off]=0x%02x d[24..27]=0x%02x%02x%02x%02x\n",
                rnf_index_, m.opcode, m.resp,
                m.txn_id, static_cast<unsigned long>(addr), beat, offset,
                m.data[offset],
                m.data[24], m.data[25], m.data[26], m.data[27]);
    }

    // Build bitMask for this beat's byte range, intersected with IPC BE array.
    // datInPort asserts bitMask.count() > 0.
    {
        WriteMask mask(cacheLineSz_);
        bool any = false;
        if (m.opcode == 0x05 && n > 0) {
            // SnpRespDataPtl: BE=1 marks dirty sublines; BE=0 bytes arrived
            // as zeros from the RTL (clean sublines the RTL discarded).
            // Restore clean bytes from the last CompData we sent for this
            // address so gem5 receives a complete, correct cache line.
            auto it = addr_to_line_data_.find(addr);
            if (it != addr_to_line_data_.end() &&
                    (int)it->second.size() >= cacheLineSz_) {
                for (int i = offset; i < offset + n; i++) {
                    if (!m.be[i]) {
                        // Replace RTL's zero with the cached clean byte.
                        msg->getdataBlk().setData(&it->second[i], i, 1);
                    }
                }
            }
            mask.setMask(offset, n);  // full beat valid after merge
        } else {
            for (int i = offset; i < offset + n; i++) {
                if (m.be[i]) { mask.setMask(i, 1); any = true; }
            }
            if (!any) mask.setMask(offset, n);  // no BE info — treat beat as fully valid
        }
        msg->setbitMask(mask);
        fprintf(stderr, "[VIP:%d] makeDatMsg txn=0x%03x beat=%d bitMask.count=%d\n",
                rnf_index_, m.txn_id, beat, mask.count());
    }
    msg->setresponder(m_machineID);
    // Route to HN-F by address.
    MachineID tgt = mapAddressToDownstreamMachine(addr, MachineType_Cache);
    msg->getDestination().add(tgt);
    return msg;
}

// ─────────────────────────────────────────────────────────────────────────────
// Outbound pack helpers (gem5 → IPC)
// ─────────────────────────────────────────────────────────────────────────────

chi_ipc::ChiIpcRsp
VIPController::packRsp(const CHIResponseMsg* msg)
{
    chi_ipc::ChiIpcRsp m{};
    m.db_id   = static_cast<uint16_t>(msg->getdbid());
    // gem5 HN-F leaves txnId=0 in DBIDResp/CompDBIDResp (not needed for
    // internal SLICC routing); recover it from dbid which equals tbe.txnId =
    // original REQ TxnID.  All other response types set txnId normally.
    {
        uint16_t tid = static_cast<uint16_t>(msg->gettxnId());
        m.txn_id = (tid == 0 && m.db_id != 0) ? m.db_id : tid;
    }
    m.src_id  = rtl_hnf_nid_;
    {
        auto it = txnid_to_src_id_.find(static_cast<uint16_t>(m.txn_id));
        m.tgt_id = (it != txnid_to_src_id_.end()) ? it->second : rtl_src_id_;
    }
    m.qos     = 0;

    // Map CHIResponseType → RTL CHI-B opcode + resp (chie_defines.v values).
    switch (msg->gettype()) {
    case CHIResponseType_Comp_I:       m.opcode=0x04; m.resp=0; break;
    case CHIResponseType_Comp_SC:      m.opcode=0x04; m.resp=1; break;
    case CHIResponseType_Comp_UC:      m.opcode=0x04; m.resp=2; break;
    case CHIResponseType_Comp_UD_PD:   m.opcode=0x04; m.resp=6; break;
    case CHIResponseType_Comp:         m.opcode=0x04;            break;
    case CHIResponseType_CompAck:      m.opcode=0x02;            break;
    case CHIResponseType_DBIDResp:     m.opcode=0x06;            break;
    case CHIResponseType_CompDBIDResp: m.opcode=0x05;            break;
    case CHIResponseType_ReadReceipt:  m.opcode=0x08;            break;
    case CHIResponseType_RespSepData:  m.opcode=0x0B;            break;
    case CHIResponseType_RetryAck:     m.opcode=0x03;            break;
    case CHIResponseType_PCrdGrant:    m.opcode=0x07;            break;
    case CHIResponseType_SnpResp_I:    m.opcode=0x01; m.resp=0; break;
    case CHIResponseType_SnpResp_SC:   m.opcode=0x01; m.resp=1; break;
    case CHIResponseType_SnpResp_I_Fwded_UC:
    case CHIResponseType_SnpResp_I_Fwded_UD_PD:
        m.opcode=0x09; m.resp=0; break;
    case CHIResponseType_SnpResp_SC_Fwded_SC:
    case CHIResponseType_SnpResp_SC_Fwded_SD_PD:
    case CHIResponseType_SnpResp_SC_Fwded_I:
        m.opcode=0x09; m.resp=1; break;
    case CHIResponseType_SnpResp_UC_Fwded_I:
    case CHIResponseType_SnpResp_UD_Fwded_I:
        m.opcode=0x09; m.resp=2; break;
    case CHIResponseType_SnpResp_SD_Fwded_I:
        m.opcode=0x09; m.resp=3; break;
    default:
        m.opcode=0x04; break;
    }

    return m;
}

chi_ipc::ChiIpcDat
VIPController::packDat(const CHIDataMsg* msg, int beatSz)
{
    chi_ipc::ChiIpcDat m{};
    m.addr      = static_cast<uint64_t>(msg->getaddr());
    m.txn_id    = static_cast<uint8_t>(msg->gettxnId());
    m.src_id    = rtl_hnf_nid_;
    {
        auto it = txnid_to_src_id_.find(static_cast<uint16_t>(m.txn_id));
        m.tgt_id = (it != txnid_to_src_id_.end()) ? it->second : rtl_src_id_;
    }
    m.home_n_id = rtl_hnf_nid_;
    m.qos       = 0;
    // Echo the original request TXNID as DBID so the DUT can return it in
    // CompAck TXNID.  The HN-F has no DBID field in CHIDataMsg, so we reuse
    // txn_id; makeRspMsg() maps it back to the address via txnid_to_addr_.
    m.db_id     = static_cast<uint16_t>(m.txn_id);

    // Determine which beat this CHIDataMsg represents from the bitMask's first
    // set byte.  The HN-F sets bitMask via setMask(offset, range) where offset
    // is a multiple of beatSz.
    const WriteMask& bm = msg->getbitMask();
    int offset = bm.firstBitSet(true);  // returns cacheLineSz_ if empty
    if (offset >= cacheLineSz_) offset = 0;
    int beat = offset / beatSz;

    m.data_id  = static_cast<uint8_t>(beat * beatSz / 16);
    m.data_len = static_cast<uint16_t>(beatSz);
    // CCID: which 16-byte chunk within the 64-byte cache line was requested.
    // gem5 SLICC hardcodes CCID=0 in CHIDataMsg; derive it from the original
    // byte address stored when the REQ was received.  m.addr is cache-line
    // aligned (bits[5:0]=0) so it cannot be used directly.
    // CCID = addr[5:4]: the 16B chunk index within the 64B cache line.
    // addr[5:4] selects which of the four 16B windows the requested byte is in.
    {
        Addr orig_addr = static_cast<Addr>(m.addr);  // fallback: line-aligned
        auto it = txnid_to_acc_addr_.find(m.txn_id);
        if (it != txnid_to_acc_addr_.end())
            orig_addr = it->second;
        m.cc_id = static_cast<uint8_t>((orig_addr & 0x30) >> 4);
        fprintf(stderr, "[VIP] packDat txn=0x%03x orig_addr=0x%lx cc_id=%u\n",
                m.txn_id, static_cast<unsigned long>(orig_addr), (unsigned)m.cc_id);
    }

    // RTL CHI-B (IHI0050B) opcode values from chie_defines.v
    switch (msg->gettype()) {
    case CHIDataType_CompData_I:     m.opcode=0x4; m.resp=0; break;
    case CHIDataType_CompData_SC:    m.opcode=0x4; m.resp=1; break;
    case CHIDataType_CompData_UC:    m.opcode=0x4; m.resp=2; break;
    case CHIDataType_CompData_UD_PD: m.opcode=0x4; m.resp=6; break;
    case CHIDataType_CompData_SD_PD: m.opcode=0x4; m.resp=7; break;
    case CHIDataType_DataSepResp_UC: m.opcode=0xB; m.resp=2; break;
    case CHIDataType_CBWrData_UC:    m.opcode=0x2; m.resp=2; break;
    case CHIDataType_CBWrData_SC:    m.opcode=0x2; m.resp=1; break;
    case CHIDataType_CBWrData_UD_PD: m.opcode=0x2; m.resp=6; break;
    case CHIDataType_CBWrData_SD_PD: m.opcode=0x2; m.resp=7; break;
    case CHIDataType_CBWrData_I:     m.opcode=0x2; m.resp=0; break;
    case CHIDataType_NCBWrData:      m.opcode=0x3;            break;
    case CHIDataType_SnpRespData_I:     m.opcode=0x1; m.resp=0; break;
    case CHIDataType_SnpRespData_SC:    m.opcode=0x1; m.resp=1; break;
    case CHIDataType_SnpRespData_UC:    m.opcode=0x1; m.resp=2; break;
    case CHIDataType_SnpRespData_SD:    m.opcode=0x1; m.resp=6; break;
    case CHIDataType_SnpRespData_I_PD:  m.opcode=0x1; m.resp=4; break;
    case CHIDataType_SnpRespData_SC_PD: m.opcode=0x1; m.resp=5; break;
    default: m.opcode=0x4; break;
    }

    // Copy beatSz bytes starting at `offset` from the DataBlock into m.data[0..].
    // Questa's IPC DAT struct always has data starting at byte 0.
    int n = std::min(beatSz, cacheLineSz_ - offset);
    if (n > 0) {
        const uint8_t* src = msg->getdataBlk().getData(offset, n);
        memcpy(m.data, src, n);
        memset(m.be, 0xff, n);
        // Snapshot this beat in addr_to_line_data_ so that a later
        // SnpRespDataPtl from the RTL can restore the clean sublines.
        {
            auto& line = addr_to_line_data_[static_cast<Addr>(m.addr)];
            if ((int)line.size() < cacheLineSz_)
                line.resize(cacheLineSz_, 0);
            memcpy(line.data() + offset, m.data, n);
        }
        fprintf(stderr,
                "[VIP:%d] packDat txn=0x%03x src=%u tgt=%u home=%u"
                " rtl_hnf_nid_=%u addr=0x%lx beat=%d off=%d n=%d"
                " d[0]=0x%02x d[24..27]=0x%02x%02x%02x%02x d[32]=0x%02x\n",
                rnf_index_,
                m.txn_id,
                (unsigned)m.src_id, (unsigned)m.tgt_id,
                (unsigned)m.home_n_id, (unsigned)rtl_hnf_nid_,
                static_cast<unsigned long>(m.addr), beat, offset, n,
                m.data[0],
                m.data[24], m.data[25], m.data[26], m.data[27],
                (n > 32 ? m.data[32] : 0));
    }
    return m;
}

chi_ipc::ChiIpcSnp
VIPController::packSnp(const CHIRequestMsg* msg)
{
    chi_ipc::ChiIpcSnp m{};
    m.addr             = static_cast<uint64_t>(msg->getaddr());
    m.txn_id           = static_cast<uint16_t>(msg->gettxnId());
    m.src_id           = static_cast<uint16_t>(msg->getrequestor().num);
    m.tgt_id           = rtl_src_id_;
    m.fwd_n_id         = static_cast<uint16_t>(msg->getfwdRequestor().num);
    m.qos              = 0;
    m.ns               = 0;
    m.ret_to_src       = msg->getretToSrc() ? 1 : 0;
    m.do_not_goto_sd   = 0;
    m.do_not_data_pull = 0;
    m.opcode           = gem5ToSccSnp(msg->gettype());
    snp_txnid_to_addr_[m.txn_id] = static_cast<Addr>(msg->getaddr());
    return m;
}

// ─────────────────────────────────────────────────────────────────────────────
// Opcode conversion table: SCC integer → gem5 CHIRequestType
// (mirrors chi_vip_controller.cc scc_to_gem5req)
// ─────────────────────────────────────────────────────────────────────────────
CHIRequestType
VIPController::sccToGem5Req(uint8_t op)
{
    // RTL CHI-B (IHI0050B) opcode values from chie_defines.v
    switch (op) {
    case 0x01: return CHIRequestType_ReadShared;
    case 0x02: return CHIRequestType_ReadClean;
    case 0x03: return CHIRequestType_ReadOnce;
    case 0x04: return CHIRequestType_ReadNoSnp;
    case 0x07: return CHIRequestType_ReadUnique;
    case 0x0B: return CHIRequestType_CleanUnique;
    case 0x0D: return CHIRequestType_Evict;
    case 0x17: return CHIRequestType_WriteCleanFull;
    case 0x18: return CHIRequestType_WriteUniquePtl;
    case 0x19: return CHIRequestType_WriteUniqueFull;
    case 0x1A: return CHIRequestType_WriteBackPtl;
    case 0x1B: return CHIRequestType_WriteBackFull;
    case 0x1C: return CHIRequestType_WriteNoSnpPtl;
    case 0x1D: return CHIRequestType_WriteNoSnp;
    case 0x15: return CHIRequestType_WriteEvictFull;
    case 0x41: return CHIRequestType_MakeReadUnique;
    // CHI-E AtomicStore (0x28–0x2f): no return value → network-facing AtomicNoReturn
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x2c: case 0x2d: case 0x2e: case 0x2f:
        return CHIRequestType_AtomicNoReturn;
    // CHI-E AtomicLoad (0x30–0x37), AtomicSwap (0x38), AtomicCompare (0x39):
    // return old value → network-facing AtomicReturn
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x34: case 0x35: case 0x36: case 0x37:
    case 0x38: case 0x39:
        return CHIRequestType_AtomicReturn;
    default:   return CHIRequestType_ReadOnce;
    }
}

// gem5 CHIRequestType → chi::snp_optype_e integer
uint8_t
VIPController::gem5ToSccSnp(CHIRequestType t)
{
    switch (t) {
    case CHIRequestType_SnpSharedFwd:         return 0x0C;
    case CHIRequestType_SnpNotSharedDirtyFwd: return 0x0E;
    case CHIRequestType_SnpUniqueFwd:         return 0x0D;
    case CHIRequestType_SnpOnceFwd:           return 0x0B;
    case CHIRequestType_SnpOnce:              return 0x03;
    case CHIRequestType_SnpShared:            return 0x04;
    case CHIRequestType_SnpUnique:            return 0x07;
    case CHIRequestType_SnpCleanInvalid:      return 0x08;
    case CHIRequestType_SnpDvmOpSync_P1:
    case CHIRequestType_SnpDvmOpSync_P2:
    case CHIRequestType_SnpDvmOpNonSync_P1:
    case CHIRequestType_SnpDvmOpNonSync_P2:   return 0x14;
    default:                                  return 0x03;
    }
}

} // namespace ruby
} // namespace gem5
