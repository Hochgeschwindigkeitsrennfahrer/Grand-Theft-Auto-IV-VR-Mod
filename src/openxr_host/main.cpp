#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>

#include "game_bridge.h"

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
constexpr uint32_t EyeCount = 2;
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
};

float4 psGame(VsOutput input) : SV_Target
{
    const float2 sourceSize = max(gameSourceAndViewport.xy, float2(1.0, 1.0));
    const float2 viewportSize = max(gameSourceAndViewport.zw, float2(1.0, 1.0));
    const float2 viewportUv = input.position.xy / viewportSize;
    const float sourceAspect = sourceSize.x / sourceSize.y;
    const float viewportAspect = viewportSize.x / viewportSize.y;

    float2 contentScale = float2(1.0, 1.0);
    if (viewportAspect > sourceAspect)
        contentScale.x = sourceAspect / viewportAspect;
    else
        contentScale.y = viewportAspect / sourceAspect;

    const float2 centered = viewportUv - 0.5;
    if (abs(centered.x) > contentScale.x * 0.5
        || abs(centered.y) > contentScale.y * 0.5)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float2 sourceUv = centered / contentScale + 0.5;
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
};
static_assert(sizeof(GameConstants) == 16, "GameConstants ABI changed");
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

struct EyeSwapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ComPtr<ID3D11RenderTargetView>> renderTargetViews;
};

class CalibrationHost
{
public:
    explicit CalibrationHost(Logger& logger, bool gameMode)
        : logger_(logger)
        , gameMode_(gameMode)
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
        createSwapchains();
        createRenderer();
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
                        << " Put on Quest 3 and launch Quest Link inside the headset.";
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
        logger_.write("XRHost: referenceSpace=LOCAL");
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
            EyeSwapchain& swapchain = swapchains_[eye];
            swapchain.format = selectedFormat;
            swapchain.width = viewConfigurationViews_[eye].recommendedImageRectWidth;
            swapchain.height = viewConfigurationViews_[eye].recommendedImageRectHeight;

            XrSwapchainCreateInfo createInfo { XR_TYPE_SWAPCHAIN_CREATE_INFO };
            createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            createInfo.format = selectedFormat;
            createInfo.sampleCount = 1;
            createInfo.width = swapchain.width;
            createInfo.height = swapchain.height;
            createInfo.faceCount = 1;
            createInfo.arraySize = 1;
            createInfo.mipCount = 1;
            checkXr(xrCreateSwapchain(session_, &createInfo, &swapchain.handle), "xrCreateSwapchain");

