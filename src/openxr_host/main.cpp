#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>

#include "game_bridge.h"
#include "game_texture_layout.h"
#include "haptic_bridge.h"
#include "menu_quad_pose.h"
#include "pose_bridge.h"
#include "presentation_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef GTAIV_XR_BUILD_ID
#define GTAIV_XR_BUILD_ID "dev"
#endif

namespace
{
using Microsoft::WRL::ComPtr;

constexpr XrDuration SwapchainWaitTimeout = 2'000'000'000;
constexpr uint32_t SwapchainWaitAttempts = 3u;
constexpr uint32_t EyeCount = 2;
constexpr uint32_t GameProjectionDimension = 1536u;
std::atomic<bool> stopRequested { false };

const char* CalibrationShader = R"HLSL(
cbuffer EyeConstants : register(b0)
{
    float4 eyeOrigin;
    float4 eyeRight;
    float4 eyeUp;
    float4 eyeBack;
    float4 tanAngles;
    float4 viewportAndTime;
    float4 accent;
};

struct VsOutput
{
    float4 position : SV_Position;
};

VsOutput vsMain(uint vertexId : SV_VertexID)
{
    const float2 vertices[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    VsOutput output;
    output.position = float4(vertices[vertexId], 0.0, 1.0);
    return output;
}

float raySphere(float3 origin, float3 direction, float3 center, float radius)
{
    const float3 offset = origin - center;
    const float b = dot(offset, direction);
    const float c = dot(offset, offset) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0)
        return -1.0;

    const float root = sqrt(discriminant);
    const float nearHit = -b - root;
    const float farHit = -b + root;
    return nearHit > 0.001 ? nearHit : (farHit > 0.001 ? farHit : -1.0);
}

float rectangleMask(float2 location, float4 bounds)
{
    return step(bounds.x, location.x) * step(bounds.y, location.y)
        * step(location.x, bounds.z) * step(location.y, bounds.w);
}

float segmentMask(float2 location, float2 startPoint, float2 endPoint, float width)
{
    const float2 fromStart = location - startPoint;
    const float2 segment = endPoint - startPoint;
    const float along = saturate(dot(fromStart, segment) / dot(segment, segment));
    const float distanceToSegment = length(fromStart - segment * along);
    return 1.0 - smoothstep(width, width + 0.003, distanceToSegment);
}

float4 psMain(VsOutput input) : SV_Target
{
    const float2 viewport = max(viewportAndTime.xy, float2(1.0, 1.0));
    const float2 uv = input.position.xy / viewport;
    const float horizontalTan = lerp(tanAngles.x, tanAngles.y, uv.x);
    const float verticalTan = lerp(tanAngles.w, tanAngles.z, uv.y);
    const float3 rayDirection = normalize(
        eyeRight.xyz * horizontalTan
        + eyeUp.xyz * verticalTan
        - eyeBack.xyz);
    const float3 rayOrigin = eyeOrigin.xyz;

    const float skyAmount = saturate(rayDirection.y * 0.5 + 0.5);
    float3 color = lerp(float3(0.015, 0.020, 0.045),
                        float3(0.080, 0.160, 0.260),
                        skyAmount);
    float nearest = 100000.0;

    if (rayDirection.y < -0.0001)
    {
        const float floorHit = (-1.25 - rayOrigin.y) / rayDirection.y;
        if (floorHit > 0.001)
        {
            nearest = floorHit;
            const float3 floorPoint = rayOrigin + rayDirection * floorHit;
            const float2 tile = floor(floorPoint.xz * 2.0);
            const float checker = fmod(abs(tile.x + tile.y), 2.0);
            const float distanceFade = saturate(1.0 - floorHit / 18.0);
            const float3 darkTile = float3(0.035, 0.045, 0.060);
            const float3 lightTile = float3(0.130, 0.150, 0.175);
            color = lerp(darkTile, lightTile, checker) * (0.35 + 0.65 * distanceFade);

            const float2 grid = abs(frac(floorPoint.xz * 2.0) - 0.5);
            const float floorGridLine =
                1.0 - smoothstep(0.455, 0.490, max(grid.x, grid.y));
            color = lerp(color, float3(0.18, 0.28, 0.35), floorGridLine * 0.65);
        }
    }

    const float3 sphereCenters[3] =
    {
        float3(-0.55, -0.53, -2.55),
        float3( 0.78, -0.72, -3.85),
        float3( 0.05,  0.15, -6.20)
    };
    const float sphereRadii[3] = { 0.72, 0.50, 0.85 };
    const float3 sphereColors[3] =
    {
        float3(0.10, 0.78, 0.95),
        float3(1.00, 0.38, 0.12),
        float3(0.72, 0.34, 0.95)
    };

    [unroll]
    for (int sphereIndex = 0; sphereIndex < 3; ++sphereIndex)
    {
        const float hit = raySphere(
            rayOrigin,
            rayDirection,
            sphereCenters[sphereIndex],
            sphereRadii[sphereIndex]);
        if (hit > 0.0 && hit < nearest)
        {
            nearest = hit;
            const float3 hitPoint = rayOrigin + rayDirection * hit;
            const float3 normal = normalize(hitPoint - sphereCenters[sphereIndex]);
            const float diffuse = saturate(dot(normal, normalize(float3(-0.4, 0.8, 0.25))));
            const float rim = pow(1.0 - saturate(dot(-rayDirection, normal)), 3.0);
            color = sphereColors[sphereIndex] * (0.18 + 0.82 * diffuse)
                + rim * accent.rgb * 0.35;
        }
    }

    const float2 pixelGrid = abs(frac((input.position.xy + 0.5) / 96.0) - 0.5) * 96.0;
    const float screenGrid = 1.0 - smoothstep(0.8, 1.8, min(pixelGrid.x, pixelGrid.y));
    color = lerp(color, accent.rgb, screenGrid * 0.10);

    const float edgeDistance = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    const float border = 1.0 - smoothstep(0.003, 0.010, edgeDistance);

    const float centerVertical = rectangleMask(uv, float4(0.498, 0.455, 0.502, 0.545));
    const float centerHorizontal = rectangleMask(uv, float4(0.465, 0.498, 0.535, 0.502));
    const float reticle = saturate(centerVertical + centerHorizontal);

    float glyph = 0.0;
    if (viewportAndTime.w < 0.5)
    {
        glyph += rectangleMask(uv, float4(0.052, 0.060, 0.068, 0.215));
        glyph += rectangleMask(uv, float4(0.052, 0.199, 0.145, 0.215));
    }
    else
    {
        glyph += rectangleMask(uv, float4(0.052, 0.060, 0.068, 0.215));
        glyph += rectangleMask(uv, float4(0.052, 0.060, 0.132, 0.076));
        glyph += rectangleMask(uv, float4(0.052, 0.128, 0.132, 0.144));
        glyph += rectangleMask(uv, float4(0.116, 0.060, 0.132, 0.144));
        glyph += segmentMask(uv, float2(0.073, 0.139), float2(0.145, 0.215), 0.009);
    }

    const float overlay = saturate(border * 0.90 + reticle * 0.85 + glyph);
    color = lerp(color, accent.rgb, overlay);
    return float4(color, 1.0);
}

Texture2D<float4> gameFrame : register(t0);
SamplerState gameSampler : register(s0);

cbuffer GameConstants : register(b1)
{
    float4 gameSourceAndViewport;
    float4 gamePresentation;
};

float4 psGame(VsOutput input) : SV_Target
{
    const float2 sourceSize = max(gameSourceAndViewport.xy, float2(1.0, 1.0));
    const float2 viewportSize = max(gameSourceAndViewport.zw, float2(1.0, 1.0));
    const float2 viewportUv = input.position.xy / viewportSize;
    const float sourceAspect = sourceSize.x / sourceSize.y;
    const float viewportAspect = viewportSize.x / viewportSize.y;

    const float2 centered = viewportUv - 0.5;
    float2 sourceUv;
    if (gamePresentation.x >= 0.5)
    {
        // Immersive mono is a center-eye render. Crop its 16:9 capture to
        // each runtime eye target instead of exposing a head-locked quad or
        // black letterbox border.
        float2 cropScale = float2(1.0, 1.0);
        if (viewportAspect > sourceAspect)
            cropScale.y = sourceAspect / viewportAspect;
        else
            cropScale.x = viewportAspect / sourceAspect;
        sourceUv = centered * cropScale + 0.5;
    }
    else
    {
        float2 contentScale = float2(1.0, 1.0);
        if (viewportAspect > sourceAspect)
            contentScale.x = sourceAspect / viewportAspect;
        else
            contentScale.y = viewportAspect / sourceAspect;
        if (abs(centered.x) > contentScale.x * 0.5
            || abs(centered.y) > contentScale.y * 0.5)
        {
            return float4(0.0, 0.0, 0.0, 1.0);
        }
        sourceUv = centered / contentScale + 0.5;
    }
    const float3 color = gameFrame.SampleLevel(gameSampler, sourceUv, 0.0).rgb;
    return float4(color, 1.0);
}
)HLSL";

struct alignas(16) EyeConstants
{
    float eyeOrigin[4];
    float eyeRight[4];
    float eyeUp[4];
    float eyeBack[4];
    float tanAngles[4];
    float viewportAndTime[4];
    float accent[4];
};
static_assert(sizeof(EyeConstants) % 16 == 0, "D3D11 constant buffer must be 16-byte aligned");

struct alignas(16) GameConstants
{
    float sourceAndViewport[4];
    float presentation[4];
};
static_assert(sizeof(GameConstants) == 32, "GameConstants ABI changed");
static_assert(sizeof(void*) == 8, "The OpenXR host must remain x64");

struct Vec3
{
    float x;
    float y;
    float z;
};

Vec3 cross(const Vec3& left, const Vec3& right)
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x
    };
}

Vec3 rotate(const XrQuaternionf& rotation, const Vec3& vector)
{
    const Vec3 quaternionVector { rotation.x, rotation.y, rotation.z };
    const Vec3 firstCross = cross(quaternionVector, vector);
    const Vec3 doubled { firstCross.x * 2.0f, firstCross.y * 2.0f, firstCross.z * 2.0f };
    const Vec3 secondCross = cross(quaternionVector, doubled);
    return {
        vector.x + rotation.w * doubled.x + secondCross.x,
        vector.y + rotation.w * doubled.y + secondCross.y,
        vector.z + rotation.w * doubled.z + secondCross.z
    };
}

std::filesystem::path executableDirectory()
{
    std::array<wchar_t, 32768> path {};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size())
        return std::filesystem::current_path();
    return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
}

class Logger
{
public:
    Logger()
    {
        path_ = executableDirectory() / L"gtaiv_xr_host.log";
        file_.open(path_, std::ios::out | std::ios::app);
        if (!file_)
            throw std::runtime_error("Could not open gtaiv_xr_host.log beside the executable.");
    }

    void write(const std::string& message)
    {
        SYSTEMTIME time {};
        GetLocalTime(&time);

        std::ostringstream line;
        line << '['
             << std::setfill('0') << std::setw(2) << time.wHour << ':'
             << std::setw(2) << time.wMinute << ':'
             << std::setw(2) << time.wSecond << '.'
             << std::setw(3) << time.wMilliseconds << "] "
             << message;

        std::cout << line.str() << '\n';
        file_ << line.str() << '\n';
        file_.flush();
    }

    const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
    std::ofstream file_;
};

std::string xrVersionString(XrVersion version)
{
    std::ostringstream out;
    out << XR_VERSION_MAJOR(version) << '.'
        << XR_VERSION_MINOR(version) << '.'
        << XR_VERSION_PATCH(version);
    return out.str();
}

std::string sessionStateName(XrSessionState state)
{
    switch (state)
    {
        case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNRECOGNIZED";
    }
}

std::string luidString(const LUID& luid)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0')
        << std::setw(8) << static_cast<uint32_t>(luid.HighPart)
        << std::setw(8) << luid.LowPart;
    return out.str();
}

std::string hresultString(HRESULT result)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << static_cast<uint32_t>(result);
    return out.str();
}

std::string utf8FromWide(const wchar_t* value)
{
    if (!value || value[0] == L'\0')
        return {};

    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        -1,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 1)
        return {};

    std::string converted(static_cast<size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        -1,
        converted.data(),
        required,
        nullptr,
        nullptr);
    if (written != required)
        throw std::runtime_error("WideCharToMultiByte failed for the adapter name.");
    converted.pop_back();
    return converted;
}

BOOL WINAPI consoleControlHandler(DWORD controlType)
{
    if (controlType == CTRL_C_EVENT
        || controlType == CTRL_BREAK_EVENT
        || controlType == CTRL_CLOSE_EVENT)
    {
        stopRequested.store(true);
        return TRUE;
    }
    return FALSE;
}

