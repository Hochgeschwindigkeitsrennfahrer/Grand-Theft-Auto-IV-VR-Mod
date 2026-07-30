HOME DROP-IN PACK — copy ONLY the files you need into the GTAIV.exe folder.
Each file is a single value (or a few lines). One change per test.

=== Session 1 (yaw fix alone) ===
  gtaiv_dxvk_vr.stereo     = 243
  gtaiv_dxvk_vr.aimmode    = 2
  gtaiv_dxvk_vr.vrinput    = 0
  (aimyaw defaults to 1 — do not create the file unless A/B testing)

=== Session 2 (IVRInput) ===
  gtaiv_dxvk_vr.vrinput    = 1
  + copy from config/vr_actions/:
      gtaiv_dxvk_vr_actions.json
      gtaiv_dxvk_vr_bindings_hpmotioncontroller.json
  (deploy-asi.ps1 does this automatically)

=== Session 3 (controller-origin ray) ===
  gtaiv_dxvk_vr.aimmode    = 3

=== Session 4 (ammo gate on synthetic fire) ===
  gtaiv_dxvk_vr.ammogate   = 1

=== Session 5 (profile switch) ===
  gtaiv_dxvk_vr.menumap    (see file)
  gtaiv_dxvk_vr.menupref   (optional — FusionFix row name)

=== Session 6 (CE / Plan B prep) ===
  gtaiv_dxvk_vr.ikprobe    = 1   (logs CPed* + candidate pointers)
  gtaiv_dxvk_vr.ikwriter   = 1   (AOB 89 46 70 + ped float3 diffs — LOG ONLY)
  gtaiv_dxvk_vr.ikcopy     = 1   (while LT: MATCH float3s near C20 + MOVER — LOG ONLY)
  gtaiv_dxvk_vr.ikesi      = 1   (hook Session6 mov [esi+70],eax — dump ESI — LOG ONLY)
  gtaiv_dxvk_vr.ikray      = 1   (while LT: MATCH vs ctrl/cam aim ray — NOT C20 — LOG ONLY)
  gtaiv_dxvk_vr.ikpin      = 1   (while LT: pin CPed+0xD58 -> P+0x90 vs origin — LOG ONLY)
  gtaiv_dxvk_vr.ikread     = 1   (while LT: VEH+PAGE_GUARD on ped+C20 — reader EIPs — LOG ONLY)
  AFTER CE finds offsets:
  gtaiv_dxvk_vr.ikoffs     (two hex numbers, e.g. "BC0 70" — CE values only)
  gtaiv_dxvk_vr.ikaim      = 1

=== KILL SWITCHES (write these FIRST if anything goes wrong) ===
  aimmode=0
  vrinput=0
  aimyaw=0
  ammogate=0
  ikaim=0
  ikprobe=0
  ikwriter=0
  ikcopy=0
  ikesi=0
  ikray=0
  ikpin=0
  ikread=0
  stereo=243
  delete menupref / menumap / ikoffs
