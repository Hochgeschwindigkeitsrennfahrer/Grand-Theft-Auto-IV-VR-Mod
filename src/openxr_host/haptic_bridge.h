#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace gtaiv_xr_host
{
struct HapticCommand
{
    uint64_t producerEpoch = 0u;
    uint64_t commandId = 0u;
    float leftAmplitude = 0.0f;
    float rightAmplitude = 0.0f;
    bool active = false;
};

class HapticBridge
{
public:
    using LogFunction = std::function<void(const std::string&)>;

    HapticBridge();
    ~HapticBridge();
    HapticBridge(const HapticBridge&) = delete;
    HapticBridge& operator=(const HapticBridge&) = delete;

    void initialize(LogFunction logger);
    HapticCommand update();
    void reset();

private:
    struct Implementation;
    Implementation* implementation_ = nullptr;
};
}
