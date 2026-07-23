# Agent-Vertrag — gtaiv-dxvk-vr

Eigenständiges Projekt. Kein anderes Repo voraussetzen.

## Prompt zum Start eines Chats

```text
Lies AGENTS.md und docs/CURRENT-STATE.md und docs/HOME_CURSOR.md und docs/VR_STRATEGY.md.
Projekt: gtaiv-dxvk-vr — GTA IV CE VR: Stock-DXVK 3.0.2 d3d9.dll + ASI Glue + OpenVR (SteamVR), Win32, Reverb G2.
Flach mit DXVK 3.0.2 als d3d9.dll ok. Kein OpenXR für v0. Kein IronWolf-tiw-rel als Basis.
Ich bin kein Programmierer — konkrete Klick-/Befehlsschritte.
Meilenstein Mono-Submit erreicht. Als Nächstes: Stabilität / Stereo / Kamera (eine Sache pro Session).
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

Stock-DXVK **3.0.2** `d3d9.dll` flach ok + ASI Glue: SteamVR/OpenVR, **Mono**-Augenbild via `ID3D9VkInterop*` → `IVRCompositor::Submit`.

## Doc-Karte

| Datei | Wann lesen |
|-------|------------|
| `docs/HOME_CURSOR.md` | User-Setup zu Hause |
| `docs/CURRENT-STATE.md` | Status / Next |
| `docs/VR_STRATEGY.md` | Interop statt IronWolf |
| `docs/DXVK_FLAT_TROUBLESHOOT.md` | Flach-DXVK Lektionen |
| `docs/ARCHITECTURE.md` | Module |
| `docs/CONSTRAINTS.md` | Harte Regeln |
| `docs/REFERENCES.md` | Links |
| `docs/IRONWOLF_DXVK.md` | Historische Mailbox (Referenz) |
| `docs/FAQ.md` | User-Fragen |
| `docs/BUILD.md` | Build |
