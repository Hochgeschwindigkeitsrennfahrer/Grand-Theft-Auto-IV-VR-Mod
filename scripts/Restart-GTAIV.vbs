' Silent GTA IV quick restart — no console, no "Enter to close".
' Pin the Desktop/Start-Menu shortcut (created by install-restart-shortcut.ps1),
' not this .vbs alone. Target is wscript.exe (pinable) + GTAIV icon.
Option Explicit
Dim sh, ps1, cmd
Set sh = CreateObject("WScript.Shell")
ps1 = CreateObject("Scripting.FileSystemObject").GetParentFolderName(WScript.ScriptFullName) & "\restart-gtaiv.ps1"
cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & ps1 & """ -NoPause"
sh.Run cmd, 0, False
