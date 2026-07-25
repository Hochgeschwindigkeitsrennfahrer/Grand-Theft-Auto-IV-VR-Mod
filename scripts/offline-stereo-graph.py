#!/usr/bin/env python3
"""One-page mapped stereo publish/upload graph for CE 1.2.0.59 (offline)."""
from __future__ import annotations

print(
    """
GTA IV CE — stereo-relevant publish graph (mapped RVAs)
========================================================

View writers
  ViewMatWriter 0x314C0  (Mode 67 COUNT)  — 9 E8; dirty +0x2F0; tail PublishSync@0x31624 + Proj
  ViewConstTRUE 0x32470  (Mode 64 COUNT)  — 1 gated E8 @0x9777C2; needs [0x1797694]!=0; NO writer

Publish / sync
  PublishSync    0x30F00 (Mode 66 COUNT)  — MatMulx3 @0x307F0; if activeView→ReplayDispatch
  helper         0x31940                  — copy + 0x30720 + PublishSync-only @0x3199F (x58 E8)
  builder        0x319B0                  — 17 E8; two Sync+Proj tails
  PublishProj    0x31BA0                  — after Sync (11/12); flag [this+0x3E0]

Upload (slot178)
  ReplayDispatch 0x30CD0 → call [eax+0x178] @0x30D0D → ret @0x30D13  (Mode 42 OWNER-EDGE)
  Upload fn      0x2A1E10 (vtable …CC; ~0x847 bytes; retn 0x14)
    TWO MatMulx3→+178 paths: @0x2A217D / @0x2A25F9 (NULL [ebp+8] → second)
    both +178 sites inline ReplayDispatch-equivalent (gate + ReplayAlt + device)
    Mode41/42 OWNER-EDGE rets: 0x30D13 + 0x2A2183 + 0x2A25FF (path=ReplayDispatch|UploadA|UploadB)
  Sibling        0x2A1D50 (vtable …C8) helper-only
  Exactly 3 MatMulx3 clusters (PublishSync + two inside 0x2A1E10)
  Only 3/+12 call[+0x178] are ReplayGate-guarded: 0x30D0D + 0x2A217D + 0x2A25F9

SetActiveView 0x30BF0
  write activeView @0x30C6F → call 0x30D30 → ReplayDispatch 0x30CD0 → 0x22FD0

Globals
  activeView     [0x17F583C]  write@0x30C6F only
  device         [0x17ED8D8]  Mode 65 peek
  replayGate     [0x17ED918]  ReplayDispatch early-out; writers 0x25562 / 0x44A6E
  matrix scratch [0x1110090]  thunk 0x32B40 → helper 0x31940
  view+0x3E0     PublishProj enable byte
  view+0x2F0     dirty rebuild bit (ViewMat/Publish helpers)

Live ladder (SameFrameSeamGate=CLOSED, no dual)
  45 → 66 → 67 → 65 → 42 → 64 → 41
  Mode66/67 cadence: PASS [0.8,4] | SHARED >4 | REJECT <0.8
  py scripts/offline-stereo-graph.py
"""
)
