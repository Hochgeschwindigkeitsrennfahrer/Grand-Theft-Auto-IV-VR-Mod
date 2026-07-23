# Spielstart beschleunigen (GTA IV CE + Rockstar Launcher)

Stand: 2026-07-23. Gilt für **Steam Complete Edition** + FusionFix + unser ASI.

---

## Was den Start langsam macht

Typische Kette:

```
Steam Play → PlayGTAIV.exe → Rockstar Launcher / Social Club → GTAIV.exe
```

Launcher-Login, SocialClubHelper-Prozesse und Online-Checks kosten oft **mehrere Sekunden bis Minuten** — nicht unser ASI.

---

## Stufe A — Schneller ohne Bypass (empfohlen zuerst)

1. **Steam + Rockstar Launcher einmal angemeldet offen lassen**  
   Nicht bei jedem Restart `Launcher` / `RockstarService` killen.  
   Unser Script: `scripts\restart-gtaiv.ps1` killt die **nicht** (außer `-KillRockstarStack`).

2. **Steam Offline** (Steam → Offline gehen), nachdem das Spiel einmal online gestartet hat.  
   Rockstar oft parallel: Launcher-Einstellungen → Offline, soweit angeboten.

3. **VR-Stack vorher warm:** SteamVR schon laufen, Dashboard zu.

4. **Antivirus-Ausnahme** für  
   `...\steamapps\common\Grand Theft Auto IV\`  
   (sonst langsames Scannen von `d3d9.dll` / ASI).

5. Restart nur über Desktop-Shortcut / `restart-gtaiv.bat` — nicht jedes Mal den ganzen Rockstar-Stack neu.

Erwartung: spürbar schneller, Launcher bleibt aber in der Kette.

---

## Stufe B — Offline-Modus (Launcher bleibt, weniger Online-Hänger)

1. Steam → **Offline gehen**.  
2. GTA starten; ggf. Offline-Social-Club-Account anlegen (einmalig).  
3. Saves liegen unter Profil-Ordnern — Offline-Account ≠ Online-Account (Saves ggf. kopieren).  
   Siehe Steam-Guides zu „CE Offline with saves“.

Kein echter Bypass: Launcher/Social Club können trotzdem kurz aufpoppen.

---

## Stufe C — Rockstar Launcher komplett weg (RGLess, reversibel)

TJGM-Guide: https://tjgm.dev/guides/gtaiv/Removing-Rockstar-Games-Launcher-from-GTA-IV/

### Installation mit Backup (empfohlen)

```powershell
# 1) Zip nach vendor\rgless\RGLess.zip legen (Guide oeffnen / Download)
# 2) GTA zu
cd C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr
.\scripts\install-rgless.ps1
```

Das Script:

- beendet GTA falls noetig  
- sichert **jede ueberschriebene Datei** nach `backups\rgless\<zeitstempel>\pre-replace\`  
- listet neu hinzugefuegte Dateien im `MANIFEST.json`  
- kopiert Documents-Saves (loescht sie **nicht**) nach `GTAIV\save\...`  
- schreibt `backups\rgless\LATEST.txt`

### Spaeter alles wiederherstellen

```powershell
.\scripts\restore-rgless.ps1
# oder: .\scripts\restore-rgless.ps1 -BackupDir "...\backups\rgless\<stempel>"
```

Restore stellt ersetzte Dateien zurueck und loescht nur die von RGLess **neu** hinzugefuegten.  
Nuclear-Fallback: Steam → Dateien ueberpruefen → danach ASI/DXVK/FusionFix neu deployen.

### Start nach RGLess

```powershell
.\scripts\restart-gtaiv.ps1 -DirectExe
```

**Downgrade 1.0.8.0:** fuer dieses VR-Projekt **nicht** (CE-AOBs).

---

## Empfehlung fuer dich (VR-Dev-Loop)

1. Stufe A (Launcher warm).  
2. RGLess mit `install-rgless.ps1` + spaeter `restore-rgless.ps1`.  
3. Kein Downgrade.
