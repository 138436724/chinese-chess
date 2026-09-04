#pragma once

#include "vulkan_buffer.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_descriptor.h"
#include "vulkan_device.h"
#include "vulkan_image.h"
#include "vulkan_physical_device.h"
#include "vulkan_pipeline.h"
#include "vulkan_pipeline_cache.h"
#include "vulkan_queue.h"
#include "vulkan_recycle_bin.h"
#include "vulkan_sampler.h"
#include "vulkan_semaphore.h"
#include "vulkan_swapchain.h"

#include <vector>
#include <vulkan/vulkan_raii.hpp>

class vulkan_application
{
public:
    vulkan_application() = default;
    ~vulkan_application();

    vulkan_application(const vulkan_application&)            = delete;
    vulkan_application& operator=(const vulkan_application&) = delete;
    vulkan_application(vulkan_application&&)                 = delete;
    vulkan_application& operator=(vulkan_application&&)      = delete;

    // public to use
    void init(const std::vector<const char*>& _instance_layers,
              const std::vector<const char*>& _instance_extensions,
              vk::InstanceCreateFlags         _flags = {});
    void create(vk::SurfaceKHR _surface, uint32_t _width, uint32_t _height);
    void resize(uint32_t _width, uint32_t _height);
    void render(const vk::SemaphoreSubmitInfo& _ui_waited_info, const vk::SemaphoreSubmitInfo& _scene_waited_info);
    void wait_frame() const;
    void wait_idle() const;

    // frame
    void bind_image(vulkan_image& _scene_image, vulkan_image& _ui_image);

    // getters
    [[nodiscard]] const vk::raii::Instance&     get_instance() const noexcept;
    [[nodiscard]] const vma::raii::Allocator&   get_allocator() const noexcept;
    [[nodiscard]] const vulkan_physical_device& get_physical_device() const noexcept;
    [[nodiscard]] const vulkan_device&          get_device() const noexcept;
    [[nodiscard]] const vulkan_pipeline_cache&  get_pipeline_cache() const noexcept;
    [[nodiscard]] const vulkan_swapchain&       get_swapchain() const noexcept;

private:
    // use in `init` and `create`
    void                   create_instance(const std::vector<const char*>& _instance_layers,
                                           const std::vector<const char*>& _instance_extensions,
                                           vk::InstanceCreateFlags         _flags = {});
    [[nodiscard]] uint32_t create_physical_device_and_device(vk::SurfaceKHR _surface);
    void                   create_pipeline();

    void pick_msaa_sample_count() const noexcept;
    void pick_depth_format() const;

    std::vector<const char*> required_instance_layers;
    std::vector<const char*> required_instance_extensions;

    vk::raii::Context                context;
    vk::raii::Instance               instance        = nullptr;
    vk::raii::DebugUtilsMessengerEXT debug_messenger = nullptr;

    vulkan_physical_device physical_device;
    vulkan_device          device;
    vma::raii::Allocator   allocator = nullptr;
    vulkan_pipeline_cache  pipeline_cache;

    uint32_t current_frame = 0;

    vulkan_queue graphic_queue;
    vulkan_queue transfer_queue;

    vulkan_swapchain swapchain;

    vulkan_pipeline   pipeline;
    vulkan_descriptor descriptor;

    vulkan_semaphore                  semaphore;
    vulkan_recycle_bin                recycle_bin;
    std::vector<vulkan_commandbuffer> commandbuffers;

    vulkan_buffer                  ocio_ubo;
    std::vector<vulkan_image>      ocio_images;
    std::vector<vk::raii::Sampler> ocio_samplers;

    vulkan_image*  bind_scene_image = nullptr;
    vulkan_image*  bind_ui_image    = nullptr;
    vulkan_sampler image_sampler;
};
