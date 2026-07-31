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
