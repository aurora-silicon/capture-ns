#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile actual recurrent cells and isolated unary geometries. Never submit."""
import argparse
import json
from pathlib import Path
import subprocess
import numpy as np
from recurrent import lstm


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--compiler", type=Path, required=True)
    p.add_argument("--models", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    probes = [(path.parent.name, path) for path in sorted(a.models.glob("*/cell.mil"))]
    # This fixture tests LSTM lowering, not a third denoising model.
    rng = np.random.default_rng(3654)
    W, R, B = [rng.normal(0, .1, shape).astype(np.float16).astype(np.float32)
               for shape in ((44, 7), (44, 11), (88,))]
    fixture = a.output / "synthetic-lstm-source"
    lstm(W, R, B, pad_to=512).write(fixture, runtime_vectors=True)
    probes.append(("synthetic-lstm-pad512", fixture / "cell.mil"))
    for channels in (24, 48, 96, 256, 512):
        for op in ("sigmoid", "tanh"):
            name = f"{op}-{channels}"
            path = a.output / f"{name}.mil"
            path.write_text('program(1.3)\n[buildInfo = dict<string, string>({})]\n{\n'
                f'  func main<ios18>(tensor<fp16, [1,{channels},1,1]> x) {{\n'
                f'    tensor<fp16, [1,{channels},1,1]> y = {op}(x = x)[name = string("y")];\n'
                '  } -> (y);\n}\n')
            probes.append((name, path))
    rows = []
    for target in ("H13", "H14"):
        for name, path in probes:
            output = a.output / f"{target}-{name}"
            result = subprocess.run([str(a.compiler.resolve()), "--target", target,
                "--mil", str(path.resolve()), "--model-root", str(path.parent.resolve()),
                "--output", str(output.resolve())], capture_output=True, text=True)
            # Raw diagnostic text is retained, including exporter mistakes.
            (a.output / f"{target}-{name}.log").write_text(result.stdout + result.stderr)
            row = {"target": target, "probe": name, "rc": result.returncode,
                   "diagnostic": result.stderr.strip(), "programs": None}
            manifest = output / "manifest.json"
            if result.returncode == 0 and manifest.exists():
                row["programs"] = len(json.loads(manifest.read_text()).get("programs", []))
            rows.append(row)
            print(json.dumps(row), flush=True)
    (a.output / "compiler-results.json").write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    main()
