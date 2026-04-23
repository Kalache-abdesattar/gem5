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

#include <cstring>

#include "base/logging.hh"
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
    , shm_(p.shm_name, /*create=*/true)  // gem5 always creates the segment
    , cacheLineSz_(p.ruby_system->getBlockSizeBytes())
{
}

void
VIPController::init()
{
    CHIGenericController::init();
    // Kick the event loop immediately so we notice early q2g messages.
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

    // Drain q2g_req → reqOut
    {
        chi_ipc::ChiIpcReq ipc;
        while (shm->q2g_req.pop(ipc)) {
            auto msg = makeReqMsg(ipc);
            if (reqOut->areNSlotsAvailable(1, ct)) {
                reqOut->enqueue(msg, ct, lat1, false, false);
            } else {
                DPRINTF(RubyCHIGeneric,
                        "VIPController: reqOut full, dropped REQ txn=%u\n",
                        ipc.txn_id);
            }
            pending = true;
        }
    }

    // Drain q2g_rsp → rspOut
    {
        chi_ipc::ChiIpcRsp ipc;
        while (shm->q2g_rsp.pop(ipc)) {
            auto msg = makeRspMsg(ipc);
            if (rspOut->areNSlotsAvailable(1, ct))
                rspOut->enqueue(msg, ct, lat1, false, false);
            pending = true;
        }
    }

    // Drain q2g_dat → datOut.
    // Each IPC DAT carries a full cache line (CHI_DATA_W_BYTES = dataChannelSize
    // when both sides use the same channel width). dataMsgsPerLine gives the
    // number of CHIDataMsgs the HN-F expects per cache line; split accordingly.
    {
        chi_ipc::ChiIpcDat ipc;
        while (shm->q2g_dat.pop(ipc)) {
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

    // Always reschedule: Questa may push into q2g at any time and there are no
    // CPUs to keep the event queue alive in standalone (Questa-only) mode.
    // Poll at 1-cycle granularity when there is pending work, otherwise back
    // off to 1000 cycles to reduce overhead during Questa inter-transaction gaps.
    bool q2g_nonempty = !shm->q2g_req.empty() ||
                        !shm->q2g_rsp.empty() ||
                        !shm->q2g_dat.empty();
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
    if (!shm->g2q_rsp.push(ipc)) {
        DPRINTF(RubyCHIGeneric, "VIPController: g2q_rsp full\n");
        return false;  // retry next cycle
    }
    return true;
}

bool
VIPController::recvDataMsg(const CHIDataMsg* msg)
{
    auto* shm = shm_.layout();
    if (!shm) return true;

    // The HN-F splits each cache-line read response into dataMsgsPerLine
    // CHIDataMsgs of dataChannelSize bytes each.  Pack each one as a
    // separate IPC DAT so Questa sees one flit per beat.
    auto ipc = packDat(msg, dataChannelSize);
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
    msg->setaddr(addr);
    msg->setaccAddr(addr);       // accAddr must be >= addr; for full-line ops addr==addr
    msg->setaccSize(cacheLineSz_); // full cache line access
    msg->settype(sccToGem5Req(m.opcode));
    msg->settxnId(static_cast<Addr>(m.txn_id));
    msg->setusesTxnId(false);  // HN-F requires usesTxnId=false for incoming REQs
    msg->setns(static_cast<bool>(m.ns));
    msg->setallowRetry(static_cast<bool>(m.allow_retry));

    // Track txnId→addr so makeDatMsg can set the correct address on
    // write-data messages (NCBWrData DAT flits carry no address field).
    txnid_to_addr_[m.txn_id] = addr;

    // Always identify as this VIPController so responses route back here.
    msg->setrequestor(m_machineID);
    // Route to the HN-F responsible for the request address.
    // CHI gem5 uses MachineType_Cache for both RNF and HNF controllers.
    MachineID tgt = mapAddressToDownstreamMachine(
        static_cast<Addr>(m.addr), MachineType_Cache);
    msg->getDestination().add(tgt);
    return msg;
}

CHIGenericController::CHIResponseMsgPtr
VIPController::makeRspMsg(const chi_ipc::ChiIpcRsp& m)
{
    Tick t = curTick();
    auto msg = std::make_shared<CHIResponseMsg>(t, cacheLineSz_, m_ruby_system);
    msg->settxnId(static_cast<Addr>(m.txn_id));
    msg->setusesTxnId(true);

    using R    = CHIResponseType;
    using Resp = uint8_t;
    switch (static_cast<Resp>(m.opcode)) {
    // These literal integers are the chi::rsp_optype_e values produced by
    // scc.  They match the IHI0050 spec and the chi_flit_pack.hh OPCODE fields.
    case 0x04:
        msg->settype(CHIResponseType_CompAck);
        // m.txn_id is the DBID echoed from packDat (= original request txn_id).
        // Recover the address so the HN-F can route CompAck to the right TBE.
        // usesTxnId must be false: if true, the Cache_Controller dispatches by
        // txnId (0x70) instead of addr (0x30000) and hits state I → panic.
        msg->setusesTxnId(false);
        {
            auto it = txnid_to_addr_.find(m.txn_id);
            if (it != txnid_to_addr_.end())
                msg->setaddr(it->second);
        }
        break;
    case 0x1:  // Comp
        switch (m.resp) {
        case 0: msg->settype(CHIResponseType_Comp_I);     break;
        case 1: msg->settype(CHIResponseType_Comp_SC);    break;
        case 2: msg->settype(CHIResponseType_Comp_UC);    break;
        case 6: msg->settype(CHIResponseType_Comp_UD_PD); break;
        default:msg->settype(CHIResponseType_Comp);       break;
        }
        break;
    case 0x5:  // DBIDResp
        msg->settype(CHIResponseType_DBIDResp); break;
    case 0x7:  // CompDBIDResp
        msg->settype(CHIResponseType_CompDBIDResp); break;
    case 0x08: msg->settype(CHIResponseType_ReadReceipt);  break;
    case 0x09: msg->settype(CHIResponseType_RespSepData);  break;
    case 0x0B: msg->settype(CHIResponseType_RetryAck);     break;
    case 0x0C: msg->settype(CHIResponseType_PCrdGrant);    break;
    case 0x13: // SnpResp
        msg->settype(m.resp == 1 ? CHIResponseType_SnpResp_SC
                                 : CHIResponseType_SnpResp_I);
        break;
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

    // DAT flits (NCBWrData etc.) carry no address.  Recover it from the
    // txnId→addr map populated when the original REQ was forwarded.
    // The HN-F datInPort asserts !usesTxnId and dispatches by address.
    auto it = txnid_to_addr_.find(m.txn_id);
    Addr addr = (it != txnid_to_addr_.end()) ? it->second
                                             : static_cast<Addr>(m.addr);
    msg->setaddr(addr);
    msg->settxnId(static_cast<Addr>(m.txn_id));
    msg->setusesTxnId(false);  // datInPort asserts !usesTxnId; routes by addr

    // Map dat opcode + resp to gem5 CHIDataType
    uint8_t op   = m.opcode;
    uint8_t resp = m.resp;
    // chi::dat_optype_e integer values from IHI0050 / scc header
    switch (op) {
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
    case 0x3: // DataSepResp
        msg->settype(CHIDataType_DataSepResp_UC); break;
    case 0x5: // CopyBackWrData
        switch (resp) {
        case 1: msg->settype(CHIDataType_CBWrData_SC);    break;
        case 5: msg->settype(CHIDataType_CBWrData_UD_PD); break;
        case 7: msg->settype(CHIDataType_CBWrData_SD_PD); break;
        case 0: msg->settype(CHIDataType_CBWrData_I);     break;
        default:msg->settype(CHIDataType_CBWrData_UC);    break;
        }
        break;
    case 0x2: // NonCopyBackWrData
    case 0xC: // NCBWrDataCompAck
        msg->settype(CHIDataType_NCBWrData); break;
    case 0x1: // SnpRespData
        switch (resp) {
        case 1: msg->settype(CHIDataType_SnpRespData_SC);   break;
        case 2: msg->settype(CHIDataType_SnpRespData_UC);   break;
        case 4: msg->settype(CHIDataType_SnpRespData_I_PD); break;
        case 5: msg->settype(CHIDataType_SnpRespData_SC_PD);break;
        case 6: msg->settype(CHIDataType_SnpRespData_SD);   break;
        default:msg->settype(CHIDataType_SnpRespData_I);    break;
        }
        break;
    case 0x6: // SnpRespDataFwded
        msg->settype(CHIDataType_SnpRespData_I_Fwded_SC); break;
    default:
        msg->settype(CHIDataType_NCBWrData); break;
    }

    // Copy the beat-sized slice of IPC data into the DataBlock at the correct
    // byte offset so that copyPartial() in the HN-F merges beats correctly.
    int offset = beat * beatSz;
    int n = std::min(beatSz, cacheLineSz_ - offset);
    if (n > 0) {
        msg->getdataBlk().setData(m.data + offset, offset, n);
        fprintf(stderr, "[VIP] makeDatMsg txn=0x%02x addr=0x%lx beat=%d "
                "offset=%d data[off]=0x%02x data[off+1]=0x%02x\n",
                m.txn_id, static_cast<unsigned long>(addr), beat, offset,
                m.data[offset], (n > 1 ? m.data[offset + 1] : 0));
    }

    // Build bitMask for this beat's byte range, intersected with IPC BE array.
    // datInPort asserts bitMask.count() > 0.
    {
        WriteMask mask(cacheLineSz_);
        bool any = false;
        for (int i = offset; i < offset + n; i++) {
            if (m.be[i]) { mask.setMask(i, 1); any = true; }
        }
        if (!any) mask.setMask(offset, n);  // no BE info — treat beat as fully valid
        msg->setbitMask(mask);
        fprintf(stderr, "[VIP] makeDatMsg txn=0x%02x beat=%d bitMask.count=%d\n",
                m.txn_id, beat, mask.count());
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
    m.txn_id  = static_cast<uint8_t>(msg->gettxnId());
    m.src_id  = static_cast<uint16_t>(msg->getresponder().num);
    m.tgt_id  = static_cast<uint16_t>(msg->getDestination().smallestElement().num);
    m.db_id   = static_cast<uint16_t>(msg->getdbid());
    m.qos     = 0;

    // Map CHIResponseType → opcode + resp using the same table as
    // chi_vip_controller.cc pack_rsp (integers from chi::rsp_optype_e).
    switch (msg->gettype()) {
    case CHIResponseType_Comp_I:       m.opcode=0x1; m.resp=0; break;
    case CHIResponseType_Comp_SC:      m.opcode=0x1; m.resp=1; break;
    case CHIResponseType_Comp_UC:      m.opcode=0x1; m.resp=2; break;
    case CHIResponseType_Comp_UD_PD:   m.opcode=0x1; m.resp=6; break;
    case CHIResponseType_Comp:         m.opcode=0x1;            break;
    case CHIResponseType_CompAck:      m.opcode=0x4;            break;
    case CHIResponseType_DBIDResp:     m.opcode=0x5;            break;
    case CHIResponseType_CompDBIDResp: m.opcode=0x7;            break;
    case CHIResponseType_ReadReceipt:  m.opcode=0x8;            break;
    case CHIResponseType_RespSepData:  m.opcode=0x9;            break;
    case CHIResponseType_RetryAck:     m.opcode=0xB;            break;
    case CHIResponseType_PCrdGrant:    m.opcode=0xC;            break;
    case CHIResponseType_SnpResp_I:    m.opcode=0x13; m.resp=0; break;
    case CHIResponseType_SnpResp_SC:   m.opcode=0x13; m.resp=1; break;
    case CHIResponseType_SnpResp_I_Fwded_UC:
    case CHIResponseType_SnpResp_I_Fwded_UD_PD:
        m.opcode=0x14; m.resp=0; break;
    case CHIResponseType_SnpResp_SC_Fwded_SC:
    case CHIResponseType_SnpResp_SC_Fwded_SD_PD:
    case CHIResponseType_SnpResp_SC_Fwded_I:
        m.opcode=0x14; m.resp=1; break;
    case CHIResponseType_SnpResp_UC_Fwded_I:
    case CHIResponseType_SnpResp_UD_Fwded_I:
        m.opcode=0x14; m.resp=2; break;
    case CHIResponseType_SnpResp_SD_Fwded_I:
        m.opcode=0x14; m.resp=3; break;
    default:
        m.opcode=0x1; break;
    }
    return m;
}

chi_ipc::ChiIpcDat
VIPController::packDat(const CHIDataMsg* msg, int beatSz)
{
    chi_ipc::ChiIpcDat m{};
    m.addr      = static_cast<uint64_t>(msg->getaddr());
    m.txn_id    = static_cast<uint8_t>(msg->gettxnId());
    m.src_id    = static_cast<uint16_t>(msg->getresponder().num);
    m.tgt_id    = static_cast<uint16_t>(msg->getDestination().smallestElement().num);
    m.home_n_id = static_cast<uint16_t>(msg->getresponder().num);
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

    switch (msg->gettype()) {
    case CHIDataType_CompData_I:     m.opcode=0x4; m.resp=0; break;
    case CHIDataType_CompData_SC:    m.opcode=0x4; m.resp=1; break;
    case CHIDataType_CompData_UC:    m.opcode=0x4; m.resp=2; break;
    case CHIDataType_CompData_UD_PD: m.opcode=0x4; m.resp=6; break;
    case CHIDataType_CompData_SD_PD: m.opcode=0x4; m.resp=7; break;
    case CHIDataType_DataSepResp_UC: m.opcode=0x3; m.resp=2; break;
    case CHIDataType_CBWrData_UC:    m.opcode=0x5; m.resp=2; break;
    case CHIDataType_CBWrData_SC:    m.opcode=0x5; m.resp=1; break;
    case CHIDataType_CBWrData_UD_PD: m.opcode=0x5; m.resp=5; break;
    case CHIDataType_CBWrData_SD_PD: m.opcode=0x5; m.resp=7; break;
    case CHIDataType_CBWrData_I:     m.opcode=0x5; m.resp=0; break;
    case CHIDataType_NCBWrData:      m.opcode=0x2;            break;
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
        fprintf(stderr, "[VIP] packDat txn=0x%02x addr=0x%lx beat=%d "
                "offset=%d data[0]=0x%02x data[1]=0x%02x\n",
                m.txn_id, static_cast<unsigned long>(m.addr), beat, offset,
                m.data[0], (n > 1 ? m.data[1] : 0));
    }
    return m;
}

chi_ipc::ChiIpcSnp
VIPController::packSnp(const CHIRequestMsg* msg)
{
    chi_ipc::ChiIpcSnp m{};
    m.addr             = static_cast<uint64_t>(msg->getaddr());
    m.txn_id           = static_cast<uint8_t>(msg->gettxnId());
    m.src_id           = static_cast<uint16_t>(msg->getrequestor().num);
    m.fwd_n_id         = static_cast<uint16_t>(msg->getfwdRequestor().num);
    m.qos              = 0;
    m.ns               = 0;
    m.ret_to_src       = static_cast<uint8_t>(msg->getretToSrc());
    m.do_not_goto_sd   = 0;
    m.do_not_data_pull = 0;
    m.opcode           = gem5ToSccSnp(msg->gettype());
    return m;
}

// ─────────────────────────────────────────────────────────────────────────────
// Opcode conversion table: SCC integer → gem5 CHIRequestType
// (mirrors chi_vip_controller.cc scc_to_gem5req)
// ─────────────────────────────────────────────────────────────────────────────
CHIRequestType
VIPController::sccToGem5Req(uint8_t op)
{
    // chi::req_optype_e integers from IHI0050 / scc header
    switch (op) {
    case 0x01: return CHIRequestType_ReadShared;
    case 0x02: return CHIRequestType_ReadShared;    // ReadClean not in this build's enum
    case 0x03: return CHIRequestType_ReadOnce;
    case 0x07: return CHIRequestType_ReadNotSharedDirty;
    case 0x0B: return CHIRequestType_ReadUnique;
    case 0x11: return CHIRequestType_CleanUnique;
    case 0x15: return CHIRequestType_MakeReadUnique;
    case 0x17: return CHIRequestType_Evict;
    case 0x1B: return CHIRequestType_WriteBackFull;
    case 0x1C: return CHIRequestType_WriteCleanFull;
    case 0x1D: return CHIRequestType_WriteEvictFull;
    case 0x20: return CHIRequestType_WriteUniquePtl;
    case 0x21: return CHIRequestType_WriteUniqueFull;
    case 0x22: return CHIRequestType_WriteUniqueZero;
    case 0x23: return CHIRequestType_WriteNoSnpPtl;
    case 0x24: return CHIRequestType_WriteNoSnp;
    case 0x30: return CHIRequestType_ReadNoSnp;
    case 0x31: return CHIRequestType_ReadNoSnpSep;
    case 0x36: return CHIRequestType_StashOnceShared;
    case 0x37: return CHIRequestType_StashOnceUnique;
    case 0x38: return CHIRequestType_DvmOpNonSync;
    case 0x40: return CHIRequestType_AtomicLoad;
    case 0x44: return CHIRequestType_AtomicStore;
    case 0x48: return CHIRequestType_AtomicReturn;
    case 0x49: return CHIRequestType_AtomicNoReturn;
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
