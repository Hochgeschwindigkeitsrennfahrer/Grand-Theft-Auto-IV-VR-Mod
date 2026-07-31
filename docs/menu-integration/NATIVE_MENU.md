# A row in GTA IV's own pause menu

`docs/MENU_INTEGRATION.md` section 3 costed this as an RE session: patch the
preference-name table in memory, ship a `frontend_menus.xml`, inline-hook
`CText::GetTextByKey` for labels, and route reads through a chained script
native. Its own risk note says step 2 double-patches the table FusionFix owns,
and getting the ordering wrong loses every FusionFix menu row.

Measured against the actual install on 2026-07-31, three of those four turned
out to be unnecessary. **Pause → Controls → "VR Render Mode" now switches the
stereo mode, and no memory is patched at all.**

---

## What the install actually looks like

```
GTAIV\update\common\data\frontend_menus.xml     FusionFix's copy, 89 KB, loaded
GTAIV\plugins\GTAIV.EFLC.FusionFix.asi          16 MB
GTAIV\plugins\GTAIV.EFLC.FusionFix.cfg          where its menu prefs persist
```

The Controls page is one `<menu>` block:

```xml
<menu HeaderText="MH_CON" enum="SCR_CONTROLS">
<optionspc action="MENUOPT_KEYBOARD_OPTIONS" label="MO_KBOPT"    value="PREF_NULL" ... />
<options   action="MENUOPT_CONTROLLER_OPTIONS" label="MO_CTRLOPT" value="PREF_NULL" ... />
<optionspc action="MENUOPT_ADJUST" label="Always Run" value="PREF_ALWAYSRUN" scaler="2" displayValue="MENU_DISPLAY_ON_OFF" />
...
<options action="END_OF_MENU_OPTIONS" label="" value="0" scaler="0" displayValue="0" />
</menu>
```

## The three assumptions that were wrong, in our favour

**1. Labels do not need a GXT hook.** `label="Always Run"` is a literal string,
not a GXT key — FusionFix's existing `CText` hook already falls through to the
literal. Adding `label="VR Render Mode"` just works. The doc budgeted an inline
hook on `GetTextByKey` / `DoesTextLabelExist` for this.

**2. The preference table does not need patching.** The row borrows
`PREF_LEDILLUMINATION` — FusionFix's own LightSyncRGB, which does nothing
without Logitech hardware. That is exactly the repurposing the doc recommends in
section 2, and it means **nothing is appended to the pref table**, so the doc's
headline risk (losing every FusionFix row) does not exist here.

**3. Reading the value does not need a script native.** This is the important
one. FusionFix persists its menu prefs to `GTAIV.EFLC.FusionFix.cfg` and writes
that file **the moment the row is toggled, with the game still running** —
verified by diffing the cfg against a snapshot while the process was up:

```
=> LightSyncRGB = 0        # appeared after toggling, game still running
```

So the ASI polls a file instead of calling
`GET_NUMBER_OF_INSTANCES_OF_STREAMED_SCRIPT`. That matters for two reasons:

* natives must not be invoked from the EndScene thread, and
* the game-thread site that could host them (`HookCopyMat`) only installs after
  360 successful VR submits (`openvr_mono.cpp`, `kCamAfter = 360`), so on flat
  it never arms at all.

File polling is safe from the render thread and works identically flat and in
VR. `MenuBridgeTickPrefFile()` stats the cfg every 500 ms and only re-reads when
the write time changes.

---

## Wiring

```
scripts/install-vr-menu-row.ps1     splices the row into the installed XML
GTAIV\gtaiv_dxvk_vr.menukey         "LightSyncRGB" — which cfg key to follow
GTAIV\gtaiv_dxvk_vr.menumap         value=stereoMode, so 0=243 / 1=170 / 2=53 / 3=0
```

The row is On/Off, so it selects between the profiles numbered **0 and 1** —
`243 TrueStereo` and `170 Clean` with the default map.

The first cfg reading is recorded but never applied, so `gtaiv_dxvk_vr.stereo`
still decides the mode at startup. Only a change made in the pause menu switches
anything.

Verified in-game, flat, 2026-07-31:

```
MenuBridge: following pause-menu pref "LightSyncRGB" in FusionFix.cfg
MenuBridge: pause-menu pref "LightSyncRGB" = 0 at startup (mode left at 0)
MenuBridge: profile 1 -> stereo 170 (was 0, live now 170) via pause menu
MenuBridge: profile 0 -> stereo 243 (was 170, live now 243) via pause menu
```

