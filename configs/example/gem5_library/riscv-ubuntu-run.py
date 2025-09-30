# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""
This script shows an example of running a full system RISCV Ubuntu boot
simulation using the gem5 library. This simulation boots Ubuntu 20.04 using
2 TIMING CPU cores. The simulation ends when the startup is completed
successfully.

Usage
-----

```
scons build/RISCV/gem5.opt
./build/RISCV/gem5.opt \
    configs/example/gem5_library/riscv-ubuntu-run.py
```
"""

import m5
from m5.objects import Root

from gem5.components.boards.riscv_board import RiscvBoard
from gem5.components.memory import (DualChannelDDR4_2400, SingleChannelDDR3_1600,)
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import(obtain_resource, DiskImageResource,
    KernelResource)
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

# This runs a check to ensure the gem5 binary is compiled for RISCV.

requires(isa_required=ISA.RISCV)

# With RISCV, we use simple caches.
from gem5.components.cachehierarchies.classic.private_l1_private_l2_walk_cache_hierarchy import (
    PrivateL1PrivateL2WalkCacheHierarchy,
)

from gem5.components.cachehierarchies.chi.l3_cache_hierarchy import (
    L3CacheHierarchy,
)

from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)

from gem5.components.cachehierarchies.classic.no_cache import (
    NoCache,
)
# Here we setup the parameters of the l1 and l2 caches.
# cache_hierarchy = PrivateL1PrivateL2WalkCacheHierarchy(
#     l1d_size="16KiB", l1i_size="16KiB", l2_size="256KiB"
# )

cache_hierarchy = L3CacheHierarchy(
    l1_size="16KiB", l1_assoc=8, l2_size="1MiB", l2_assoc=16, l3_size="16MiB", l3_assoc=32, cores_per_cluster=1)

cache_hierarchy = NoCache()

# Memory: Dual Channel DDR4 2400 DRAM device.

memory = DualChannelDDR4_2400(size="3GiB")

# Setup the system memory.
memory = SingleChannelDDR3_1600()


# Here we setup the processor. We use a simple processor.
# processor = SimpleProcessor(
#     cpu_type=CPUTypes.TIMING, isa=ISA.RISCV, num_cores=4
# )

processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.ATOMIC,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.RISCV,
    num_cores=4,
)


default_args = [
            "console=ttyS0",
            "root=/dev/vda1",
            "disk_device=/dev/vda1",
            "rw",
            "no_systemd=true",      # Disable systemd
            "interactive=false"      # Enable interactive shell
        ]



# Here we setup the board. The RiscvBoard allows for Full-System RISCV
# simulations.
board = RiscvBoard(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    new_kernel_args=default_args
)


# Here we a full system workload: "riscv-ubuntu-24.04-img" which boots
# Ubuntu 24.04. Once the system successfully boots it encounters an `m5_exit`
# instruction which stops the simulation. When the simulation has ended you may
# inspect `m5out/system.pc.com_1.device` to see the stdout.

# add command to be executed immediately after boot
command = ()

board.set_kernel_disk_workload(
    
    
    kernel=obtain_resource(
        "riscv-linux-6.6.33-kernel", resource_version="1.0.0"
    ),

    bootloader=obtain_resource("riscv-bootloader-opensbi-1.3.1", resource_version="1.0.0"),

    # disk image is currently stored locally in /mnt within docker
    disk_image=DiskImageResource(local_path="/mnt/riscv-ubuntu-24.04-img"),

    readfile_contents=command,
)



def exit_event_handler():
    print("First exit: kernel booted")
    yield False  # gem5 is now executing systemd startup


    print("Second exit: Started `after_boot.sh` script")
    # The after_boot.sh script is executed after the kernel and systemd have
    # booted.
    yield False  # gem5 is now executing the `after_boot.sh` script
    print("Third exit: Finished `after_boot.sh` script")
    # The after_boot.sh script will run a script if it is passed via
    # m5 readfile. This is the last exit event before the simulation exits.
    
    print("Take a checkpoint")
    simulator.save_checkpoint("riscv_ubuntu_checkpoint")

    # processor.switch()
    
    yield True


simulator = Simulator(
    board=board,
    on_exit_event={
        # Here we want override the default behavior for the first m5 exit
        # exit event.
        ExitEvent.EXIT: exit_event_handler()
    },

    checkpoint_path="/opt/gem5/riscv_ubuntu_checkpoint",
)
simulator.run()
