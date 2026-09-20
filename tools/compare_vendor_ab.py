#!/usr/bin/env python3
"""Paired black-box A/B comparison for this scanner and a vendor reader.

Vendor CSV/JSON fields: image, status (success/no_read), text, latencyMs or
latencyUs. Run QRCodeScannerBenchmark with --iterations 1 on the same frozen
manifest so expectedText is available in its records.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Any


def load_records(path: Path) -> list[dict[str, Any]]:
    suffix = path.suffix.lower()
    if suffix == ".csv":
        with path.open("r", encoding="utf-8-sig", newline="") as stream:
            return list(csv.DictReader(stream))
    if suffix == ".jsonl":
        with path.open("r", encoding="utf-8") as stream:
            return [json.loads(line) for line in stream if line.strip() and not line.startswith("#")]
    document = json.loads(path.read_text(encoding="utf-8"))
    return document.get("records", []) if isinstance(document, dict) else document


def key(value: object) -> str:
    return str(value or "").replace("\\", "/").strip()


def succeeded(record: dict[str, Any]) -> bool:
    if "success" in record:
        value = record["success"]
        return value is True or str(value).lower() in {"1", "true", "yes"}
    return str(record.get("status", "")).lower() == "success"


def latency_ms(record: dict[str, Any]) -> float | None:
    try:
        if record.get("latencyMs") not in (None, ""):
            return float(record["latencyMs"])
        if record.get("latencyUs") not in (None, ""):
            return float(record["latencyUs"]) / 1000.0
    except (TypeError, ValueError):
        pass
    return None


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[max(0, min(len(ordered) - 1, math.ceil(len(ordered) * fraction) - 1))]


def mcnemar_exact(ours_only: int, vendor_only: int) -> float:
    discordant = ours_only + vendor_only
    if discordant == 0:
        return 1.0
    tail = sum(math.comb(discordant, index)
               for index in range(min(ours_only, vendor_only) + 1)) / (2 ** discordant)
    return min(1.0, 2.0 * tail)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ours", type=Path, required=True,
                        help="QRCodeScannerBenchmark JSON output")
    parser.add_argument("--vendor", type=Path, required=True,
                        help="vendor CSV, JSON, or JSONL result")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    ours_all = load_records(args.ours)
    ours = {key(row.get("image")): row for row in ours_all
            if int(row.get("iteration", 0)) == 0 and row.get("expectedText")}
    vendor = {key(row.get("image")): row for row in load_records(args.vendor)}
    missing_vendor = sorted(set(ours) - set(vendor))
    extra_vendor = sorted(set(vendor) - set(ours))

    ours_only = vendor_only = both_correct = neither_correct = 0
    ours_wrong = vendor_wrong = 0
    ours_latencies: list[float] = []
    vendor_latencies: list[float] = []
    paired: list[dict[str, Any]] = []
    for image in sorted(set(ours) & set(vendor)):
        our_row = ours[image]
        vendor_row = vendor[image]
        expected = str(our_row["expectedText"])
        our_success = succeeded(our_row)
        vendor_success = succeeded(vendor_row)
        our_text = str(our_row.get("text", ""))
        vendor_text = str(vendor_row.get("text", ""))
        our_correct = our_success and our_text == expected
        vendor_correct = vendor_success and vendor_text == expected
        ours_wrong += our_success and not our_correct
        vendor_wrong += vendor_success and not vendor_correct
        both_correct += our_correct and vendor_correct
        ours_only += our_correct and not vendor_correct
        vendor_only += vendor_correct and not our_correct
        neither_correct += not our_correct and not vendor_correct
        our_latency = latency_ms(our_row)
        vendor_latency = latency_ms(vendor_row)
        if our_latency is not None:
            ours_latencies.append(our_latency)
        if vendor_latency is not None:
            vendor_latencies.append(vendor_latency)
        paired.append({
            "image": image,
            "expectedText": expected,
            "ours": {"status": our_row.get("status"), "text": our_text,
                     "correct": our_correct, "latencyMs": our_latency},
            "vendor": {"status": vendor_row.get("status"), "text": vendor_text,
                       "correct": vendor_correct, "latencyMs": vendor_latency},
        })

    sample_count = len(paired)
    summary = {
        "pairedImages": sample_count,
        "missingVendorImages": missing_vendor,
        "extraVendorImages": extra_vendor,
        "bothCorrect": both_correct,
        "oursOnlyCorrect": ours_only,
        "vendorOnlyCorrect": vendor_only,
        "neitherCorrect": neither_correct,
        "ours": {
            "firstReadRate": (both_correct + ours_only) / sample_count if sample_count else 0.0,
            "wrongOutputs": ours_wrong,
            "p50Ms": percentile(ours_latencies, 0.50),
            "p95Ms": percentile(ours_latencies, 0.95),
        },
        "vendor": {
            "firstReadRate": (both_correct + vendor_only) / sample_count if sample_count else 0.0,
            "wrongOutputs": vendor_wrong,
            "p50Ms": percentile(vendor_latencies, 0.50),
            "p95Ms": percentile(vendor_latencies, 0.95),
        },
        "mcnemarExactPValue": mcnemar_exact(ours_only, vendor_only),
        "pairedRecords": paired,
    }
    args.output.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({key: value for key, value in summary.items()
                      if key != "pairedRecords"}, ensure_ascii=False, indent=2))
    if missing_vendor or sample_count == 0:
        return 3
    return 4 if ours_wrong else 0


if __name__ == "__main__":
    raise SystemExit(main())
