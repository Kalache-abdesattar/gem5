# Handoff: making Pin-derived elastic traces temporally accurate

Status as of 2026-10-23. Read this before touching `etrace_to_elastic.py`.

## Goal

Generate gem5 elastic traces from Intel Pin traces that replay to the *same
simulated time* as the O3 run they represent. Structural fidelity is largely
solved; temporal fidelity is not.

## Scoreboard (single-core `l1` kernel)

O3 reference: **3,389,734,000 ticks**. Error of each replay against it:

| trace | error |
|---|---|
| gem5's own captured trace (control) | **-0.00007%** |
| Pin, original (no uop cracking, flat delay) | -55.64% |
| Pin, uncracked + delay table | -15.15% |
| **Pin, cracked + flat 500** | **-20.72%** (best Pin result) |
| Pin, cracked + gem5-derived delay table | +307.33% |
| Pin, cracked + scheduler model (`sched`) | +179.90% |
| Pin, cracked + issue-order edges | +544.45% |

Four successive "principled" delay models each did worse than a flat constant.
The reason is in the next section and it is now understood.

## The finding that changes everything (measured, not inferred)

**1. The replay clock period is 500 ticks** (`clock=500`, 2 GHz) and
`TraceCPU` quantises every event to a clock edge
(`src/cpu/trace/trace_cpu.cc:567` and `:817`, `std::max(execTick,
owner.clockEdge(Cycles(1)))`). So a flat delay has **no sub-cycle resolution**.
The calibration sweep that looked non-monotonic is just quantisation:

| flat delay | simTicks | effective | error |
|---|---|---|---|
| 450 | 2,687,417,950 | 1 cycle | -20.72% |
| 500 | 2,687,418,000 | 1 cycle | -20.72% |
| 550 | 5,263,178,550 | 2 cycles | +55.27% |
| 600 | 5,263,178,600 | 2 cycles | +55.27% |
| 630 | 5,263,178,630 | 2 cycles | +55.27% |
| 660 | 5,263,178,660 | 2 cycles | +55.27% |

`simTicks` is exactly `base + delay` with only two bases. Verified not to be a
trace-side effect: the 500 and 550 data traces are structurally identical
(both 20,013,311 records, 21,727,571 reg_dep, 4,873,600 rob_dep); only the
uniform delay differs. Verified not memory- or fetch-bound either: between the
two runs the dcache/icache miss latencies *fell* 15-22% while runtime doubled,
`instLastTick` is only ~24M ticks (vs 2.7e9 data side), and
`maxReadyListSize` is 33-35. The target needs an effective **1.26 cycles**,
which no single flat value can express.

**2. gem5's own delay distribution is per-type and quantised.** Measured over
the full reference trace
(`/home/dmk/fedora/pintools/traces/sizecap/l1.bcap/cpu0.data.trace.gz`):

- 19,099,119 records, total delay 28,298,225,992, mean 1481.7 ticks = **2.963 cycles**
- **99.997% of delays are exact multiples of 500** (5 records out of 19M are not)
- `COMP`  n=15,782,857  mean **0.775 cyc** -- 32% at **0**, 63% at 1, 5% at 2
- `LOAD`  n= 1,661,666  mean **23.705 cyc** -- **98% at 24 cycles (12,000 ticks)**
- `STORE` n= 1,654,596  mean **3.005 cyc** -- **99% at 3 cycles (1,500 ticks)**

`flat 500` therefore under-delays loads by **24x**, under-delays stores by 3x,
and over-delays the third of COMPs that should be zero. Total Pin delay mass at
flat 500 is 10,006,655,500 against gem5's 28,298,225,992 -- only 35%.

**Important correction to earlier notes:** the 12,000-tick LOAD delay that a
previous session diagnosed as a derivation bug is *genuine* -- it is gem5's real
load delay. The bug was only ever that 12,000 was being applied to COMP
records too.

## The next experiment (was interrupted mid-run; start here)

Apply gem5's measured per-type delays directly. The converter already has the
flags, no code change needed:

```sh
cd /home/dmk/fedora/gem5_co_simulation/gem5
T=/home/dmk/fedora/pintools/tracewk
D=/home/dmk/fedora/pintools/pbenv

$D/venv/bin/python configs/trace_sim/etrace_to_elastic.py $T/crack $T/pertype \
  --pb2-dir $D/pb2 --delay-model flat \
  --comp-delay 500 --load-delay 12000 --store-delay 1500

build/X86_CHI/gem5.opt --outdir=$T/pertype.rep configs/trace_sim/replay.py \
  --trace-dir $T/pertype --num-cores 1 --rob 192 --lq 32 --sq 32 --mem-size 2GB
```

Then compare `simTicks` against 3,389,734,000. Also run the `--comp-delay 0`
variant: COMP's true mean is 0.775 cycles, so 0 and 500 bracket it, and the
answer tells you whether COMP needs the 0/1/2 mixture or a constant suffices.

A stale `$T/pertype` directory from the interrupted run exists -- delete it
first.

If the per-type constants land close, the remaining refinement is to reproduce
COMP's 0/1/2-cycle *mixture* rather than its mean, since quantisation means the
mean alone is not reachable. Deciding which COMPs get 0 is the open modelling
question (likely those issuing in the same cycle their producer completed,
i.e. chained COMPs with a register dependency on the immediately preceding uop).

## What is still structurally wrong

- `rob_dep` per record: Pin **0.244** vs gem5 **0.52**. Still the main
  structural gap.
