#pragma once

#include "scene_camera.h"
#include "scene_light_manager.h"
#include "scene_material_manager.h"
#include "scene_model_manager.h"
#include "scene_rasterization_render.h"
#include "scene_raytracing_render.h"
#include "vulkan_core/vulkan_recycle_bin.h"
#include "vulkan_core/vulkan_shader_binding_table.h"

class scene_manager
{
public:
    scene_manager(vulkan_application& _app, uint32_t _width, uint32_t _height);
    ~scene_manager() = default;

    void                                  resize(uint32_t _width, uint32_t _height);
    void                                  update() noexcept;
    [[nodiscard]] vk::SemaphoreSubmitInfo render();
    void                                  destroy() noexcept;
    void                                  handle(int _glfw_key) noexcept;

    void need_update() noexcept;

    void save_image();

    template <typename T, typename... Args>
        requires(std::same_as<T, scene_model> || std::same_as<T, scene_material> || std::same_as<T, scene_image>
                 || std::same_as<T, directional_light> || std::same_as<T, point_light> || std::same_as<T, spot_light>)
    [[nodiscard]] auto create(Args&&... args) noexcept;

    void set_use_ray_tracing(bool _use_ray_tracing) noexcept;

    scene_camera& get_active_camera() noexcept;
    vulkan_image& get_render_image() noexcept;

private:
    template <typename T>
        requires(std::same_as<T, directional_light> || std::same_as<T, point_light> || std::same_as<T, spot_light>)
    [[nodiscard]] std::shared_ptr<scene_light> create(std::type_identity<T>) noexcept;
    [[nodiscard]] std::shared_ptr<scene_model> create(std::type_identity<scene_model>, const std::filesystem::path& _model_name) noexcept;
    [[nodiscard]] std::shared_ptr<scene_material> create(std::type_identity<scene_material>) noexcept;
    [[nodiscard]] std::shared_ptr<scene_image>    create(std::type_identity<scene_image>,
                                                         const std::filesystem::path& _font_path,
                                                         uint32_t                     _font_size,
                                                         const std::wstring&          _characters) noexcept;
    [[nodiscard]] std::shared_ptr<scene_image>    create(std::type_identity<scene_image>,
                                                         const std::filesystem::path& _image_path,
                                                         bool                         _is_hdr) noexcept;

private:
    bool is_dirty = true;

    bool                         use_ray_tracing = true;
    uint32_t                     skybox_index    = std::numeric_limits<uint32_t>::max();
    std::shared_ptr<scene_image> skybox_image    = nullptr;

    vulkan_application& app;
    vulkan_recycle_bin  recycle_bin;
    vulkan_semaphore    semaphore;

    vulkan_queue                      graphic_queue;
    vulkan_queue                      compute_queue;
    vulkan_queue                      transfer_queue;
    std::vector<vulkan_commandbuffer> commandbuffers;

    uint32_t                             width         = 0;
    uint32_t                             height        = 0;
    uint32_t                             current_frame = 0;
    std::vector<vk::SemaphoreSubmitInfo> wait_infos;

    std::unique_ptr<scene_model_manager>    model_manager    = nullptr;
    std::unique_ptr<scene_material_manager> material_manager = nullptr;
    std::unique_ptr<scene_light_manager>    light_manager    = nullptr;

    scene_camera      active_camera;
    vk::raii::Sampler image_sampler = nullptr;

    const vk::Format                            color_format = vk::Format::eR16G16B16A16Sfloat;
    vulkan_image                                render_output;
    std::unique_ptr<scene_rasterization_render> rasterization_render = nullptr;
    std::unique_ptr<scene_raytracing_render>    raytracing_render    = nullptr;
};

template <typename T, typename... Args>
    requires(std::same_as<T, scene_model> || std::same_as<T, scene_material> || std::same_as<T, scene_image>
             || std::same_as<T, directional_light> || std::same_as<T, point_light> || std::same_as<T, spot_light>)
inline auto scene_manager::create(Args&&... args) noexcept
{
    is_dirty = true;
    return create(std::type_identity<T>{}, std::forward<Args>(args)...);
}

template <typename T>
    requires(std::same_as<T, directional_light> || std::same_as<T, point_light> || std::same_as<T, spot_light>)
inline std::shared_ptr<scene_light> scene_manager::create(std::type_identity<T>) noexcept
{
    return light_manager->create<T>();
}
