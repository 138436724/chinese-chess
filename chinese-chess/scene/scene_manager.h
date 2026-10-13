#pragma once

#include "scene_base.h"
#include "scene_camera.h"
#include "scene_data_type.h"
#include "scene_handle.h"
#include "scene_light_manager.h"
#include "scene_material_manager.h"
#include "scene_model_manager.h"
#include "scene_texture_manager.h"
#include "vulkan_core/vulkan_commandbuffer.h"
#include "vulkan_core/vulkan_data_type.h"
#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

class vulkan_application;

enum class render_mode : uint32_t
{
    rasterization,
    ray_tracing,
    ray_query,
};

class scene_manager
{
public:
    scene_manager(vulkan_application& _app, uint32_t _width, uint32_t _height);
    ~scene_manager();

    void                                  resize(uint32_t _width, uint32_t _height);
    void                                  update();
    [[nodiscard]] vk::SemaphoreSubmitInfo render(bool _need_capture);
    void                                  handle(int _key, int _scancode, int _action, int _mods);

    void need_update() noexcept;
    void need_camera_update() noexcept;
    void need_light_update() noexcept;
    void need_material_update() noexcept;
    void need_model_update() noexcept;
    void need_environment_update() noexcept;

    [[nodiscard]] light_handle    create_light();
    [[nodiscard]] texture_handle  create_texture(const std::filesystem::path& _image_path, texture_usage _usage);
    [[nodiscard]] texture_handle  create_texture(const std::filesystem::path& _font_path,
                                                 uint32_t                     _font_size,
                                                 std::wstring_view            _characters,
                                                 uint32_t                     _padding,
                                                 const glm::vec3&             _foreground_color,
                                                 const glm::vec3&             _background_color);
    [[nodiscard]] material_handle create_material();
    [[nodiscard]] model_handle    create_model(const std::filesystem::path& _model_path);

    [[nodiscard]] scene_light_data*    get(light_handle _handle) noexcept;
    [[nodiscard]] scene_texture_data*  get(texture_handle _handle) noexcept;
    [[nodiscard]] scene_material_data* get(material_handle _handle) noexcept;
    [[nodiscard]] scene_model_data*    get(model_handle _handle) noexcept;

    void destroy(light_handle _handle) noexcept;
    void destroy(material_handle _handle) noexcept;
    void destroy(model_handle _handle) noexcept;

    void clear();

    void set_material_texture(material_handle _handle, texture_channel _channel, texture_handle _texture) noexcept;
    void set_environment_image(texture_handle _image) noexcept;
    void set_render_mode(render_mode _mode);

    [[nodiscard]] bool reload_texture(texture_handle _handle);

    [[nodiscard]] scene_environment_data& get_environment() noexcept;
    [[nodiscard]] bool                    get_need_update() const noexcept;
    [[nodiscard]] scene_camera&           get_active_camera() noexcept;
    [[nodiscard]] vulkan_image&           get_render_image() noexcept;


private:
    [[nodiscard]] vk::SemaphoreSubmitInfo capture_frame(const vk::SemaphoreSubmitInfo& _waited_info);

private:
    bool is_dirty               = true;
    bool need_descriptor_update = true;

    vulkan_application& app;

    scene_environment_data environment;

    vulkan_queue graphic_queue;
    vulkan_queue compute_queue;
    vulkan_queue transfer_queue;

    vulkan_semaphore                  semaphore;
    vulkan_recycle_bin                recycle_bin;
    std::vector<vulkan_commandbuffer> commandbuffers;

    uint32_t                             width         = 0;
    uint32_t                             height        = 0;
    uint32_t                             current_frame = 0;
    std::vector<vk::SemaphoreSubmitInfo> waited_infos;

    scene_light_manager    light_manager;
    scene_texture_manager  texture_manager;   // must before material
    scene_material_manager material_manager;  // must before model
    scene_model_manager    model_manager;

    scene_camera active_camera;

    static constexpr vk::Format color_format = vk::Format::eR16G16B16A16Sfloat;
    vulkan_image                render_output;

    std::unordered_map<render_mode, pro::proxy<manager_render>> scene_renders;
    pro::proxy_view<manager_render>                             active_render = nullptr;

    std::jthread save_thread;
};
