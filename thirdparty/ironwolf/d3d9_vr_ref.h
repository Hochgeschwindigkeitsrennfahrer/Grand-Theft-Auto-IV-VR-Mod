#pragma once
// Snapshot reference of IronWolf tiw-rel-241-260612 d3d9_vr.h (OpenVR-relevant parts documented).
// After submodule init, prefer: dxvk/src/d3d9/d3d9_vr.h
// This file is NOT compiled into a product yet — planning / glue stubs only.
//
// Full upstream: https://github.com/TheIronWolfModding/dxvk/blob/tiw-rel-241-260612/src/d3d9/d3d9_vr.h
// See docs/IRONWOLF_API.md

// Intentionally no Vulkan includes here so the stub tree stays header-light.
// Real glue will include d3d9.h + vulkan + openvr_headers.

struct GtaivVrDescPlaceholder {
  unsigned long long image;
  unsigned width;
  unsigned height;
  unsigned format;
  unsigned sample_count;
};

// Planned call sequence (mono):
//   Direct3DCreateVRImpl(device, &vr)
//   vr->BeginVRSubmit()
//   vr->GetVRDesc(backbufferOrEye, &desc)
//   fill VRVulkanTextureData_t + IVRCompositor::Submit
//   vr->EndVRSubmit()