- Record counts after uop cracking: Pin 20,013,311 vs gem5 19,099,119 (0.95x,
  was 1.43x short before cracking). LOAD and STORE counts now match almost
  exactly (1,667,019 vs 1,661,666; 1,654,631 vs 1,654,596) -- the excess is all
  in COMP (16,691,661 vs 15,782,857).
- `reg_dep` per record: Pin 1.086 vs gem5 ~1.08. Matched.
- The synthesised instruction-fetch trace is short and sparse: only **27,680
  records** spanning 13.8M-26.3M ticks, against a 3.39e9-tick data side. Not
  currently binding, so it has not caused a wrong answer, but
  `etrace_to_elastic.py:497` (`tick += args.comp_delay`) couples the fetch
  timeline to the data-side delay knob, which is wrong in principle and will
  bite if the fetch side ever becomes the constraint. `instLastTick` moved
  23.9M -> 26.3M across the 500/550 sweep purely from this coupling.

## Core conceptual constraint (do not re-derive this the hard way)

`comp_delay` is defined as the **minimum across a node's own dependency
edges** -- the gap from a producer's completion to this node's *issue*.
Therefore delays and edges must be derived **together**. Copying gem5's delay
values onto a different edge set over-serialises (this is why the table and
sched models exploded), and adding edges without matching delays is pure added
latency (this is why issue-order edges gave +544%).

## Dead ends -- do not repeat

- **Issue-order dependency edges** after filtering: serialises the stream.
  -20.72% -> -24.53% and +307% -> +544%. Reverted, with a comment in the source.
- **`_Sched` returning the absolute issue tick** for dependency-free records:
  produced 8e15 total delay. Fixed -- it now returns 0. Same class of mistake
  as ElasticTrace's own `firstWin` fallback.
- **`table3` falling back to a median-of-class-medians**: +228%, because
  classes with <300 samples were filtered so survivors skew slow. Now falls
  back to the 2-feature table.
- **Computing dependency distance from unfiltered deps**: charged 50,002
  records 12,000 ticks (83% of COMP delay mass). Must use *surviving* deps
  (`_lo = seq - args.window`).
- **Deriving the delay table from a fixed 3-4M record window**: excludes all
  steady-state data. Use proportional sampling (30/55/80%).
- **Comparing the first 4M *records* of two traces**: different program spans.
  Compare over full traces.

## Where everything is

| what | where |
|---|---|
| Pin tool source | `/home/dmk/fedora/pintools/etrace/etrace.cpp` -- **NOT under version control** |
| Pin raw trace (cracked) | `/home/dmk/fedora/pintools/tracewk/crack` |
| Converted traces + replay outdirs | `/home/dmk/fedora/pintools/tracewk/*`, `*.rep` |
| gem5 reference traces | `/home/dmk/fedora/pintools/traces/sizecap/{l1,l2,fp,seq,rand,stride,chase,store}.bcap` |
| protobuf python bindings | `/home/dmk/fedora/pintools/pbenv/pb2`, venv at `pbenv/venv` |
| converter | `gem5/configs/trace_sim/etrace_to_elastic.py` |
| findings doc (~1,800 lines) | `gem5/configs/trace_sim/README.md` |

`etrace.cpp` being untracked is a real risk -- consider committing it somewhere.

## Reading an elastic trace from Python

The only non-obvious part is the framing: a **4-byte magic prefix**, then a
varint-delimited header, then varint-delimited records.

```python
import gzip, sys
sys.path.insert(0, "/home/dmk/fedora/pintools/pbenv/pb2")
import inst_dep_record_pb2 as pb

def rv(f):                      # protobuf varint
    s = r = 0
    while True:
        b = f.read(1)
        if not b: return None
        b = b[0]; r |= (b & 0x7f) << s
        if not (b & 0x80): return r
        s += 7

f = gzip.open(path, "rb")
f.read(4)                       # magic -- easy to forget, gives "wire format corrupt"
n = rv(f); h = pb.InstDepRecordHeader(); h.ParseFromString(f.read(n))
while (n := rv(f)) is not None:
    m = pb.InstDepRecord(); m.ParseFromString(f.read(n))
    # m.type: 1=LOAD 2=STORE 3=COMP ; m.comp_delay ; m.reg_dep ; m.rob_dep
```

Helper scripts live in `/tmp` and the session scratchpad, both of which are
**tmpfs and will be lost**. `/tmp` has also been filled to EDQUOT by
GB-scale traces before -- keep large traces in
`/home/dmk/fedora/pintools/tracewk`.

## Uncommitted work

Nothing here has been committed. The user's standing rule is **do not commit
unless explicitly asked**.

`gem5/` (branch `trace_sim`):
```
M configs/trace_sim/README.md
M configs/trace_sim/etrace_to_elastic.py
M configs/trace_sim/o3_chi.py
M configs/trace_sim/replay_chi.py
M configs/trace_sim/replicate_traces.py
```

parent repo (branch `main`):
```
M config/chi/l3_cache_hierarchy.py
M config/chi/nodes/abstract_node.py
M config/chi/nodes/memory_controller.py
m gem5                        (submodule pointer)
```

Untracked outputs worth keeping: `out/flamegraphs/` (16 profiles, 14MB),
`out/accuracy16/`.

## Environment gotchas

- The `rm -rf` safety check blocks unguarded shell-variable expansion. Use
  `"${VAR:?}"` or literal paths. **Do not work around it.**
- Foreground `sleep` is blocked by the harness.
- gem5's `pre-commit` hook is unusable here (venv shebang
  `/workspace/venv/bin/python3`); `--no-verify` is required.
- Build target is `X86_CHI`, not RISCV.
