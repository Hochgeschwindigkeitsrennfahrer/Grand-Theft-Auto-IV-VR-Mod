# Faster game startup (GTA IV CE + Rockstar Launcher)

Status: 2026-07-23. Applies to **Steam Complete Edition** + FusionFix + our ASI.

---

## What makes startup slow

Typical chain:

```
Steam Play → PlayGTAIV.exe → Rockstar Launcher / Social Club → GTAIV.exe
```

Launcher login, SocialClubHelper processes, and online checks often cost **several seconds to minutes** — not our ASI.

---

## Level A — Faster without bypass (recommended first)

1. **Keep Steam + Rockstar Launcher logged in and open**  
   Do not kill `Launcher` / `RockstarService` on every restart.  
   Our script: `scripts\restart-gtaiv.ps1` does **not** kill them (except with `-KillRockstarStack`).

2. **Steam offline** (Steam → Go Offline), after the game has started online once.  
   Rockstar in parallel: Launcher settings → Offline, where offered.

3. **Warm VR stack beforehand:** SteamVR already running, dashboard closed.

4. **Antivirus exception** for  
   `...\steamapps\common\Grand Theft Auto IV\`  
   (otherwise slow scanning of `d3d9.dll` / ASI).

5. Restart only via desktop shortcut / `restart-gtaiv.bat` — do not restart the entire Rockstar stack every time.

Expected result: noticeably faster; launcher still in the chain.

---

## Level B — Offline mode (launcher remains, fewer online stalls)

1. Steam → **Go Offline**.  
2. Start GTA; create offline Social Club account if needed (one-time).  
3. Saves live under profile folders — offline account ≠ online account (copy saves if needed).  
   See Steam guides on "CE Offline with saves".

Not a real bypass: Launcher/Social Club may still pop up briefly.

---

## Level C — Remove Rockstar Launcher entirely (RGLess, reversible)

TJGM guide: https://tjgm.dev/guides/gtaiv/Removing-Rockstar-Games-Launcher-from-GTA-IV/

### Installation with backup (recommended)

```powershell
# 1) Place zip in vendor\rgless\RGLess.zip (open guide / download)
# 2) GTA path
cd C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr
.\scripts\install-rgless.ps1
```

The script:

- terminates GTA if needed  
- backs up **every overwritten file** to `backups\rgless\<timestamp>\pre-replace\`  
- lists newly added files in `MANIFEST.json`  
- copies Documents saves (does **not** delete them) to `GTAIV\save\...`  
- writes `backups\rgless\LATEST.txt`

### Restore everything later

```powershell
.\scripts\restore-rgless.ps1
# or: .\scripts\restore-rgless.ps1 -BackupDir "...\backups\rgless\<stamp>"
```

Restore puts replaced files back and deletes only files **newly** added by RGLess.  
Nuclear fallback: Steam → Verify files → then redeploy ASI/DXVK/FusionFix.

### Start after RGLess

```powershell
.\scripts\restart-gtaiv.ps1 -NoPause
```

(DirectExe is the default for everyone now — RGLess just removes the Rockstar login step.)

**Downgrade 1.0.8.0:** **not** for this VR project (CE AOBs).

---

## Recommendation for you (VR dev loop)

1. **Always restart with DirectExe** (default now):
   - Desktop/taskbar: **GTA IV Quick Restart**, or
   - Double-click `scripts\restart-gtaiv.bat`, or
   - `.\scripts\restart-gtaiv.ps1 -NoPause`
2. Do **not** use Steam Play / `-applaunch` for restarts — that is what left Steam on **LAUNCHING**.
3. Keep Steam + SteamVR already running; script only kills GTAIV/PlayGTAIV.
4. Optional: RGLess (`install-rgless.ps1`) if Rockstar login still blocks DirectExe.
5. Fallback only: `.\scripts\restart-gtaiv.ps1 -SteamLaunch -NoPause`
6. No downgrade.
