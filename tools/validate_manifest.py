#!/usr/bin/env python3
"""Validate the industrial barcode JSONL schema and detect split leakage."""

from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter, defaultdict
from pathlib import Path


REQUIRED = ("image", "split", "batch", "device", "tags")
VALID_SPLITS = {"train", "validation", "test", "regression"}


def valid_corners(value: object) -> bool:
    return (
        isinstance(value, list)
        and len(value) == 4
        and all(
            isinstance(point, list)
            and len(point) == 2
            and all(isinstance(coordinate, (int, float)) for coordinate in point)
            for point in value
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--require-ground-truth", action="store_true")
    args = parser.parse_args()
    base = args.manifest.resolve().parent
    errors: list[str] = []
    split_counts: Counter[str] = Counter()
    batch_splits: defaultdict[str, set[str]] = defaultdict(set)
    sequence_splits: defaultdict[str, set[str]] = defaultdict(set)
    hashes: defaultdict[str, list[tuple[int, str]]] = defaultdict(list)
    row_count = 0

    with args.manifest.open("r", encoding="utf-8") as stream:
        for line_number, raw in enumerate(stream, 1):
            raw = raw.strip()
            if not raw or raw.startswith("#"):
                continue
            row_count += 1
            try:
                item = json.loads(raw)
            except json.JSONDecodeError as exc:
                errors.append(f"line {line_number}: invalid JSON: {exc}")
                continue
            missing = [key for key in REQUIRED if key not in item]
            if missing:
                errors.append(f"line {line_number}: missing {', '.join(missing)}")
            split = str(item.get("split", ""))
            if split not in VALID_SPLITS:
                errors.append(f"line {line_number}: invalid split {split!r}")
            split_counts[split] += 1
            batch_splits[str(item.get("batch", ""))].add(split)
            sequence = str(item.get("sequence", ""))
            if sequence:
                sequence_splits[sequence].add(split)
            expected = str(item.get("expectedText", ""))
            if args.require_ground_truth and not expected:
                errors.append(f"line {line_number}: expectedText is required")
            for corner_field in ("corners", "groundTruthCorners"):
                if corner_field in item and not valid_corners(item[corner_field]):
                    errors.append(
                        f"line {line_number}: {corner_field} must contain four [x,y] points"
                    )
            outcome = str(item.get("expectedOutcome", "success"))
            if outcome not in {"success", "detect_only", "no_read"}:
                errors.append(f"line {line_number}: invalid expectedOutcome {outcome!r}")
            image_value = str(item.get("image", ""))
            if image_value.startswith("generated:"):
                continue
            image_path = (base / image_value).resolve()
            if not image_path.is_file():
                errors.append(f"line {line_number}: missing image {image_path}")
                continue
            digest = hashlib.sha256(image_path.read_bytes()).hexdigest()
            hashes[digest].append((line_number, split))

    for batch, splits in batch_splits.items():
        if batch and len(splits) > 1:
            errors.append(f"batch leakage: {batch!r} occurs in {sorted(splits)}")
    for sequence, splits in sequence_splits.items():
        if len(splits) > 1:
            errors.append(f"sequence leakage: {sequence!r} occurs in {sorted(splits)}")
    for digest, occurrences in hashes.items():
        splits = {split for _, split in occurrences}
        if len(splits) > 1:
            errors.append(f"exact-image leakage {digest[:12]}: {occurrences}")

    print(json.dumps({
        "rows": row_count,
        "splits": split_counts,
        "uniqueImages": len(hashes),
        "errors": errors,
    }, ensure_ascii=False, indent=2))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
