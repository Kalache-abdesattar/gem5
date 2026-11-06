from m5.objects.ClockedObject import ClockedObject
from m5.params import *
from m5.proxy import *


class IntervalCPU(ClockedObject):
    """Mechanistic out-of-order core driven by a micro-op stream.

    Reads the same cpuN.data.trace.gz files as TraceCPU but uses only what
    describes the program -- record type, physical address, size, PC and
    register dependencies. comp_delay and rob_dep, which encode the timing of
    whichever core produced the trace, are ignored: timing is computed here
    from dispatch/issue/commit widths, window sizes and a fixed compute
    latency, with every load, store and instruction fetch going through the
    real memory system. This is the instruction-window form of the interval
    model (Sniper's 'rob' core type).

    Memory disambiguation follows O3's store-set predictor (memDepPred).
    Not modelled: branch prediction (the traces carry no branch outcomes)
    (store-to-load forwarding is modelled).
    """

    type = "IntervalCPU"
    cxx_header = "cpu/interval/interval_cpu.hh"
    cxx_class = "gem5::IntervalCPU"

    @classmethod
    def memory_mode(cls):
        return "timing"

    @classmethod
    def require_caches(cls):
        return True

    system = Param.System(Parent.any, "system object")
    icache_port = RequestPort("Instruction fetch port")
    dcache_port = RequestPort("Data port")

    dataTraceFile = Param.String("Micro-op stream (InstDepRecord protobuf)")

    # Multithreaded replay. Each CPU replays one thread's trace; threadId is
    # that thread's number, which spawn/join events in other threads' sync
    # traces refer to. Event semantics follow TraceCPU (barriers, critical
    # sections) plus thread creation and join.
    syncTraceFile = Param.String(
        "", "SyncEvent trace for this thread; empty = no synchronisation"
    )
    arbiter = Param.SyncArbiter(
        NULL, "Arbiter shared by all CPUs; required with syncTraceFile"
    )
    threadId = Param.Unsigned(0, "Thread number of the trace replayed here")

    dispatchWidth = Param.Unsigned(4, "Micro-ops dispatched per cycle")
    issueWidth = Param.Unsigned(6, "Micro-ops issued per cycle")
    commitWidth = Param.Unsigned(4, "Micro-ops committed per cycle")
    robSize = Param.Unsigned(192, "Reorder buffer entries")
    iqSize = Param.Unsigned(64, "Issue queue entries")
    lqSize = Param.Unsigned(32, "Load queue entries")
    sqSize = Param.Unsigned(32, "Store queue entries")

    compLatency = Param.Cycles(
        1, "Execution latency of a COMP micro-op when fuModel is off"
    )
    fuModel = Param.Bool(
        True,
        "Model O3's x86 functional-unit pool (DefaultX86FUPool): each COMP's "
        "op class (carried in the record's flags by etrace -uoptable) picks "
        "its latency and unit; units are limited per cycle, divide and sqrt "
        "are not pipelined, loads and stores share 4 ports. Traces without "
        "op classes run every COMP as an integer ALU op.",
    )
    storeLatency = Param.Cycles(
        1, "Cycles from a store's issue until dependents see it complete"
    )

    fetchEnabled = Param.Bool(
        True,
        "Fetch each new instruction cache line through icache_port using "
        "the record's PC. False makes the front end ideal.",
    )
    fetchWidth = Param.Unsigned(
        8, "Micro-ops fetched per cycle (O3's fetchWidth)"
    )
    fetchQueueSize = Param.Unsigned(
        32, "Fetched micro-ops queued for decode (O3's fetchQueueSize); "
        "frontendDepth * fetchWidth more are in the stage latches"
    )
    frontendDepth = Param.Cycles(
        2, "Cycles from fetch to dispatch (O3's fetchToDecodeDelay + "
        "decodeToRenameDelay; rename is where O3 checks for window space)"
    )
    lookahead = Param.Unsigned(
        256, "Micro-ops read ahead from the trace (no timing effect)"
    )

    # Memory dependence prediction, following O3's StoreSet + MemDepUnit.
    # Measured on synth l1: 98.8% of O3's loads wait on a predicted store,
    # and that load->add->store->next-load chain (4 cycles per 12-micro-op
    # iteration) is what holds O3 to 3.0 micro-ops/cycle.
    memDepPred = Param.Bool(
        True,
        "Store-set memory dependence prediction as in O3. False lets every "
        "load issue as soon as its address is ready.",
    )
    depCheckShift = Param.Unsigned(
        4,
        "Violations are detected on address >> depCheckShift (O3's "
        "LSQDepCheckShift), i.e. 16-byte blocks by default",
    )
    storeSetClearPeriod = Param.Unsigned(
        250000, "Stores dispatched between predictor clears (O3's "
        "store_set_clear_period; O3 counts stores only)"
    )
    LFSTSize = Param.Unsigned(1024, "Store sets (O3's LFSTSize)")
    tsoStores = Param.Bool(
        True,
        "Write committed stores to the cache one at a time, the next only "
        "after the previous is acknowledged (O3 with needsTSO, i.e. x86: "
        "LSQUnit::writebackStores). False sends each at commit.",
    )
    # O3 executes the load the cycle after issue and forwards with a
    # WritebackEvent, so dependents issue 2 cycles after the load: the
    # 2-cycle peak of issue-to-consumer-issue in a blackscholes O3PipeView.
    forwardLatency = Param.Cycles(
        2, "Load completion after forwarding from an older store's data "
        "(store queue or committed stores not yet sent), without a cache "
        "access"
    )
    loadIssueDelay = Param.Cycles(
        1,
        "Cycles from a load's issue to its cache request (O3's "
        "issueToExecuteDelay: the LSQ executes it the cycle after issue). "
        "On Ruby CHI, synth l1 runs 6 cycles per iteration on O3 and 5 "
        "without this."
    )
    commitDelay = Param.Cycles(
        2,
        "Cycles from completion until a micro-op may commit (O3: "
        "iewToCommitDelay, then commit; minimum complete-to-retire in a "
        "blackscholes O3PipeView is 2)"
    )
    storeWriteDelay = Param.Cycles(
        2,
        "Cycles from a store's commit until its write may be sent (O3: IEW "
        "learns of the commit commitToIEWDelay later and marks the store "
        "after that tick's writebackStores, so it goes the tick after). On "
        "Ruby the phase matters: a load to a line with a write in flight "
        "is coalesced behind it by the Sequencer and returns with it"
    )
    dispatchToIssueDelay = Param.Cycles(
        2,
        "Cycles from dispatch (O3's rename) until a micro-op can issue: "
        "O3's renameToIEWDelay; IEW may issue in the cycle it inserts"
    )
    staleOccupancy = Param.Bool(
        True,
        "Dispatch sees ROB/IQ/LQ/SQ occupancy as of the previous cycle plus "
        "what it has dispatched since, as O3's rename does from IEW and "
        "commit's time-buffered free-entry counts"
    )
    storeHoldsIq = Param.Bool(
        True,
        "A store keeps its IQ entry until it writes back, the cycle after "
        "issue (O3 frees memory instructions' entries in wakeDependents)"
    )
    branchPred = Param.Bool(
        True,
        "Predict branches as O3's default predictor does (TournamentBP, "
        "4096-entry BTB, 16-entry RAS, SimpleIndirectPredictor) using the "
        "branch kind and outcome etrace records; fetch stops at a "
        "mispredicted branch until it is redirected. Traces without branch "
        "information predict nothing."
    )
    branchRedirectDelay = Param.Cycles(
        3,
        "Cycles from a mispredicted branch's execution until fetch "
        "restarts (O3: iewToCommitDelay + commitToFetchDelay + a squashing "
        "cycle; complete-to-refetch is 3 or 6 cycles in an O3PipeView)"
    )
    skidModel = Param.Bool(
        True,
        "Model O3's rename/decode block-unblock protocol: after a full "
        "ROB/IQ/LQ/SQ, rename renames only its skid buffer, then decode "
        "resumes 2 cycles and fetch's output 4 cycles later. A blackscholes "
        "worker in O3 renames in 1-2 cycle bursts of 8 with 2+ cycle gaps "
        "(rename unblocking 41% of cycles)"
    )
    skidSize = Param.Unsigned(
        16, "Rename and decode skid buffer size in micro-ops (gem5: "
        "(decodeToRenameDelay + 1) x decodeWidth)"
    )
    squashUnblockDelay = Param.Cycles(
        6,
        "Cycles from the end of a ROB squash until rename renames again "
        "(the IEW stall clearing through rename and decode; measured 6 in "
        "O3 debug traces)"
    )
    squashWidth = Param.Unsigned(
        8,
        "Wrong-path micro-ops commit removes from the ROB per cycle; rename "
        "is stalled until it finishes (O3's squashWidth)"
    )
    decodeRedirectDelay = Param.Cycles(
        2,
        "Cycles from fetching a direct branch with a wrong predicted target "
        "until fetch restarts (O3 decode: fetchToDecodeDelay + "
        "decodeToFetchDelay)"
    )
    loadWakeNextEdge = Param.Bool(
        False,
        "A load response arriving exactly on a clock edge wakes dependents "
        "from the next edge (O3 writes back in the following IEW cycle)"
    )
    squashPenalty = Param.Cycles(
        10, "Dispatch bubble after a memory-order violation"
    )
