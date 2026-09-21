"""gem5's own MemTest coherence stressor on the stdlib CHI hierarchy.

MemTest generates deliberately-conflicting shared accesses and checks the
values it reads back. If THIS asserts, the CHI protocol is breaking on traffic
gem5 itself generates, independently of any Pin trace.
"""
import os, sys
sys.path.insert(0, "/home/dmk/fedora/gem5_co_simulation/gem5/configs/trace_sim")

import m5
from m5.objects import *
from trace_board import TraceBoard, TraceCore, TraceProcessor
from gem5.isas import ISA

NUM = 2
DMT_SETTING = (os.environ.get("DMT","0") == "1")
# MemTest has a single port, so pair each with an idle generator for the
# I-sequencer, exactly as inject_chi.py does.
testers = [MemTest(max_loads=20000, percent_functional=0,
                   percent_uncacheable=0, progress_interval=0,
                   suppress_func_errors=True)
           for _ in range(NUM)]
idles = [PyTrafficGen() for _ in range(NUM)]

board = TraceBoard(testers, ISA.X86, mem_size="512MB")
board.idle = idles
cores = []
for t, i in zip(testers, idles):
    c = TraceCore(t, ISA.X86)
    c.connect_icache = (lambda port, g=i: setattr(g, "port", port))
    cores.append(c)
board._processor = TraceProcessor(cores, ISA.X86)

if os.environ.get("HIER","l1") == "l2":
    from gem5.components.cachehierarchies.chi.private_l1_shared_l2_cache_hierarchy import (
        PrivateL1SharedL2CacheHierarchy)
    h = PrivateL1SharedL2CacheHierarchy(size="32KiB", assoc=8,
                                        l2_size="256KiB", l2_assoc=8)
else:
    from gem5.components.cachehierarchies.chi.private_l1_cache_hierarchy import (
        PrivateL1CacheHierarchy)
    h = PrivateL1CacheHierarchy(size="32KiB", assoc=8)
board.cache_hierarchy = h
h.incorporate_cache(board)

d = getattr(h, "directory", None)
for dd in (d if isinstance(d, list) else [d]):
    if dd is not None and hasattr(dd, "enable_DMT"):
        dd.enable_DMT = DMT_SETTING

root = Root(full_system=False, system=board)
root.system.mem_mode = "timing"
m5.instantiate()
for g in idles:
    g.start((lambda gg: (yield gg.createIdle(int(1e13))))(g))
print(f"[memtest] {NUM} MemTest agents on stdlib CHI, shared address space")
e = m5.simulate(int(2e12))
print(f"[memtest] done @ {m5.curTick()}: {e.getCause()}")
