#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) Microsoft Corporation (DNSMOS reference algorithm).
# DNSMOS windowing/calibration adapted from Microsoft DNS-Challenge, Reddy,
# Gopal and Cutler, DNSMOS P.835, ICASSP 2022. See NOTICES.md for attribution.
"""File-only host benchmark. No audio device, playback or PipeWire connection."""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import platform
import time
import urllib.request

import numpy as np
import onnxruntime as ort
from scipy.signal import resample_poly
import soundfile as sf

DNS_REV = "591184a9fcb2cbdec02520fed81a32bbbf9d73ff"


class Stats(C.Structure):
    _fields_ = [(name, C.c_uint64) for name in (
        "calls", "cpu_calls", "native_calls", "failed_calls", "total_ns", "max_ns", "cpu_ns", "last_ns")]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def download(url, path):
    if not path.exists():
        urllib.request.urlretrieve(url, path)


class DNSMOS:
    """Nonpersonalized P.835 DNSMOS, primary model only (not P.808).

    Windowing and polynomial calibration from Microsoft's MIT-licensed code
    DNSMOS/dnsmos_local.py at DNS_REV (LICENSE-CODE). Model attribution is in
    NOTICES.md. Sessions are pinned to one CPU thread.
    """
    def __init__(self, directory):
        self.path = directory / "sig_bak_ovr.onnx"
        download(f"https://raw.githubusercontent.com/microsoft/DNS-Challenge/{DNS_REV}/DNSMOS/DNSMOS/sig_bak_ovr.onnx", self.path)
        options = ort.SessionOptions()
        options.intra_op_num_threads = options.inter_op_num_threads = 1
        self.session = ort.InferenceSession(str(self.path), options, providers=["CPUExecutionProvider"])

    def __call__(self, audio48):
        audio = resample_poly(audio48, 1, 3).astype(np.float32)
        length = int(9.01 * 16000)
        while len(audio) < length:
            audio = np.concatenate((audio, audio))
        scores = []
        for start in range(0, len(audio) - length + 1, 16000):
            sig, bak, ovr = self.session.run(None, {"input_1": audio[start:start+length][None]})[0][0]
            scores.append([
                np.polyval([-0.08397278, 1.22083953, 0.0052439], sig),
                np.polyval([-0.13166888, 1.60915514, -0.39604546], bak),
                np.polyval([-0.06766283, 1.11546468, 0.04602535], ovr)])
        result = np.mean(scores, axis=0)
        return dict(zip(("sig", "bak", "ovrl"), map(float, result)))


