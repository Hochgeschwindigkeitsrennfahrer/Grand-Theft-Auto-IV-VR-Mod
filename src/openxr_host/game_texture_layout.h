#pragma once

#include "../bridge/gtaiv_xr_frame_bridge.h"

#include <cstdint>
#include <string>

namespace gtaiv_xr_host
{
struct GameTextureLayout
{
    uint32_t aspectWidth = 0u;
    uint32_t aspectHeight = 0u;
};

inline GameTextureLayout ResolveGameTextureLayout(
    gtaiv_xr_bridge::PresentationMode presentationMode,
    uint32_t textureWidth,
    uint32_t textureHeight,
    uint32_t contentWidth,
    uint32_t contentHeight,
    uint32_t targetWidth,
    uint32_t targetHeight)
{
    if (presentationMode
        == gtaiv_xr_bridge::PresentationMode::UiQuad)
    {
        return {
            contentWidth != 0u ? contentWidth : textureWidth,
            contentHeight != 0u ? contentHeight : textureHeight
        };
    }
    if (presentationMode
        == gtaiv_xr_bridge::PresentationMode::WorldMono)
    {
        // The mono source is a real center-eye GTA capture. The host shader
        // uses this aspect to cover-crop it into each OpenXR eye target, so
        // it fills the headset without stretching or a visible screen edge.
        return { textureWidth, textureHeight };
    }
    if (presentationMode
        == gtaiv_xr_bridge::PresentationMode::WorldStereo)
    {
        // World canvases already span normalized eye tangent space. They must
        // fill the projection swapchain, irrespective of allocation aspect.
        return { targetWidth, targetHeight };
    }
    return { textureWidth, textureHeight };
}

inline bool GameTextureLayoutSelfTest(std::string& failure)
{
    const GameTextureLayout mono = ResolveGameTextureLayout(
        gtaiv_xr_bridge::PresentationMode::WorldMono,
        1280u,
        720u,
        3840u,
        2160u,
        2064u,
        2208u);
    if (mono.aspectWidth != 1280u
        || mono.aspectHeight != 720u)
    {
        failure = "immersive mono did not preserve capture aspect for cover fill";
        return false;
    }
    const GameTextureLayout stereo = ResolveGameTextureLayout(
        gtaiv_xr_bridge::PresentationMode::WorldStereo,
        1536u,
        1536u,
        1536u,
        1536u,
        2064u,
        2208u);
    if (stereo.aspectWidth != 2064u
        || stereo.aspectHeight != 2208u)
    {
        failure = "world stereo did not fill the eye projection";
        return false;
    }
    const GameTextureLayout ui = ResolveGameTextureLayout(
        gtaiv_xr_bridge::PresentationMode::UiQuad,
        1536u,
        1536u,
        1920u,
        1080u,
        2064u,
        1161u);
    if (ui.aspectWidth != 1920u
        || ui.aspectHeight != 1080u)
    {
        failure = "UI quad did not restore GTA source aspect";
        return false;
    }
    failure.clear();
    return true;
}
}
