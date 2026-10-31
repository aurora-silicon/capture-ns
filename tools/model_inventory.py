#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Audit model ops and emit explicit-state recurrent MIL from public weights."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import tarfile

import numpy as np
import onnx
from onnx import numpy_helper

from recurrent import gru, lstm


def digest(data):
    return hashlib.sha256(data).hexdigest()


def attributes(node):
    return {a.name: onnx.helper.get_attribute_value(a) for a in node.attribute}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--df-model", type=Path, required=True)
    p.add_argument("--rnnoise-source", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    report = {"df_archive_sha256": digest(a.df_model.read_bytes()), "df_graphs": [], "rnnoise_cells": []}
    with tarfile.open(a.df_model) as archive:
        for member in archive.getmembers():
            if not member.name.endswith(".onnx"):
                continue
            data = archive.extractfile(member).read()
            model = onnx.load_from_string(data)
            constants = {v.name: numpy_helper.to_array(v) for v in model.graph.initializer}
            for node in model.graph.node:
                if node.op_type == "Constant" and "value" in attributes(node):
                    constants[node.output[0]] = numpy_helper.to_array(attributes(node)["value"])
            entry = {"file": member.name, "sha256": digest(data),
                     "ops": dict(sorted(Counter(n.op_type for n in model.graph.node).items())), "recurrent": []}
            for index, node in enumerate(model.graph.node):
                if node.op_type not in ("GRU", "LSTM"):
                    continue
                attrs = attributes(node)
                cell = {"name": node.name, "op": node.op_type, "inputs": list(node.input),
                        "hidden_size": attrs.get("hidden_size"), "emitted": False}
                try:
                    if attrs.get("direction", b"forward") != b"forward" or attrs.get("layout", 0) != 0:
                        raise ValueError("only forward layout-0 cells supported")
                    if "clip" in attrs or attrs.get("activations") or attrs.get("input_forget") or len(node.input) > 7:
                        raise ValueError("nondefault recurrent attributes/peepholes need explicit lowering")
                    W, R = (constants[n][0] for n in node.input[1:3])
                    B = constants[node.input[3]][0] if len(node.input) > 3 and node.input[3] else None
                    graph = (gru(W, R, B, linear_before_reset=attrs.get("linear_before_reset", 0))
                             if node.op_type == "GRU" else lstm(W, R, B))
                    directory = Path(member.name).stem + f"-cell-{index}"
                    graph.write(a.output / directory)
                    cell.update(emitted=True, directory=directory, primitives=dict(Counter(op for _, op, _, _ in graph.ops)))
                    padded = (gru(W, R, B, linear_before_reset=attrs.get("linear_before_reset", 0), pad_to=512)
                              if node.op_type == "GRU" else lstm(W, R, B, pad_to=512))
                    padded.write(a.output / (directory + "-pad512"), runtime_vectors=True)
                    cell["padded_directory"] = directory + "-pad512"
                except (KeyError, ValueError) as error:
                    cell["reason"] = str(error)
                entry["recurrent"].append(cell)
            report["df_graphs"].append(entry)
    source = (a.rnnoise_source / "src/rnn_data.c").read_text()
    report["rnnoise_source_sha256"] = digest(source.encode())
    arrays = {}
    for name, count, values in re.findall(r"static const rnn_weight\s+(\w+)\[(\d+)\]\s*=\s*\{([^}]+)\}", source):
        values = np.array([int(n) for n in re.findall(r"-?\d+", values)], dtype=np.float32) / 256
        if len(values) != int(count):
            raise ValueError("RNNoise array length mismatch")
        arrays[name] = values
    for name, I, H in (("vad_gru", 24, 24), ("noise_gru", 90, 48), ("denoise_gru", 114, 96)):
        W = arrays[name+"_weights"].reshape(I, 3*H).T
        R = arrays[name+"_recurrent_weights"].reshape(H, 3*H).T
        B = np.concatenate((arrays[name+"_bias"], np.zeros(3*H, np.float32)))
        graph = gru(W, R, B, linear_before_reset=0, candidate="relu")
        graph.write(a.output / ("rnnoise-"+name))
        padded = gru(W, R, B, linear_before_reset=0, candidate="relu", pad_to=512)
        padded.write(a.output / ("rnnoise-"+name+"-pad512"), runtime_vectors=True)
        report["rnnoise_cells"].append({"name": name, "input": I, "hidden": H, "candidate": "relu",
                                        "padded_directory": "rnnoise-"+name+"-pad512",
                                        "primitives": dict(Counter(op for _, op, _, _ in graph.ops))})
    (a.output / "inventory.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
