# Aurora capture noise suppression

An experimental, opt-in PipeWire microphone source using the existing filter-chain LADSPA mechanism. RNNoise v0.1.1 and DeepFilterNet3 run on the CPU through the shared [libane fallback API](https://github.com/aurora-silicon/omarchy-ane/blob/main/docs/CPU-FALLBACK-BENCH.md). There is currently **no complete native ANE model backend**. The recurrent exporter and compiler probes make that remaining work explicit. See [compiler coverage](docs/COMPILER.md).

The plugin buffers 10 ms mono hops at 48 kHz on a worker. The audio callback uses bounded queues and never waits for inference. Missed deadlines produce silence and increment counters. Independent sources have independent model, DSP and recurrent state. It requests a 10 ms PipeWire quantum; a server that ignores this request can miss deadlines even when inference is fast. It never forces a server quantum.

The launcher requires an existing microphone node name, exports `Aurora Microphone (rnnoise)` or `Aurora Microphone (df3)`, and leaves selection to the application. Installation adds a plugin and a launcher only. No source is enabled, no default is selected, and no service or configuration fragment is installed. See [the capture boundary](docs/CAPTURE-BOUNDARY.md).

## Build

Linux dependencies: a C11 compiler, pthreads, OpenSSL development headers, LADSPA headers, and libane with `ane_fallback_init`/`ane_fallback_run` (the merged CPU fallback API). Runtime model libraries are loaded dynamically.

```sh
make LIBANE_DIR=/usr/include/libane
make install PREFIX=/usr/local
```

Build RNNoise from revision `6cbfd53eb348a8d394e0757b4025c6ded34eb2b6` and libDF from `d375b2d8309e0935d165700c91da9de862a99c31`. The libDF build uses `--release --locked -p deep_filter --no-default-features --features capi` with the integration's recorded dependency lock; the published workspace lock needs refresh. The full build and recorded-file evaluation are in [tools/verify-buildbox.sh](tools/verify-buildbox.sh). All dependencies and weights have a [licence audit](docs/LICENSE-AUDIT.md).

Serialize the audited RNNoise weights without modifying them:

```sh
python3 tools/export_rnnoise.py /path/to/rnnoise/src/rnn_data.c /path/to/rnnoise-v011.txt
export AURORA_RNNOISE_MODEL=/path/to/rnnoise-v011.txt
export AURORA_RNNOISE_LIBRARY=/path/to/rnnoise/.libs/librnnoise.so
```

For DeepFilterNet3:

```sh
export AURORA_DF_MODEL=/path/to/DeepFilterNet/models/DeepFilterNet3_onnx.tar.gz
export AURORA_DF_LIBRARY=/path/to/DeepFilterNet/target/release/libdf.so
```

Library path variables are optional when the audited libraries are installed in the dynamic loader search path. Model paths are required and hash checked. A wrong model or unavailable library fails initialization; it does not substitute another model or pass through an unfiltered microphone. The launcher runs a dedicated filter-chain client process, so libDF's aborting C API cannot abort the main PipeWire daemon.

## Select a microphone

Find the microphone's `node.name` in `pw-dump`. Explicitly choose it; neither the default source nor a sink monitor is accepted as a shortcut.

```sh
aurora-capture-ns --source alsa_input.YOUR_MIC --model rnnoise --print-config
aurora-capture-ns --source alsa_input.YOUR_MIC --model rnnoise
```

Use `--model df3` for DeepFilterNet3. In the recording application, select the exported Aurora source. Stop the launcher to remove it. No playback is needed for any verification; use the [lead test plan](docs/LEAD-TEST.md).

RNNoise's algorithmic delay is 10 ms; DFN3's is 30 ms. The plugin adds a fixed 20 ms worker queue, giving nominal processing delays of 30/50 ms before capture hardware, resampling and PipeWire scheduling buffers. The `latency` output port reports 1440/2400 samples to PipeWire. `Late Hops` and `Dropped Hops` expose overload. This is a soft realtime worker design; its LADSPA descriptor does not claim hard realtime capability.

## Validation

`make check` tests variable block sizes, in-place buffers, fixed queue delay, bounded overload, isolated model state, the LADSPA lifecycle, source selection and recurrent lowering. The recurrent equations are compared against independent ONNX Runtime GRU/LSTM operators across 100 stateful steps, including both GRU reset conventions. Tests and benchmarks require no audio device or ANE.

[Measurement method and results](docs/MEASUREMENTS.md) distinguish host CPU measurements from the pending native ANE and lead-run PipeWire checks. The code is MIT; third-party model and API licences remain theirs.
