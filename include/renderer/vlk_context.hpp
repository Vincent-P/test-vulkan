#pragma once

#include <optional>
#include <vk_mem_alloc.h>

#include "types.hpp"
#include <vulkan/vulkan.hpp>

/***
 * VlkContext is a small abstraction over vulkan.
 * It is used by the HL API and the renderer to handle the recreation of the swapchain on resize
 ***/

namespace my_app
{

class Window;

namespace vulkan
{

constexpr inline auto ENABLE_VALIDATION_LAYERS = true;
constexpr inline auto FRAME_IN_FLIGHT = 1;

template <typename T> inline u64 get_raw_vulkan_handle(T const &cpp_handle)
{
    return u64(static_cast<typename T::CType>(cpp_handle));
}

template <typename T> inline auto get_c_vulkan_handle(T const &cpp_handle)
{
    return static_cast<typename T::CType>(cpp_handle);
}

struct SwapChain
{
    vk::UniqueSwapchainKHR handle;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> image_views;
    vk::SurfaceFormatKHR format;
    vk::PresentModeKHR present_mode;
    vk::Extent2D extent;
    u32 current_image;

    vk::Image get_current_image() { return images[current_image]; }
    vk::ImageView get_current_image_view() { return image_views[current_image]; }
};

struct FrameResource
{
    vk::UniqueFence fence;
    vk::UniqueSemaphore image_available;
    vk::UniqueSemaphore rendering_finished;
    vk::UniqueFramebuffer framebuffer;

    vk::UniqueCommandPool command_pool;
    vk::UniqueCommandBuffer command_buffer; // main command buffer
};

struct FrameResources
{
    std::vector<FrameResource> data;
    usize current;

    FrameResource &get_current() { return data[current]; }
};

struct Context
{
    vk::UniqueInstance instance;

    std::optional<vk::DebugUtilsMessengerEXT> debug_messenger;

    vk::UniqueSurfaceKHR surface;
    vk::PhysicalDevice physical_device;
    vk::PhysicalDeviceProperties physical_props;

    vk::PhysicalDeviceVulkan12Features vulkan12_features;
    vk::PhysicalDeviceFeatures2 physical_device_features;

    // gpu props?
    // surface caps?
    vk::UniqueDevice device;
    VmaAllocator allocator;

    u32 graphics_family_idx;
    u32 present_family_idx;

    vk::UniqueDescriptorPool descriptor_pool;

    SwapChain swapchain;
    FrameResources frame_resources;
    usize frame_count{0};
    usize descriptor_sets_count{0};

    // query pool for timestamps

    static Context create(const Window &window);
    void create_swapchain();
    void create_frame_resources(usize count = 1);
    void destroy_swapchain();
    void on_resize(int width, int height);
    void destroy();
};

} // namespace vulkan

} // namespace my_app
