#pragma once

#include "scene/scene_manager.h"
#include "ui_base.h"
#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"
#include "vulkan_core/vulkan_semaphore.h"

#include <GLFW/glfw3.h>
#include <vector>

class vulkan_application;
class vulkan_commandbuffer;
struct ImDrawData;

class ui_manager
{
public:
    ui_manager(GLFWwindow* _window, vulkan_application& _app, scene_manager& _manager, uint32_t _width, uint32_t _height);
    ~ui_manager();

    void                                  resize(uint32_t _width, uint32_t _height);
    void                                  update();
    [[nodiscard]] vk::SemaphoreSubmitInfo render();
    void                                  handle(int _glfw_key) noexcept;

    [[nodiscard]] vulkan_image& get_render_image() noexcept;

private:
    void render_mode_ui() noexcept;

private:
    vk::Format color_format = vk::Format::eUndefined;

    vulkan_application&      app;
    vulkan_semaphore         semaphore;
    vulkan_recycle_bin       recycle_bin;
    vulkan_queue             graphic_queue;
    vk::raii::DescriptorPool descriptor_pool = nullptr;

    ImDrawData* draw_data = nullptr;

    vulkan_image color_image;
    vulkan_image render_output;

    scene_manager&                   manager;
    std::vector<pro::proxy<ui_base>> ui_managers;
    int                              current_mode = static_cast<int>(render_mode::ray_tracing);

    std::vector<vulkan_commandbuffer> commandbuffers;
    uint32_t                          current_frame = 0;
};
