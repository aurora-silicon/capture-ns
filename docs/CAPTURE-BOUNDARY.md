# Capture boundary and diff review

This standalone repository adds one LADSPA plugin, a launcher, file-only tools, tests and documentation. It does not modify the PipeWire fork or any distribution's audio configuration.

The launcher reads `pw-dump`, checks that the requested node is an `Audio/Source`, and binds to that node's `object.serial`. Sinks, missing sources, ambiguous names and recursive selection of this filter are rejected. `node.dont-fallback` prevents fallback to a default device and `node.dont-reconnect` prevents a removed mic being silently replaced. The source's removal/reappearance needs an explicit restart. No source or sink default metadata is written.

The generated graph has one LADSPA node with one mono input and output. Its input stream captures the explicitly selected source (`stream.capture.sink=false`). Its output stream is exported as `media.class=Audio/Source`, with hardware autoconnection disabled and session priority zero. Applications select it as a recording source. `playback.props` is PipeWire filter-chain's name for its output-stream properties; it does not select or open a playback device.

The launcher starts a dedicated `pipewire -c` filter-chain client with an isolated, temporary config directory and empty fragment directory. The loaded modules are realtime support, native protocol, client-node, adapter and filter-chain. There is no ALSA, UCM or session-manager module. The client connects to the existing server; the main PipeWire daemon and WirePlumber are never restarted or reloaded. The launcher forwards termination to the client and removes its temporary files.

`make install` installs only `lib/ladspa/libaurora_capture_ns.so` and `bin/aurora-capture-ns`. It does not install any `conf.d`, systemd unit, UCM PCM, WirePlumber hook, default-source rule or speaker policy. No volume API, mixer API, playback device, speaker protection or protection-daemon operation exists in the implementation. No audio playback command is part of the test scripts.

Review the entire change against `main`:

```sh
git diff --stat main...HEAD
git diff --name-only main...HEAD
git diff main...HEAD -- Makefile tools/aurora-capture-ns src include
```

The launcher's only filesystem writes are in its private temporary directory. Its PipeWire client creates the two transient capture-filter nodes described above. The only subprocesses are the read-only `pw-dump` and the dedicated `pipewire` filter client. Build/evaluation scripts write their own build and result directories and download licensed public sources. They neither connect to a reference Mac nor open an audio device.

Operational speaker protection remains outside this source's graph and must remain active and governing during any lead capture test. The test plan never needs sound output, protection changes, UCM edits or playback graph inspection/reconfiguration.
