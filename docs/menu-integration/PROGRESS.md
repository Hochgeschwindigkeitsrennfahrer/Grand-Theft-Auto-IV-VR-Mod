# Progress — `menu-integration`

Branch `menu-integration`, based on `origin/master` = `74e2ada`
("Checkpoint Mode 243: fused stereo + late-latch Submit on Mode 203").

Goal for this session: an in-game settings menu that can be judged **on flat**,
because there is no headset available.

---

## Status

| | |
|---|---|
| ASI builds clean (x86, VS 2022) | ✅ |
| Overlay renders — verified offline at 1920×1080 and 1280×720 | ✅ |
| Device state save/restore — verified by the magenta-strip test | ✅ |
| Settings persist to the existing `gtaiv_dxvk_vr.*` sidecar files | ✅ by construction (same set+save pairs as the F5..F11 cyclers) |
| **In-game on flat** | ⬜ **needs a run — this is the next step** |
| In-headset | ⬜ blocked, no headset |
| Visible in the headset on the DrawWalk dual modes (200/203/204/241–243) | ⬜ known gap, see README |

---

## What was added

```
src/asi/menu_draw.{h,cpp}        D3D9 overlay renderer (atlas + quads + state save)
src/asi/vr_menu.{h,cpp}          the menu: rows, input, layout
src/asi/menu_bridge.{h,cpp}      render-profile list, shared with the F4 hotkey
tools/menu_preview/              offline render test (no game, no headset)
scripts/build-menu-preview.ps1   builds it
docs/menu-integration/           this folder
docs/MENU_INTEGRATION.md         copied in from claude/mode-244-render-pose
```

Modified:

```
src/asi/hooks.cpp                one call at the top of HookEndScene
src/asi/stereo_config.{h,cpp}    Menu* accessors (+~100 lines, no behaviour change)
scripts/build-asi.ps1            three new sources + gdi32.lib
.gitignore                       /out-preview/
```

---

## Decisions worth remembering

**Base is `origin/master`, not the active `claude/mode-244-render-pose` branch.**
That branch carries 27 in-flight Mode 244–267 render-pose experiments. Basing on
the shipped checkpoint keeps this branch reviewable and mergeable.

**`menu_bridge.cpp` was ported, not copied.** The original on
`claude/mode-244-render-pose` depends on `vr_input.h` (not present at this base)
and on invoking a game native from a per-frame tick. What came across is the part
that is safe on the render thread: the profile table, the F4 cycle, and apply.
The `PREF_*` poll stayed behind — see README, "What is not on this branch".

**F3 for the toggle.** F4–F11 are taken by the mod (F4 profile cycle, F5 vres,
F6 stereoscale, F7 worldscale, F8 IPD, F9 recenter, F10 cam, F11 truescale) and
F12 is Steam's screenshot key. F1–F3 were free — verified by grepping every
`VK_F*` in `src/asi/`.

**Arrow keys *and* numpad.** The overlay cannot swallow input without hooking
WndProc or DirectInput, which is new risk for little gain. So the game still sees
whatever you press while the menu is open. The numpad is the safer set — GTA IV
does not bind it by default — and the arrows are there for keyboards without one.

**Input is polled from `HookEndScene`, not from inside `TryMonoSubmit`.** Every
existing hotkey poll sits inside `TryMonoSubmit`, which early-returns when there
is no headset. That is why F5..F11 do nothing on a flat machine, and why the menu
had to be wired one level up.

**The overlay only draws when it is open.** Closed, the per-frame cost is one
`GetAsyncKeyState` sweep and a foreground check — no device calls at all.

---

## Open items, roughly in order

1. **Run it in-game on flat.** Nothing here has touched GTA IV yet. Watch for:
   the panel appearing at all (the fixed-function pipeline may never have been
   exercised in this process before — GTA IV is a shader-only renderer, and DXVK
   has to synthesise the FF shaders); the game image being untouched after F3
   closes it; no FPS cliff while it is open.
2. **Eye-texture draw site for the DrawWalk dual modes.** Needs a headset to
   judge. Site is identified in README.
3. **Controller binding.** A headset user cannot see the keyboard. The natural
   route is the right menu button — `vr_input.cpp`'s `VrInputMenuChordPressed()`
   already exists on `claude/mode-244-render-pose`. `VrMenuSetOpen(bool)` is the
   seam it would call.
4. **Gate rows that do nothing on the live mode.** `GetStereoScale()` force-returns
   1.0 on modes 231–240, and `ReloadStereoMode` resets world scale to 100 on modes
   191–243, so those two rows can read as "changed" while the mode ignores them.
   Greying them out would be honest.
5. **`WriteStereoModeFile` silently rejects out-of-range modes.** It accepts
   `<=53`, 120–136, 140–150, 151–216 and 230–243, and returns without a word for
   anything else. A custom `gtaiv_dxvk_vr.menumap` naming e.g. mode 220 would look
   like the menu is broken. Worth a log line.

---

## How this was verified

Offline, via `tools/menu_preview` — a real D3D9 device, the real `menu_draw.cpp`,
`vr_menu.cpp` and `menu_bridge.cpp`, with only the `stereo_config` setters stubbed
so the harness does not have to link the whole VR pipeline.

The harness clears the bottom strip to magenta before the overlay draws and paints
over it with its own untextured gradient afterwards. Magenta surviving in the
output would mean the overlay left its texture, FVF or blend state bound. It does
not survive — see the screenshots in this folder.

The draw-site, threading, state-save and DXVK constraints above were cross-checked
against the source by a separate review pass before the code was written; the
DrawWalk-dual gap came out of that pass, not out of testing.

---

## Row naming, and what actually applies on Mode 243

Two rows originally both said "world scale" and are unrelated knobs. Renamed to
match the hotkey and the log lines:

* **Framing preset (F7)** — `gtaiv_dxvk_vr.worldscale`. A *pair*: `fovadd` plus
  stereo strength. One axis, zoom vs open. The name is the fovadd value
  (Open12 = 12 deg). Higher fovadd renders wider, so the canvas overfills the
  eye and the StretchRect crops it — that reads as zoom-in. Lower opens out;
  MatchH6 is ~100% horizontal match on a G2 at 16:9, Window0 can show V bars.
  Stereo strength moves the other way (115 -> 130) to cancel the apparent size
  change; capped at 130 because 150% x IPD broke fusion.
* **6DoF move scale (F11)** — `gtaiv_dxvk_vr.scale`. How far the world moves
  when the head moves. On Mode 187+ it also multiplies the IPD baseline.

**The fovadd half of the framing preset is inert on every mode past 162,
including 243.** `WantsFpAbsoluteFov` is true there (`IsOursFpPost162`), and
that branch of `HookCamFovSite` (cam_matrix.cpp:1781) writes `fpfov` straight
into `CCam+0x60` and never reaches the `add` branch at cam_matrix.cpp:1806.
On those modes **First-person FOV** is the live FOV knob; the framing preset
only moves stereo strength. The row help now says so.

Stereo strength itself is separately force-ignored on modes 231-240
(`GetStereoScale` returns 1.0 for `IsTrueStereoExact`). 243 is not in that set,
so it does apply there.
