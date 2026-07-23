# Build — x86 `d3d9.dll` (Win32)

Ziel Schritt 1: **32-bit** `d3d9.dll` aus dem IronWolf-Submodule, die GTA IV CE **flach** (Monitor) unter FusionFix startet.  
Noch **kein** OpenVR-Submit in diesem Schritt.

Submodule-Branch: `tiw-rel-241-260612` (siehe `.gitmodules`).  
Compositor später: **OpenVR / SteamVR** — nicht OpenXR.

---

## 0. Einmalig: Tools prüfen

| Tool | Pflicht? | Check in PowerShell |
|------|----------|---------------------|
| Git for Windows | ja (Submodule) | `git --version` |
| Visual Studio 2022 + **Desktopentwicklung mit C++** | empfohlen | VS Installer |
| [MSYS2](https://www.msys2.org/) | ja (DXVK baut mit MinGW/Meson) | Startmenü → **MSYS2 UCRT64** |
| Vulkan SDK | oft nötig (glslang) | `C:\VulkanSDK\...` |
| Python 3 | oft schon mit Meson | `python --version` |

Wenn `git` nicht gefunden wird: [Git for Windows](https://git-scm.com/download/win) installieren, **neuen** Cursor-/PowerShell-Tab öffnen, erneut prüfen.

---

## 1. Submodule holen (Internet)

In Cursor: Terminal `` Ctrl+` ``, dann:

```powershell
cd C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr
.\scripts\init-submodules.ps1
```

**Erfolg:** Ordner `dxvk\src` existiert und ist nicht leer.  
Zusätzlich prüfen:

```powershell
Test-Path .\dxvk\src\d3d9\d3d9_vr.h
```

Muss `True` sein. Wenn `False`: falscher Branch / leeres Submodule → komplette Terminal-Ausgabe an den Agenten pasten.

---

## 2. MSYS2-Pakete (einmalig)

`build-win32.txt` verlangt Compiler namens `i686-w64-mingw32-gcc` → dafür die **MINGW32**-Umgebung.

1. **MSYS2** installieren: https://www.msys2.org/ (Standardpfad `C:\msys64` ok).  
2. Startmenü → **„MSYS2 MINGW32“** öffnen (nicht UCRT64 für diesen Build).  
3. Aktualisieren:

```bash
pacman -Syu
```

Fenster kann sich schließen → **MINGW32** erneut öffnen, ggf. nochmal `pacman -Syu`.

4. Build-Tools (**ohne** `i686-glslang` — Paket existiert nicht mehr):

In **MINGW32**:

```bash
pacman -S --needed \
  mingw-w64-i686-gcc \
  mingw-w64-i686-meson \
  mingw-w64-i686-ninja \
  mingw-w64-i686-pkgconf \
  git
```

In **MINGW64** (eigenes Fenster — nur fürs Shader-Tool):

```bash
pacman -S --needed mingw-w64-x86_64-glslang
```

5. Kurz prüfen (wieder in **MINGW32**):

```bash
export PATH="/mingw32/bin:/mingw64/bin:$PATH"
gcc -dumpmachine
which glslangValidator || which glslang
meson --version
ninja --version
```

Erwartung: `gcc -dumpmachine` enthält `i686`; `glslangValidator` gefunden.  
Wenn `i686-w64-mingw32-gcc` fehlt, aber `gcc` da ist: `which gcc` + `gcc -dumpmachine` pasten.

---

## 3. x86-Build (Meson + Ninja)

In **MSYS2 UCRT64** (Pfade anpassen, falls dein User anders heißt):

```bash
cd /c/Users/Henning/Documents/cursor/gtaiv-dxvk-vr/dxvk

# Build-Verzeichnis nur für Win32
# MSYS2: scripts/msys2-win32.txt (nicht IronWolf build-win32.txt — ar-Namen)
meson setup \
  --cross-file ../scripts/msys2-win32.txt \
  --buildtype release \
  --prefix "$PWD/../out-win32" \
  build.w32

cd build.w32
ninja
ninja install
```

**Erwartete DLL:**

```text
C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr\out-win32\bin\d3d9.dll
```

(Exakter Unterordner kann `bin` oder ähnlich sein — nach `ninja install` im Explorer `out-win32` öffnen und `d3d9.dll` suchen.)

### Wenn `build-win32.txt` fehlt oder fehlschlägt

1. Im Ordner `dxvk\` nach `build-win*.txt` / `BUILD` / `README` schauen.  
2. Komplette Fehlerausgabe an den Agenten pasten.  
3. Alternative-Vorbild (nur lesen, nicht deren DLL nach GTA kopieren):  
   [L4D2VR Build](https://github.com/sd805/l4d2vr) → `l4d2vr.sln` → Plattform **x86**.

---

## 4. Prüfen: wirklich 32-bit?

In PowerShell (Developer PowerShell for VS oder normales PowerShell mit VS-Tools):

```powershell
dumpbin /headers "C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr\out-win32\bin\d3d9.dll" | Select-String "machine"
```

Erwartung: etwas mit **`8664` ist falsch (x64)** — gesucht wird **`14C` / x86**.  
Ohne `dumpbin`: Agent kann später ein kleines Check-Skript ergänzen; grob: Datei darf **nicht** aus einem `x64`/`win64`-Build kommen.

---

## 5. Deploy nach GTA IV

Spielordner = Ordner mit `GTAIV.exe` (oft `...\Grand Theft Auto IV\GTAIV`).

```powershell
cd C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr
.\scripts\deploy.ps1 `
  -GameDir "D:\Steam\steamapps\common\Grand Theft Auto IV\GTAIV" `
  -DllPath ".\out-win32\bin\d3d9.dll"
```

`-GameDir` auf **deinen** echten Pfad setzen.

Dann:

1. FusionFix-**Vulkan/DXVK-Toggle aus**  
2. Spiel **offline / Singleplayer** starten  
3. Monitor-Bild ok? → Erfolg für Schritt „flach“  
4. Log neben der EXE (DXVK legt oft `GTAIV_d3d9.log` o. Ä. an) bei Problemen pasten  

Zurückrollen:

```powershell
copy "…\GTAIV\d3d9.dll.gtaiv-dxvk-vr.bak" "…\GTAIV\d3d9.dll"
```

---

## 6. Reihenfolge danach (noch nicht jetzt)

1. Flache DLL ok  
2. L4D2VR `vr.cpp` Submit-Muster lesen (`docs/L4D2VR_MAP.md`)  
3. Glue an `GetVRDesc` + `IVRCompositor::Submit`  
4. SteamVR + Reverb G2 → **Mono** in der Brille  

Eine Verhaltensänderung pro Test-Build.

---

## Konflikte / Fallstricke

| Problem | Aktion |
|---------|--------|
| `git` unbekannt | Git installieren, Terminal neu |
| `dxvk\src` leer | `init-submodules.ps1` erneut; Log pasten |
| `d3d9_vr.h` fehlt | Branch muss `tiw-rel-*` sein, nicht `master` |
| Spiel startet nicht | Backup zurück; FusionFix-Vulkan aus; Log pasten |
| x64-DLL in GTA | Neu mit `build-win32` / i686 bauen |
| Antivirus | Neue `d3d9.dll` ggf. freigeben |

Siehe auch `config/dxvk.conf.example`, `docs/HOME_CURSOR.md`, `docs/CONSTRAINTS.md`.
