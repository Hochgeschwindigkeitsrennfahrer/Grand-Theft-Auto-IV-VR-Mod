#pragma once

#include "../bridge/gtaiv_xr_pose_bridge.h"

#include <functional>
#include <string>

namespace gtaiv_xr_host
{
class PoseBridgePublisher
{
public:
    using LogFunction = std::function<void(const std::string&)>;

    PoseBridgePublisher();
    ~PoseBridgePublisher();
    PoseBridgePublisher(const PoseBridgePublisher&) = delete;
    PoseBridgePublisher& operator=(const PoseBridgePublisher&) = delete;

    bool initialize(LogFunction logger);
    void publish(gtaiv_xr_bridge::PoseBridge value);
    void reset();

private:
    struct Implementation;
    Implementation* implementation_ = nullptr;
};
}
