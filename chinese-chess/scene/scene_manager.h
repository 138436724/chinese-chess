#pragma once

#include "scene_base.h"
#include "scene_light_manager.h"
#include "scene_material_manager.h"
#include "scene_model_manager.h"
#include "vulkan_core/vulkan_commandbuffer.h"
#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"
#include "vulkan_core/vulkan_sampler.h"

#include <concepts>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

class vulkan_application;

// Ray Query 路径已于 2026 退役:它曾与 RT pipeline 各自实现同一套着色逻辑,
// 物理正确性改造必须同时改两遍,维护成本与静默分叉风险都不可接受。
// 现在物理正确性以光栅化(可视化)/ 光线追踪(唯一物理正确路径)二者为准。
enum class render_mode : uint32_t
{
    rasterization,
    ray_tracing,
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

    template <typename T, typename... Args>
        requires(std::same_as<T, scene_model> || std::same_as<T, scene_material> || std::same_as<T, scene_image>
                 || std::same_as<T, scene_light>)
    [[nodiscard]] auto create(Args&&... args);

    // 程序化 UV 球(白炉测试球体阵用):所有实例共用同一份网格。
    // 公开接口 —— 与模板 create<T> 同属"造对象"的入口。
    [[nodiscard]] std::shared_ptr<scene_model> create_procedural_sphere(float _radius,
                                                                       uint32_t _segments = 48u,
                                                                       uint32_t _rings    = 24u);

    void set_render_mode(render_mode _mode);

    // ---- 物理正确性调试通道(RT 渲染器专属;光栅化模式忽略) ----
    // 能量守恒验证必须能关闭全部启发式钳制,否则白炉测试测到的是钳制而非物理。
    //
    // 注意:三个 getter **不能加 const** —— 它们经 `active_render` 转发到
    // manager_render facade 的约定成员,而该约定声明为非 const
    // (`uint32_t()` 而非 `uint32_t() const`),const 成员函数里调用会因
    // "转换丢失限定符"编译失败。这与同文件的 get_active_camera()/
    // get_render_image() 保持一致的写法(它们同样是 proxy 转发)。
    void              set_debug_flags(uint32_t _flags) noexcept;
    [[nodiscard]] uint32_t get_debug_flags() noexcept;
    void              set_debug_mat_override(float _metallic, float _roughness) noexcept;
    [[nodiscard]] float get_debug_force_metallic() noexcept;
    [[nodiscard]] float get_debug_force_roughness() noexcept;

    [[nodiscard]] bool          get_need_update() const noexcept;
    [[nodiscard]] scene_camera& get_active_camera() noexcept;
    [[nodiscard]] vulkan_image& get_render_image() noexcept;

private:
    [[nodiscard]] std::shared_ptr<scene_light> create(std::type_identity<scene_light>);
    [[nodiscard]] std::shared_ptr<scene_model> create(std::type_identity<scene_model>, const std::filesystem::path& _model_name);
    [[nodiscard]] std::shared_ptr<scene_material> create(std::type_identity<scene_material>);
    [[nodiscard]] std::shared_ptr<scene_image>    create(std::type_identity<scene_image>,
                                                         const std::filesystem::path& _font_path,
                                                         uint32_t                     _font_size,
                                                         std::wstring_view            _characters,
                                                         uint32_t                     _padding);
    [[nodiscard]] std::shared_ptr<scene_image>    create(std::type_identity<scene_image>,
                                                         const std::filesystem::path& _image_path,
                                                         bool                         _is_hdr,
                                                         sampler_type                 _type);

    [[nodiscard]] vk::SemaphoreSubmitInfo capture_frame(const vk::SemaphoreSubmitInfo& _waited_info);

private:
    bool is_dirty        = true;
    bool is_render_dirty = true;

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

    static constexpr vk::Format color_format = vk::Format::eR16G16B16A16Sfloat;
    vulkan_image                render_output;

    std::unordered_map<render_mode, pro::proxy<manager_render>> scene_renders;
    pro::proxy_view<manager_render>                             active_render = nullptr;

    std::jthread save_thread;
};

template <typename T, typename... Args>
    requires(std::same_as<T, scene_model> || std::same_as<T, scene_material> || std::same_as<T, scene_image>
             || std::same_as<T, scene_light>)
inline auto scene_manager::create(Args&&... args)
{
    is_dirty = true;
    return create(std::type_identity<T>{}, std::forward<Args>(args)...);
}
