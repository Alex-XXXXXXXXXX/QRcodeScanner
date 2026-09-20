#!/usr/bin/env python3
"""Train a small offline CART route selector with only the Python stdlib.

Input may be a CSV or a JSON result emitted by QRCodeScannerBenchmark. The
export contains fixed, auditable nodes; the scanner never loads it at runtime.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


NUMERIC_FEATURES = [
    "contrast", "mean_luminance", "saturated_ratio", "sharpness",
    "edge_consistency", "pixels", "roi_fraction",
]
MAX_DEPTH = 4
MIN_SAMPLES_LEAF = 20


def number(value: object, fallback: float = 0.0) -> float:
    try:
        result = float(value)
        return result if math.isfinite(result) else fallback
    except (TypeError, ValueError):
        return fallback


def load_rows(path: Path) -> list[dict[str, object]]:
    if path.suffix.lower() == ".csv":
        with path.open("r", encoding="utf-8-sig", newline="") as stream:
            return list(csv.DictReader(stream))
    document = json.loads(path.read_text(encoding="utf-8"))
    rows: list[dict[str, object]] = []
    for record in document.get("records", []):
        quality = record.get("quality", {})
        rows.append({
            "contrast": quality.get("contrast", 0.0),
            "mean_luminance": quality.get("meanLuminance", 0.0),
            "saturated_ratio": quality.get("saturatedRatio", 0.0),
            "sharpness": quality.get("sharpness", 0.0),
            "edge_consistency": quality.get("edgeConsistency", 0.0),
            "pixels": quality.get("pixels", 0.0),
            "roi_fraction": quality.get("roiFraction", 1.0),
            "previous_route": quality.get("previousSuccessfulRoute", "none") or "none",
            "route": record.get("route") or record.get("status", "no_read"),
        })
    return rows


@dataclass
class Node:
    counts: Counter[str]
    feature_index: int = -1
    threshold: float = 0.0
    left: "Node | None" = None
    right: "Node | None" = None

    @property
    def label(self) -> str:
        return self.counts.most_common(1)[0][0]


def gini(counts: Counter[str], size: int) -> float:
    if size <= 0:
        return 0.0
    return 1.0 - sum((count / size) ** 2 for count in counts.values())


def best_split(values: list[list[float]], labels: list[str], indices: list[int]):
    total = Counter(labels[index] for index in indices)
    parent_impurity = gini(total, len(indices))
    best = None
    for feature_index in range(len(values[0])):
        ordered = sorted(indices, key=lambda index: values[index][feature_index])
        left_counts: Counter[str] = Counter()
        right_counts = total.copy()
        for position in range(len(ordered) - 1):
            index = ordered[position]
            label = labels[index]
            left_counts[label] += 1
            right_counts[label] -= 1
            left_size = position + 1
            right_size = len(ordered) - left_size
            if left_size < MIN_SAMPLES_LEAF or right_size < MIN_SAMPLES_LEAF:
                continue
            current = values[index][feature_index]
            following = values[ordered[position + 1]][feature_index]
            if current == following:
                continue
            impurity = (left_size * gini(left_counts, left_size)
                        + right_size * gini(right_counts, right_size)) / len(ordered)
            gain = parent_impurity - impurity
            if best is None or gain > best[0]:
                threshold = (current + following) * 0.5
                best = (gain, feature_index, threshold,
                        ordered[:left_size], ordered[left_size:])
    return best


def build_tree(values: list[list[float]], labels: list[str],
               indices: list[int], depth: int) -> Node:
    node = Node(Counter(labels[index] for index in indices))
    if depth >= MAX_DEPTH or len(node.counts) == 1 or len(indices) < 2 * MIN_SAMPLES_LEAF:
        return node
    split = best_split(values, labels, indices)
    if split is None or split[0] <= 1e-12:
        return node
    _, node.feature_index, node.threshold, left_indices, right_indices = split
    node.left = build_tree(values, labels, left_indices, depth + 1)
    node.right = build_tree(values, labels, right_indices, depth + 1)
    return node


def flatten(root: Node, feature_names: list[str], classes: list[str]):
    nodes: list[dict[str, object]] = []

    def emit(node: Node) -> int:
        index = len(nodes)
        nodes.append({})
        left = emit(node.left) if node.left else -1
        right = emit(node.right) if node.right else -1
        nodes[index] = {
            "left": left,
            "right": right,
            "featureIndex": node.feature_index,
            "feature": feature_names[node.feature_index] if node.feature_index >= 0 else "leaf",
            "threshold": node.threshold,
            "classCounts": [node.counts.get(label, 0) for label in classes],
            "prediction": node.label,
        }
        return index

    emit(root)
    return nodes


def readable_tree(node: Node, feature_names: list[str], indent: str = "") -> list[str]:
    if node.feature_index < 0:
        return [f"{indent}predict {node.label} {dict(node.counts)}"]
    lines = [f"{indent}if {feature_names[node.feature_index]} <= {node.threshold:.8g}:"]
    lines.extend(readable_tree(node.left, feature_names, indent + "  "))
    lines.append(f"{indent}else:")
    lines.extend(readable_tree(node.right, feature_names, indent + "  "))
    return lines


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("dataset", type=Path)
    parser.add_argument("--output", type=Path, default=Path("route-selector.json"))
    args = parser.parse_args()
    rows = load_rows(args.dataset)
    if not rows:
        raise ValueError("route-training dataset is empty")
    missing = [name for name in NUMERIC_FEATURES + ["route"] if name not in rows[0]]
    if missing:
        raise ValueError(f"missing route-training columns: {', '.join(missing)}")
    previous_routes = sorted({str(row.get("previous_route", "none") or "none") for row in rows})
    feature_names = NUMERIC_FEATURES + [f"previous_route={value}" for value in previous_routes]
    values = [
        [number(row.get(name), 1.0 if name == "roi_fraction" else 0.0)
         for name in NUMERIC_FEATURES]
        + [1.0 if str(row.get("previous_route", "none") or "none") == category else 0.0
           for category in previous_routes]
        for row in rows
    ]
    labels = [str(row["route"]) for row in rows]
    classes = sorted(set(labels))
    root = build_tree(values, labels, list(range(len(rows))), 0)
    rules = "\n".join(readable_tree(root, feature_names))
    artifact = {
        "features": feature_names,
        "classes": classes,
        "nodes": flatten(root, feature_names, classes),
        "rules": rules,
        "trainingRows": len(rows),
        "maxDepth": MAX_DEPTH,
        "minimumSamplesPerLeaf": MIN_SAMPLES_LEAF,
        "deployment": "Review rules and copy accepted thresholds into src/RouteModelParameters.h; never load this file at runtime.",
    }
    args.output.write_text(json.dumps(artifact, ensure_ascii=False, indent=2), encoding="utf-8")
    print(rules)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
