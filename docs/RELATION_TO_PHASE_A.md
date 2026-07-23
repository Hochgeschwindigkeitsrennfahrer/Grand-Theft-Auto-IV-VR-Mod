# Relation to Phase A (`gtaiv-openxr`)

## Two projects, one goal

| | Phase A | Phase B (this repo) |
|--|---------|---------------------|
| Path | `Documents\gtaiv-openxr` | `Documents\gtaiv-dxvk-vr` |
| Mechanism | ASI + companion **D3D11** + OpenXR | VR-**DXVK `d3d9.dll`** + Vulkan → OpenXR |
| Role | **Default / active** | **Pivot ready** |
| Inspired by | Halo / crysis companion D3D11 | L4D2VR / HL2VR / openRBRVR |

Keep **both**. Do not delete Phase A when opening this folder.

## When to pivot to Phase B

Switch when **any** of these are true:

- D3D9→D3D11 blit repeatedly fails (MSAA, shared resource, stability) after serious attempts
- You explicitly want the HL2/L4D2 architecture
- You are tired of Phase A and accept a longer DXVK-fork ramp

Do **not** pivot only because DXVK “sounds easier” — Phase B is heavier up front (see Phase A chat conclusions).

## When to stay on Phase A

- OpenXR session + Present hooks already work and only blit/camera remain
- You want the smallest next step to first pixels

## Sharing work between repos

- Camera RE, FusionFix deploy habits, WMR runtime choice → apply to both
- Do not merge trees; copy docs/snippets deliberately
- If Phase B wins long-term, archive Phase A with a README pointer here
