# Recurrent lowering and native integration work

`tools/model_inventory.py` inspects the three public DFN3 ONNX graphs and the audited RNNoise C weight arrays. It emits `inventory.json`, explicit-state recurrent `cell.mil` files, compiler-format fp16 weight blobs and NumPy weight copies. `tools/compiler_sweep.py` compiles these exact cells for H13/H14 and probes sigmoid/tanh at the actual recurrent channel counts. It preserves raw diagnostics, including exporter errors; a parser or malformed-blob failure must not be presented as a missing compiler operation.

The exporter contains no opaque GRU/LSTM call. GRU update/reset use `sigmoid(xW+b+hR+c)`. The candidate handles ONNX `linear_before_reset=0` (`(r*h)R+c`) and `=1` (`r*(hR+c)`) distinctly. `h_next = z*h + (1-z)*candidate`; `1-z` is lowered into multiply/add. RNNoise's candidate is ReLU, not tanh. Its three GRUs have input/hidden sizes 24/24, 90/48 and 114/96 and use reset-before semantics. Float weights are the exact original int8 values divided by 256.

LSTM uses ONNX's i,o,f,c gate ordering, emits both `c_next` and `h_next`, and supports default sigmoid/tanh activations without peepholes, coupled gates, clipping, reverse/bidirectional layout or a sequence loop. Nondefault model attributes are refused. Single-step inputs/state are explicit; streaming DSP/history and time-loop management remain the existing model runtime's responsibility. Recurrent tests use independent ONNX Runtime operators over 100 stateful steps. This proves the equations and weight mapping, not native fp16 parity of a full denoiser.

MIL constants are fp16-rounded copies of the same source weights. The CPU denoisers use their original numerical implementations (RNNoise's approximated sigmoid and DFN3's float32 Tract path). Native-versus-CPU tolerance must be qualified over complete sequences; the exporter does not claim bitwise equivalence to those implementations. The CPU source never silently swaps to a different model when a device is unavailable.

## Handoff to compiler task t-3651

The full models need more than a recurrent-cell spelling:

- RNNoise: exact small/non-square matmul geometries, sigmoid at hidden sizes 24/48/96, a ReLU candidate, dense layers and concatenation/fan-in at the model boundary. Padding/legalization may solve individual shapes, but must preserve the valid lanes and be checked against the published reference.
- DFN3: encoder, ERB decoder and deep-filter decoder inventories, recurrent states, temporal convolution caches, grouped operations, transposed convolutions, slicing/concatenation/shape operations and all actual small-channel geometries. Successful isolated cells alone do not constitute a complete native graph.
- Fusion: a GRU cell has 24 primitive operations (excluding no-op reshapes). Seventy-two per-op dispatches for RNNoise's three cells exceed a 10 ms budget on the task's historical M2 Max overhead even before dense layers and DSP. Fused recurrent regions and resident state are needed; exact compiler program counts remain in the probe receipt.
- State transaction: on native failure, no partially advanced recurrent or DSP state may reach the CPU replay. The existing owner must finish cleanup/quarantine before fallback continues; use shared libane and the compiler-owned graph runner.

The plugin presently calls `ane_fallback_init(..., 0)` and supplies its CPU callback to the shared runtime. It does not probe devices, load programs, pretend a native callback exists, or add a second model loader. The missing deliverable is a complete compiled model backend with same-weight state/parity qualification and measurements. Compiler receipts below will distinguish confirmed refusals from this static integration inventory.
