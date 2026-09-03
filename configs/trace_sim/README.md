# trace_sim — ElasticSimMATE-style trace-driven multicore simulation

Goal: reproduce the approach of Nocua et al., *ElasticSimMATE: a Fast and
Accurate gem5 Trace-Driven Simulator for Multicore Systems* (ReCoSoC 2017) —
Elastic Traces extended with inter-core synchronisation so that multicore
replay is meaningful.

Built incrementally. Each step is independently runnable.

## Why stock gem5 isn't enough

gem5 ships Elastic Traces (`ElasticTrace` probe + `TraceCPU`), but:

* `configs/common/CpuConfig.py:config_etrace()` assigns **the same trace
  filename to every CPU**, with an upstream comment saying it "must be
  revisited when creating elastic traces for multi processor systems".
* `TraceCPU` has no notion of another TraceCPU. Replaying N traces gives N
  independent streams that never wait for each other, so every barrier,
  lock and condvar in the original program silently disappears.

ElasticSimMATE's contribution is a third trace — synchronisation events — plus
an arbiter that stalls TraceCPUs at the recorded sync points.

## Steps

| Step | State | What it adds |
|------|-------|--------------|
| 1. Single-core capture + replay | **working, validated** | `capture.py`, `replay.py`; validates the ET pipeline end to end and gives a replay-vs-O3 accuracy baseline |
| 2. Per-core traces, N-core replay | **working** | one trace pair per core; N TraceCPUs sharing a cache hierarchy. Still *no* sync — the known-wrong baseline that motivates step 3 |
| 3. Synchronisation trace | planned | record sync events (barrier / critical / parallel-region) with instruction + dependency counts per region |
| 4. Arbiter | planned | a SimObject that locks/unlocks TraceCPUs at region boundaries, per Fig. 4b of the paper |
| 5. Replication | planned | replay an N-core trace onto M > N cores for weak-scaling studies |
| 6. CHI / Ruby back end | planned | swap the classic hierarchy for the CHI setup this repo already uses |

## Prerequisite: protobuf

Elastic Traces needs protobuf. This tree previously capped `protoc` at 3.99.99
in `src/proto/SConsopts`, which disabled `TraceCPU` and `ElasticTrace`
entirely (`HAVE_PROTOBUF 0`). The cap is lifted on this branch: protoc 33.1 was
verified to generate and compile gem5's `.proto` files under C++17, and
`src/proto/protobuf.cc` builds against libprotobuf 33.1 unchanged.

Verify with:

```
build/X86_CHI/gem5.opt --quiet -c \
  'import m5.objects; print(hasattr(m5.objects,"TraceCPU"))'
```

## Usage

Capture (reference run, O3 cores):

```
build/X86_CHI/gem5.opt --outdir=m5out/capture \
    configs/trace_sim/capture.py \
    --cmd tests/test-progs/hello/bin/x86/linux/hello \
    --num-cores 1
```

Replay (TraceCPU, same memory system):

```
build/X86_CHI/gem5.opt --outdir=m5out/replay \
    configs/trace_sim/replay.py \
    --trace-dir m5out/capture --num-cores 1
```

Compare `simTicks` / `system.l2.overallMisses` between the two directories.
Elastic Traces claims <10% error and 6–8x speedup versus O3 on a single core.

## Notes on fidelity

Capture deliberately runs with an oversized ROB/LQ/SQ (512/128/128) so the
reference run does not stall on resource limits — such stalls would be baked
into the trace as compute delay. The real sizes are modelled at replay by
TraceCPU (`--rob/--lq/--sq`).


## Validated results (step 1)

x86, `tests/test-progs/hello`, classic L1+L2, ROB/LQ/SQ matched at 192/32/32
between the reference O3 run and the TraceCPU replay:

| | simTicks | hostSeconds | L2 misses |
|---|---|---|---|
| Reference O3 (`--no-etrace`) | 16,991,500 | 0.06 | 410 |
| TraceCPU replay | 17,009,000 | 0.01 | 406 |

**+0.1% error, 6.0x speedup** — in line with the Elastic Traces paper's <10%
error and 6-8x speedup.

Getting a meaningful error number requires three runs, not two:

1. `--no-etrace` -> reference, stock O3 sizing (192/32/32)
2. default -> capture, ROB/LQ/SQ deliberately oversized to 512/128/128 so the
   reference run does not stall on resources
3. `replay.py --rob 192 --lq 32 --sq 32` -> replay, sized to match run 1

Comparing the replay against the *capture* run instead of the reference gives a
meaningless -34% because the capture CPU is 512-wide.

Step 2 (2 cores, `--maxinsts 4000`): four distinct per-core traces, both
TraceCPUs replay through a shared L2, 12,531,000 ticks / 577 L2 misses. No
synchronisation yet -- the streams are independent, which is exactly the
limitation step 3/4 exist to remove.

## Fixes required to gem5 itself

`ElasticTrace` had been compiled out of this tree for long enough to bit-rot
against the probe API. On this branch:

* `src/proto/SConsopts` -- lifted the protoc <= 3.99.99 cap (see above).
* `src/cpu/o3/probe/elastic_trace.{cc,hh}` -- the O3 probe points are
  `ProbePointArg<DynInstPtr>`, but ElasticTrace connected listeners as
  `DynInstConstPtr`, so every registration hit
  "Wrong type of listener" at startup. Switched to `DynInstPtr`.
* `src/cpu/o3/probe/elastic_trace.hh` -- `SeqNumRegPair` was
  `pair<InstSeqNum, RegIndex>`; the `SquashInRename` probe point now carries
  `pair<InstSeqNum, PhysRegIdPtr>`. Matched it, and key `physRegDepMap` off
  `->flatIndex()` as the insert path already did.
* `elastic_trace.cc:flushTraces()` -- made idempotent (it deleted the stream
  objects without nulling them). Defensive; it was not the cause of the crash
  below.

## Known limitation: multi-process SE teardown

Running N cores each with its *own* SE process segfaults at exit inside stock
gem5, in `o3::Commit::clearStates` via `CPU::exitThreads` ->
`haltContext` -> `removeThread`. It reproduces with `--no-etrace`, i.e. with no
tracing involved at all, so it is not an elastic-trace bug. Because the crash is
inside `doSimLoop`, `simulate()` never returns and the exit callbacks that flush
the protobuf streams never run, leaving truncated traces.

Workaround: bound the capture with `--maxinsts` so the run ends on
"max instruction count" before the processes exit. This is what you want for
trace capture anyway -- you trace a region of interest, not a whole program.
This goes away in FS mode, which is what the paper uses and what step 3 needs.
