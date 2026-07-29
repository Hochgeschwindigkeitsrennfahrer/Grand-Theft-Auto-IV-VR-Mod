# UI / text stereo fusion — next track (after Mode 241 checkpoint)

## Symptom
World fuses (Mode 241). HUD / on-screen text does **not** — double image, wrong depth.

## Why (this engine)
Rage draws world with our L/R cameras (±IPD). Most HUD/text is **screen-space** (or a
separate 2D pass): same pixels for both eyes, or drawn with a mono projection. After
stereo Submit, each eye sees the same HUD shifted by world parallax → text cannot fuse.

Fixing world IPD cannot fix this. HUD needs its **own** stereo rule.

## Serious options (order)

### A — Preferred: SteamVR overlay for HUD (compositor layer)
1. Detect / capture the HUD RT (or BB region) once per frame after UI draw.
2. Submit it via `IVROverlay` (or a quad layer) at a fixed comfort depth (~1.5–2 m),
   **not** as part of the stereo eye textures.
3. World stays Mode 241 dual; HUD is always fused by the compositor.

Pros: correct for all head motion; industry-standard. Cons: need HUD surface isolation.

### B — Same-HUD both eyes at fixed depth (in-engine)
1. After world dual, draw HUD once into both eye textures with **identical** view
   (center camera, no IPD) **or** with a tiny matching IPD for a chosen depth plane.
2. Or: StretchRect mono HUD into both eyes with matching UV (Mode 175 phone bounds
   idea, but for whole HUD).

Pros: no OpenVR overlay API. Cons: still fights depth vs world; menus may look flat.

### C — Late-latch / TextureWithPose for head ghosting (separate issue)
Heavy head + ghosting is mostly **latency**: pose latched at dual start, head moves
before Submit. Next look-track experiment: `Submit_TextureWithPose` / sample pose at
Submit (Mode 136 late-latch pattern), not another IPD change.

Do **not** mix C with A in one build.

## Do not
- Toe-in or micro-IPD to “fix text”
- TrueStereo measured publish (FOV spazz) just for HUD
- CTimer freeze

## Suggested session order
1. Mode **242** = 204 seam + cam-right flip (A/B vs 241 scale/fusion) — this session.
2. Then HUD track: start with **B** (same HUD both eyes) as Mode 243; if flat/wrong,
   implement **A** overlay.
3. Parallel look-feel: Mode 24x late-latch Submit — only after 242 verdict.
