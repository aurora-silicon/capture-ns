# Measurement method and receipts

The file benchmark uses four LibriSpeech test-clean utterances from distinct speakers, selected in sorted file order mixed with deterministic white/colored noise at 0 and 5 dB SNR. This is a small, reproducible synthetic noisy-speech set, not the full VoiceBank-DEMAND or DNS Challenge test set. Speech IDs, source hashes, seeds and model hashes are written into the JSON. No private recording is used or published.

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

CPU and compiler receipts are added after controller jobs finish. Native ANE and reference-Mac live numbers remain unmeasured until the corresponding implementation and approved lead runs exist.
