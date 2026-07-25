#!/usr/bin/env python3
"""Audit docs for likely file-offset-as-RVA leftovers in the publish family range."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Old file-offs that must not appear as bare "live RVA" without mapped note
SUSPECT = {
    0x30300: 0x30F00,
    0x308C0: 0x314C0,
    0x308C9: 0x314C9,
    0x3187C: 0x3247C,
    0x31870: 0x32470,
    0x300D0: 0x30CD0,
    0x3010D: 0x30D0D,
    0x2FFF0: 0x30BF0,
    0x2FBF0: 0x307F0,
    0x30FA0: 0x31BA0,
    0x2C73E: 0x2D33E,
    0x2C6AC: 0x2D2AC,
    0x2C180: 0x2CD80,
    0x70637C: 0x706F7C,
}


def main() -> int:
    docs = list((ROOT / "docs").glob("*.md"))
    print("Scanning docs for bare suspect file-off tokens...")
    total = 0
    for path in sorted(docs):
        text = path.read_text(encoding="utf-8", errors="replace")
        for fo, mapped in SUSPECT.items():
            # bare hex like 0x30300 not immediately followed by mapped mention on same line
            for i, line in enumerate(text.splitlines(), 1):
                if f"0x{fo:X}" not in line and f"0x{fo:x}" not in line:
                    continue
                # skip conversion-table rows that intentionally show both
                if f"0x{mapped:X}" in line or "file" in line.lower() or "old" in line.lower():
                    continue
                if "WRONG" in line or "file-off" in line or "file off" in line.lower():
                    continue
                print(f"  {path.name}:{i}: file-off 0x{fo:X} (want mapped 0x{mapped:X}) :: {line.strip()[:100]}")
                total += 1
    print(f"Done. suspicious bare refs: {total}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
