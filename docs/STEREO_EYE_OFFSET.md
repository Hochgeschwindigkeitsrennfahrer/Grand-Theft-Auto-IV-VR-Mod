# Stereo FOV notes

## Rolled back

- **Square UV crop on Submit** → black bar, blur, no fusion.  
- **Engine `commandline.txt` 1080×1080** → desktop OK, VR double + stretched. Disabled for now.  
  ( needs square **plus** his FOV/stereo pipeline; square alone is not enough for us.)

## Current Mode 13

- Temporal L/R + IPD  
- **Soft-inset** TextureBounds (clamped, no square crop)  
- Game keeps normal resolution (e.g. 2560×1440)

## New: Mode 14 (angle-correct canvas)

Why fusion failed with nullptr bounds: the game renders ~70° h / 16:9, but Submit with
nullptr bounds stretches that image over each eye's FULL asymmetric HMD frustum
(~94°+, ~1:1). The stretch differs per eye → same object lands at different angles in
L vs R → diplopia. Mono (Mode 0) fuses because both eyes share the identical error.

Mode 14 fixes the mapping instead of the engine: per eye, the game backbuffer is
StretchRect'ed into the correct angular rectangle of a larger black canvas; the canvas
is submitted with nullptr bounds (it spans the full frustum by construction).
Black border = expected. `gtaiv_dxvk_vr.fov` (horizontal degrees) tunes world size.

## Optional later

`config/commandline.square-vr.txt` — only with a working FOV pipeline, not alone.
Once Mode 14 proves fusion, widen the SOURCE FOV (FusionFix/ZolikaPatch-style engine
FOV, not CCam+0x60) to shrink the black border.