def run(lib, model, signal):
    start = time.perf_counter_ns()
    engine = lib.ns_engine_new(model)
    if not engine:
        raise RuntimeError("engine initialization failed")
    initialization_ms = (time.perf_counter_ns() - start) / 1e6
    output, hops = [], []
    # Flush the algorithmic delay without including padding in the quality clip.
    padded = np.pad(signal, (0, (-len(signal)) % 480 + 480 * 10))
    stats = Stats()
    process_cpu_start = time.process_time_ns()
    try:
        for hop in padded.reshape(-1, 480):
            x = np.ascontiguousarray(hop, dtype=np.float32)
            y = np.empty(480, np.float32)
            if lib.ns_engine_process(engine, x.ctypes.data, y.ctypes.data):
                raise RuntimeError("inference failed")
            lib.ns_engine_stats(engine, C.byref(stats))
            hops.append(stats.last_ns)
            output.append(y)
    finally:
        process_cpu_ns = time.process_time_ns() - process_cpu_start
        lib.ns_engine_free(engine)
    # Model delay, excluding the LADSPA worker queue which this synchronous
    # harness intentionally does not exercise.
    delay = 480 if model == 0 else 1440
    y = np.concatenate(output)[delay:delay+len(signal)]
    seconds = len(padded) / 48000
    return y, {
        "init_ms": initialization_ms, "audio_seconds_including_flush": seconds,
        "cpu_percent_one_core": 100 * process_cpu_ns / (seconds * 1e9),
        "engine_thread_cpu_percent_one_core": 100 * stats.cpu_ns / (seconds * 1e9),
        "hop_wall_ms": {name: float(np.percentile(hops, q) / 1e6)
                        for name, q in (("p50", 50), ("p95", 95), ("p99", 99), ("max", 100))},
        "deadline_misses_10ms": sum(t > 10_000_000 for t in hops),
        "cpu_calls": stats.cpu_calls, "native_calls": stats.native_calls,
        "failed_calls": stats.failed_calls, "model_delay_samples": delay,
        "plugin_queue_delay_samples": 960}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--clean-dir", type=Path, required=True, help="OpenSLR LibriSpeech test-clean root")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--clips", type=int, default=4, help="one utterance per distinct speaker")
    p.add_argument("--models", nargs="+", choices=("rnnoise", "df3"), default=["rnnoise", "df3"])
    a = p.parse_args()
    if a.clips <= 0:
        p.error("--clips must be positive")
    if len(set(a.models)) != len(a.models):
        p.error("--models must not contain duplicates")
    a.output.mkdir(parents=True, exist_ok=True)
    lib = C.CDLL(str(a.library.resolve()))
    lib.ns_engine_new.argtypes = [C.c_int]
    lib.ns_engine_new.restype = C.c_void_p
    lib.ns_engine_free.argtypes = [C.c_void_p]
    lib.ns_engine_free.restype = None
    lib.ns_engine_process.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p]
    lib.ns_engine_stats.argtypes = [C.c_void_p, C.POINTER(Stats)]
    lib.ns_engine_stats.restype = None
    dns = DNSMOS(a.output)
    files, speakers = [], set()
    for path in sorted(a.clean_dir.rglob("*.flac")):
        speaker = path.stem.split("-")[0]
        if speaker in speakers:
            continue
        speakers.add(speaker)
        files.append(path)
        if len(files) == a.clips:
            break
    if len(files) != a.clips:
        raise ValueError("not enough distinct speakers")
    rows = []
    result = {"schema": 1, "complete": False, "selected_models": a.models,
              "host": {"system": platform.system(), "release": platform.release(),
                       "machine": platform.machine(), "processor": platform.processor()},
              "dataset": "LibriSpeech test-clean, first sorted utterance per distinct speaker, seeded synthetic 0/5 dB mixtures",
              "quality": "DNSMOS P.835, nonpersonalized, primary model only",
              "dns_revision": DNS_REV, "dns_model_sha256": sha(dns.path),
              "plugin_sha256": sha(a.library),
              "df_model_sha256": sha(os.environ["AURORA_DF_MODEL"]) if "AURORA_DF_MODEL" in os.environ else None,
              "rnnoise_model_sha256": sha(os.environ["AURORA_RNNOISE_MODEL"]) if "AURORA_RNNOISE_MODEL" in os.environ else None,
              "rows": rows}
    def checkpoint():
        temporary = a.output / "benchmark.tmp"
        temporary.write_text(json.dumps(result, indent=2) + "\n")
        temporary.replace(a.output / "benchmark.json")
    checkpoint()
    for index, path in enumerate(files):
        clean, rate = sf.read(path, dtype="float32")
        if clean.ndim != 1 or rate != 16000:
            raise ValueError("expected mono 16 kHz LibriSpeech")
        clean = resample_poly(clean, 3, 1).astype(np.float32)
        # Seeded colored noise; no third-party noise recording or licence.
        rng = np.random.default_rng(3654 + index)
        white = rng.standard_normal(len(clean))
        colored = np.convolve(white, np.ones(9) / 9, mode="same")
        noise = white + 2 * colored
        for snr in (0, 5):
            noise_scaled = noise * np.sqrt(np.mean(clean**2) / np.mean(noise**2) / (10**(snr/10)))
            noisy = (clean + noise_scaled).astype(np.float32)
            peak = max(float(np.max(np.abs(noisy))), float(np.max(np.abs(clean))))
            gain = min(1.0, 0.9 / peak)
            noisy *= gain
            stem = f"{path.stem}-snr{snr}"
            row = {"speech": path.stem, "speech_sha256": sha(path), "snr_db": snr,
                   "noise_seed": 3654+index, "noisy_dnsmos": dns(noisy), "models": {}}
            sf.write(a.output / f"{stem}-noisy.wav", noisy, 48000, subtype="FLOAT")
            for name in a.models:
                model = {"rnnoise": 0, "df3": 1}[name]
                enhanced, receipt = run(lib, model, noisy)
                receipt["dnsmos"] = dns(enhanced)
                row["models"][name] = receipt
                sf.write(a.output / f"{stem}-{name}.wav", enhanced, 48000, subtype="FLOAT")
            rows.append(row)
            checkpoint()
            print(json.dumps(row), flush=True)
    result["complete"] = True
    checkpoint()


if __name__ == "__main__":
    main()
