#pragma once

#include "scene_base.h"
#include "scene_light_manager.h"
#include "scene_material_manager.h"
#include "scene_model_manager.h"
#include "scene_rasterization_render.h"
#include "scene_raytracing_render.h"
#include "vulkan_core/vulkan_commandbuffer.h"
#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"
#include "vulkan_core/vulkan_sampler.h"

class vulkan_application;

class scene_manager
{
public:
    scene_manager(vulkan_application& _app, uint32_t _width, uint32_t _height);
    ~scene_manager() = default;

    void                                  resize(uint32_t _width, uint32_t _height);
    void                                  update();
    [[nodiscard]] vk::SemaphoreSubmitInfo render();
    void                                  destroy();
    void                                  handle(int _glfw_key);

    void need_update() noexcept;
    void need_camera_update() noexcept;
    void need_light_update() noexcept;
    void need_material_update() noexcept;
    void need_model_update() noexcept;

    void save_image();

    template <typename T, typename... Args>
        requires(std::same_as<T, scene_model> || std::same_as<T, scene_material> || std::same_as<T, scene_image>
                 || std::same_as<T, scene_light>)
    [[nodiscard]] auto create(Args&&... args);

    void set_use_ray_tracing(bool _use_ray_tracing);

    bool          get_need_update() const noexcept;
    scene_camera& get_active_camera() noexcept;
    vulkan_image& get_render_image() noexcept;

private:
    [[nodiscard]] std::shared_ptr<scene_light> create(std::type_identity<scene_light>);
    [[nodiscard]] std::shared_ptr<scene_model> create(std::type_identity<scene_model>, const std::filesystem::path& _model_name);
    [[nodiscard]] std::shared_ptr<scene_material> create(std::type_identity<scene_material>);
    [[nodiscard]] std::shared_ptr<scene_image>    create(std::type_identity<scene_image>,
                                                         const std::filesystem::path& _font_path,
                                                         uint32_t                     _font_size,
                                                         const std::wstring&          _characters);
    [[nodiscard]] std::shared_ptr<scene_image>    create(std::type_identity<scene_image>,
                                                         const std::filesystem::path& _image_path,
                                                         bool                         _is_hdr,
                                                         sampler_type                 _type);

private:
    bool is_dirty = true;

    uint32_t                     skybox_index = std::numeric_limits<uint32_t>::max();
    std::shared_ptr<scene_image> skybox_image = nullptr;

    vulkan_application& app;

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

    scene_light_manager                        light_manager;
    scene_material_manager                     material_manager;
    scene_model_manager                        model_manager;
    std::vector<pro::proxy_view<manager_base>> managers;

    scene_camera active_camera;

    const vk::Format color_format = vk::Format::eR16G16B16A16Sfloat;
    vulkan_image     render_output;

    std::unique_ptr<scene_rasterization_render> rasterization_render = nullptr;
    std::unique_ptr<scene_raytracing_render>    raytracing_render    = nullptr;
    pro::proxy_view<manager_render>             active_render        = nullptr;
};

template <typename T, typename... Args>
    requires(std::same_as<T, scene_model> || std::same_as<T, scene_material> || std::same_as<T, scene_image>
             || std::same_as<T, scene_light>)
inline auto scene_manager::create(Args&&... args)
{
    is_dirty = true;
    return create(std::type_identity<T>{}, std::forward<Args>(args)...);
}
