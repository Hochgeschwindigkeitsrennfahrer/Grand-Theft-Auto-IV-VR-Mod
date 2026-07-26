#pragma once

#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <string>

namespace gtaiv_xr_host
{
struct GameFrameView
{
    ID3D11ShaderResourceView* shaderView = nullptr;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint64_t frameId = 0u;

    explicit operator bool() const noexcept
    {
        return shaderView != nullptr && width != 0u && height != 0u;
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
        LogFunction logger);
    GameFrameView update();
    void reset();

private:
    struct Implementation;
    Implementation* implementation_ = nullptr;
};
}
