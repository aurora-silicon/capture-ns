#!/bin/bash
# SPDX-License-Identifier: MIT
set -euo pipefail
BUILD_ROOT=$(realpath -m "${BUILD_OUT:?}")
mkdir -p "$BUILD_ROOT"
source tools/python-env.sh
setup_python "$BUILD_ROOT"
pip install --only-binary=:all: numpy==2.2.6 onnx==1.18.0 onnxruntime==1.22.1 cmake==4.1.2 ninja==1.13.0
OPENBLAS_NUM_THREADS=1 python3 -m unittest discover -s tests -p 'test_recurrent.py' -v
git clone --quiet https://github.com/aurora-silicon/mil-hwx-compiler.git "$BUILD_ROOT/compiler"
git -C "$BUILD_ROOT/compiler" checkout --quiet --detach 45558fb76e08f4c12d9b7d3e333dbac7595f2e80
export GNUSTEP_PREFIX="$BUILD_ROOT/gnustep" GNUSTEP_BUILD_ROOT="$BUILD_ROOT/gnustep-build" JOBS=8
"$BUILD_ROOT/compiler/scripts/verify-linux-compiler.sh" build-only
export LD_LIBRARY_PATH="$GNUSTEP_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p "$BUILD_ROOT/df" "$BUILD_ROOT/rnnoise"
curl -fL --retry 3 https://codeload.github.com/Rikorose/DeepFilterNet/tar.gz/d375b2d8309e0935d165700c91da9de862a99c31 | tar -xz --strip-components=1 -C "$BUILD_ROOT/df"
curl -fL --retry 3 https://codeload.github.com/xiph/rnnoise/tar.gz/6cbfd53eb348a8d394e0757b4025c6ded34eb2b6 | tar -xz --strip-components=1 -C "$BUILD_ROOT/rnnoise"
python tools/model_inventory.py --df-model "$BUILD_ROOT/df/models/DeepFilterNet3_onnx.tar.gz" \
    --rnnoise-source "$BUILD_ROOT/rnnoise" --output "$BUILD_ROOT/models"
python tools/compiler_sweep.py --compiler "$BUILD_ROOT/compiler/build/mil-hwxc" \
    --models "$BUILD_ROOT/models" --output "$BUILD_ROOT/compiler-results"
