# Offline RE scripts (no game launch)

All addresses are **mapped RVAs** unless noted. CE `.text`: `mapped = file + 0xC00`.

| Script | Purpose |
|--------|---------|
| `verify-mapped-sites.py` | Assert critical prologues still match CE exe (exit 1 on drift) |
| `offline-seam-mapped.py` | Deep seam disasm: PublishSync / ViewConst / ReplayDispatch / … |
| `offline-caller-map.py` | ViewConst sole caller + PublishSync / ViewMat E8 lists |
| `offline-publishproj-map.py` | PublishSync↔PublishProj pairing (11/12) |
| `offline-publishsync-frames.py` | Bytes before each PublishSync call |
| `offline-gate-refs.py` / `offline-gate-cluster.py` | ViewConst gate `[0x1797694]` BSS cluster |
| `offline-ff178-map.py` | All `call [reg+0x178]` / `+0x1B4` sites |
| `offline-vsret-compare.py` | VsRet `0x2D33E` vs epilogue `0x2C73E` clarity |
| `offline-viewport-fn.py` | Viewport `0x31110` → optional ViewMat |
| `offline-22fd0.py` | `0x22FD0` view+0x280 apply |
| `offline-remap-audit.py` | Find bare file-off leftovers in docs |
| `offline-e8-31940.py` / `offline-31940-clusters.py` | PublishSync-only helper `0x31940` (×58) |
| `offline-32b40.py` | activeView thunks `0x32B40`/`0x32B60` |
| `offline-activeview-all.py` | activeView `[0x17F583C]` write/read map |
| `offline-viewmat-e8-map.py` | All 9 ViewMatWriter callers + dirty flags |
| `offline-viewmat-epilogue.py` | ViewMat → PublishSync `@0x31624` tail |
| `offline-publishsync-funcs.py` | Classify 12 PublishSync E8 sites (helper vs +Proj) |
| `offline-cc-fn-starts.py` | CC-padded fn candidates near publish cluster |
| `offline-global-1110090.py` | Refs to global matrix src `0x1110090` |
| `offline-helper-31940-body.py` | Helper `0x31940` body + thunks |
| `offline-e8-to.py` | Generic `E8 → <mappedRVA>` caller list |
| `offline-fn-319b0.py` | Publish builder `0x319B0` / local helpers |
| `offline-setactive-callers.py` | SetActiveView `0x30BF0` callers |
| `offline-aob-unique.py` / `offline-aob-mode-patterns.py` | Short vs full Mode AOB hit counts |
| `offline-gate-cluster-writers.py` / `offline-gate-neighbor.py` | Gate neighborhood WRITE map |
| `offline-dirty-2f0.py` | `view+0x2F0` dirty rebuild bit |
| `offline-replay-gate.py` / `offline-replay-writers.py` | ReplayDispatch gate `[0x17ED918]` |
| `offline-178-context.py` / `offline-178-clones.py` | All +0x178 sites; MatMul+178 clones |
| `offline-re-scan.py` | Large legacy scanner (deep section now mapped) |

```powershell
cd C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr
py scripts\verify-mapped-sites.py
py scripts\offline-seam-mapped.py
```
