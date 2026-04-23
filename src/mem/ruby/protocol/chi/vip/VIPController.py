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
    # gem5 creates it; questa_top_ipc attaches.
    shm_name = Param.String("/chi_vip_0",
        "POSIX shm segment name for gem5 ↔ Questa IPC")
