# Agent-Vertrag — gtaiv-dxvk-vr

Eigenständiges Projekt. Kein anderes Repo voraussetzen.

## Prompt zum Start eines Chats

```text
Lies AGENTS.md und docs/CURRENT-STATE.md und docs/HOME_CURSOR.md.
Projekt: gtaiv-dxvk-vr — GTA IV CE VR via VR-DXVK d3d9.dll + OpenVR (SteamVR), Win32, Reverb G2.
Wie L4D2VR/HL2VR. Kein OpenXR für den ersten Meilenstein.
Ich bin kein Programmierer — konkrete Klick-/Befehlsschritte.
Als Nächstes: Submodule initialisieren und x86-Build-Plan.
```

## Nicht verhandelbar

1. **Win32 / x86** für alles, was in `GTAIV.exe` lädt.  
2. Singleplayer / offline während der Entwicklung.  
3. Keine Rockstar-Assets, keine geschlossenen HL2VR-/Luke-Ross-Binaries vertreiben.  
4. **OpenVR (SteamVR) ist der Compositor** — erster Meilenstein ohne OpenXR.  
5. L4D2VR / IronWolf / sd805 sind **Referenzen**; GTA braucht eigenes Glue.  
6. Fremde Spiel-`d3d9.dll` nicht 1:1 als „VR für GTA“ verkaufen.  
7. Eine Verhaltensänderung pro Test-Build; Logs in Datei neben der EXE.  
8. Nach Headset-Tests `docs/CURRENT-STATE.md` aktualisieren.  
9. Anleitungen für den User auf **Deutsch**, konkret, kurz.

## Definition of Done (erster Meilenstein)

x86-`d3d9.dll` baut, lädt unter FusionFix/CE, SteamVR+OpenVR starten, **Mono**-Augenbild in der Brille via `IVRCompositor::Submit`.

## Doc-Karte

| Datei | Wann lesen |
|-------|------------|
| `docs/HOME_CURSOR.md` | User-Setup zu Hause |
| `docs/CURRENT-STATE.md` | Status / Next |
| `docs/ARCHITECTURE.md` | Module |
| `docs/CONSTRAINTS.md` | Harte Regeln |
| `docs/REFERENCES.md` | Links |
| `docs/IRONWOLF_DXVK.md` | Mailbox-Erklärung |
| `docs/FAQ.md` | User-Fragen |
| `docs/BUILD.md` | Build |
