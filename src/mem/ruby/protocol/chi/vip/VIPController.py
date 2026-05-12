# VIPController.py — gem5 Python SimObject for the CHI VIP co-simulation bridge
#
# This is the concrete CHIGenericController that owns the POSIX shared-memory
# segment used to communicate with a Questa vsim instance running questa_top_ipc.
#
# Usage in a config script:
#
#   from m5.objects.VIPController import VIPController
#
#   ctrl = VIPController(
#       version     = vip_node.version,
#       ruby_system = ruby_system,
#       shm_name    = "/chi_vip_0",  # match the name given to questa_top_ipc
#   )

from m5.objects.CHIGeneric import CHIGenericController
from m5.params import *


class VIPController(CHIGenericController):
    type       = "VIPController"
    cxx_header = "mem/ruby/protocol/chi/vip/VIPController.hh"
    cxx_class  = "gem5::ruby::VIPController"

    # Name of the POSIX shared-memory segment (must begin with '/').
    # The VIPController with rnf_index=0 creates it; all others open it.
    shm_name = Param.String("/chi_vip_0",
        "POSIX shm segment name for gem5 ↔ Questa IPC")

    # Index into the per-RN-F q2g ring buffer arrays (0-based).
    # Must be unique across VIPControllers sharing the same shm_name.
    rnf_index = Param.Int(0,
        "Index into per-RN-F q2g ring buffer arrays (0 = creator)")

    # RTL CHI NodeID of the RN-F this VIPController proxies.
    # Written into shm->rnf_nid[rnf_index] at init so questa_top_ipc
    # can route incoming flits by src_id to the correct q2g slot.
    rtl_src_id = Param.Int(0,
        "RTL CHI NodeID of the RN-F this controller proxies")
