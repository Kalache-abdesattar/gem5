/*
 * VIPController.hh — gem5 SimObject that bridges the Ruby CHI network to a
 * Questa RTL DUT (RN-F) running in a separate vsim process via POSIX
 * shared-memory ring buffers (chi_ipc).
 *
 * Inheritance chain:
 *
 *   AbstractController          (gem5 Ruby)
 *       └── CHIGenericController  (CHI-specific base, provides 8 MessageBuffers)
 *               └── VIPController   (this file)
 *
 * Transport:
 *
 *   Inbound (DUT → gem5 HN-F): wakeup() drains q2g_req/q2g_rsp/q2g_dat from
 *   the shm segment, converts each ChiIpcXxx struct to a gem5 CHI message, and
 *   enqueues it on the appropriate MessageBuffer (reqOut / rspOut / datOut).
 *
 *   Outbound (gem5 HN-F → DUT): recvResponseMsg / recvDataMsg / recvSnoopMsg
 *   are called by CHIGenericController::wakeup() when the Ruby network delivers
 *   a message.  The methods convert the message to a ChiIpcXxx struct and push
 *   it into the g2q_rsp / g2q_dat / g2q_snp ring buffer.
 *
 *   wakeup() is driven by gem5's event queue; it is scheduled every tick while
 *   any ring buffer is non-empty.
 */

#ifndef __MEM_RUBY_PROTOCOL_CHI_VIP_VIPCONTROLLER_HH__
#define __MEM_RUBY_PROTOCOL_CHI_VIP_VIPCONTROLLER_HH__

#include <queue>
#include <string>
#include <unordered_map>

#include "mem/ruby/protocol/chi/generic/CHIGenericController.hh"
#include "mem/ruby/protocol/CHI/CHIDataType.hh"
#include "mem/ruby/protocol/CHI/CHIRequestType.hh"
#include "mem/ruby/protocol/CHI/CHIResponseType.hh"
#include "params/VIPController.hh"

// IPC layer (lives in chi_vip/ipc/; CPPPATH points to that directory)
#include "chi_ipc.hh"

namespace gem5 {
namespace ruby {

class VIPController : public CHIGenericController
{
  public:
    PARAMS(VIPController);
    VIPController(const Params& p);
    ~VIPController() = default;

    void init() override;
    void wakeup() override;

    // ── CHIGenericController pure virtuals ────────────────────────────────────
    bool recvRequestMsg(const CHIRequestMsg*  msg) override;
    bool recvSnoopMsg(const CHIRequestMsg*    msg) override;
    bool recvResponseMsg(const CHIResponseMsg* msg) override;
    bool recvDataMsg(const CHIDataMsg*         msg) override;

  private:
    chi_ipc::ChiShmHandle shm_;
    const int             cacheLineSz_;
    const int             rnf_index_;    // which q2g slot this VIP reads from
    const uint16_t        rtl_src_id_;  // RTL CHI NodeID of the RN-F we proxy
    const uint16_t        rtl_hnf_nid_; // RTL CHI NodeID of the HN-F (SRCID/HOMENID in outbound flits)
    const uint64_t        quantum_ps_;  // co-sim time-quantum size in ps

    // Staging queue for inbound requests: q2g_req is always drained into this
    // buffer; each wakeup() then flushes as many entries as reqOut can accept.
    // This prevents silent drops when reqOut is temporarily full.
    std::queue<CHIRequestMsgPtr> pendingReqs_;

    // Maps txnId → cache-line address for outstanding write transactions.
    // Populated in makeReqMsg; entries are removed when a write completes
    // (Comp or CompDBIDResp received).  Allows makeDatMsg to set the correct
    // addr on NCBWrData messages (DAT flits carry no address field).
    std::unordered_map<uint16_t, Addr> txnid_to_addr_;

    // Maps txnId → original byte-accurate request address (not cache-line
    // aligned).  Used in packDat to compute CCID correctly: gem5 SLICC always
    // emits CCID=0 in CHIDataMsg; we must reconstruct it from the request addr.
    std::unordered_map<uint16_t, Addr> txnid_to_acc_addr_;

    // Maps txnId → RTL SrcID of the original requester.  In multi-core RTL
    // designs (CORE_COUNT>1) each core has a different CHI NodeID; the ICN
    // routes inbound dat/rsp flits to the correct per-core RXDAT/RXRSP channel
    // based on TgtID.  Populated in makeReqMsg; used in packDat / packRsp so
    // CompData and Comp are delivered to the right core.
    std::unordered_map<uint16_t, uint16_t> txnid_to_src_id_;

    // Maps snoop TxnId → snoop cache-line address.  Populated in packSnp so
    // makeDatMsg can recover the correct address for SnpRespData/SnpRespDataPtl
    // flits (DAT flits carry no address; the txnId is the HN-F's snoop TxnId,
    // not a requestor write TxnId tracked by txnid_to_addr_).
    std::unordered_map<uint16_t, Addr> snp_txnid_to_addr_;

    // Snapshot of the most-recent complete cache-line data sent to the RTL,
    // keyed by cache-line-aligned address.  Used in makeDatMsg to reconstruct
    // the full line when the RTL returns a SnpRespDataPtl (partially dirty):
    // clean sublines from the RTL contain zeros; we restore them from this
    // cache so gem5 receives a full, valid cache line.
    std::unordered_map<Addr, std::vector<uint8_t>> addr_to_line_data_;

    // ── Inbound (q2g): IPC → gem5 message objects ─────────────────────────────
    CHIRequestMsgPtr  makeReqMsg(const chi_ipc::ChiIpcReq& m);
    CHIResponseMsgPtr makeRspMsg(const chi_ipc::ChiIpcRsp& m);
    // beat selects which dataChannelSize-byte slice of the IPC DAT to convert.
    CHIDataMsgPtr     makeDatMsg(const chi_ipc::ChiIpcDat& m, int beat, int beatSz);

    // ── Outbound (g2q): gem5 message objects → IPC ────────────────────────────
    chi_ipc::ChiIpcRsp packRsp(const CHIResponseMsg* msg);
    // beatSz = dataChannelSize; offset is read from the msg bitMask.
    chi_ipc::ChiIpcDat packDat(const CHIDataMsg* msg, int beatSz);
    chi_ipc::ChiIpcSnp packSnp(const CHIRequestMsg*  msg);

    // Opcode conversion tables (same as chi_vip_controller.cc shims)
    static CHI::CHIRequestType  sccToGem5Req(uint8_t op);
    static uint8_t              gem5ToSccRsp(CHI::CHIResponseType t,
                                             uint8_t& resp_out,
                                             uint16_t& db_id_out);
    static uint8_t              gem5ToSccDat(CHI::CHIDataType t,
                                             uint8_t& resp_out);
    static uint8_t              gem5ToSccSnp(CHI::CHIRequestType t);
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_PROTOCOL_CHI_VIP_VIPCONTROLLER_HH__
