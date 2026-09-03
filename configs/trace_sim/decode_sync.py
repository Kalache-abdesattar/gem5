#!/usr/bin/env python3
"""Decode a trace_sim synchronisation trace to text.

Needs the Python protobuf runtime and sync_event_pb2 (generate with:
    protoc --python_out=<dir> -I src/proto src/proto/sync_event.proto )

Usage: decode_sync.py <cpuN.sync.trace.gz> [sync_event_pb2 dir]
"""
import gzip, struct, sys, os

TYPES = {0: "parallel", 1: "critical", 2: "barrier"}


def read_varint(f):
    shift = result = 0
    while True:
        b = f.read(1)
        if not b:
            return None
        b = b[0]
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result
        shift += 7


def main():
    path = sys.argv[1]
    if len(sys.argv) > 2:
        sys.path.insert(0, sys.argv[2])
    import sync_event_pb2

    with gzip.open(path, "rb") as f:
        magic = f.read(4)
        if magic != b"gem5":
            sys.exit(f"bad magic {magic!r}; not a gem5 protobuf stream")
        n = read_varint(f)
        hdr = sync_event_pb2.SyncEventHeader()
        hdr.ParseFromString(f.read(n))
        print(f"# obj_id={hdr.obj_id} tick_freq={hdr.tick_freq}")
        print(f"{'tick':>12} {'type':<9} {'edge':<6} {'tid':>4} "
              f"{'insts':>9} {'deps':>9}  {'d_insts':>8} {'d_deps':>8}")
        pi = pd = 0
        count = 0
        while True:
            n = read_varint(f)
            if n is None:
                break
            ev = sync_event_pb2.SyncEvent()
            ev.ParseFromString(f.read(n))
            edge = "end" if ev.is_end else "begin"
            print(f"{ev.tick:>12} {TYPES.get(ev.type, ev.type):<9} {edge:<6} "
                  f"{ev.tid:>4} {ev.num_insts:>9} {ev.num_deps:>9} "
                  f" {ev.num_insts-pi:>8} {ev.num_deps-pd:>8}")
            pi, pd = ev.num_insts, ev.num_deps
            count += 1
        print(f"# {count} sync records")


if __name__ == "__main__":
    main()
