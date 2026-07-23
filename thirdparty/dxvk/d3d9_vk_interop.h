// DXVK v3.0.2 D3D9↔Vulkan interop (vtable order must match upstream).
// https://github.com/doitsujin/dxvk/blob/v3.0.2/src/d3d9/d3d9_interfaces.h
#pragma once

#include <d3d9.h>
#include <cstdint>

// Minimal Vulkan handle typedefs (no Vulkan SDK required for QI probe)
#if !defined(VK_DEFINE_HANDLE)
#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;
VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_HANDLE(VkImage)
typedef enum VkImageLayout {
  VK_IMAGE_LAYOUT_UNDEFINED = 0,
  VK_IMAGE_LAYOUT_GENERAL = 1,
  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2,
  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5,
  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6,
  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002,
} VkImageLayout;
typedef struct VkImageSubresourceRange {
  uint32_t aspectMask;
  uint32_t baseMipLevel;
  uint32_t levelCount;
  uint32_t baseArrayLayer;
  uint32_t layerCount;
} VkImageSubresourceRange;
typedef struct VkImageCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  int32_t imageType;
  int32_t format;
  struct { uint32_t width, height, depth; } extent;
  uint32_t mipLevels;
  uint32_t arrayLayers;
  int32_t samples;
  int32_t tiling;
  uint32_t usage;
  int32_t sharingMode;
  uint32_t queueFamilyIndexCount;
  const uint32_t* pQueueFamilyIndices;
  int32_t initialLayout;
} VkImageCreateInfo;
#endif

struct D3D9VkExtImageDesc;

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