Confirmed stable with no input: the switch count and the cfg write time both
held steady over 12 s, so the poll does not oscillate.

Rollback, which also stops the ASI following the row:

```
powershell -File scripts/install-vr-menu-row.ps1 -Uninstall
```

The patched `frontend_menus.xml` is **not** committed — it is Rockstar-derived
(AGENTS.md rule 3). The script edits the installed copy and keeps a
`.gtaiv-dxvk-vr.bak` next to it.

---

## What is still not possible this way

**A new submenu page** — "VR Settings" sitting next to Keyboard/Mouse Options —
cannot be done from XML. Submenus are declared as

```xml
<optionspc action="MENUOPT_KEYBOARD_OPTIONS" ... />   <!-- link -->
<menu HeaderText="MH_CON" enum="SCR_KEYBOARD_OPTIONS">  <!-- page -->
```

and both `MENUOPT_*` and `SCR_*` are resolved against enum tables compiled into
`GTAIV.exe`. The XML can only point at screens that already exist; it cannot
declare `SCR_VR_OPTIONS`. Making one real means patching those tables — a bigger
job than section 3 estimated, and a strictly worse trade than the F3 overlay,
which already shows nine settings with no patching at all.

**Settings under Stats** — the eight rows there are `MENUOPT_NONE` display-only
slots fed by the stat system. They render text; they do not take input.

**Custom strings for the values.** The row is four-state — `scaler` IS the state
count, and `PREF_LEDILLUMINATION` happily persists 0..3 despite FusionFix
treating it as a boolean (measured: `LightSyncRGB = 3`). What cannot be changed
cheaply is the *words*. They come from the `displayValue` enum, not from
`label`, so the row reads "Low / Medium / High / Very High".

Renaming them means inline-hooking `CText::GetTextByKey` and overriding only the
keys that enum requests — doc section 3 step 4. That does not touch FusionFix's
preference table, so it carries none of the table-patch risk, but it does need an
AOB pattern for `GetTextByKey` on this Complete Edition build, which we do not
have. A key-logging hook would find it; that is a whole RE session.

Mitigation instead of RE: the default profile list is ordered **worst to best**
(`0=0 flat, 1=53 AER, 2=170 Clean, 3=243 TrueStereo`), so the borrowed quality
words are honest — Low really is the least stereo, Very High really is
TrueStereo.

---

## Custom text, without a debugger

The value strings CAN be renamed, and it needs no hook and no memory patch.

`update\common\text\americanFF.gxt` is an 8 KB GXT overlay FusionFix already
ships and the game already loads. It carries not just labels but per-state value
strings: `Lampposts`, `Lampposts and Headlights`, `Lampposts and Headlights +
Vehicle Night Shadows` are the four states of FusionFix's **Extra Night
Shadows**, which itself borrows the multiplayer-only
`MENU_DISPLAY_NETSTATS_SCORES` enum — the same trick, one layer down.

So the VR row points at that same enum and the strings are rewritten in place:

| state | text | mode |
|---|---|---|
| 0 | Off (stock string, left alone) | 0 flat |
| 1 | AER (53) | 53 |
| 2 | Clean (170) | 170 |
| 3 | TrueStereo (243) | 243 |

### Format

```
u16 version, u16 ?                    4 bytes  (04 00 10 00)
'TKEY', u32 size, size/8 x (u32 offset, u32 hash)
'TDAT', u32 size, UTF-16LE NUL-terminated strings
```

TKEY is **not** sorted, and TDAT is **not** padded — the shipped TDAT is 7466
bytes. `tools/gxt/ff_gxt.py` round-trips the file byte-identically (sha1
`e982888f933abc90`, 8158 bytes); that check caught a 2-byte padding bug before
anything was written, and any rebuild that does not round-trip should be treated
as corrupt.

### The key is the string

Keys are `atStringHash` — Jenkins one-at-a-time over the **lowercased** key.
Verified: `oaat("skip intro") == 0xAEAE49BB`, plus 17 more of the first 40. For
labels FusionFix uses the literal text as its own key, which is exactly why
`label="VR Render Mode"` works in the XML at all.

