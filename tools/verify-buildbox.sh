#!/bin/bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT=$PWD
BUILD_ROOT=${BUILD_OUT:?controller build output required}
mkdir -p "$BUILD_ROOT/deps" "$BUILD_ROOT/include" "$BUILD_ROOT/receipts"
BUILD_ROOT=$(realpath "$BUILD_ROOT")
sha256sum src/*.c include/capture_ns.h tools/*.py tools/aurora-capture-ns > "$BUILD_ROOT/receipts/integration-sources.sha256"
cp tools/verify-buildbox.sh "$BUILD_ROOT/receipts/build-script.txt"
fetch() {
    local project=$1 revision=$2 destination=$3
    mkdir -p "$destination"
    curl --fail --location --retry 3 "https://codeload.github.com/$project/tar.gz/$revision" |
        tar -xz --strip-components=1 -C "$destination"
}
fetch aurora-silicon/omarchy-ane 1adf2ee392ef7d3d64a24743d293dbc1670f0e8b "$BUILD_ROOT/deps/runtime"
fetch xiph/rnnoise 6cbfd53eb348a8d394e0757b4025c6ded34eb2b6 "$BUILD_ROOT/deps/rnnoise"
fetch Rikorose/DeepFilterNet d375b2d8309e0935d165700c91da9de862a99c31 "$BUILD_ROOT/deps/df"
# Use PipeWire's bundled LGPL-2.1-or-later LADSPA API header without vendoring it.
curl --fail --location --retry 3 https://raw.githubusercontent.com/aurora-silicon/pipewire/5009ec694a0a6ec6351eac787769a128b6a168e1/spa/plugins/filter-graph/ladspa.h -o "$BUILD_ROOT/include/ladspa.h"
cp LICENSE "$BUILD_ROOT/receipts/integration-LICENSE.txt"
cp NOTICES.md "$BUILD_ROOT/receipts/attribution.txt"
cp "$BUILD_ROOT/deps/runtime/LICENSE" "$BUILD_ROOT/receipts/libane-LICENSE.txt"
cp "$BUILD_ROOT/deps/rnnoise/COPYING" "$BUILD_ROOT/receipts/RNNOISE-COPYING.txt"
cp "$BUILD_ROOT/deps/df/LICENSE-MIT" "$BUILD_ROOT/receipts/DF-LICENSE-MIT.txt"
cp "$BUILD_ROOT/include/ladspa.h" "$BUILD_ROOT/receipts/LADSPA-API.txt"
make -C "$BUILD_ROOT/deps/runtime/libane" -j8
(
    cd "$BUILD_ROOT/deps/rnnoise"
    ./autogen.sh
    ./configure --disable-examples
    make -j8
)
export AURORA_RNNOISE_LIBRARY="$BUILD_ROOT/deps/rnnoise/.libs/librnnoise.so"
export AURORA_DF_LIBRARY="$BUILD_ROOT/df-target/release/libdf.so"
export AURORA_DF_MODEL="$BUILD_ROOT/deps/df/models/DeepFilterNet3_onnx.tar.gz"
export AURORA_RNNOISE_MODEL="$BUILD_ROOT/deps/rnnoise-model.txt"
python3 tools/export_rnnoise.py "$BUILD_ROOT/deps/rnnoise/src/rnn_data.c" "$AURORA_RNNOISE_MODEL"
source tools/python-env.sh
setup_python "$BUILD_ROOT"
pip install --only-binary=:all: numpy==2.2.6 scipy==1.15.3 soundfile==0.13.1 onnxruntime==1.22.1 onnx==1.18.0
make BUILD="$BUILD_ROOT/plugin" LIBANE_DIR="$BUILD_ROOT/deps/runtime/libane" \
    LIBANE_LIB="$BUILD_ROOT/deps/runtime/libane/libane.a" CFLAGS="-O2 -g -I$BUILD_ROOT/include" \
    all "$BUILD_ROOT/plugin/test-stream" "$BUILD_ROOT/plugin/test-engine" "$BUILD_ROOT/plugin/test-ladspa"
"$BUILD_ROOT/plugin/test-stream"
"$BUILD_ROOT/plugin/test-engine"
OPENBLAS_NUM_THREADS=1 python3 -m unittest discover -s tests -p 'test_*.py' -v
python3 tools/model_inventory.py --df-model "$AURORA_DF_MODEL" --rnnoise-source "$BUILD_ROOT/deps/rnnoise" --output "$BUILD_ROOT/receipts/models"
(
    cd "$BUILD_ROOT/deps/df"
    # The published workspace lock is stale. Retain the resolved lock from
    # the first build, then replay it exactly on subsequent builds.
    if [[ -f "$ROOT/tools/df-Cargo.lock" ]]; then
        cp "$ROOT/tools/df-Cargo.lock" Cargo.lock
    else
        cargo generate-lockfile
    fi
    cp Cargo.lock "$BUILD_ROOT/receipts/DF-Cargo.lock.txt"
    sha256sum Cargo.lock > "$BUILD_ROOT/receipts/DF-Cargo.lock.sha256"
    CARGO_BUILD_JOBS=8 CARGO_TARGET_DIR="$BUILD_ROOT/df-target" cargo build --release --locked -p deep_filter --no-default-features --features capi
)
"$BUILD_ROOT/plugin/test-ladspa" "$BUILD_ROOT/plugin/libaurora_capture_ns.so"
mkdir -p "$BUILD_ROOT/data"
curl --fail --location --retry 3 https://www.openslr.org/resources/12/test-clean.tar.gz -o "$BUILD_ROOT/data/test-clean.tar.gz"
sha256sum "$BUILD_ROOT/data/test-clean.tar.gz" > "$BUILD_ROOT/receipts/dataset.sha256"
tar -xzf "$BUILD_ROOT/data/test-clean.tar.gz" -C "$BUILD_ROOT/data"
python3 tools/benchmark.py --library "$BUILD_ROOT/plugin/libaurora_capture_ns.so" \
    --clean-dir "$BUILD_ROOT/data/LibriSpeech/test-clean" --output "$BUILD_ROOT/receipts/bench" --clips 4
pip freeze > "$BUILD_ROOT/receipts/python-packages.txt"
uname -srvmo > "$BUILD_ROOT/receipts/host.txt"
lscpu > "$BUILD_ROOT/receipts/cpu.txt"
echo "file-only validation complete"
