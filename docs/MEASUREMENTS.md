# Measurement method and receipts

The file benchmark uses four LibriSpeech test-clean utterances from distinct speakers, selected in sorted file order and mixed with deterministic white/colored noise at 0 and 5 dB SNR. This is a small, reproducible synthetic noisy-speech set, not the full VoiceBank-DEMAND or DNS Challenge test set. Speech IDs, source hashes, seeds and model hashes are written into the JSON. No private recording is used or published.

The build harness writes separate RNNoise and DFN3 receipts for the same mixtures. Each JSON names its selected models and marks whether the run completed; per-mixture checkpoints survive later failures. Only complete runs are used for aggregate measurements. `--models rnnoise` or `--models df3` can also be used for independent reruns.

Quality is nonpersonalized DNSMOS P.835 (`sig`, `bak`, `ovrl`) using Microsoft's primary `sig_bak_ovr.onnx`. The implementation follows the published 9.01 s windows, 1 s stride, repeat-padding of short clips and polynomial calibration. It resamples 48 kHz output to 16 kHz with SciPy polyphase filtering. No P.808 or PESQ number is claimed. Compare input/output scores for the same mixture; four speakers cannot establish population-level quality or absence of speech distortion.

The synchronous C engine invokes the shared libane fallback policy on each 480-sample hop. Its latency quantiles measure that policy and inference, including normalized/PCM scaling for RNNoise, with model initialization, output concatenation and DNSMOS outside the per-hop window. Process CPU time around the inference loop measures host CPU per stream; 100% means one full CPU core. A separate engine-thread CPU percentage helps identify harness/extra-thread cost. Model initialization is reported separately; first-hop inference is retained in the quantiles rather than silently excluded.

This harness does not exercise the LADSPA worker's wakeups or PipeWire graph scheduling. Live capture validation must record those costs and notify-port counters separately. A 10 ms hop deadline is measured per stream, with every over-budget hop counted. Flush padding is included in the reported audio duration and CPU timing and excluded from the quality clip.

## Latency accounting

| Model | Hop | Algorithmic model delay | Plugin queue | Nominal plugin processing delay |
| --- | --- | --- | --- | --- |
| RNNoise v0.1.1 | 480 samples / 10 ms | 480 samples / 10 ms (20 ms analysis window, one-hop overlap) | 960 samples / 20 ms | 1440 samples / 30 ms |
| DeepFilterNet3 | 480 samples / 10 ms | `(960-480) + max(2,2)*480 = 1440` samples / 30 ms | 960 samples / 20 ms | 2400 samples / 50 ms |

DFN3's calculation follows `libDF/src/bin/enhance_wav.rs` and the audited model's config. Hardware buffering, resampling and graph scheduling add to these delays. The LADSPA `latency` port is initialized before processing and uses the exact name PipeWire recognizes as process latency.

## Native planning budget

The task's earlier measured per-submit overheads, 0.04 ms for H13 and 0.25 ms for M2 Max, are historical inputs, not new measurements here. The emitted recurrent cell uses 24 primitive operations before layout-only reshapes. Three RNNoise cells therefore require at least 72 per-op dispatches without fusion: 2.88 ms / 18 ms of those historical call overheads alone. Dense layers, DSP, packing and any split matmuls add cost. This estimate explains why fusion is necessary, especially on H14; it is not an ANE benchmark or a prediction of measured speedup.

## Measured host results

Linux x86_64, Intel Core i9-13900K, kernel 7.0.12-arch1-1; a shared, nonisolated build host. Each model processed 8,494 hops, 84.94 s including flush padding, across the same eight mixtures from four speakers. This is one run per model, not an ARM reference-machine comparison.

| CPU model path | Process CPU per stream, one core | Engine thread CPU | Per-clip hop p50 range | Worst per-clip hop p99 | Maximum hop | 10 ms misses |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| RNNoise v0.1.1 | 1.018% | 0.985% | 0.078-0.130 ms | 0.381 ms | 0.481 ms | 0 |
| DeepFilterNet3 | 5.915% | 5.803% | 0.429-1.102 ms | 1.839 ms | 2.157 ms | 0 |

CPU percentages are weighted by processed audio duration. Quantiles are reported per clip; the table does not average quantiles into a fictitious aggregate percentile. Both models recorded 8,494 CPU calls, zero native calls and zero failed calls. Initialization ranged from 3.05-5.53 ms for RNNoise and 190.59-357.93 ms for DFN3, outside the hop timing window.

| Mean DNSMOS P.835 | SIG | BAK | OVRL |
| --- | ---: | ---: | ---: |
| Noisy input | 2.876 | 1.713 | 1.757 |
| RNNoise | 3.167 | 3.592 | 2.745 |
| DeepFilterNet3 | 3.429 | 4.102 | 3.176 |

These are equal-weight means over eight mixtures, with identical noisy-input scores in the two runs. DFN3 improved OVRL by 0.431 relative to RNNoise on this small synthetic set, at about 5.8 times its measured process CPU cost. This does not establish a general quality ranking or target-Mac CPU use. The file benchmark excludes live PipeWire worker/scheduling overhead; that remains a lead measurement.

Raw, path-free receipts: [RNNoise](benchmark-rnnoise.json), [DFN3](benchmark-df3.json), [aggregate](host-summary.json). DNSMOS model SHA256: `269fbebdb513aa23cddfbb593542ecc540284a91849ac50516870e1ac78f6edd`. The exact successful dependency lock is [tools/df-Cargo.lock](../tools/df-Cargo.lock), SHA256 `9267e89f88e5d347f40f7409c2711d05a8b9ddee1223017973321799227035d3`, retaining Tract 0.21.4.

The same build passed C stream/overload/state tests, original RNNoise weight bit parity, both LADSPA descriptor/lifecycle checks, and all nine Python tests. No audio device, reference Mac or ANE was opened. Native ANE and reference-Mac live numbers remain unmeasured until the corresponding implementation and approved lead runs exist.
