#pragma once

#include "../bridge/gtaiv_xr_frame_bridge.h"

#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <string>

namespace gtaiv_xr_host
{
constexpr uint32_t GameEyeCount = 2u;

struct GameFrameView
{
    ID3D11ShaderResourceView* eyeViews[GameEyeCount] = {};
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint64_t transactionId = 0u;
    uint64_t sourceFrameId[GameEyeCount] = {};
    uint64_t poseSequence[GameEyeCount] = {};
    int64_t renderedDisplayTime[GameEyeCount] = {};
    gtaiv_xr_bridge::PresentationMode presentationMode =
        gtaiv_xr_bridge::PresentationMode::Unknown;
    uint32_t uiReasonFlags = gtaiv_xr_bridge::UiReasonNone;
    uint32_t uiEye = 0u;
    bool sameSimulationTick = false;

    explicit operator bool() const noexcept
    {
        return eyeViews[0] != nullptr
            && eyeViews[1] != nullptr
            && eyeViews[0] != eyeViews[1]
            && width != 0u
            && height != 0u
            && transactionId != 0u;
    }
};

class GameBridge
{
public:
    using LogFunction = std::function<void(const std::string&)>;

    GameBridge();
    ~GameBridge();
    GameBridge(const GameBridge&) = delete;
    GameBridge& operator=(const GameBridge&) = delete;

    bool initialize(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        LogFunction logger,
        bool allowTemporalStereo = false);
    GameFrameView update();
    void reset();

private:
    struct Implementation;
    Implementation* implementation_ = nullptr;
};

// Pure protocol checks used by --self-test. Does not open OpenXR, D3D, GTA,
// Steam, or any shared mapping.
bool GameBridgeProtocolSelfTest(std::string& failure);
}
