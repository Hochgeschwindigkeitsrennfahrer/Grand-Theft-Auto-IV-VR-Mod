# In-game settings menu integration (researched offline, 2026-07-30)

Question: can we put a "VR render mode" row into GTA IV's own pause menu, the way
FusionFix puts Motion Blur / FOV / Graphics API there, so AER and Mode 243 can be
swapped without Notepad?

**Short answer: yes, but in three separate layers, and only one of them is cheap.**
The cheap layer is already implemented (`src/asi/menu_bridge.cpp`). The expensive
layer is a real RE session and is written up here so it can be scheduled later.

Everything below was read from FusionFix `master` (`source/settings.ixx`) and the
FusionFix data folder. Nothing here is headset- or CE-verified.

---

## 1. How FusionFix actually does it

### Layer A — the menu rows come from a data file

`common/data/frontend_menus.xml` defines the pause-menu pages and their rows.
FusionFix ships its own copy at `data/update/common/data/frontend_menus.xml`
(~88 KB plain XML) and loads it through the `update` folder / Fusion OverLoader.
That file is what makes the extra Display and Game rows appear at all.

Consequence: adding a row does **not** require patching the menu renderer. It
requires shipping a `frontend_menus.xml` that already contains the row — and the
row must reference a preference name the executable knows.

### Layer B — the preference-name table is patched in memory

The game holds an array of `{ uint32 prefID; char* name; }` plus a count.
FusionFix (`settings.ixx` ~254-282, ~356):

1. finds the pointer to that array and the count with byte patterns
   (`8B 04 FD ? ? ? ? 5F 5E C3`, count at `81 FF ? ? ? ? 7C DF`),
2. copies every original entry into its own `std::vector aMenuPrefs`,
3. appends its own `PREF_SKIP_INTRO`, `PREF_CUSTOMFOV`, `PREF_GRAPHICSAPI`, …
   starting at `firstCustomID = last original ID + 1`,
4. rewrites the array pointer and the count with `injector::WriteMemory`.

The `MENU_DISPLAY_*` enum table (the "Low / Medium / High" strings) is extended
the same way (~359-402), and it is **already at capacity** — the source says so
in a comment. Plain on/off rows still fit.

### Layer C — value routing and labels

* Four `injector::MakeInline` mid-hooks on the menu's get/set sites route reads
  and writes for custom IDs to `FusionFixSettings.Get/Set` (~657-782). Custom
  values live in FusionFix's own map and persist to its `.cfg`.
* `CText::GetTextByKey` / `DoesTextLabelExist` are inline-hooked (~35-82) so
  custom GXT keys resolve without touching the game's GXT archives.
* Sliders are made by re-pointing unused episodic race entries
  (`PREF_EPISODIC_RACECLASS_RACE_3` → `PREF_CUSTOMFOV`, ~405-436).

### Layer D — the read-back API, and this is our seam

```cpp
int32_t __cdecl NATIVE_GET_NUMBER_OF_INSTANCES_OF_STREAMED_SCRIPT(char* name)
{
    auto ret = hbGET_NUMBER_OF_INSTANCES_OF_STREAMED_SCRIPT.fun(name);
    if (!ret && std::string_view(name).starts_with("PREF_"))
        return FusionFixSettings.Get(name);   // settings.ixx ~633-647
    return ret;
}
```

FusionFix chains that native so **any** mod can read **any** preference — its own
custom ones included — by passing the `PREF_*` string. We already have
`GetNativeHandlerByName` + `InvokeNativeFn` working (PedHide precedent), so this
costs us about thirty lines and zero memory patching.

### What does not apply to us

**IVMenuAPI** (Zolika1351, `LoadLibraryA("IVMenuAPI.asi")` + `AddIVMenuOption`)
is the clean plugin API for exactly this, and FusionFix forks use it. It supports
**1.0.7.0 / 1.0.8.0 only** and is documented as incompatible with upstream
FusionFix. We are on Complete Edition with upstream FusionFix, so it is out.

---

## 2. What is implemented now — `src/asi/menu_bridge.cpp`

No patching, no new hook, off unless configured.

| File next to the ASI | Meaning |
|---|---|
| `gtaiv_dxvk_vr.menupref` | name of the FusionFix pref to follow, e.g. `PREF_LEDILLUMINATION`. Missing = pref polling off |
| `gtaiv_dxvk_vr.menumap` | one `value=stereoMode` per line. Default `0=243`, `1=53`, `2=0` |

Behaviour: every 400 ms on the game thread, read the pref through the native
above. On change, look up the profile, `WriteStereoModeFile(mode)` +
`ReloadStereoMode()`, and log the transition. `F4` and the right controller menu
button cycle the same profile list, so the feature works even with no FusionFix
row at all.

### Choosing a row to repurpose (until we own one)

Any FusionFix pref works as the selector. Pick one that is inert in VR — the
label will still read the original name, and FusionFix will still apply the
original effect.

| Candidate | States | Side effect in VR | Verdict |
|---|---|---|---|
| `PREF_LEDILLUMINATION` (LightSyncRGB) | 0/1 | none without Logitech hardware | **best 2-state pick** |
| `PREF_UPDATE` (CheckForUpdates) | 0/1 | startup only | fine, semantically odd |
| `PREF_EXTRANIGHTSHADOWS` | 0..3 | actually enables broken night shadows above 0 | only if you need 4 states |
| `PREF_MOTIONBLUR` | 0..4 | applies motion blur — the one thing VR must not have | do not use |

Start with `PREF_LEDILLUMINATION` and a two-entry map:

```text
0=243
1=53
```

---

## 3. The real row — scheduled, not done

To get a row that says "VR Render Mode: TrueStereo / AER / Off":

1. **Run after FusionFix.** Our ASI must not touch the table before FusionFix has
   replaced it. Gate on `GetModuleHandleA("GTAIV.EFLC.FusionFix.asi")` plus a
   delay, and re-read the *current* pointer/count — which will already be
   FusionFix's vector, not the game's.
2. **Append our entry** using FusionFix's own recipe: copy the current array into
   a static vector that outlives the process, append
   `{ lastID + 1, "PREF_VRMODE" }`, rewrite the pointer and the count at the same
   pattern sites. Every original FusionFix entry must keep its ID or its rows
   break.
3. **Add the row** to `update/common/data/frontend_menus.xml`, copying an
   existing on/off Display row and swapping in `PREF_VRMODE`.
4. **Label it** by inline-hooking `CText::GetTextByKey` / `DoesTextLabelExist`
   for our key only, falling through for everything else.
5. `menu_bridge` then needs **no change** — set `gtaiv_dxvk_vr.menupref` to
   `PREF_VRMODE` and it reads it through the same native.

Risk to weigh before scheduling: step 2 double-patches the exact table FusionFix
owns. Getting the ordering wrong loses every FusionFix menu row, and FusionFix
updates can move the patterns. The fallback (`F4` + repurposed row) costs nothing
and cannot break the pause menu, so this is a polish task, not a blocker.

---

## 4. Nicer than a menu row, in VR

The pause menu is small, flat and hard to read through a G2. Two cheaper wins:

* **Haptic feedback on profile change** — pulse the right controller N+1 times
  for profile N via `IVRInput::TriggerHapticVibrationAction`. Confirms the switch
  without taking the headset off.
* **Canvas OSD** — draw the profile name into the eye canvas for two seconds.
  `hud_layout.cpp` already knows the canvas geometry; this is where it would go.

Neither is implemented yet.
