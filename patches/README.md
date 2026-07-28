# Elliott OpenXR Simulator patch package

These files extend
[elliotttate/OpenXR-Simulator](https://github.com/elliotttate/OpenXR-Simulator)
at pinned commit `48a70f440ac7d9bda385994937e3da8e15a4d9bb` with:

- a no-window/headless runtime switch;
- external-focus selection for `GTAIV.exe`;
- acknowledged JSON controller and pose-sweep file IPC;
- a standard-library-only command-line input helper.

The upstream simulator is MIT licensed, copyright OpenXR Simulator
Contributors. Its `LICENSE` remains in every checkout produced by
`scripts/setup-elliott-openxr-simulator.ps1`. The patch and helper are source
only; this repository does not redistribute the built simulator DLL.