            uint32_t imageCount = 0;
            checkXr(
                xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr),
                "xrEnumerateSwapchainImages(count)");
            swapchain.images.assign(
                imageCount,
                XrSwapchainImageD3D11KHR { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
            checkXr(
                xrEnumerateSwapchainImages(
                    swapchain.handle,
                    imageCount,
                    &imageCount,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data())),
                "xrEnumerateSwapchainImages(list)");

            swapchain.renderTargetViews.reserve(imageCount);
            for (const XrSwapchainImageD3D11KHR& image : swapchain.images)
            {
                D3D11_RENDER_TARGET_VIEW_DESC viewDescription {};
                viewDescription.Format = static_cast<DXGI_FORMAT>(selectedFormat);
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
                        "CreateRenderTargetView failed: " + hresultString(result));
                }
                swapchain.renderTargetViews.push_back(renderTargetView);
            }

            std::ostringstream message;
            message << "XRHost: view[" << eye << "] recommended="
                    << swapchain.width << 'x' << swapchain.height
                    << " samples=" << viewConfigurationViews_[eye].recommendedSwapchainSampleCount
                    << " swapchainImages=" << imageCount;
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
                });
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
                    logger_.write("XRHost: session stopped");
                }
                else if (sessionState_ == XR_SESSION_STATE_EXITING
                         || sessionState_ == XR_SESSION_STATE_LOSS_PENDING)
                {
                    exitRequested_ = true;
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

    bool renderFrame()
    {
        XrFrameWaitInfo waitInfo { XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState frameState { XR_TYPE_FRAME_STATE };
        checkXr(xrWaitFrame(session_, &waitInfo, &frameState), "xrWaitFrame");

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

        std::array<XrCompositionLayerProjectionView, EyeCount> projectionViews {
            XrCompositionLayerProjectionView {
                XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
            XrCompositionLayerProjectionView {
                XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }
        };

        bool rendered = false;
        if (viewsValid)
        {
            if (gameMode_)
                gameFrame_ = gameBridge_.update();

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

            for (uint32_t eye = 0; eye < EyeCount; ++eye)
            {
                renderEye(
                    eye,
                    views[eye],
                    static_cast<float>(frameCounter_ % 36000) / 90.0f);

                projectionViews[eye].pose = views[eye].pose;
                projectionViews[eye].fov = views[eye].fov;
                projectionViews[eye].subImage.swapchain = swapchains_[eye].handle;
                projectionViews[eye].subImage.imageRect.offset = { 0, 0 };
                projectionViews[eye].subImage.imageRect.extent = {
                    static_cast<int32_t>(swapchains_[eye].width),
                    static_cast<int32_t>(swapchains_[eye].height)
                };
                projectionViews[eye].subImage.imageArrayIndex = 0;
            }
            rendered = true;
        }

        XrCompositionLayerProjection projectionLayer {
            XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        projectionLayer.space = space_;
        projectionLayer.viewCount = EyeCount;
        projectionLayer.views = projectionViews.data();

        const XrCompositionLayerBaseHeader* layers[] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer)
        };
        XrFrameEndInfo endInfo { XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = environmentBlendMode_;
        endInfo.layerCount = rendered ? 1u : 0u;
        endInfo.layers = rendered ? layers : nullptr;
        checkXr(xrEndFrame(session_, &endInfo), "xrEndFrame");
        ++frameCounter_;
        return rendered;
    }

    void renderEye(uint32_t eye, const XrView& view, float timeSeconds)
    {
        EyeSwapchain& swapchain = swapchains_[eye];

        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        checkXr(
            xrAcquireSwapchainImage(swapchain.handle, &acquireInfo, &imageIndex),
            "xrAcquireSwapchainImage");

        XrSwapchainImageWaitInfo waitInfo { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = SwapchainWaitTimeout;
        checkXr(xrWaitSwapchainImage(swapchain.handle, &waitInfo), "xrWaitSwapchainImage");

        if (gameMode_)
        {
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
            if (gameFrame_)
            {
                GameConstants constants {};
                constants.sourceAndViewport[0] =
                    static_cast<float>(gameFrame_.width);
                constants.sourceAndViewport[1] =
                    static_cast<float>(gameFrame_.height);
                constants.sourceAndViewport[2] =
                    static_cast<float>(swapchain.width);
                constants.sourceAndViewport[3] =
                    static_cast<float>(swapchain.height);
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
                ID3D11ShaderResourceView* views[] = {
                    gameFrame_.shaderView
                };
                context_->PSSetShaderResources(0, 1, views);
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
                "xrReleaseSwapchainImage");
            return;
        }

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

    void shutdown() noexcept
    {
        if (context_)
        {
            context_->ClearState();
            context_->Flush();
        }
        gameFrame_ = {};
        gameBridge_.reset();

        for (EyeSwapchain& swapchain : swapchains_)
        {
            swapchain.renderTargetViews.clear();
            swapchain.images.clear();
            if (swapchain.handle != XR_NULL_HANDLE)
            {
                xrDestroySwapchain(swapchain.handle);
                swapchain.handle = XR_NULL_HANDLE;
            }
        }

        constantBuffer_.Reset();
        gameConstantBuffer_.Reset();
        gameSampler_.Reset();
        gamePixelShader_.Reset();
        rasterizerState_.Reset();
        pixelShader_.Reset();
        vertexShader_.Reset();

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

    std::vector<XrViewConfigurationView> viewConfigurationViews_;
    std::array<EyeSwapchain, EyeCount> swapchains_;
    uint64_t frameCounter_ = 0;
    bool sessionRunning_ = false;
    bool exitRequested_ = false;
    bool loggedFov_ = false;
    bool gameMode_ = false;
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
                << "gtaiv_xr_host [--game] [--self-test] [--frames N] [--timeout-ms N]\n"
                << "  --game       Show live GTAIV.exe frames; black while the game starts.\n"
                << "  --self-test  Compile all shaders without touching OpenXR.\n"
                << "  --frames N   Exit after N submitted OpenXR frames.\n"
                << "  --timeout-ms Exit unsuccessfully if the test exceeds N milliseconds.\n";
            return 0;
        }

        if (hasArgument(argc, argv, "--self-test"))
        {
            compileShader("vsMain", "vs_5_0");
            compileShader("psMain", "ps_5_0");
            compileShader("psGame", "ps_5_0");
            logger->write("SelfTest: PASS pointerBits=64 shaders=ok runtimeUntouched=1");
            return 0;
        }

        const uint64_t frameLimit =
            parsePositiveArgument(argc, argv, "--frames", 0);
        const uint64_t timeoutMilliseconds =
            parsePositiveArgument(argc, argv, "--timeout-ms", 0);
        SetConsoleCtrlHandler(consoleControlHandler, TRUE);

        const bool gameMode = hasArgument(argc, argv, "--game");
        logger->write(gameMode ? "Mode: GAME" : "Mode: CALIBRATION");
        CalibrationHost host(*logger, gameMode);
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
