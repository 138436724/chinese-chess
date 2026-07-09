#pragma once
#define GLFW_INCLUDE_VULKAN

#include "ui_base.h"
#include "ui_camera.h"
#include "ui_light.h"
#include "ui_node.h"
#include "ui_record.h"
#include "vulkan_core/vulkan_application.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

class ui_manager
{
public:
    ui_manager(GLFWwindow* _window, vulkan_application& _app, scene_manager& _manager, uint32_t _width, uint32_t _height);
    ~ui_manager() = default;

    void                    resize(uint32_t _width, uint32_t _height);
    void                    update();
    vk::SemaphoreSubmitInfo render();
    void                    destroy();
    void                    handle(int _glfw_key);

    vulkan_image& get_render_image() noexcept;

private:
    void ray_tracing_ui() noexcept;
    bool use_ray_tracing = true;

private:
    vk::Format color_format = vk::Format::eUndefined;

    vulkan_application&      app;
    vulkan_recycle_bin&      recycle_bin;
    vulkan_queue             graphic_queue;
    vk::raii::DescriptorPool descriptor_pool = nullptr;

    ImDrawData* draw_data = nullptr;

    vulkan_image color_image;
    vulkan_image render_output;

    scene_manager&                   manager;
    std::vector<pro::proxy<ui_base>> ui_managers;

    std::vector<vulkan_commandbuffer> commandbuffers;
    uint32_t                          current_frame = 0;
};
