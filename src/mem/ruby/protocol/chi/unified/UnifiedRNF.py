# Unified L1I + L1D + L2 CHI request node (RN-F).
#
# Replaces a core's Ruby Sequencer and its L1I, L1D and L2 CHI controllers
# with one C++ controller. The CPU calls it directly (FastMem interface):
# hits resolve in one table lookup with no events; misses, writebacks and
# snoops are real CHI messages on the Ruby network towards the HN-F.
# L2 is inclusive of both L1s. Latencies are in this controller's cycles.

from m5.objects.CHIGeneric import CHIGenericController
from m5.objects.ReplacementPolicies import RRIPRP, TreePLRURP
from m5.params import *


class UnifiedRNF(CHIGenericController):
    type = "UnifiedRNF"
    cxx_header = "mem/ruby/protocol/chi/unified/UnifiedRNF.hh"
    cxx_class = "gem5::ruby::UnifiedRNF"

    # Geometry (set index = line address bits above the offset, as RubyCache)
    l1d_size = Param.MemorySize("32KiB", "L1D size")
    l1d_assoc = Param.Unsigned(8, "L1D associativity")
    l1i_size = Param.MemorySize("32KiB", "L1I size")
    l1i_assoc = Param.Unsigned(8, "L1I associativity")
    l2_size = Param.MemorySize("256KiB", "L2 size")
    l2_assoc = Param.Unsigned(8, "L2 associativity")
    # TreePLRU builds one tree per num_leaves entries: num_leaves must equal
    # the array's associativity (RubyCache gets it from Parent.assoc).
    l1d_replacement_policy = Param.BaseReplacementPolicy(
        TreePLRURP(num_leaves=8), "L1D replacement")
    l1i_replacement_policy = Param.BaseReplacementPolicy(
        TreePLRURP(num_leaves=8), "L1I replacement")
    l2_replacement_policy = Param.BaseReplacementPolicy(
        RRIPRP(), "L2 replacement")

    # Resource pools (Ruby TBE tables)
    num_l1d_tbes = Param.Unsigned(8, "L1D misses in flight (incl. L2 hits)")
    num_l1i_tbes = Param.Unsigned(8, "L1I misses in flight (incl. L2 hits)")
    num_req_tbes = Param.Unsigned(8, "L2 request TBEs = MSHRs towards L3")
    num_repl_tbes = Param.Unsigned(8, "L2 replacement TBEs (writebacks)")
    num_snp_tbes = Param.Unsigned(4, "L2 snoop TBEs")
    max_outstanding_requests = Param.Unsigned(16,
        "Per side (data, fetch), as RubySequencer: accesses accepted and "
        "not yet completed, including ones coalesced on a busy line")

    hits_use_tbe = Param.Bool(True,
        "L1 hits hold an L1 TBE for their latency, as gem5's CHI L1")
    allow_SD = Param.Bool(False,
        "Read misses send ReadShared (else ReadNotSharedDirty)")

    # Defaults: measured on Ruby CHI with ProtocolTrace (repo-l3 hierarchy,
    # core 2 GHz, L1/L2 routers on the core clock: replay_chi.py
    # --l1l2-link-clock core; NoC/L3 1 GHz); see doc/unified_rnf.md.
    # Core side: request issued -> data usable by the core
    l1d_hit_latency = Param.Cycles(5, "L1D hit")
    l1i_hit_latency = Param.Cycles(5, "L1I hit")
    l2_hit_latency = Param.Cycles(26, "L1 miss, L2 hit")
    l2_upgrade_latency = Param.Cycles(17,
        "Store to a line L1D holds shared while L2 holds it unique")
    # Towards the HN-F
    miss_issue_latency = Param.Cycles(12,
        "Request issued by the core -> REQ leaves the RN-F")
    ack_latency = Param.Cycles(0, "First data beat / Comp -> CompAck sent")
    fill_latency = Param.Cycles(14,
        "Last data beat -> request TBE (MSHR) freed and waiting accesses "
        "of the other side retried. Ruby frees the L2 TBE 25 cycles after "
        "the data but allocates it 11 cycles after the request; this "
        "model allocates at the request, so 25 - 11 keeps the occupancy")
    resp_latency = Param.Cycles(9, "Last data beat -> data at the core")
    # L2 victim (measured on Ruby, parsec-blackscholes): the replacement
    # starts after the fill; with no L1 copy the REQ follows at once, else
    # the L2 first back-invalidates the L1s (SnpCleanInvalid down the link,
    # the L1 handles it as an invalidating snoop, response or data up) and
    # sends its REQ when they have answered.
    victim_start_latency = Param.Cycles(10,
        "L2 fill -> replacement of its victim starts")
    victim_evict_latency = Param.Cycles(2,
        "Replacement starts -> Evict sent (no L1 copy)")
    victim_wb_latency = Param.Cycles(4,
        "Replacement starts -> WriteBack/WriteEvict sent (no L1 copy)")
    backinv_send_latency = Param.Cycles(2,
        "Replacement starts -> SnpCleanInvalid sent to the L1")
    backinv_data_latency = Param.Cycles(2,
        "L1's data (last beat) at the L2 -> REQ sent; 0 after a response")
    wb_data_latency = Param.Cycles(2, "CompDBIDResp -> CBWrData sent")
    # L1 <-> L2 links. Ruby's repo-l3 hierarchy connects each L1 to the L2
    # through routers (16 B per router cycle, all CHI channels sharing a
    # link). With the routers on the core clock a control message holds a
    # link 0.5 cycle, a DAT beat 4.5 (Ruby charges a full line + header per
    # beat). The L2 -> L3 uplinks stay on the NoC clock.
    # When on, the L2-hit, upgrade, issue and response latencies above are
    # replaced by link slots (earliest free, per direction and per L1, plus
    # the L2's input link shared with traffic from the L3) and fixed hop and
    # L2 processing times. Hop/processing defaults reproduce the idle
    # latencies measured on Ruby (26 / 17 / 12 / 9 cycles). They assume
    # core-clocked L1/L2 routers; for NoC-clocked ones (--l1l2-link-clock
    # noc) double the hop latencies and set link_bytes_per_cycle = 8.
    model_l1l2_links = Param.Bool(True, "Model the L1<->L2 links")
    l1_send_latency = Param.Cycles(1, "Core request -> L1 sends to L2")
    up_pre_latency = Param.Cycles(2, "L1 -> its router's link")
    up_mid_latency = Param.Cycles(2, "L1 router's link -> L2 input link")
    up_post_latency = Param.Cycles(1, "L2 input link -> L2")
    down_pre_latency = Param.Cycles(2, "L2 -> link towards the L1")
    down_post_latency = Param.Cycles(3, "Link towards the L1 -> L1")
    # Link occupancy per message, as Ruby's throttle charges it: bytes /
    # (bytes per cycle of this RN-F's clock). A DAT beat is charged a full
    # line + header whatever the channel width.
    link_bytes_per_cycle = Param.Unsigned(16, "L1<->L2 link bandwidth")
    ctrl_msg_bytes = Param.Unsigned(8, "Link bytes of a control message")
    data_msg_bytes = Param.Unsigned(72, "Link bytes of one DAT beat")
    l2_hit_proc_latency = Param.Cycles(11, "L2 gets the read -> sends data")
    l2_upgrade_proc_latency = Param.Cycles(6,
        "L2 gets an L1 CleanUnique it can grant -> sends Comp")
    l2_miss_proc_latency = Param.Cycles(6,
        "L2 gets the read -> REQ to the L3")
    l2_fwd_latency = Param.Cycles(0,
        "L2 has the line from the L3 -> sends it to the L1")
    l1_evict_proc_latency = Param.Cycles(2,
        "L2 gets an L1 writeback/evict -> CompDBIDResp/Comp")
    l1_wbdata_latency = Param.Cycles(1, "L1 gets CompDBIDResp -> data")

    # Snoops (measured on Ruby, parsec-canneal-4w): the L2 pipeline, then,
    # if an L1 holds the line in a state the snoop must reach, the snoop goes
    # down the L1 link, the L1 handles it and its response (data if the L1
    # held the line unique) comes back up the link; the L2 then responds.
    # Without the link model the L1 hops are fixed (down 5, up 5, +4 for
    # the second data beat).
    snp_latency = Param.Cycles(5,
        "Non-invalidating snoop received -> sent to the L1, or response "
        "sent if the L2 handles it alone")
    snp_inv_latency = Param.Cycles(8, "Same, invalidating snoop")
    snp_l1_latency = Param.Cycles(6,
        "L1 gets a non-invalidating snoop -> sends its response")
    snp_l1_inv_latency = Param.Cycles(3,
        "L1 gets an invalidating snoop -> sends its response")
    snp_resp_latency = Param.Cycles(0,
        "L1's response (last beat) at the L2 -> L2 sends its response")

    # Occupancy sampler (0 = off): one CSV row per period, time-averaged
    sample_period = Param.Cycles(0, "Occupancy sample period")
    sample_file = Param.String("", "CSV name in the output dir "
                               "(default: <name>.occupancy.csv)")
