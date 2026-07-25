# Mapped RVA cheat sheet — GTA IV CE 1.2.0.59

**Rule:** `.text` / `.rdata` → **`mappedRVA = fileOffset + 0xC00`**  
(`.data` globals use absolute VAs as encoded in instructions, e.g. `[0x17F583C]`.)

Verify anytime: `py scripts/verify-mapped-sites.py`

| Name | File-off (old docs) | **Mapped RVA** | Mode / status |
|------|---------------------|----------------|---------------|
| PublishSync | `0x30300` | **`0x30F00`** | **66** COUNT |
| ViewMatWriter | `0x308C0` | **`0x314C0`** | **67** COUNT |
| ViewConst TRUE | `0x31870` | **`0x32470`** | **64** COUNT (gated) |
| ViewConst mid | `0x3187C` | **`0x3247C`** | unsafe mid |
| PublishProj | `0x30FA0` | **`0x31BA0`** | after PublishSync |
| MatMul | `0x2FBF0` | **`0x307F0`** | PublishSync callee |
| SetActiveView | `0x2FFF0` | **`0x30BF0`** | writes `[0x17F583C]` |
| SetActive helper | — | **`0x30D30`** | after activeView write; before ReplayDispatch |
| ReplayDispatch | `0x300D0` | **`0x30CD0`** | **68** COUNT |
| Upload fn (vtable) | — | **`0x2A1E10`** | two +178; vtable slot after `0x2A1D50` (`…CC`) |
| Upload +178 rets | — | **`0x2A2183`**, **`0x2A25FF`** | Mode **41/42** OWNER-EDGE (with `0x30D13`) |
| Sibling helper-only | — | **`0x2A1D50`** | vtable neighbor; helper only |
| `call [eax+0x178]` | `0x3010D` | **`0x30D0D`** | Mode **42** edge; uploads view+0x80 → **0x220D0** |
| ret after slot178 | `0x30113` | **`0x30D13`** | Mode **42** OWNER-EDGE |
| slot178 live target | — | **`0x220D0`** | Mode **65** OK = DXVK SetVSConstF |
| VsRet (SetVSConstF obs) | `0x2C73E` | **`0x2D33E`** | Mode 34/41 — `85 C0 75 14` |
| call178 near VS region | — | **`0x2C738`** | different path; ret epilogue at mapped `0x2C73E` |
| VS wrap FORBIDDEN | `0x2C6AC` | **`0x2D2AC`** | crash |
| VS wrapper fn | `0x2C180` | **`0x2CD80`** | |
| view+0x280 apply | — | **`0x22FD0`** | SetActive/viewport tail |
| Copy→PublishSync helper | — | **`0x31940`** | copy src→this; `call 0x30720`; PublishSync `@0x3199F` (×58) |
| Small copy helper | — | **`0x30720`** | used by `0x31940` (not MatMul `0x307F0`) |
| activeView thunks | — | **`0x32B40`**, **`0x32B60`** | `ecx=activeView; call 0x31940` |
| Global matrix src | — | **`[0x1110090]`** | pushed by thunk **`0x32B40`** into helper |
| Publish builder | — | **`0x319B0`** | 17 E8; two PublishSync+Proj tails |
| Small dword copy | — | **`0x31810`** | 44 E8; often just before PublishSync |
| Local helper | — | **`0x317E0`** | 2 E8; PublishSync+Proj |
| FovSite CALL | `0x70637C` | **`0x706F7C`** | Mode 45 SAFE |
| FindPlayerPed | `0x4D14E0` | **`0x4D20E0`** | SAFE AOB |
| ViewConst parent | — | **`0x977600`** | sole `E8→0x32470` |
| ViewConst call | — | **`0x9777C2`** | gated by `[0x1797694]` |
| activeView write | — | **`0x30C6F`** | only static writer of `[0x17F583C]` |
| activeView cmp (PublishSync) | — | **`0x30F49`** | gate before `call 0x30CD0` |
| activeView cmp (near ViewMat) | — | **`0x3143F`** | in viewport fn **`0x31110`** after optional ViewMat |
| Viewport/resize | — | **`0x31110`** | writes view rect/`+0x280..`; may call ViewMat |

## Live test order (when game quit)

1. Deploy `out-asi` ASI (`gtaiv_dxvk_vr.buildid` = **`20260725-0359-mode68-count`** or later)  
2. stereo **`68`** → `COUNT armed exeRva=0x30CD0` then `cadence=PASS|SHARED|REJECT`  
3. Kill **`45`**  
4. Already done: **66 PASS**, **67 REJECT**, **65 OK** (`slot178=0x220D0`), **42 dry**, **64 REJECT**

SameFrameSeamGate=CLOSED. No dual. **SHARED** is OK (shared publish helper).

### Dual plan NOTES only (not implemented)

ReplayDispatch ABI → slot178 **`0x220D0`**:
- `ecx` = device `[0x17ED8D8]`; push count=`0x10`, src=`view+0x80`, startReg=`0`, device
- Future L/R: write distinct matrices at **view+0x80** **before** `call [eax+0x178]` @ **`0x30D0D`**
- Candidate inject: after gate inside **`0x30CD0`**, before lea/push (~`0x30CF0`..`0x30D0D`)
- Do **not** open SameFrameSeamGate until Mode **68** PASS + dual plan review