That means keys can be **added**, not just overwritten (`ff_gxt.py add`). Useful
for labels — but not for value strings, whose key names come from the display
enum, not from the text. Two of the three Lampposts strings hash to something
other than their own text, so **rename value strings with `patch` (match on
text), never with `add`**. Getting that backwards silently leaves the old text in
place at the real hash.

### Collateral

Graphics → Extra Night Shadows now shows the VR mode names too, because it
shares the enum. FusionFix's own warning string calls that setting
"Extremely broken, not recommended", so the trade is accepted deliberately.

Backups, independently restorable:

```
update\common\text\americanFF.gxt.gtaiv-dxvk-vr.bak
update\common\data\frontend_menus.xml.gtaiv-dxvk-vr.bak
```

---

## VR Aiming Mode — placeholder

```xml
<optionspc action="MENUOPT_NONE" label="VR Aiming Mode" value="PREF_NULL" scaler="0" displayValue="MENU_DISPLAY_NONE" />
```

`MENUOPT_NONE` + `PREF_NULL` renders a label and nothing else: it cannot be
selected, cannot be changed, and touches no preference. The GXT gives it
`VR Aiming Mode  ~r~(not integrated yet)` — `~r~` is the red colour code
FusionFix uses for its own warnings.

To make it real (head aim / motion controls) it needs:

1. **A donor pref.** `PREF_LEDILLUMINATION` was a genuinely free ride and there
   is no second one that clean. The `PREF_EPISODIC_RACENAME_RACE_*` family is the
   best lead — FusionFix already repurposes `PREF_EPISODIC_RACECLASS_RACE_*` for
   its sliders (`MO_FOV` → `RACECLASS_RACE_3`, persisted as `FieldOfView` in the
   cfg), and only `RACENAME_RACE_5` of the RACENAME family is currently used.
2. **A display enum with spare states.** `MENU_DISPLAY_NETSTATS_RACETYPE`
   (5 states, multiplayer-only, untouched by FusionFix) is the obvious pick.
3. Its strings renamed by the same `ff_gxt.py patch` route.
4. A `menukey`-style follow in `menu_bridge.cpp`, IF the donor pref persists to
   the cfg — that is the thing to verify first, exactly as was done for
   `LightSyncRGB`.

---

## IPD and FOV in the pause menu — tried, reverted

Both were built and reverted on 2026-07-31. Keeping the notes so the next
attempt starts from what was learned rather than from scratch.

Donor prefs and their cfg keys, **discovered, not guessed** — the ASI logs every
FusionFix.cfg key that changes, so one run with the row moved names it:

| row | donor pref | cfg key |
|---|---|---|
| VR Eye Separation | `PREF_EPISODIC_RACECLASS_RACE_4` | `DelayBeforeCenteringCameraKB` |
| VR Field of View | `PREF_EPISODIC_RACECLASS_RACE_3` | `FieldOfView` |

That discovery mechanism is worth keeping: which cfg key a repurposed `PREF_` writes
is whatever FusionFix named it, and it is not derivable from the pref name.

**Why they were dropped:** with `MENU_DISPLAY_VALUE_SLIDERBAR` the number shown
next to the bar did not track the slider — it sat on a constant. So the rows moved
the underlying value (the log shows the cfg key changing) while displaying
something meaningless. A slider you cannot read is worse than no slider.

Unresolved: whether `VALUE_SLIDERBAR` needs a `scaler` the exe recognises (the
stock users all run `scaler="100"` — `PREF_VIEW_DISTANCE`, `PREF_DETAIL_QUALITY`,
`PREF_CAR_DENSITY`), or whether the displayed number comes from somewhere other
than the pref value entirely. **Try `scaler="100"` first** — that is the one
configuration known to render a live number in the stock Graphics page.

Also note `FieldOfView` is NOT inert: FusionFix's own FOV handling applies it on
flat. In VR the mod wins anyway, because `WantsFpAbsoluteFov` stamps `CCam+0x60`
from `fpfov` every frame on modes past 162. Stretching that pref's range to show
true degrees risks breaking FusionFix's FOV maths on flat.

The follow plumbing stays in `menu_bridge.cpp` and is config-driven, so bringing
the rows back needs no code change — add the rows to
`scripts/install-vr-menu-row.ps1` and the keys to `gtaiv_dxvk_vr.menufollow`:

```
mode=LightSyncRGB
ipd=DelayBeforeCenteringCameraKB
fov=FieldOfView
```
