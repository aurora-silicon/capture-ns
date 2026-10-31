# Lead test plan: files first, microphone only

No step requires playback, a sink monitor, volume changes, protection changes, UCM changes or an audio-service restart. The worker task does not authorize target execution. Every lead target run must be submitted to the controller with the task ID, an explicit device identity (first `m1pro`, then `m2` if required), the exact command/source identity and policy approval. Preserve request IDs, logs and hashes.

## Build and files

1. Request an ordinary buildbox build slot for the pinned source. Run `bash tools/verify-buildbox.sh`, collecting `out/receipts/`, the plugin and its source commit. The build uses public dependencies, an isolated Python 3.12 environment, eight build jobs and the original weight files. It opens neither PipeWire nor an audio device.
2. Before running on an ARM reference host, obtain native aarch64 plugin/libane/RNNoise/libDF artifacts from the buildbox or the packaging task. An x86_64 buildbox binary is not a reference-Mac artifact. Keep hashes, compiler versions and model licences with the artifacts. Building on the target is not part of this plan.
3. Through a controller-approved `execute` request for `device=m1pro`, use the installed audited libraries and weights and run the file-only benchmark below from a task-owned scratch directory. The dataset is the public OpenSLR test-clean archive, not a private mic recording. The script creates all output under the requested result directory.

```sh
export AURORA_RNNOISE_MODEL=/path/to/rnnoise-v011.txt
export AURORA_DF_MODEL=/path/to/DeepFilterNet3_onnx.tar.gz
export AURORA_RNNOISE_LIBRARY=/path/to/aarch64/librnnoise.so
export AURORA_DF_LIBRARY=/path/to/aarch64/libdf.so
python3 tools/benchmark.py \
  --library /path/to/aarch64/libaurora_capture_ns.so \
  --clean-dir /path/to/LibriSpeech/test-clean \
  --output results/m1pro-files --clips 4
```

Repeat three times for CPU/latency spread. The JSON identifies every speech file, weight artifact, CPU/native call count, 10 ms miss count and DNSMOS model. Record the machine model, kernel, plugin and dependency commits alongside it. Initialization is reported separately. The host's one-core CPU percent is process CPU time during sequential file inference divided by audio duration; it includes the thin Python harness. Compare the C engine thread's CPU percent as well. File results do not prove live graph scheduling or native ANE performance.

## Opt-in live capture

Only after a controller-approved microphone-only request for the exact reference machine:

1. Leave the existing speaker protection daemon active and governing. Do not start a second protection daemon or reload any audio/protection service. A read-only service-state receipt may be collected; no speaker output will be generated.
2. Save a private read-only `pw-dump`/default-source receipt to the task's evidence directory, then identify the physical microphone node name. Do not publish the full dump or a mic recording.
3. Run `aurora-capture-ns --source <explicit-mic-node-name> --model rnnoise --print-config`. Review the generated capture serial and virtual source properties. Then run the same command without `--print-config`. No normal configuration file is edited.
4. In a capture client select `aurora.capture.source.rnnoise` explicitly. For example, record ten seconds using `timeout 10s pw-record --target aurora.capture.source.rnnoise --rate 48000 --channels 1 results/rnnoise-mic.wav`. Timeout's expected termination is not an inference failure. Do not play the recording. Inspect duration, RMS, finite samples and the filter's `Late Hops`, `Dropped Hops` and `latency` notify ports through read-only `pw-dump`/graph parameter reads.
5. Stop only the launcher. Confirm that its source disappears and the original microphone/default metadata is unchanged. Repeat for `--model df3` and a separate result file. Run one stream at a time for CPU attribution, then two independent instances to verify state isolation if needed.
6. Disconnect/reconnect the selected microphone during an approved capture run. The source must not silently fall back to another mic or monitor. Restarting the opt-in launcher is the recovery action. Do not alter global routing to recover it.

Pass criteria: explicit virtual-source selection; no default-source write; finite capture output; 1440/2400 reported latency samples; zero `Error Hops` and steady-state late/dropped hops at a negotiated quantum at most 480 samples; process CPU and hop latency recorded for each model. If the server chooses a larger quantum, retain the late-hop evidence and run the file-only variant; do not force the server quantum or change another stream's configuration.

Rollback is stopping this dedicated client. Installed files can be removed by their recorded paths after review; there is no service, UCM or persistent microphone policy to undo. Private recordings stay in private evidence and are never attached to a public PR.

## Native ANE, after compiler support

The current plugin deliberately reports CPU-only calls. Do not label any current result ANE. A future native backend must use the existing compiler-owned package validator/resident runner through shared libane, choose generation from device capabilities, and preserve its ownership/recovery rules. Do not add a parallel model scheduler or submit generated cells directly from this plugin.

Before enabling it, prove the entire neural graph and DSP boundary against the CPU model, with the same source-weight identity and explicit fp16 rounding tolerances over long recurrent sequences. Commit output samples and next recurrent state only after success. A failed native attempt must leave the previous state intact for CPU replay of that hop and retire the failed native owner. Hangs follow the existing ANE recovery protocol, without an in-flight owner being killed by a timeout wrapper. No native submission, boot or driver operation is part of this test plan today.
