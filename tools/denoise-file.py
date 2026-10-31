#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Denoise a recorded file with the capture plugin's engine. File only: no audio device,
PipeWire connection or playback. Input is mixed to mono and resampled to 48 kHz; the
output is 48 kHz mono, aligned by removing the model's algorithmic delay."""
import argparse
import ctypes as C
import json
from math import gcd
from pathlib import Path
import sys
import time

import numpy as np
from scipy.signal import resample_poly
import soundfile as sf

HOP = 480
DELAY = {"rnnoise": 480, "df3": 1440}


class Stats(C.Structure):
    _fields_ = [(name, C.c_uint64) for name in (
        "calls", "cpu_calls", "native_calls", "failed_calls", "total_ns", "max_ns", "cpu_ns", "last_ns")]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input", type=Path)
    p.add_argument("output", type=Path)
    p.add_argument("--model", choices=tuple(DELAY), default="rnnoise")
    p.add_argument("--library", type=Path, default=Path("/usr/local/lib/ladspa/libaurora_capture_ns.so"))
    a = p.parse_args()
    if a.output.resolve() == a.input.resolve():
        p.error("output must differ from input")
    lib = C.CDLL(str(a.library.resolve()))
    lib.ns_engine_new.argtypes = [C.c_int]
    lib.ns_engine_new.restype = C.c_void_p
    lib.ns_engine_free.argtypes = [C.c_void_p]
    lib.ns_engine_free.restype = None
    lib.ns_engine_process.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p]
    lib.ns_engine_stats.argtypes = [C.c_void_p, C.POINTER(Stats)]
    lib.ns_engine_stats.restype = None
    x, rate = sf.read(str(a.input), dtype="float32", always_2d=True)
    x = x.mean(axis=1)
    if rate != 48000:
        g = gcd(rate, 48000)
        x = resample_poly(x, 48000 // g, rate // g).astype(np.float32)
    delay = DELAY[a.model]
    padded = np.pad(x, (0, (-len(x)) % HOP + delay + HOP)).astype(np.float32)
    engine = lib.ns_engine_new(0 if a.model == "rnnoise" else 1)
    if not engine:
        sys.exit("engine initialization failed (check AURORA_*_MODEL and AURORA_*_LIBRARY)")
    out, stats = np.empty_like(padded), Stats()
    start = time.process_time_ns()
    try:
        for i in range(0, len(padded), HOP):
            if lib.ns_engine_process(engine, padded[i:i + HOP].ctypes.data, out[i:i + HOP].ctypes.data):
                sys.exit("inference failed")
        lib.ns_engine_stats(engine, C.byref(stats))
    finally:
        cpu_ns = time.process_time_ns() - start
        lib.ns_engine_free(engine)
    sf.write(str(a.output), np.clip(out[delay:delay + len(x)], -1, 1), 48000, subtype="PCM_16")
    seconds = len(padded) / 48000
    print(json.dumps({"model": a.model, "audio_s": round(len(x) / 48000, 3),
                      "cpu_percent_one_core": round(100 * cpu_ns / (seconds * 1e9), 3),
                      "max_hop_ms": round(stats.max_ns / 1e6, 3), "cpu_calls": stats.cpu_calls,
                      "native_calls": stats.native_calls, "failed_calls": stats.failed_calls}))


if __name__ == "__main__":
    main()
