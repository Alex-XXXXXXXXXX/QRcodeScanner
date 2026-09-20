# Validation snapshot — 2026-09-11

## Environment

- CPU: Intel Core i5-7260U @ 2.20 GHz, 4 logical processors
- OS: Windows 10.0.22631 x64
- Build: Visual Studio 2022, Release x64, CPU-only
- Dependencies: Qt 5.14.2, ZXing-C++ 3.0.2, OpenCV 4.14.0

## Automated tests

- `QRCodeScannerSelfTest`: passed (clean QR Code and Data Matrix round trip)
- `QRCodeScannerPipelineTests`: passed (square/rectangular ECC200 dimensions, finder parity, module confidence, clean/curved/multi-region/perspective L3 recovery, OpenCV ROI recall, fixed ROI, payload validation, conflict rejection, local/camera frame sources, dual exposure, ROI tracking, continuous deadline)
- CTest: 2/2 passed
- Configurations: Release 2/2 passed; Debug 2/2 passed (performance acceptance remains Release-only)

## Performance

Clean generated set, 500 Data Matrix + 500 QR runs after build:

- correct: 1000/1000
- wrong output: 0
- No Read: 0
- first-read rate: 100%
- P50: 0.292 ms
- P95: 0.745 ms
- L0 stage P95: 0.388 ms
- cross-split near-duplicate leakage: 0

Current field 48×48 Data Matrix, 50 continuous runs, fixed four-corner recipe, no ground-truth payload:

- detector/regression reached: 50/50
- wrong output: 0
- P50: 80.396 ms
- P95: 80.817 ms
- maximum: 81.070 ms
- ROI stage P95: 6.711 ms
- L3 stage P95: 52.521 ms
- result: safe No Read/Timeout; no unchecked payload was emitted

This field image is a `detect_only` regression because its true encoded payload has not been supplied. It cannot contribute to the first-read-rate claim.

## Synthetic boundary sweep

42 cases across perspective, defocus, motion blur, low contrast, glare and occlusion:

- correct: 35
- safe No Read: 7
- wrong output: 0
- P95: 80.580 ms
- maximum: 91.854 ms
- maximum successful level (0–6): perspective 6, defocus 6, motion 3, contrast 6, glare 2, occlusion 6

## Acceptance work still requiring external data/hardware

- collect at least 500 initial independent images and freeze at least 1000 labelled images;
- provide the true payload for the current field image;
- connect a concrete camera SDK implementation to `IIndustrialCameraBackend` and validate global shutter, PLC trigger, exposure and strobe recipes;
- run the same frozen images through the incumbent industrial scanner for black-box A/B;
- measure automatic ROI recall on independently labelled `groundTruthCorners`; the current 6.711 ms ROI number uses the locked field quadrilateral and does not prove the 99.8% automatic-localization gate;
- do not claim the 99.5% vendor-level target until those measurements pass with zero wrong outputs.

## Tool-chain checks

- a deliberately duplicated generated image in train/test produced benchmark exit code 5 and `nearDuplicateLeakageCount: 1`;
- the pure-stdlib route trainer exported a valid one-node model from 1000 clean benchmark records (a useful multi-route model still requires field data);
- `compare_vendor_ab.py` paired both rows in the example vendor CSV and emitted `ab-example.json`; this is a format/logic check, not a real vendor comparison.
