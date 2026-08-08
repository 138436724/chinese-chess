#pragma once

#include "vulkan_queue.h"

#include <vulkan/vulkan_raii.hpp>

class vulkan_swapchain
{
public:
    vulkan_swapchain()                                   = default;
    ~vulkan_swapchain()                                  = default;
    vulkan_swapchain(const vulkan_swapchain&)            = delete;
    vulkan_swapchain& operator=(const vulkan_swapchain&) = delete;
    vulkan_swapchain(vulkan_swapchain&& _other) noexcept;
    vulkan_swapchain& operator=(vulkan_swapchain&& _other) noexcept;

    void create(const vk::raii::Instance&       _instance,
                const vk::raii::PhysicalDevice& _physical_device,
                const vk::raii::Device&         _device,
                vk::SurfaceKHR                  _surface,
                uint32_t                        _present_index,
                uint32_t                        _width,
                uint32_t                        _height);
    void recreate(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, uint32_t _width, uint32_t _height);

    [[nodiscard]] bool acquire_image();
    void               present_image();

    [[nodiscard]] vk::Format                    get_format() const noexcept;
    [[nodiscard]] vk::Extent2D                  get_extent() const noexcept;
    [[nodiscard]] const vk::raii::SwapchainKHR& get_swapchain() const noexcept;
    [[nodiscard]] vk::Image                     get_current_image() const noexcept;
    [[nodiscard]] const vk::raii::ImageView&    get_current_imageview() const noexcept;
    [[nodiscard]] const vulkan_queue&           get_present_queue() const noexcept;
    [[nodiscard]] vk::SemaphoreSubmitInfo       get_waited_info() const noexcept;
    [[nodiscard]] vk::SemaphoreSubmitInfo       get_signal_info() const noexcept;

private:
    vk::SurfaceCapabilitiesKHR       surface_capabilities = {};
    vk::Format                       format               = vk::Format::eUndefined;
    vk::Extent2D                     extent               = vk::Extent2D{0, 0};
    vk::PresentModeKHR               present_mode         = vk::PresentModeKHR::eImmediate;
    vk::raii::SurfaceKHR             surface              = nullptr;
    vk::raii::SwapchainKHR           swapchain            = nullptr;
    std::vector<vk::Image>           images;
    std::vector<vk::raii::ImageView> imageviews;

    uint32_t                         current_index = 0;
    uint32_t                         current_frame = 0;
    vulkan_queue                     present_queue;
    std::vector<vk::raii::Semaphore> before_rendering;
    std::vector<vk::raii::Semaphore> after_rendering;
};