ComPtr<ID3DBlob> compileShader(const char* entryPoint, const char* target)
{
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT result = D3DCompile(
        CalibrationShader,
        std::strlen(CalibrationShader),
        "gtaiv_xr_calibration.hlsl",
        nullptr,
        nullptr,
        entryPoint,
        target,
        flags,
        0,
        &bytecode,
        &errors);
    if (FAILED(result))
    {
        std::string detail;
        if (errors && errors->GetBufferPointer())
        {
            detail.assign(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        throw std::runtime_error(
            std::string("D3DCompile failed for ") + entryPoint + " (" + hresultString(result)
            + "): " + detail);
    }
    return bytecode;
}

struct ColorSwapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ComPtr<ID3D11RenderTargetView>> renderTargetViews;
};

struct InputActions
{
    XrAction gripPose = XR_NULL_HANDLE;
    XrAction aimPose = XR_NULL_HANDLE;
    XrAction trigger = XR_NULL_HANDLE;
    XrAction squeeze = XR_NULL_HANDLE;
    XrAction thumbstick = XR_NULL_HANDLE;
    XrAction primaryClick = XR_NULL_HANDLE;
    XrAction secondaryClick = XR_NULL_HANDLE;
    XrAction menuClick = XR_NULL_HANDLE;
    XrAction thumbstickClick = XR_NULL_HANDLE;
    XrAction primaryTouch = XR_NULL_HANDLE;
    XrAction secondaryTouch = XR_NULL_HANDLE;
    XrAction thumbstickTouch = XR_NULL_HANDLE;
    XrAction thumbrestTouch = XR_NULL_HANDLE;
    XrAction haptic = XR_NULL_HANDLE;
};

struct FloatActionSample
{
    float value = 0.0f;
    bool active = false;
};

struct Vector2ActionSample
{
    XrVector2f value {};
    bool active = false;
};

struct BooleanActionSample
{
    bool value = false;
    bool active = false;
};

struct LocatedViewSample
{
    uint64_t sequence = 0u;
    int64_t predictedDisplayTime = 0;
    std::array<XrView, EyeCount> views {
        XrView { XR_TYPE_VIEW },
        XrView { XR_TYPE_VIEW }
    };
};

struct PreparedGamePresentation
{
    std::array<XrView, EyeCount> submittedViews {
        XrView { XR_TYPE_VIEW },
        XrView { XR_TYPE_VIEW }
    };
    XrPosef uiPose {};
    bool quadPresentation = false;
};

gtaiv_xr_host::GamePresentationKey makeGamePresentationKey(
    const gtaiv_xr_host::GameFrameView& frame,
    uint32_t referenceSpaceGeneration)
{
    gtaiv_xr_host::GamePresentationKey key {};
    key.transactionId = frame.transactionId;
    key.sourceFrameId[0] = frame.sourceFrameId[0];
    key.sourceFrameId[1] = frame.sourceFrameId[1];
    key.poseSequence[0] = frame.poseSequence[0];
    key.poseSequence[1] = frame.poseSequence[1];
    key.renderedDisplayTime[0] = frame.renderedDisplayTime[0];
    key.renderedDisplayTime[1] = frame.renderedDisplayTime[1];
    key.referenceSpaceGeneration = referenceSpaceGeneration;
    key.width = frame.width;
    key.height = frame.height;
    key.contentWidth = frame.contentWidth;
    key.contentHeight = frame.contentHeight;
    key.presentationMode =
        static_cast<uint32_t>(frame.presentationMode);
    key.uiReasonFlags = frame.uiReasonFlags;
    key.uiEye = frame.uiEye;
    key.sameSimulationTick = frame.sameSimulationTick;
    key.temporalStereo = frame.temporalStereo;
    key.parentDualStereo = frame.parentDualStereo;
    key.firstPersonCamera = frame.firstPersonCamera;
    key.nativeHeadHidden = frame.nativeHeadHidden;
    key.pixelDistinct = frame.pixelDistinct;
    return key;
}

const char* gamePresentationRoute(
    const gtaiv_xr_host::GamePresentationKey& key) noexcept
{
    const auto mode = static_cast<gtaiv_xr_bridge::PresentationMode>(
        key.presentationMode);
    if (mode == gtaiv_xr_bridge::PresentationMode::UiQuad)
        return "stationary-ui-quad";
    if (mode == gtaiv_xr_bridge::PresentationMode::WorldMono)
        return "world-headtracked-mono-projection";
    if (mode == gtaiv_xr_bridge::PresentationMode::WorldStereo)
    {
        if (key.parentDualStereo)
            return "world-parent-dual-stereo";
        return key.temporalStereo
            ? "world-temporal-stereo"
            : "world-stereo";
    }
    return "unknown";
}

bool swapchainImageWaitSucceeded(XrResult result) noexcept
{
    return result == XR_SUCCESS;
}

class CalibrationHost
{
public:
    explicit CalibrationHost(
        Logger& logger,
        bool gameMode,
        bool allowTemporalStereo)
        : logger_(logger)
        , gameMode_(gameMode)
        , allowTemporalStereo_(allowTemporalStereo)
    {
    }

    ~CalibrationHost()
    {
        shutdown();
    }

    void initialize()
    {
        createInstance();
        querySystem();
        createD3D11Device();
        createSession();
        createReferenceSpace();
        createInputActions();
        createSwapchains();
        createRenderer();
        if (!poseBridge_.initialize([this](const std::string& message) {
                logger_.write(message);
            }))
        {
            throw std::runtime_error("The x64 pose bridge could not initialize.");
        }
        hapticBridge_.initialize([this](const std::string& message) {
            logger_.write(message);
        });
    }

    int run(uint64_t frameLimit, uint64_t timeoutMilliseconds)
    {
        uint64_t submittedFrames = 0;
        const uint64_t startTick = GetTickCount64();
        logger_.write("XRHost: waiting for session READY");

        while (!exitRequested_ && !stopRequested.load())
        {
            if (timeoutMilliseconds != 0
                && GetTickCount64() - startTick >= timeoutMilliseconds)
            {
                std::ostringstream message;
                message << "OpenXR host timed out in session state "
                        << sessionStateName(sessionState_) << '.';
                if (sessionState_ == XR_SESSION_STATE_IDLE)
                {
                    message
                        << " Focus the active OpenXR runtime: open Meta XR Simulator "
                           "or enter Quest Link in the headset.";
                }
                throw std::runtime_error(message.str());
            }

            pollEvents();
            if (exitRequested_ || stopRequested.load())
                break;

            if (!sessionRunning_)
            {
                Sleep(10);
                continue;
            }

            if (renderFrame())
            {
                ++submittedFrames;
                if (submittedFrames == 1 || submittedFrames % 300 == 0)
                {
                    std::ostringstream message;
                    message << "XRHost: projection submit ok frame=" << submittedFrames;
                    logger_.write(message.str());
                }
            }

            if (frameLimit != 0 && submittedFrames >= frameLimit)
            {
                logger_.write("XRHost: requested frame limit reached");
                break;
            }
        }

        if (gameMode_)
        {
            std::ostringstream message;
            message
                << "XRHost: game presentation pacing swapchainUpdates="
                << gamePresentationCache_.updateCount()
                << " reusedHostFrames="
                << gamePresentationCache_.reuseCount();
            logger_.write(message.str());
        }
        logger_.write("XRHost: clean shutdown requested");
        return 0;
    }

private:
    void checkXr(XrResult result, const char* operation) const
    {
        if (XR_SUCCEEDED(result))
            return;

        char resultText[XR_MAX_RESULT_STRING_SIZE] {};
        if (instance_ != XR_NULL_HANDLE)
            xrResultToString(instance_, result, resultText);

        std::ostringstream message;
        message << operation << " failed result=" << static_cast<int32_t>(result);
        if (resultText[0] != '\0')
            message << " (" << resultText << ')';
        throw std::runtime_error(message.str());
    }

    void waitForSwapchainImageReady(
        XrSwapchain swapchain,
        const char* operation)
    {
        XrSwapchainImageWaitInfo waitInfo {
            XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = SwapchainWaitTimeout;
        for (uint32_t attempt = 1u;
             attempt <= SwapchainWaitAttempts;
             ++attempt)
        {
            const XrResult result =
                xrWaitSwapchainImage(swapchain, &waitInfo);
            if (swapchainImageWaitSucceeded(result))
                return;
            if (result == XR_TIMEOUT_EXPIRED)
            {
                std::ostringstream message;
                message
                    << "XRHost: " << operation
                    << " timed out attempt=" << attempt
                    << '/' << SwapchainWaitAttempts
                    << "; waiting on the same acquired image";
                logger_.write(message.str());
                continue;
            }
            checkXr(result, operation);
            std::ostringstream message;
            message << operation
                    << " returned unexpected non-success result="
                    << static_cast<int32_t>(result);
            throw std::runtime_error(message.str());
        }
        throw std::runtime_error(
            std::string(operation)
            + " timed out before the acquired image became ready.");
    }

    void createInstance()
    {
        uint32_t extensionCount = 0;
        checkXr(
            xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr),
            "xrEnumerateInstanceExtensionProperties(count)");

        std::vector<XrExtensionProperties> extensions(
            extensionCount,
            XrExtensionProperties { XR_TYPE_EXTENSION_PROPERTIES });
        checkXr(
            xrEnumerateInstanceExtensionProperties(
                nullptr,
                extensionCount,
                &extensionCount,
                extensions.data()),
            "xrEnumerateInstanceExtensionProperties(list)");

        bool hasD3D11 = false;
        for (const XrExtensionProperties& extension : extensions)
        {
            if (std::strcmp(
                    extension.extensionName,
                    XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
            {
                hasD3D11 = true;
                break;
            }
        }
        if (!hasD3D11)
            throw std::runtime_error("The active OpenXR runtime does not expose XR_KHR_D3D11_enable.");

        const char* enabledExtensions[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
        XrInstanceCreateInfo createInfo { XR_TYPE_INSTANCE_CREATE_INFO };
        strcpy_s(
            createInfo.applicationInfo.applicationName,
            gameMode_ ? "GTA IV XR Game" : "GTA IV XR Calibration");
        createInfo.applicationInfo.applicationVersion = 1;
        strcpy_s(createInfo.applicationInfo.engineName, "gtaiv-dxvk-vr");
        createInfo.applicationInfo.engineVersion = 1;
        createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        createInfo.enabledExtensionCount = 1;
        createInfo.enabledExtensionNames = enabledExtensions;

        checkXr(xrCreateInstance(&createInfo, &instance_), "xrCreateInstance");

        XrInstanceProperties properties { XR_TYPE_INSTANCE_PROPERTIES };
        checkXr(xrGetInstanceProperties(instance_, &properties), "xrGetInstanceProperties");

        std::ostringstream message;
        message << "XRHost: runtime=" << properties.runtimeName
                << " version=" << xrVersionString(properties.runtimeVersion)
                << " api=" << xrVersionString(XR_CURRENT_API_VERSION);
        logger_.write(message.str());
    }

    void querySystem()
    {
        XrSystemGetInfo getInfo { XR_TYPE_SYSTEM_GET_INFO };
        getInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        checkXr(xrGetSystem(instance_, &getInfo, &systemId_), "xrGetSystem(HMD)");

        XrSystemProperties properties { XR_TYPE_SYSTEM_PROPERTIES };
        checkXr(xrGetSystemProperties(instance_, systemId_, &properties), "xrGetSystemProperties");

        std::ostringstream message;
        message << "XRHost: system=" << properties.systemName
                << " vendorId=" << properties.vendorId
                << " maxLayerWidth=" << properties.graphicsProperties.maxSwapchainImageWidth
                << " maxLayerHeight=" << properties.graphicsProperties.maxSwapchainImageHeight;
        logger_.write(message.str());

        PFN_xrVoidFunction function = nullptr;
        checkXr(
            xrGetInstanceProcAddr(
                instance_,
                "xrGetD3D11GraphicsRequirementsKHR",
                &function),
            "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)");
        getD3D11GraphicsRequirements_ =
            reinterpret_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(function);
        if (!getD3D11GraphicsRequirements_)
            throw std::runtime_error("xrGetD3D11GraphicsRequirementsKHR is null.");

        graphicsRequirements_ = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
        checkXr(
            getD3D11GraphicsRequirements_(instance_, systemId_, &graphicsRequirements_),
            "xrGetD3D11GraphicsRequirementsKHR");
    }

    void createD3D11Device()
    {
        ComPtr<IDXGIFactory1> factory;
        HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(result))
            throw std::runtime_error("CreateDXGIFactory1 failed: " + hresultString(result));

        for (UINT adapterIndex = 0;; ++adapterIndex)
        {
            ComPtr<IDXGIAdapter1> candidate;
            result = factory->EnumAdapters1(adapterIndex, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(result))
                throw std::runtime_error("EnumAdapters1 failed: " + hresultString(result));

            DXGI_ADAPTER_DESC1 description {};
            result = candidate->GetDesc1(&description);
            if (FAILED(result))
                throw std::runtime_error("IDXGIAdapter1::GetDesc1 failed: " + hresultString(result));

            if (description.AdapterLuid.HighPart == graphicsRequirements_.adapterLuid.HighPart
                && description.AdapterLuid.LowPart == graphicsRequirements_.adapterLuid.LowPart)
            {
                adapter_ = candidate;
                adapterDescription_ = description;
                break;
            }
        }

        if (!adapter_)
        {
            throw std::runtime_error(
                "No DXGI adapter matched the OpenXR runtime LUID "
                + luidString(graphicsRequirements_.adapterLuid) + '.');
        }

        const std::array<D3D_FEATURE_LEVEL, 6> allFeatureLevels {
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };
        std::vector<D3D_FEATURE_LEVEL> allowedFeatureLevels;
        for (D3D_FEATURE_LEVEL level : allFeatureLevels)
        {
            if (level >= graphicsRequirements_.minFeatureLevel)
                allowedFeatureLevels.push_back(level);
        }
        if (allowedFeatureLevels.empty())
            throw std::runtime_error("The runtime requested an unsupported D3D feature level.");

        result = D3D11CreateDevice(
            adapter_.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            allowedFeatureLevels.data(),
            static_cast<UINT>(allowedFeatureLevels.size()),
            D3D11_SDK_VERSION,
            &device_,
            &selectedFeatureLevel_,
            &context_);
        if (FAILED(result))
            throw std::runtime_error("D3D11CreateDevice failed: " + hresultString(result));

        const std::string adapterNameUtf8 = utf8FromWide(adapterDescription_.Description);
        std::ostringstream message;
        message << "XRHost: adapterLuid=" << luidString(adapterDescription_.AdapterLuid)
                << " adapter=\"" << adapterNameUtf8 << "\""
                << " featureLevel=0x" << std::hex << static_cast<uint32_t>(selectedFeatureLevel_)
                << " minFeatureLevel=0x" << static_cast<uint32_t>(graphicsRequirements_.minFeatureLevel);
        logger_.write(message.str());
    }

    void createSession()
    {
        XrGraphicsBindingD3D11KHR graphicsBinding { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
        graphicsBinding.device = device_.Get();

        XrSessionCreateInfo createInfo { XR_TYPE_SESSION_CREATE_INFO };
        createInfo.next = &graphicsBinding;
        createInfo.systemId = systemId_;
        checkXr(xrCreateSession(instance_, &createInfo, &session_), "xrCreateSession");

        uint32_t blendModeCount = 0;
        checkXr(
            xrEnumerateEnvironmentBlendModes(
                instance_,
                systemId_,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                0,
                &blendModeCount,
                nullptr),
            "xrEnumerateEnvironmentBlendModes(count)");
        std::vector<XrEnvironmentBlendMode> blendModes(blendModeCount);
        checkXr(
            xrEnumerateEnvironmentBlendModes(
                instance_,
                systemId_,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                blendModeCount,
                &blendModeCount,
                blendModes.data()),
            "xrEnumerateEnvironmentBlendModes(list)");
        if (blendModes.empty())
            throw std::runtime_error("The OpenXR runtime returned no environment blend mode.");

        environmentBlendMode_ = blendModes.front();
        for (XrEnvironmentBlendMode mode : blendModes)
        {
            if (mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
            {
                environmentBlendMode_ = mode;
                break;
            }
        }
        logger_.write("XRHost: D3D11 session created");
    }

    void createReferenceSpace()
    {
        XrReferenceSpaceCreateInfo createInfo { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        createInfo.poseInReferenceSpace.orientation.w = 1.0f;
        checkXr(xrCreateReferenceSpace(session_, &createInfo, &space_), "xrCreateReferenceSpace(LOCAL)");
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        checkXr(
            xrCreateReferenceSpace(session_, &createInfo, &viewSpace_),
            "xrCreateReferenceSpace(VIEW)");
        logger_.write("XRHost: referenceSpace=LOCAL");
    }

    XrPath xrPath(const char* value)
    {
        XrPath result = XR_NULL_PATH;
        checkXr(xrStringToPath(instance_, value, &result), "xrStringToPath");
        return result;
    }

    void createInputAction(
        XrActionType type,
        const char* name,
        const char* localizedName,
        XrAction& action)
    {
        XrActionCreateInfo createInfo { XR_TYPE_ACTION_CREATE_INFO };
        createInfo.actionType = type;
        strcpy_s(createInfo.actionName, name);
        strcpy_s(createInfo.localizedActionName, localizedName);
        createInfo.countSubactionPaths = static_cast<uint32_t>(hands_.size());
        createInfo.subactionPaths = hands_.data();
        checkXr(xrCreateAction(inputActionSet_, &createInfo, &action), "xrCreateAction");
    }

    void createActionSpace(XrAction action, uint32_t hand, XrSpace& space)
    {
        XrActionSpaceCreateInfo createInfo { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        createInfo.action = action;
        createInfo.subactionPath = hands_[hand];
        createInfo.poseInActionSpace.orientation.w = 1.0f;
        checkXr(xrCreateActionSpace(session_, &createInfo, &space), "xrCreateActionSpace");
    }

    void createInputActions()
    {
        hands_[0] = xrPath("/user/hand/left");
        hands_[1] = xrPath("/user/hand/right");

        XrActionSetCreateInfo actionSetInfo { XR_TYPE_ACTION_SET_CREATE_INFO };
        strcpy_s(actionSetInfo.actionSetName, "gtaiv_touch");
        strcpy_s(actionSetInfo.localizedActionSetName, "GTA IV Touch Controllers");
        checkXr(
            xrCreateActionSet(instance_, &actionSetInfo, &inputActionSet_),
            "xrCreateActionSet(GTAIV Touch)");

        createInputAction(
            XR_ACTION_TYPE_POSE_INPUT,
            "grip_pose",
            "Grip Pose",
            inputActions_.gripPose);
        createInputAction(
            XR_ACTION_TYPE_POSE_INPUT,
            "aim_pose",
            "Aim Pose",
            inputActions_.aimPose);
        createInputAction(
            XR_ACTION_TYPE_FLOAT_INPUT,
            "trigger",
            "Trigger",
            inputActions_.trigger);
        createInputAction(
            XR_ACTION_TYPE_FLOAT_INPUT,
            "squeeze",
            "Squeeze",
            inputActions_.squeeze);
        createInputAction(
            XR_ACTION_TYPE_VECTOR2F_INPUT,
            "thumbstick",
            "Thumbstick",
            inputActions_.thumbstick);
        createInputAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "primary_click",
            "Primary Button",
            inputActions_.primaryClick);
        createInputAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "secondary_click",
            "Secondary Button",
            inputActions_.secondaryClick);
        createInputAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "menu_click",
            "Menu Button",
            inputActions_.menuClick);
        createInputAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "thumbstick_click",
            "Thumbstick Click",
            inputActions_.thumbstickClick);
        createInputAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "primary_touch",
            "Primary Touch",
            inputActions_.primaryTouch);
        createInputAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "secondary_touch",
            "Secondary Touch",
            inputActions_.secondaryTouch);
        createInputAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "thumbstick_touch",
            "Thumbstick Touch",
            inputActions_.thumbstickTouch);
        createInputAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "thumbrest_touch",
            "Thumbrest Touch",
            inputActions_.thumbrestTouch);
        createInputAction(
            XR_ACTION_TYPE_VIBRATION_OUTPUT,
            "haptic",
            "Controller Vibration",
            inputActions_.haptic);

        std::vector<XrActionSuggestedBinding> bindings;
        const auto bind = [this, &bindings](XrAction action, const char* path) {
            bindings.push_back({ action, xrPath(path) });
        };
        bind(inputActions_.gripPose, "/user/hand/left/input/grip/pose");
        bind(inputActions_.gripPose, "/user/hand/right/input/grip/pose");
        bind(inputActions_.aimPose, "/user/hand/left/input/aim/pose");
        bind(inputActions_.aimPose, "/user/hand/right/input/aim/pose");
        bind(inputActions_.trigger, "/user/hand/left/input/trigger/value");
        bind(inputActions_.trigger, "/user/hand/right/input/trigger/value");
        bind(inputActions_.squeeze, "/user/hand/left/input/squeeze/value");
        bind(inputActions_.squeeze, "/user/hand/right/input/squeeze/value");
        bind(inputActions_.thumbstick, "/user/hand/left/input/thumbstick");
        bind(inputActions_.thumbstick, "/user/hand/right/input/thumbstick");
        bind(inputActions_.primaryClick, "/user/hand/left/input/x/click");
        bind(inputActions_.primaryClick, "/user/hand/right/input/a/click");
        bind(inputActions_.secondaryClick, "/user/hand/left/input/y/click");
        bind(inputActions_.secondaryClick, "/user/hand/right/input/b/click");
        bind(inputActions_.menuClick, "/user/hand/left/input/menu/click");
        bind(inputActions_.thumbstickClick, "/user/hand/left/input/thumbstick/click");
        bind(inputActions_.thumbstickClick, "/user/hand/right/input/thumbstick/click");
        bind(inputActions_.primaryTouch, "/user/hand/left/input/x/touch");
        bind(inputActions_.primaryTouch, "/user/hand/right/input/a/touch");
        bind(inputActions_.secondaryTouch, "/user/hand/left/input/y/touch");
        bind(inputActions_.secondaryTouch, "/user/hand/right/input/b/touch");
        bind(inputActions_.thumbstickTouch, "/user/hand/left/input/thumbstick/touch");
        bind(inputActions_.thumbstickTouch, "/user/hand/right/input/thumbstick/touch");
        bind(inputActions_.thumbrestTouch, "/user/hand/left/input/thumbrest/touch");
        bind(inputActions_.thumbrestTouch, "/user/hand/right/input/thumbrest/touch");
        bind(inputActions_.haptic, "/user/hand/left/output/haptic");
        bind(inputActions_.haptic, "/user/hand/right/output/haptic");

        XrInteractionProfileSuggestedBinding suggested {
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggested.interactionProfile =
            xrPath("/interaction_profiles/oculus/touch_controller");
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        checkXr(
            xrSuggestInteractionProfileBindings(instance_, &suggested),
            "xrSuggestInteractionProfileBindings(Oculus Touch)");

        createActionSpace(inputActions_.gripPose, 0u, gripSpaces_[0]);
        createActionSpace(inputActions_.gripPose, 1u, gripSpaces_[1]);
        createActionSpace(inputActions_.aimPose, 0u, aimSpaces_[0]);
        createActionSpace(inputActions_.aimPose, 1u, aimSpaces_[1]);

        XrSessionActionSetsAttachInfo attachInfo {
            XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
        attachInfo.countActionSets = 1u;
        attachInfo.actionSets = &inputActionSet_;
        checkXr(
            xrAttachSessionActionSets(session_, &attachInfo),
            "xrAttachSessionActionSets(GTAIV Touch)");
        inputReady_ = true;
        logger_.write(
            "XRHost: Touch actions ready "
            "(grip/aim, triggers, squeeze, sticks, buttons, touches, haptics)");
    }

    void createColorSwapchain(
        ColorSwapchain& swapchain,
        int64_t format,
        uint32_t width,
        uint32_t height,
        const char* operation)
    {
        swapchain.format = format;
        swapchain.width = width;
        swapchain.height = height;

        XrSwapchainCreateInfo createInfo { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.format = format;
        createInfo.sampleCount = 1;
        createInfo.width = width;
        createInfo.height = height;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;
        checkXr(
            xrCreateSwapchain(session_, &createInfo, &swapchain.handle),
            operation);

        uint32_t imageCount = 0;
        checkXr(
            xrEnumerateSwapchainImages(
                swapchain.handle,
                0,
                &imageCount,
                nullptr),
            "xrEnumerateSwapchainImages(count)");
        swapchain.images.assign(
            imageCount,
            XrSwapchainImageD3D11KHR { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        checkXr(
            xrEnumerateSwapchainImages(
                swapchain.handle,
                imageCount,
                &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(
                    swapchain.images.data())),
            "xrEnumerateSwapchainImages(list)");

        swapchain.renderTargetViews.reserve(imageCount);
        for (const XrSwapchainImageD3D11KHR& image : swapchain.images)
        {
            D3D11_RENDER_TARGET_VIEW_DESC viewDescription {};
            viewDescription.Format = static_cast<DXGI_FORMAT>(format);
            viewDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            viewDescription.Texture2D.MipSlice = 0;

            ComPtr<ID3D11RenderTargetView> renderTargetView;
            const HRESULT result = device_->CreateRenderTargetView(
                image.texture,
                &viewDescription,
                &renderTargetView);
            if (FAILED(result))
            {
                throw std::runtime_error(
                    "CreateRenderTargetView failed: "
                    + hresultString(result));
            }
            swapchain.renderTargetViews.push_back(renderTargetView);
        }
    }

    void createSwapchains()
    {
        uint32_t viewCount = 0;
        checkXr(
            xrEnumerateViewConfigurationViews(
                instance_,
                systemId_,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                0,
                &viewCount,
                nullptr),
            "xrEnumerateViewConfigurationViews(count)");
        if (viewCount != EyeCount)
        {
            std::ostringstream message;
            message << "PRIMARY_STEREO returned " << viewCount << " views; expected exactly 2.";
            throw std::runtime_error(message.str());
        }

        viewConfigurationViews_.assign(
            viewCount,
            XrViewConfigurationView { XR_TYPE_VIEW_CONFIGURATION_VIEW });
        checkXr(
            xrEnumerateViewConfigurationViews(
                instance_,
                systemId_,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                viewCount,
                &viewCount,
                viewConfigurationViews_.data()),
            "xrEnumerateViewConfigurationViews(list)");

        uint32_t formatCount = 0;
        checkXr(
            xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr),
            "xrEnumerateSwapchainFormats(count)");
        std::vector<int64_t> formats(formatCount);
        checkXr(
            xrEnumerateSwapchainFormats(
                session_,
                formatCount,
                &formatCount,
                formats.data()),
            "xrEnumerateSwapchainFormats(list)");

        const std::array<DXGI_FORMAT, 4> preferredFormats {
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM
        };
        int64_t selectedFormat = -1;
        for (DXGI_FORMAT preferred : preferredFormats)
        {
            for (int64_t available : formats)
            {
                if (available == static_cast<int64_t>(preferred))
                {
                    selectedFormat = available;
                    break;
                }
            }
            if (selectedFormat != -1)
                break;
        }
        if (selectedFormat == -1)
            throw std::runtime_error("The runtime exposes no supported RGBA/BGRA D3D11 swapchain format.");

        for (uint32_t eye = 0; eye < EyeCount; ++eye)
        {
            ColorSwapchain& swapchain = swapchains_[eye];
            const uint32_t projectionWidth = gameMode_
                ? (std::min)(
                    GameProjectionDimension,
                    viewConfigurationViews_[eye].maxImageRectWidth)
                : viewConfigurationViews_[eye].recommendedImageRectWidth;
            const uint32_t projectionHeight = gameMode_
                ? (std::min)(
                    GameProjectionDimension,
                    viewConfigurationViews_[eye].maxImageRectHeight)
                : viewConfigurationViews_[eye].recommendedImageRectHeight;
            createColorSwapchain(
                swapchain,
                selectedFormat,
                projectionWidth,
                projectionHeight,
                "xrCreateSwapchain(projection)");

            std::ostringstream message;
            message << "XRHost: view[" << eye << "] projection="
                    << swapchain.width << 'x' << swapchain.height
                    << " samples=" << viewConfigurationViews_[eye].recommendedSwapchainSampleCount
                    << " swapchainImages=" << swapchain.images.size();
            logger_.write(message.str());
        }

        if (gameMode_)
        {
            const uint32_t uiWidth =
                viewConfigurationViews_[0].recommendedImageRectWidth;
            const uint32_t uiHeight =
                (std::max)(1u, uiWidth * 9u / 16u);
            createColorSwapchain(
                uiSwapchain_,
                selectedFormat,
                uiWidth,
                uiHeight,
                "xrCreateSwapchain(UI quad)");
            std::ostringstream message;
            message << "XRHost: stationary UI swapchain="
                    << uiSwapchain_.width << 'x' << uiSwapchain_.height
                    << " swapchainImages=" << uiSwapchain_.images.size();
            logger_.write(message.str());
        }

        std::ostringstream formatMessage;
        formatMessage << "XRHost: swapchainFormat=" << selectedFormat;
        logger_.write(formatMessage.str());
    }

    void createRenderer()
    {
        const ComPtr<ID3DBlob> vertexBytecode = compileShader("vsMain", "vs_5_0");
        const ComPtr<ID3DBlob> pixelBytecode = compileShader("psMain", "ps_5_0");
        const ComPtr<ID3DBlob> gamePixelBytecode =
            compileShader("psGame", "ps_5_0");

        HRESULT result = device_->CreateVertexShader(
            vertexBytecode->GetBufferPointer(),
            vertexBytecode->GetBufferSize(),
            nullptr,
            &vertexShader_);
        if (FAILED(result))
            throw std::runtime_error("CreateVertexShader failed: " + hresultString(result));

        result = device_->CreatePixelShader(
            pixelBytecode->GetBufferPointer(),
            pixelBytecode->GetBufferSize(),
            nullptr,
            &pixelShader_);
        if (FAILED(result))
            throw std::runtime_error("CreatePixelShader failed: " + hresultString(result));

        result = device_->CreatePixelShader(
            gamePixelBytecode->GetBufferPointer(),
            gamePixelBytecode->GetBufferSize(),
            nullptr,
            &gamePixelShader_);
        if (FAILED(result))
            throw std::runtime_error("CreatePixelShader(game) failed: " + hresultString(result));

        D3D11_BUFFER_DESC constantBufferDescription {};
        constantBufferDescription.ByteWidth = sizeof(EyeConstants);
        constantBufferDescription.Usage = D3D11_USAGE_DEFAULT;
        constantBufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        result = device_->CreateBuffer(
            &constantBufferDescription,
            nullptr,
            &constantBuffer_);
        if (FAILED(result))
            throw std::runtime_error("CreateBuffer(EyeConstants) failed: " + hresultString(result));

        D3D11_BUFFER_DESC gameConstantBufferDescription {};
        gameConstantBufferDescription.ByteWidth = sizeof(GameConstants);
        gameConstantBufferDescription.Usage = D3D11_USAGE_DEFAULT;
        gameConstantBufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        result = device_->CreateBuffer(
            &gameConstantBufferDescription,
            nullptr,
            &gameConstantBuffer_);
        if (FAILED(result))
            throw std::runtime_error("CreateBuffer(GameConstants) failed: " + hresultString(result));

        D3D11_SAMPLER_DESC samplerDescription {};
        samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
        result = device_->CreateSamplerState(
            &samplerDescription,
            &gameSampler_);
        if (FAILED(result))
            throw std::runtime_error("CreateSamplerState(game) failed: " + hresultString(result));

        D3D11_RASTERIZER_DESC rasterizerDescription {};
        rasterizerDescription.FillMode = D3D11_FILL_SOLID;
        rasterizerDescription.CullMode = D3D11_CULL_NONE;
        rasterizerDescription.DepthClipEnable = TRUE;
        result = device_->CreateRasterizerState(
            &rasterizerDescription,
            &rasterizerState_);
        if (FAILED(result))
            throw std::runtime_error("CreateRasterizerState failed: " + hresultString(result));

        if (gameMode_)
        {
            const bool bridgeReady = gameBridge_.initialize(
                device_.Get(),
                context_.Get(),
                [this](const std::string& message) {
                    logger_.write(message);
                },
                allowTemporalStereo_);
            if (!bridgeReady)
                throw std::runtime_error("The GTA GPU frame bridge could not initialize.");
            logger_.write(
                "XRHost: GAME mode ready; black until GTAIV.exe publishes a frame");
        }
        else
        {
            logger_.write(
                "XRHost: calibration renderer ready (procedural 3D + L/R markers)");
        }
    }

    void pollEvents()
    {
        XrEventDataBuffer event { XR_TYPE_EVENT_DATA_BUFFER };
        for (;;)
        {
            const XrResult pollResult = xrPollEvent(instance_, &event);
            if (pollResult == XR_EVENT_UNAVAILABLE)
                break;
            checkXr(pollResult, "xrPollEvent");

            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                const auto* changed =
                    reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                sessionState_ = changed->state;
                logger_.write("XRHost: session " + sessionStateName(sessionState_));

                if (sessionState_ == XR_SESSION_STATE_READY && !sessionRunning_)
                {
                    resetReferenceSpacePresentation("session-start");
                    XrSessionBeginInfo beginInfo { XR_TYPE_SESSION_BEGIN_INFO };
                    beginInfo.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    checkXr(xrBeginSession(session_, &beginInfo), "xrBeginSession");
                    sessionRunning_ = true;
                    logger_.write("XRHost: session RUNNING");
                }
                else if (sessionState_ == XR_SESSION_STATE_STOPPING && sessionRunning_)
                {
                    checkXr(xrEndSession(session_), "xrEndSession");
                    sessionRunning_ = false;
                    gamePresentationCache_.invalidate();
                    referenceSpaceChangePending_ = false;
                    logger_.write("XRHost: session stopped");
                }
                else if (sessionState_ == XR_SESSION_STATE_EXITING
                         || sessionState_ == XR_SESSION_STATE_LOSS_PENDING)
                {
                    exitRequested_ = true;
                }
            }
            else if (
                event.type
                    == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
            {
                const auto* changed =
                    reinterpret_cast<
                        const XrEventDataReferenceSpaceChangePending*>(
                            &event);
                if (changed->session == session_
                    && (changed->referenceSpaceType
                            == XR_REFERENCE_SPACE_TYPE_LOCAL
                        || changed->referenceSpaceType
                            == XR_REFERENCE_SPACE_TYPE_VIEW))
                {
                    referenceSpaceChangePending_ = true;
                    referenceSpaceChangeTime_ = changed->changeTime;
                    std::ostringstream message;
                    message
                        << "XRHost: reference-space change pending type="
                        << static_cast<int32_t>(
                            changed->referenceSpaceType)
                        << " changeTime="
                        << static_cast<int64_t>(changed->changeTime)
                        << "; old generation remains valid until changeTime";
                    logger_.write(message.str());
                }
            }
            else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
            {
                logger_.write("XRHost: instance loss pending");
                exitRequested_ = true;
            }

            event = { XR_TYPE_EVENT_DATA_BUFFER };
        }
    }

    FloatActionSample readFloatAction(XrAction action, XrPath hand) const
    {
        XrActionStateGetInfo getInfo { XR_TYPE_ACTION_STATE_GET_INFO };
        getInfo.action = action;
        getInfo.subactionPath = hand;
        XrActionStateFloat state { XR_TYPE_ACTION_STATE_FLOAT };
        if (XR_FAILED(xrGetActionStateFloat(session_, &getInfo, &state))
            || state.isActive != XR_TRUE)
        {
            return {};
        }
        FloatActionSample sample {};
        sample.value = std::clamp(state.currentState, 0.0f, 1.0f);
        sample.active = true;
        return sample;
    }

    Vector2ActionSample readVector2Action(XrAction action, XrPath hand) const
    {
        XrActionStateGetInfo getInfo { XR_TYPE_ACTION_STATE_GET_INFO };
        getInfo.action = action;
        getInfo.subactionPath = hand;
        XrActionStateVector2f state { XR_TYPE_ACTION_STATE_VECTOR2F };
        if (XR_FAILED(xrGetActionStateVector2f(session_, &getInfo, &state))
            || state.isActive != XR_TRUE)
        {
            return {};
        }
        return { state.currentState, true };
    }

    BooleanActionSample readBooleanAction(XrAction action, XrPath hand) const
    {
        XrActionStateGetInfo getInfo { XR_TYPE_ACTION_STATE_GET_INFO };
        getInfo.action = action;
        getInfo.subactionPath = hand;
        XrActionStateBoolean state { XR_TYPE_ACTION_STATE_BOOLEAN };
        if (XR_FAILED(xrGetActionStateBoolean(session_, &getInfo, &state))
            || state.isActive != XR_TRUE)
        {
            return {};
        }
        return { state.currentState == XR_TRUE, true };
    }

    bool poseActionActive(XrAction action, XrPath hand) const
    {
        XrActionStateGetInfo getInfo { XR_TYPE_ACTION_STATE_GET_INFO };
        getInfo.action = action;
        getInfo.subactionPath = hand;
        XrActionStatePose state { XR_TYPE_ACTION_STATE_POSE };
        return XR_SUCCEEDED(xrGetActionStatePose(session_, &getInfo, &state))
            && state.isActive == XR_TRUE;
    }

    static void initializePose(gtaiv_xr_bridge::Pose& destination)
    {
        destination.orientation[3] = 1.0f;
    }

    static void copyPose(
        gtaiv_xr_bridge::Pose& destination,
        const XrPosef& source)
    {
        destination.orientation[0] = source.orientation.x;
        destination.orientation[1] = source.orientation.y;
        destination.orientation[2] = source.orientation.z;
        destination.orientation[3] = source.orientation.w;
        destination.position[0] = source.position.x;
        destination.position[1] = source.position.y;
        destination.position[2] = source.position.z;
    }

    static bool deriveHmdPoseFromViews(
        gtaiv_xr_bridge::Pose& destination,
        const std::array<XrView, EyeCount>& views)
    {
        const XrQuaternionf left = views[0].pose.orientation;
        XrQuaternionf right = views[1].pose.orientation;
        const float dot =
            left.x * right.x +
            left.y * right.y +
            left.z * right.z +
            left.w * right.w;
        if (dot < 0.0f)
        {
            right.x = -right.x;
            right.y = -right.y;
            right.z = -right.z;
            right.w = -right.w;
        }
        const float qx = left.x + right.x;
        const float qy = left.y + right.y;
        const float qz = left.z + right.z;
        const float qw = left.w + right.w;
        const float norm = std::sqrt(
            qx * qx + qy * qy + qz * qz + qw * qw);
        if (!std::isfinite(norm) || norm < 0.5f)
            return false;
        const float inverseNorm = 1.0f / norm;
        destination.orientation[0] = qx * inverseNorm;
        destination.orientation[1] = qy * inverseNorm;
        destination.orientation[2] = qz * inverseNorm;
        destination.orientation[3] = qw * inverseNorm;
        destination.position[0] =
            0.5f * (
                views[0].pose.position.x +
                views[1].pose.position.x);
        destination.position[1] =
            0.5f * (
                views[0].pose.position.y +
                views[1].pose.position.y);
        destination.position[2] =
            0.5f * (
                views[0].pose.position.z +
                views[1].pose.position.z);
        return
            std::isfinite(destination.position[0]) &&
            std::isfinite(destination.position[1]) &&
            std::isfinite(destination.position[2]);
    }

    bool locatePose(
        XrSpace source,
        XrTime displayTime,
        gtaiv_xr_bridge::Pose& destination) const
    {
        if (source == XR_NULL_HANDLE)
            return false;
        XrSpaceLocation location { XR_TYPE_SPACE_LOCATION };
        if (XR_FAILED(xrLocateSpace(source, space_, displayTime, &location)))
            return false;
        const XrSpaceLocationFlags required =
            XR_SPACE_LOCATION_POSITION_VALID_BIT
            | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((location.locationFlags & required) != required)
            return false;
        copyPose(destination, location.pose);
        return true;
    }

    void setActive(
        gtaiv_xr_bridge::ControllerState& destination,
        uint32_t activeBit,
        const FloatActionSample& value,
        float& output) const
    {
        if (!value.active)
            return;
        destination.activeFlags |= activeBit;
        output = value.value;
    }

    void setButton(
        gtaiv_xr_bridge::ControllerState& destination,
        uint32_t activeBit,
        uint32_t buttonBit,
        const BooleanActionSample& value) const
    {
        if (!value.active)
            return;
        destination.activeFlags |= activeBit;
        if (value.value)
            destination.buttons |= buttonBit;
    }

    bool fillControllerState(
        uint32_t hand,
        XrTime displayTime,
        gtaiv_xr_bridge::ControllerState& destination) const
    {
        initializePose(destination.gripPose);
        initializePose(destination.aimPose);
        bool valid = false;
        if (poseActionActive(inputActions_.gripPose, hands_[hand])
            && locatePose(gripSpaces_[hand], displayTime, destination.gripPose))
        {
            destination.activeFlags |= gtaiv_xr_bridge::ControllerGripPose;
            valid = true;
        }
        if (poseActionActive(inputActions_.aimPose, hands_[hand])
            && locatePose(aimSpaces_[hand], displayTime, destination.aimPose))
        {
            destination.activeFlags |= gtaiv_xr_bridge::ControllerAimPose;
            valid = true;
        }

        setActive(
            destination,
            gtaiv_xr_bridge::ControllerTrigger,
            readFloatAction(inputActions_.trigger, hands_[hand]),
            destination.trigger);
        setActive(
            destination,
            gtaiv_xr_bridge::ControllerSqueeze,
            readFloatAction(inputActions_.squeeze, hands_[hand]),
            destination.squeeze);
        const Vector2ActionSample stick =
            readVector2Action(inputActions_.thumbstick, hands_[hand]);
        if (stick.active)
        {
            destination.activeFlags |= gtaiv_xr_bridge::ControllerThumbstick;
            destination.thumbstickX = stick.value.x;
            destination.thumbstickY = stick.value.y;
        }
        setButton(
            destination,
            gtaiv_xr_bridge::ControllerPrimary,
            gtaiv_xr_bridge::ControllerPrimaryClick,
            readBooleanAction(inputActions_.primaryClick, hands_[hand]));
        setButton(
            destination,
            gtaiv_xr_bridge::ControllerSecondary,
            gtaiv_xr_bridge::ControllerSecondaryClick,
            readBooleanAction(inputActions_.secondaryClick, hands_[hand]));
        setButton(
            destination,
            gtaiv_xr_bridge::ControllerMenu,
            gtaiv_xr_bridge::ControllerMenuClick,
            readBooleanAction(inputActions_.menuClick, hands_[hand]));
        setButton(
            destination,
            gtaiv_xr_bridge::ControllerThumbstickClick,
            gtaiv_xr_bridge::ControllerThumbstickPressed,
            readBooleanAction(inputActions_.thumbstickClick, hands_[hand]));
        setButton(
            destination,
            gtaiv_xr_bridge::ControllerPrimaryTouch,
            gtaiv_xr_bridge::ControllerPrimaryTouched,
            readBooleanAction(inputActions_.primaryTouch, hands_[hand]));
        setButton(
            destination,
            gtaiv_xr_bridge::ControllerSecondaryTouch,
            gtaiv_xr_bridge::ControllerSecondaryTouched,
            readBooleanAction(inputActions_.secondaryTouch, hands_[hand]));
        setButton(
            destination,
            gtaiv_xr_bridge::ControllerThumbstickTouch,
            gtaiv_xr_bridge::ControllerThumbstickTouched,
            readBooleanAction(inputActions_.thumbstickTouch, hands_[hand]));
        setButton(
            destination,
            gtaiv_xr_bridge::ControllerThumbrestTouch,
            gtaiv_xr_bridge::ControllerThumbrestTouched,
            readBooleanAction(inputActions_.thumbrestTouch, hands_[hand]));

        if ((destination.activeFlags & gtaiv_xr_bridge::ControllerTrigger) != 0u
            && destination.trigger >= 0.75f)
        {
            destination.activeFlags |= gtaiv_xr_bridge::ControllerTriggerClick;
            destination.buttons |= gtaiv_xr_bridge::ControllerTriggerPressed;
        }
        if ((destination.activeFlags & gtaiv_xr_bridge::ControllerSqueeze) != 0u
            && destination.squeeze >= 0.75f)
        {
            destination.activeFlags |= gtaiv_xr_bridge::ControllerSqueezeClick;
            destination.buttons |= gtaiv_xr_bridge::ControllerSqueezePressed;
        }
        return valid;
    }

    void stopAllHaptics() noexcept
    {
        if (session_ == XR_NULL_HANDLE
            || inputActions_.haptic == XR_NULL_HANDLE)
        {
            hapticActive_ = {};
            return;
        }
        for (uint32_t hand = 0u; hand < EyeCount; ++hand)
        {
            if (!hapticActive_[hand])
                continue;
            XrHapticActionInfo actionInfo { XR_TYPE_HAPTIC_ACTION_INFO };
            actionInfo.action = inputActions_.haptic;
            actionInfo.subactionPath = hands_[hand];
            xrStopHapticFeedback(session_, &actionInfo);
            hapticActive_[hand] = false;
        }
    }

    void updateHaptics()
    {
        const gtaiv_xr_host::HapticCommand command =
            hapticBridge_.update();
        const bool active =
            command.active
            && sessionState_ == XR_SESSION_STATE_FOCUSED;
        const std::array<float, EyeCount> amplitudes {
            active ? command.leftAmplitude : 0.0f,
            active ? command.rightAmplitude : 0.0f
        };
        if (command.producerEpoch == lastHapticEpoch_
            && command.commandId == lastHapticCommandId_
            && amplitudes == lastHapticAmplitudes_)
        {
            return;
        }

        for (uint32_t hand = 0u; hand < EyeCount; ++hand)
        {
            XrHapticActionInfo actionInfo { XR_TYPE_HAPTIC_ACTION_INFO };
            actionInfo.action = inputActions_.haptic;
            actionInfo.subactionPath = hands_[hand];
            XrResult result = XR_SUCCESS;
            if (amplitudes[hand] > 0.0f)
            {
                XrHapticVibration vibration { XR_TYPE_HAPTIC_VIBRATION };
                vibration.duration = XR_INFINITE_DURATION;
                vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
                vibration.amplitude = amplitudes[hand];
                result = xrApplyHapticFeedback(
                    session_,
                    &actionInfo,
                    reinterpret_cast<const XrHapticBaseHeader*>(
                        &vibration));
                hapticActive_[hand] = XR_SUCCEEDED(result);
            }
            else
            {
                result = xrStopHapticFeedback(session_, &actionInfo);
                hapticActive_[hand] = false;
            }
            if (XR_FAILED(result))
            {
                const uint64_t now = GetTickCount64();
                if (lastHapticErrorLogTick_ == 0u
                    || now - lastHapticErrorLogTick_ >= 2000u)
                {
                    std::ostringstream message;
                    message << "XRHost: Touch haptic command rejected hand="
                            << hand
                            << " result=" << static_cast<int32_t>(result);
                    logger_.write(message.str());
                    lastHapticErrorLogTick_ = now;
                }
            }
        }
        lastHapticEpoch_ = command.producerEpoch;
        lastHapticCommandId_ = command.commandId;
        lastHapticAmplitudes_ = amplitudes;
    }

    void publishPoseAndInput(
        const XrFrameState& frameState,
        const std::array<XrView, EyeCount>& views,
        bool viewsValid)
    {
        gtaiv_xr_bridge::PoseBridge frame {};
        frame.flags = gtaiv_xr_bridge::PoseBridgeHostRunning
            | gtaiv_xr_bridge::PoseBridgeSessionRunning;
        if (sessionState_ == XR_SESSION_STATE_FOCUSED)
            frame.flags |= gtaiv_xr_bridge::PoseBridgeInputFocused;
        frame.frameId = frameCounter_ + 1u;
        frame.predictedDisplayTime = static_cast<int64_t>(frameState.predictedDisplayTime);
        frame.referenceSpaceGeneration = referenceSpaceGeneration_;
        frame.recenterRequestId = recenterRequestId_;
        initializePose(frame.hmdPose);
        for (uint32_t eye = 0u; eye < EyeCount; ++eye)
            initializePose(frame.eyePoses[eye]);

        if (viewsValid)
        {
            frame.flags |= gtaiv_xr_bridge::PoseBridgeViewsValid;
            for (uint32_t eye = 0u; eye < EyeCount; ++eye)
            {
                copyPose(frame.eyePoses[eye], views[eye].pose);
                frame.eyeFovs[eye].angleLeft = views[eye].fov.angleLeft;
                frame.eyeFovs[eye].angleRight = views[eye].fov.angleRight;
                frame.eyeFovs[eye].angleUp = views[eye].fov.angleUp;
                frame.eyeFovs[eye].angleDown = views[eye].fov.angleDown;
            }
        }

        const bool viewDerivedHmd =
            viewsValid &&
            deriveHmdPoseFromViews(frame.hmdPose, views);
        if (viewDerivedHmd ||
            locatePose(
                viewSpace_,
                frameState.predictedDisplayTime,
                frame.hmdPose))
            frame.flags |= gtaiv_xr_bridge::PoseBridgeHmdValid;

        if (inputReady_)
        {
            XrActiveActionSet activeSet {};
            activeSet.actionSet = inputActionSet_;
            XrActionsSyncInfo syncInfo { XR_TYPE_ACTIONS_SYNC_INFO };
            syncInfo.countActiveActionSets = 1u;
            syncInfo.activeActionSets = &activeSet;
            const XrResult syncResult = xrSyncActions(session_, &syncInfo);
            if (XR_SUCCEEDED(syncResult))
            {
                if (fillControllerState(
                        0u,
                        frameState.predictedDisplayTime,
                        frame.controllers[0]))
                {
                    frame.flags |= gtaiv_xr_bridge::PoseBridgeLeftControllerValid;
                }
                if (fillControllerState(
                        1u,
                        frameState.predictedDisplayTime,
                        frame.controllers[1]))
                {
                    frame.flags |= gtaiv_xr_bridge::PoseBridgeRightControllerValid;
                }
            }
            else if (!inputSyncErrorLogged_)
            {
                std::ostringstream message;
                message << "XRHost: xrSyncActions unavailable result="
                        << static_cast<int32_t>(syncResult);
                logger_.write(message.str());
                inputSyncErrorLogged_ = true;
            }
        }

        const bool recenterChord =
            (frame.controllers[0].buttons & gtaiv_xr_bridge::ControllerMenuClick) != 0u
            && (frame.controllers[1].buttons
                & gtaiv_xr_bridge::ControllerThumbstickPressed) != 0u;
        if (recenterChord && !recenterChordDown_)
        {
            ++recenterRequestId_;
            if (recenterRequestId_ == 0u)
                ++recenterRequestId_;
            frame.recenterRequestId = recenterRequestId_;
            logger_.write("XRHost: Touch recenter chord requested (left Menu + right stick click)");
        }
        recenterChordDown_ = recenterChord;

        poseBridge_.publish(frame);
        updateHaptics();
        if (!loggedInput_)
        {
            logger_.write(
                "XRHost: Pose/Input publishing started");
            loggedInput_ = true;
        }
    }

    void rememberLocatedViews(
        uint64_t sequence,
        XrTime predictedDisplayTime,
        const std::array<XrView, EyeCount>& views)
    {
        LocatedViewSample& sample =
            locatedViewHistory_[sequence % locatedViewHistory_.size()];
        sample.sequence = sequence;
        sample.predictedDisplayTime =
            static_cast<int64_t>(predictedDisplayTime);
        sample.views = views;
    }

    bool findRenderedView(
        uint64_t sequence,
        int64_t predictedDisplayTime,
        uint32_t eye,
        XrView& output) const
    {
        if (sequence == 0u || eye >= EyeCount)
            return false;
        const LocatedViewSample& sample =
            locatedViewHistory_[sequence % locatedViewHistory_.size()];
        if (sample.sequence != sequence
            || sample.predictedDisplayTime != predictedDisplayTime)
        {
            return false;
        }
        output = sample.views[eye];
        return true;
    }

    void commitGamePresentation(
        const gtaiv_xr_host::GamePresentationKey& key,
        const PreparedGamePresentation& presentation)
    {
        gamePresentationCache_.commit(key, presentation);
        const uint64_t updates = gamePresentationCache_.updateCount();
        if (updates <= 4u || updates % 120u == 0u)
        {
            std::ostringstream message;
            message
                << "XRHost: game swapchain updated transaction="
                << key.transactionId
                << " presentation="
                << gamePresentationRoute(key)
                << " updates=" << updates
                << " reusedHostFrames="
                << gamePresentationCache_.reuseCount();
            logger_.write(message.str());
        }
    }

    void resetReferenceSpacePresentation(const char* reason)
    {
        ++referenceSpaceGeneration_;
        if (referenceSpaceGeneration_ == 0u)
            ++referenceSpaceGeneration_;
        gamePresentationCache_.invalidate();
        menuQuadLatch_.reset();
        for (LocatedViewSample& sample : locatedViewHistory_)
            sample = LocatedViewSample {};
        referenceSpaceChangePending_ = false;
        referenceSpaceChangeTime_ = 0;
        std::ostringstream message;
        message
            << "XRHost: reference-space presentation reset generation="
            << referenceSpaceGeneration_
            << " reason=" << reason;
        logger_.write(message.str());
    }

    void applyPendingReferenceSpaceChange(XrTime predictedDisplayTime)
    {
        if (!referenceSpaceChangePending_)
            return;
        if (referenceSpaceChangeTime_ > 0
            && predictedDisplayTime < referenceSpaceChangeTime_)
        {
            return;
        }
        resetReferenceSpacePresentation("runtime-change");
    }

    bool renderFrame()
    {
        XrFrameWaitInfo waitInfo { XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState frameState { XR_TYPE_FRAME_STATE };
        checkXr(xrWaitFrame(session_, &waitInfo, &frameState), "xrWaitFrame");
        applyPendingReferenceSpaceChange(
            frameState.predictedDisplayTime);

        XrFrameBeginInfo beginInfo { XR_TYPE_FRAME_BEGIN_INFO };
        checkXr(xrBeginFrame(session_, &beginInfo), "xrBeginFrame");

        std::array<XrView, EyeCount> views {
            XrView { XR_TYPE_VIEW },
            XrView { XR_TYPE_VIEW }
        };
        XrViewState viewState { XR_TYPE_VIEW_STATE };
        XrViewLocateInfo locateInfo { XR_TYPE_VIEW_LOCATE_INFO };
        locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = space_;

        uint32_t locatedViewCount = 0;
        checkXr(
            xrLocateViews(
                session_,
                &locateInfo,
                &viewState,
                static_cast<uint32_t>(views.size()),
                &locatedViewCount,
                views.data()),
            "xrLocateViews");

        const XrViewStateFlags requiredFlags =
            XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        const bool viewsValid =
            frameState.shouldRender == XR_TRUE
            && locatedViewCount == EyeCount
            && (viewState.viewStateFlags & requiredFlags) == requiredFlags;

        if (viewsValid)
        {
            rememberLocatedViews(
                frameCounter_ + 1u,
                frameState.predictedDisplayTime,
                views);
        }
        publishPoseAndInput(frameState, views, viewsValid);

        std::array<XrCompositionLayerProjectionView, EyeCount> projectionViews {
            XrCompositionLayerProjectionView {
                XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
            XrCompositionLayerProjectionView {
                XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }
        };
        XrCompositionLayerProjection projectionLayer {
            XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        projectionLayer.space = space_;
        projectionLayer.viewCount = EyeCount;
        projectionLayer.views = projectionViews.data();

        XrCompositionLayerQuad uiLayer { XR_TYPE_COMPOSITION_LAYER_QUAD };
        uiLayer.space = space_;
        uiLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        uiLayer.size.width = 1.8f;
        uiLayer.size.height =
            uiLayer.size.width
            * static_cast<float>(uiSwapchain_.height)
            / static_cast<float>((std::max)(1u, uiSwapchain_.width));
        uiLayer.subImage.swapchain = uiSwapchain_.handle;
        uiLayer.subImage.imageRect.offset = { 0, 0 };
        uiLayer.subImage.imageRect.extent = {
            static_cast<int32_t>(uiSwapchain_.width),
            static_cast<int32_t>(uiSwapchain_.height)
        };
        uiLayer.subImage.imageArrayIndex = 0u;

        bool rendered = false;
        const XrCompositionLayerBaseHeader* submittedLayer = nullptr;
        if (viewsValid)
        {
            if (gameMode_)
                gameFrame_ = gameBridge_.update();

            const bool uiQuad =
                gameMode_
                && gameFrame_
                && gameFrame_.presentationMode
                    == gtaiv_xr_bridge::PresentationMode::UiQuad;
            const bool quadPresentation = uiQuad;
            const bool menuQuadWasLatched =
                menuQuadLatch_.active();
            const bool uiQuadPoseReady =
                menuQuadLatch_.update(
                    uiQuad,
                    referenceSpaceGeneration_,
                    views,
                    1.8f);
            if (uiQuadPoseReady)
                uiLayer.pose = menuQuadLatch_.pose();
            const bool quadPoseReady = uiQuadPoseReady;
            if (uiQuadPoseReady && !menuQuadWasLatched)
            {
                std::ostringstream message;
                message << std::fixed << std::setprecision(3)
                        << "XRHost: stationary UI latched localPose=("
                        << uiLayer.pose.position.x << ','
                        << uiLayer.pose.position.y << ','
                        << uiLayer.pose.position.z << ')';
                logger_.write(message.str());
            }
            if (gameMode_
                && gameFrame_
                && gameFrame_.presentationMode
                    != lastLoggedPresentationMode_)
            {
                std::ostringstream message;
                message << "XRHost: presentation="
                        << (uiQuad
                                ? "stationary-ui-quad"
                                : gameFrame_.presentationMode
                                        == gtaiv_xr_bridge::PresentationMode::WorldMono
                                    ? "world-headtracked-mono-projection"
                                    : gameFrame_.parentDualStereo
                                        ? "world-parent-dual-stereo"
                                        : gameFrame_.temporalStereo
                                            ? "world-temporal-stereo"
                                            : "world-stereo")
                        << " reasons=0x" << std::hex
                        << gameFrame_.uiReasonFlags
                        << std::dec
                        << " parentDual="
                        << (gameFrame_.parentDualStereo ? 1 : 0)
                        << " fp="
                        << (gameFrame_.firstPersonCamera ? 1 : 0)
                        << " headHide="
                        << (gameFrame_.nativeHeadHidden ? 1 : 0)
                        << " pixelDistinct="
                        << (gameFrame_.pixelDistinct ? 1 : 0);
                logger_.write(message.str());
                lastLoggedPresentationMode_ =
                    gameFrame_.presentationMode;
            }

            std::array<XrView, EyeCount> submittedViews = views;
            gtaiv_xr_host::GamePresentationKey presentationKey {};
            PreparedGamePresentation cachedPresentation {};
            bool reusedGamePresentation = false;
            if (gameMode_ && gameFrame_)
            {
                presentationKey = makeGamePresentationKey(
                    gameFrame_,
                    referenceSpaceGeneration_);
                reusedGamePresentation =
                    gamePresentationCache_.tryReuse(
                        presentationKey,
                        cachedPresentation);
                if (reusedGamePresentation)
                {
                    if (cachedPresentation.quadPresentation)
                        uiLayer.pose = cachedPresentation.uiPose;
                    else
                        submittedViews =
                            cachedPresentation.submittedViews;
                    const uint64_t reuseFrames =
                        gamePresentationCache_.reuseCount();
                    if (reuseFrames == 1u || reuseFrames % 300u == 0u)
                    {
                        std::ostringstream message;
                        message
                            << "XRHost: game swapchain reused transaction="
                            << presentationKey.transactionId
                            << " presentation="
                            << gamePresentationRoute(presentationKey)
                            << " reusedHostFrames=" << reuseFrames;
                        logger_.write(message.str());
                    }
                }
            }
            else if (gameMode_)
            {
                gamePresentationCache_.invalidate();
            }

            gameFramePoseMatched_ =
                reusedGamePresentation
                || !gameMode_
                || !gameFrame_
                || quadPresentation;
            if (!reusedGamePresentation
                && gameMode_
                && gameFrame_
                && !quadPresentation)
            {
                if (gameFrame_.requiresExactCapturePose())
                {
                    // Temporal eyes were rendered on different GTA frames;
                    // parent-dual eyes were rendered from one shared Mode 58
                    // pose. In both cases submit each eye's exact capture
                    // pose/FOV from the retained history. Never silently
                    // substitute the host's latest view for a proved route.
                    std::array<XrView, EyeCount> renderedViews {
                        XrView { XR_TYPE_VIEW },
                        XrView { XR_TYPE_VIEW }
                    };
                    gameFramePoseMatched_ = true;
                    for (uint32_t eye = 0u; eye < EyeCount; ++eye)
                    {
                        if (!findRenderedView(
                                gameFrame_.poseSequence[eye],
                                gameFrame_.renderedDisplayTime[eye],
                                eye,
                                renderedViews[eye]))
                        {
                            gameFramePoseMatched_ = false;
                            break;
                        }
                    }
                    if (gameFramePoseMatched_)
                    {
                        submittedViews = renderedViews;
                        uint64_t& lastPoseLogTransaction =
                            gameFrame_.parentDualStereo
                                ? lastParentDualPoseLogTransaction_
                                : lastTemporalPoseLogTransaction_;
                        if (lastPoseLogTransaction == 0u
                            || gameFrame_.transactionId
                                >= lastPoseLogTransaction + 120u)
                        {
                            std::ostringstream message;
                            if (gameFrame_.parentDualStereo)
                            {
                                message
                                    << "XRHost: parent-dual exact capture pose "
                                       "active transaction="
                                    << gameFrame_.transactionId
                                    << " sharedPose="
                                    << gameFrame_.poseSequence[0]
                                    << " displayTime="
                                    << gameFrame_.renderedDisplayTime[0]
                                    << " parentDual=1 fp="
                                    << (gameFrame_.firstPersonCamera ? 1 : 0)
                                    << " headHide="
                                    << (gameFrame_.nativeHeadHidden ? 1 : 0)
                                    << " pixelDistinct="
                                    << (gameFrame_.pixelDistinct ? 1 : 0);
                            }
                            else
                            {
                                message
                                    << "XRHost: temporal pair capture poses active "
                                    << "transaction=" << gameFrame_.transactionId
                                    << " source="
                                    << gameFrame_.sourceFrameId[0] << '/'
                                    << gameFrame_.sourceFrameId[1]
                                    << " pose="
                                    << gameFrame_.poseSequence[0] << '/'
                                    << gameFrame_.poseSequence[1];
                            }
                            logger_.write(message.str());
                            lastPoseLogTransaction =
                                gameFrame_.transactionId;
                        }
                    }
                    else
                    {
                        const uint64_t now = GetTickCount64();
                        uint64_t& lastPoseMissLogTick =
                            gameFrame_.parentDualStereo
                                ? lastParentDualPoseMissLogTick_
                                : lastTemporalPoseMissLogTick_;
                        if (lastPoseMissLogTick == 0u
                            || now - lastPoseMissLogTick >= 2000u)
                        {
                            std::ostringstream message;
                            if (gameFrame_.parentDualStereo)
                            {
                                message
                                    << "XRHost: parent-dual exact capture pose "
                                       "missing; projection layer held "
                                       "transaction="
                                    << gameFrame_.transactionId
                                    << " sharedPose="
                                    << gameFrame_.poseSequence[0];
                            }
                            else
                            {
                                message
                                    << "XRHost: temporal pair capture pose missing; "
                                       "projection layer held transaction="
                                    << gameFrame_.transactionId;
                            }
                            logger_.write(message.str());
                            lastPoseMissLogTick = now;
                        }
                    }
                }
                else
                {
                    // Same-frame world routes remain a latest-frame usability
                    // path. Their pair/pose identities are diagnostics, not a
                    // black-frame gate.
                    gameFramePoseMatched_ = true;
                    if (!loggedGamePoseMatch_)
                    {
                        std::ostringstream message;
                        message
                            << "XRHost: latest-frame world presentation active "
                               "(no pair/pose black gate) "
                            << "transaction=" << gameFrame_.transactionId
                            << " pose=" << gameFrame_.poseSequence[0];
                        logger_.write(message.str());
                        loggedGamePoseMatch_ = true;
                    }
                }
            }
            if (!reusedGamePresentation
                && gameMode_
                && gameFrame_
                && gameFrame_.presentationMode
                    == gtaiv_xr_bridge::PresentationMode::WorldMono)
            {
                // The game produced one center-eye camera image. Submit that
                // same render pose to both eyes so the compositor has an
                // honest mono source for reprojection; the current per-eye
                // FOVs still fill the headset and head motion remains live.
                XrView renderedLeft { XR_TYPE_VIEW };
                XrView renderedRight { XR_TYPE_VIEW };
                const bool haveRenderedViews =
                    findRenderedView(
                        gameFrame_.poseSequence[0],
                        gameFrame_.renderedDisplayTime[0],
                        0u,
                        renderedLeft)
                    && findRenderedView(
                        gameFrame_.poseSequence[1],
                        gameFrame_.renderedDisplayTime[1],
                        1u,
                        renderedRight);
                const XrView& left =
                    haveRenderedViews ? renderedLeft : views[0];
                const XrView& right =
                    haveRenderedViews ? renderedRight : views[1];
                XrPosef centerPose = left.pose;
                centerPose.position.x =
                    (left.pose.position.x + right.pose.position.x) * 0.5f;
                centerPose.position.y =
                    (left.pose.position.y + right.pose.position.y) * 0.5f;
                centerPose.position.z =
                    (left.pose.position.z + right.pose.position.z) * 0.5f;
                submittedViews[0].pose = centerPose;
                submittedViews[1].pose = centerPose;
            }

            if (!loggedFov_)
            {
                for (uint32_t eye = 0; eye < EyeCount; ++eye)
                {
                    std::ostringstream message;
                    message << std::fixed << std::setprecision(4)
                            << "XRHost: view[" << eye << "] fov="
                            << views[eye].fov.angleLeft << ','
                            << views[eye].fov.angleRight << ','
                            << views[eye].fov.angleDown << ','
                            << views[eye].fov.angleUp
                            << " pose=("
                            << views[eye].pose.position.x << ','
                            << views[eye].pose.position.y << ','
                            << views[eye].pose.position.z << ')';
                    logger_.write(message.str());
                }
                loggedFov_ = true;
            }

            if (quadPresentation)
            {
                if (quadPoseReady)
                {
                    if (!reusedGamePresentation)
                    {
                        const uint32_t contentWidth =
                            gameFrame_.contentWidth != 0u
                                ? gameFrame_.contentWidth
                                : gameFrame_.width;
                        const uint32_t contentHeight =
                            gameFrame_.contentHeight != 0u
                                ? gameFrame_.contentHeight
                                : gameFrame_.height;
                        const uint32_t sourceEye =
                            uiQuad ? gameFrame_.uiEye : 0u;
                        renderGameTexture(
                            uiSwapchain_,
                            gameFrame_.eyeViews[sourceEye],
                            contentWidth,
                            contentHeight,
                            false);
                        PreparedGamePresentation prepared {};
                        prepared.submittedViews = submittedViews;
                        prepared.uiPose = uiLayer.pose;
                        prepared.quadPresentation = true;
                        commitGamePresentation(
                            presentationKey,
                            prepared);
                    }
                    submittedLayer =
                        reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                            &uiLayer);
                }
            }
            else if (!gameMode_
                     || !gameFrame_
                     || !gameFrame_.requiresExactCapturePose()
                     || gameFramePoseMatched_)
            {
                if (!reusedGamePresentation)
                {
                    for (uint32_t eye = 0; eye < EyeCount; ++eye)
                    {
                        renderEye(
                            eye,
                            submittedViews[eye],
                            static_cast<float>(frameCounter_ % 36000) / 90.0f);
                    }
                    if (gameMode_ && gameFrame_)
                    {
                        PreparedGamePresentation prepared {};
                        prepared.submittedViews = submittedViews;
                        prepared.quadPresentation = false;
                        commitGamePresentation(
                            presentationKey,
                            prepared);
                    }
                }

                for (uint32_t eye = 0; eye < EyeCount; ++eye)
                {
                    projectionViews[eye].pose = submittedViews[eye].pose;
                    projectionViews[eye].fov = submittedViews[eye].fov;
                    projectionViews[eye].subImage.swapchain =
                        swapchains_[eye].handle;
                    projectionViews[eye].subImage.imageRect.offset = { 0, 0 };
                    projectionViews[eye].subImage.imageRect.extent = {
                        static_cast<int32_t>(swapchains_[eye].width),
                        static_cast<int32_t>(swapchains_[eye].height)
                    };
                    projectionViews[eye].subImage.imageArrayIndex = 0;
                }
                submittedLayer =
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                        &projectionLayer);
            }
            rendered = quadPresentation
                ? quadPoseReady
                : !gameMode_
                    || !gameFrame_
                    || !gameFrame_.requiresExactCapturePose()
                    || gameFramePoseMatched_;
        }

        XrFrameEndInfo endInfo { XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = environmentBlendMode_;
        endInfo.layerCount = rendered ? 1u : 0u;
        endInfo.layers = rendered ? &submittedLayer : nullptr;
        checkXr(xrEndFrame(session_, &endInfo), "xrEndFrame");
        ++frameCounter_;
        return rendered;
    }

    void renderGameTexture(
        ColorSwapchain& swapchain,
        ID3D11ShaderResourceView* sourceView,
        uint32_t contentWidth,
        uint32_t contentHeight,
        bool cropToFill)
    {
        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        checkXr(
            xrAcquireSwapchainImage(swapchain.handle, &acquireInfo, &imageIndex),
            "xrAcquireSwapchainImage(game)");

        waitForSwapchainImageReady(
            swapchain.handle,
            "xrWaitSwapchainImage(game)");

        D3D11_VIEWPORT viewport {};
        viewport.Width = static_cast<float>(swapchain.width);
        viewport.Height = static_cast<float>(swapchain.height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);

        ID3D11RenderTargetView* renderTarget =
            swapchain.renderTargetViews.at(imageIndex).Get();
        context_->OMSetRenderTargets(1, &renderTarget, nullptr);
        context_->RSSetState(rasterizerState_.Get());
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        context_->ClearRenderTargetView(renderTarget, black);
        if (gameFrame_ && sourceView)
        {
            GameConstants constants {};
            constants.sourceAndViewport[0] =
                static_cast<float>(
                    contentWidth != 0u
                        ? contentWidth
                        : gameFrame_.width);
            constants.sourceAndViewport[1] =
                static_cast<float>(
                    contentHeight != 0u
                        ? contentHeight
                        : gameFrame_.height);
            constants.sourceAndViewport[2] =
                static_cast<float>(swapchain.width);
            constants.sourceAndViewport[3] =
                static_cast<float>(swapchain.height);
            constants.presentation[0] = cropToFill ? 1.0f : 0.0f;
            context_->UpdateSubresource(
                gameConstantBuffer_.Get(),
                0,
                nullptr,
                &constants,
                0,
                0);

            context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
            context_->PSSetShader(gamePixelShader_.Get(), nullptr, 0);
            ID3D11Buffer* buffers[] = { gameConstantBuffer_.Get() };
            context_->PSSetConstantBuffers(1, 1, buffers);
            context_->PSSetShaderResources(0, 1, &sourceView);
            ID3D11SamplerState* samplers[] = { gameSampler_.Get() };
            context_->PSSetSamplers(0, 1, samplers);
            context_->Draw(3, 0);

            ID3D11ShaderResourceView* noView = nullptr;
            context_->PSSetShaderResources(0, 1, &noView);
        }

        XrSwapchainImageReleaseInfo releaseInfo {
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        checkXr(
            xrReleaseSwapchainImage(swapchain.handle, &releaseInfo),
            "xrReleaseSwapchainImage(game)");
    }

    void renderEye(uint32_t eye, const XrView& view, float timeSeconds)
    {
        ColorSwapchain& swapchain = swapchains_[eye];
        if (gameMode_)
        {
            const bool immersiveMono =
                gameFrame_.presentationMode
                    == gtaiv_xr_bridge::PresentationMode::WorldMono;
            const uint32_t sourceEye = immersiveMono ? 0u : eye;
            ID3D11ShaderResourceView* sourceView =
                gameFrame_ && gameFramePoseMatched_
                    ? gameFrame_.eyeViews[sourceEye]
                    : nullptr;
            const gtaiv_xr_host::GameTextureLayout layout = immersiveMono
                ? gtaiv_xr_host::GameTextureLayout {
                    gameFrame_.width,
                    gameFrame_.height }
                : gtaiv_xr_host::ResolveGameTextureLayout(
                    gameFrame_.presentationMode,
                    gameFrame_.width,
                    gameFrame_.height,
                    gameFrame_.contentWidth,
                    gameFrame_.contentHeight,
                    swapchain.width,
                    swapchain.height);
            renderGameTexture(
                swapchain,
                sourceView,
                layout.aspectWidth,
                layout.aspectHeight,
                immersiveMono);
            return;
        }

        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        checkXr(
            xrAcquireSwapchainImage(swapchain.handle, &acquireInfo, &imageIndex),
            "xrAcquireSwapchainImage");

        waitForSwapchainImageReady(
            swapchain.handle,
            "xrWaitSwapchainImage");

        const Vec3 right = rotate(view.pose.orientation, { 1.0f, 0.0f, 0.0f });
        const Vec3 up = rotate(view.pose.orientation, { 0.0f, 1.0f, 0.0f });
        const Vec3 back = rotate(view.pose.orientation, { 0.0f, 0.0f, 1.0f });

        EyeConstants constants {};
        constants.eyeOrigin[0] = view.pose.position.x;
        constants.eyeOrigin[1] = view.pose.position.y;
        constants.eyeOrigin[2] = view.pose.position.z;
        constants.eyeOrigin[3] = 1.0f;
        constants.eyeRight[0] = right.x;
        constants.eyeRight[1] = right.y;
        constants.eyeRight[2] = right.z;
        constants.eyeUp[0] = up.x;
        constants.eyeUp[1] = up.y;
        constants.eyeUp[2] = up.z;
        constants.eyeBack[0] = back.x;
        constants.eyeBack[1] = back.y;
        constants.eyeBack[2] = back.z;
        constants.tanAngles[0] = std::tan(view.fov.angleLeft);
        constants.tanAngles[1] = std::tan(view.fov.angleRight);
        constants.tanAngles[2] = std::tan(view.fov.angleDown);
        constants.tanAngles[3] = std::tan(view.fov.angleUp);
        constants.viewportAndTime[0] = static_cast<float>(swapchain.width);
        constants.viewportAndTime[1] = static_cast<float>(swapchain.height);
        constants.viewportAndTime[2] = timeSeconds;
        constants.viewportAndTime[3] = static_cast<float>(eye);
        if (eye == 0)
        {
            constants.accent[0] = 0.10f;
            constants.accent[1] = 0.82f;
            constants.accent[2] = 1.00f;
        }
        else
        {
            constants.accent[0] = 1.00f;
            constants.accent[1] = 0.28f;
            constants.accent[2] = 0.08f;
        }
        constants.accent[3] = 1.0f;

        context_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &constants, 0, 0);

        D3D11_VIEWPORT viewport {};
        viewport.Width = static_cast<float>(swapchain.width);
        viewport.Height = static_cast<float>(swapchain.height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);

        ID3D11RenderTargetView* renderTarget =
            swapchain.renderTargetViews.at(imageIndex).Get();
        context_->OMSetRenderTargets(1, &renderTarget, nullptr);
        context_->RSSetState(rasterizerState_.Get());
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        ID3D11Buffer* constantBuffers[] = { constantBuffer_.Get() };
        context_->PSSetConstantBuffers(0, 1, constantBuffers);
        context_->Draw(3, 0);

        XrSwapchainImageReleaseInfo releaseInfo { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        checkXr(
            xrReleaseSwapchainImage(swapchain.handle, &releaseInfo),
            "xrReleaseSwapchainImage");
    }

    void destroyInputActions() noexcept
    {
        for (XrSpace& actionSpace : gripSpaces_)
        {
            if (actionSpace != XR_NULL_HANDLE)
            {
                xrDestroySpace(actionSpace);
                actionSpace = XR_NULL_HANDLE;
            }
        }
        for (XrSpace& actionSpace : aimSpaces_)
        {
            if (actionSpace != XR_NULL_HANDLE)
            {
                xrDestroySpace(actionSpace);
                actionSpace = XR_NULL_HANDLE;
            }
        }
        const auto destroyAction = [](XrAction& action) {
            if (action != XR_NULL_HANDLE)
            {
                xrDestroyAction(action);
                action = XR_NULL_HANDLE;
            }
        };
        destroyAction(inputActions_.gripPose);
        destroyAction(inputActions_.aimPose);
        destroyAction(inputActions_.trigger);
        destroyAction(inputActions_.squeeze);
        destroyAction(inputActions_.thumbstick);
        destroyAction(inputActions_.primaryClick);
        destroyAction(inputActions_.secondaryClick);
        destroyAction(inputActions_.menuClick);
        destroyAction(inputActions_.thumbstickClick);
        destroyAction(inputActions_.primaryTouch);
        destroyAction(inputActions_.secondaryTouch);
        destroyAction(inputActions_.thumbstickTouch);
        destroyAction(inputActions_.thumbrestTouch);
        destroyAction(inputActions_.haptic);
        if (inputActionSet_ != XR_NULL_HANDLE)
        {
            xrDestroyActionSet(inputActionSet_);
            inputActionSet_ = XR_NULL_HANDLE;
        }
        hands_ = {};
        inputReady_ = false;
    }

    void shutdown() noexcept
    {
        if (context_)
        {
            context_->ClearState();
            context_->Flush();
        }
        gameFrame_ = {};
        gameBridge_.reset();
        poseBridge_.reset();
        stopAllHaptics();
        hapticBridge_.reset();

        const auto destroySwapchain = [](ColorSwapchain& swapchain) {
            swapchain.renderTargetViews.clear();
            swapchain.images.clear();
            if (swapchain.handle != XR_NULL_HANDLE)
            {
                xrDestroySwapchain(swapchain.handle);
                swapchain.handle = XR_NULL_HANDLE;
            }
            swapchain.width = 0u;
            swapchain.height = 0u;
        };
        for (ColorSwapchain& swapchain : swapchains_)
        {
            destroySwapchain(swapchain);
        }
        destroySwapchain(uiSwapchain_);

        constantBuffer_.Reset();
        gameConstantBuffer_.Reset();
        gameSampler_.Reset();
        gamePixelShader_.Reset();
        rasterizerState_.Reset();
        pixelShader_.Reset();
        vertexShader_.Reset();

        destroyInputActions();

        if (viewSpace_ != XR_NULL_HANDLE)
        {
            xrDestroySpace(viewSpace_);
            viewSpace_ = XR_NULL_HANDLE;
        }
        if (space_ != XR_NULL_HANDLE)
        {
            xrDestroySpace(space_);
            space_ = XR_NULL_HANDLE;
        }
        if (session_ != XR_NULL_HANDLE)
        {
            xrDestroySession(session_);
            session_ = XR_NULL_HANDLE;
        }

        context_.Reset();
        device_.Reset();
        adapter_.Reset();

        if (instance_ != XR_NULL_HANDLE)
        {
            xrDestroyInstance(instance_);
            instance_ = XR_NULL_HANDLE;
        }
    }

    Logger& logger_;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace space_ = XR_NULL_HANDLE;
    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    XrEnvironmentBlendMode environmentBlendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    PFN_xrGetD3D11GraphicsRequirementsKHR getD3D11GraphicsRequirements_ = nullptr;
    XrGraphicsRequirementsD3D11KHR graphicsRequirements_ {
        XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };

    ComPtr<IDXGIAdapter1> adapter_;
    DXGI_ADAPTER_DESC1 adapterDescription_ {};
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    D3D_FEATURE_LEVEL selectedFeatureLevel_ = D3D_FEATURE_LEVEL_11_0;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11PixelShader> gamePixelShader_;
    ComPtr<ID3D11Buffer> constantBuffer_;
    ComPtr<ID3D11Buffer> gameConstantBuffer_;
    ComPtr<ID3D11SamplerState> gameSampler_;
    ComPtr<ID3D11RasterizerState> rasterizerState_;
    gtaiv_xr_host::GameBridge gameBridge_;
    gtaiv_xr_host::GameFrameView gameFrame_;
    gtaiv_xr_host::HapticBridge hapticBridge_;
    gtaiv_xr_host::PoseBridgePublisher poseBridge_;
    gtaiv_xr_host::StationaryMenuQuadLatch menuQuadLatch_;
    gtaiv_xr_host::GamePresentationCache<PreparedGamePresentation>
        gamePresentationCache_;
    std::array<LocatedViewSample, 256u> locatedViewHistory_;

    std::vector<XrViewConfigurationView> viewConfigurationViews_;
    std::array<ColorSwapchain, EyeCount> swapchains_;
    ColorSwapchain uiSwapchain_;
    std::array<XrPath, EyeCount> hands_ {};
    std::array<XrSpace, EyeCount> gripSpaces_ {};
    std::array<XrSpace, EyeCount> aimSpaces_ {};
    InputActions inputActions_ {};
    XrActionSet inputActionSet_ = XR_NULL_HANDLE;
    uint64_t frameCounter_ = 0;
    uint64_t lastPoseMissLogTick_ = 0u;
    uint64_t lastHapticEpoch_ = 0u;
    uint64_t lastHapticCommandId_ = 0u;
    uint64_t lastHapticErrorLogTick_ = 0u;
    std::array<float, EyeCount> lastHapticAmplitudes_ {};
    std::array<bool, EyeCount> hapticActive_ {};
    uint32_t referenceSpaceGeneration_ = 1u;
    uint32_t recenterRequestId_ = 0u;
    XrTime referenceSpaceChangeTime_ = 0;
    bool referenceSpaceChangePending_ = false;
    bool sessionRunning_ = false;
    bool exitRequested_ = false;
    bool loggedFov_ = false;
    bool loggedInput_ = false;
    bool inputReady_ = false;
    bool inputSyncErrorLogged_ = false;
    bool recenterChordDown_ = false;
    bool gameFramePoseMatched_ = false;
    bool loggedGamePoseMatch_ = false;
    uint64_t lastTemporalPoseLogTransaction_ = 0u;
    uint64_t lastTemporalPoseMissLogTick_ = 0u;
    uint64_t lastParentDualPoseLogTransaction_ = 0u;
    uint64_t lastParentDualPoseMissLogTick_ = 0u;
    bool gameMode_ = false;
    bool allowTemporalStereo_ = false;
    gtaiv_xr_bridge::PresentationMode lastLoggedPresentationMode_ =
        gtaiv_xr_bridge::PresentationMode::Unknown;
};

uint64_t parsePositiveArgument(
    int argc,
    char** argv,
    const char* argument,
    uint64_t defaultValue)
{
    for (int index = 1; index < argc; ++index)
    {
        if (std::strcmp(argv[index], argument) == 0)
        {
            if (index + 1 >= argc)
                throw std::runtime_error(std::string(argument) + " requires a positive integer.");

            const std::string value = argv[++index];
            size_t parsed = 0;
            const uint64_t parsedValue = std::stoull(value, &parsed);
            if (parsed != value.size() || parsedValue == 0)
                throw std::runtime_error(std::string(argument) + " requires a positive integer.");
            return parsedValue;
        }
    }
    return defaultValue;
}

bool hasArgument(int argc, char** argv, const char* argument)
{
    for (int index = 1; index < argc; ++index)
    {
        if (std::strcmp(argv[index], argument) == 0)
            return true;
    }
    return false;
}
}

int main(int argc, char** argv)
{
    std::unique_ptr<Logger> logger;
    try
    {
        logger = std::make_unique<Logger>();
        logger->write(
            std::string("Build: host=") + GTAIV_XR_BUILD_ID
            + " machine=x64 sdk=OpenXR-1.1.61");
        logger->write("Log: " + logger->path().string());

        if (hasArgument(argc, argv, "--help"))
        {
            std::cout
                << "gtaiv_xr_host [--game] [--allow-temporal-stereo] "
                   "[--self-test] [--frames N] [--timeout-ms N]\n"
                << "  --game       Show live GTAIV.exe frames; black while the game starts.\n"
                << "  --allow-temporal-stereo\n"
                   "               Accept explicitly tagged ordered L/R GTA frame pairs.\n"
                << "  --self-test  Check shaders/protocol without touching OpenXR.\n"
                << "  --frames N   Exit after N submitted OpenXR frames.\n"
                << "  --timeout-ms Exit unsuccessfully if the test exceeds N milliseconds.\n";
            return 0;
        }

        if (hasArgument(argc, argv, "--self-test"))
        {
            compileShader("vsMain", "vs_5_0");
            compileShader("psMain", "ps_5_0");
            compileShader("psGame", "ps_5_0");
            if (!swapchainImageWaitSucceeded(XR_SUCCESS)
                || swapchainImageWaitSucceeded(XR_TIMEOUT_EXPIRED)
                || swapchainImageWaitSucceeded(
                    XR_ERROR_RUNTIME_FAILURE))
            {
                throw std::runtime_error(
                    "Swapchain wait result classifier self-test failed.");
            }
            std::string protocolFailure;
            if (!gtaiv_xr_host::GameBridgeProtocolSelfTest(protocolFailure))
            {
                throw std::runtime_error(
                    "Frame bridge protocol self-test failed: "
                    + protocolFailure);
            }
            std::string cpuMailboxFailure;
            if (!gtaiv_xr_host::GameBridgeCpuMailboxSelfTest(
                    cpuMailboxFailure))
            {
                throw std::runtime_error(
                    "CPU mailbox self-test failed: "
                    + cpuMailboxFailure);
            }
            std::string menuQuadFailure;
            if (!gtaiv_xr_host::StationaryMenuQuadPoseSelfTest(
                    menuQuadFailure))
            {
                throw std::runtime_error(
                    "Stationary menu quad self-test failed: "
                    + menuQuadFailure);
            }
            std::string textureLayoutFailure;
            if (!gtaiv_xr_host::GameTextureLayoutSelfTest(
                    textureLayoutFailure))
            {
                throw std::runtime_error(
                    "Game/menu texture routing self-test failed: "
                    + textureLayoutFailure);
            }
            std::string presentationCacheFailure;
            if (!gtaiv_xr_host::GamePresentationCacheSelfTest(
                    presentationCacheFailure))
            {
                throw std::runtime_error(
                    "Game presentation cache self-test failed: "
                    + presentationCacheFailure);
            }
            logger->write(
                "SelfTest: PASS pointerBits=64 shaders=ok protocol=v6 "
                "worldStrict=1 wvpProof=1 drawSceneProof=1 "
                "parentDualProof=1 firstPersonProof=1 headHideProof=1 "
                "temporalOptIn=1 immersiveMono=1 "
                "cpuMailbox=1 cpuMailboxStereo=1 cpuMailboxWorldUi=1 "
                "stationaryUiQuad=1 uiAspect=1 routeSwitch=1 "
                "heldFrameReuse=1 exactPoseCache=1 parentExactPose=1 "
                "strictSwapchainWait=1 referenceSpaceReset=1 "
                "srgbDecode=1 "
                "runtimeUntouched=1");
            return 0;
        }

        const uint64_t frameLimit =
            parsePositiveArgument(argc, argv, "--frames", 0);
        const uint64_t timeoutMilliseconds =
            parsePositiveArgument(argc, argv, "--timeout-ms", 0);
        SetConsoleCtrlHandler(consoleControlHandler, TRUE);

        const bool gameMode = hasArgument(argc, argv, "--game");
        const bool allowTemporalStereo =
            hasArgument(argc, argv, "--allow-temporal-stereo");
        if (allowTemporalStereo && !gameMode)
        {
            throw std::runtime_error(
                "--allow-temporal-stereo requires --game.");
        }
        logger->write(
            gameMode
                ? allowTemporalStereo
                    ? "Mode: GAME (temporal stereo explicitly enabled)"
                    : "Mode: GAME (strict same-tick stereo)"
                : "Mode: CALIBRATION");
        CalibrationHost host(
            *logger,
            gameMode,
            allowTemporalStereo);
        host.initialize();
        return host.run(frameLimit, timeoutMilliseconds);
    }
    catch (const std::exception& error)
    {
        if (logger)
            logger->write(std::string("FATAL: ") + error.what());
        std::cerr << "FATAL: " << error.what() << '\n';
        return 1;
    }
}
