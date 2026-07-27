// DXVK v3.0.2 D3D9↔Vulkan interop (vtable order must match upstream).
// https://github.com/doitsujin/dxvk/blob/v3.0.2/src/d3d9/d3d9_interfaces.h
#pragma once

#include <d3d9.h>
#include <cstdint>

// Full Khronos definitions are required by the OpenXR sidecar transport.
// VK_NO_PROTOTYPES keeps the ASI independent of a Vulkan import library: all
// functions are resolved from the loader/device already used by DXVK.
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

// Exact DXVK 3.0.2 ABI:
// https://github.com/doitsujin/dxvk/blob/v3.0.2/src/d3d9/d3d9_interfaces.h
struct D3D9VkExtImageDesc {
  D3DRESOURCETYPE Type;
  UINT Width;
  UINT Height;
  UINT Depth;
  UINT MipLevels;
  DWORD Usage;
  D3DFORMAT Format;
  D3DPOOL Pool;
  D3DMULTISAMPLE_TYPE MultiSample;
  DWORD MultiSampleQuality;
  bool Discard;
  bool IsAttachmentOnly;
  bool IsLockable;
  VkImageUsageFlags ImageUsage;
};

MIDL_INTERFACE("d56344f5-8d35-46fd-806d-94c351b472c1")
ID3D9VkInteropTexture : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(
      VkImage* pHandle,
      VkImageLayout* pLayout,
      VkImageCreateInfo* pInfo) = 0;
};

MIDL_INTERFACE("2eaa4b89-0107-4bdb-87f7-0f541c493ce0")
ID3D9VkInteropDevice : public IUnknown {
  virtual void STDMETHODCALLTYPE GetVulkanHandles(
      VkInstance* pInstance,
      VkPhysicalDevice* pPhysDev,
      VkDevice* pDevice) = 0;

  virtual void STDMETHODCALLTYPE GetSubmissionQueue(
      VkQueue* pQueue,
      uint32_t* pQueueIndex,
      uint32_t* pQueueFamilyIndex) = 0;

  virtual void STDMETHODCALLTYPE TransitionTextureLayout(
      ID3D9VkInteropTexture* pTexture,
      const VkImageSubresourceRange* pSubresources,
      VkImageLayout OldLayout,
      VkImageLayout NewLayout) = 0;

  virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
  virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
  virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
  virtual void STDMETHODCALLTYPE LockDevice() = 0;
  virtual void STDMETHODCALLTYPE UnlockDevice() = 0;

  virtual bool STDMETHODCALLTYPE WaitForResource(
      IDirect3DResource9* pResource,
      DWORD MapFlags) = 0;

  virtual HRESULT STDMETHODCALLTYPE CreateImage(
      const D3D9VkExtImageDesc* desc,
      IDirect3DResource9** ppResult) = 0;
};

#ifdef _MSC_VER
struct __declspec(uuid("2eaa4b89-0107-4bdb-87f7-0f541c493ce0")) ID3D9VkInteropDevice;
struct __declspec(uuid("d56344f5-8d35-46fd-806d-94c351b472c1")) ID3D9VkInteropTexture;
#endif
