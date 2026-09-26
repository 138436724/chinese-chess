#include "scene_manager.h"

#include "scene_rasterization_render.h"
#include "scene_raytracing_render.h"
#include "tools/image_helper.h"
#include "tools/ocio_helper.h"
#include "vulkan_core/vulkan_application.h"
#include "vulkan_core/vulkan_common.h"

#include <GLFW/glfw3.h>
#include <array>
#include <chrono>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vulkan/utility/vk_format_utils.h>

namespace {

struct image_save_info
{
    vulkan_buffer         staging_buffer;
    vulkan_semaphore*     semaphore  = nullptr;
    uint64_t              wait_value = 0;
    std::filesystem::path save_path;
    std::filesystem::path ocio_config_path;
    uint32_t              width       = 0;
    uint32_t              height      = 0;
    size_t                pixel_count = 0;
};

template <typename T>
void save_frame(image_save_info _info)
{
    _info.semaphore->wait(_info.wait_value);
    _info.staging_buffer.invalidate();

    std::span<T> pixels(static_cast<T*>(_info.staging_buffer.get_buffer_address().hostAddress), _info.pixel_count);

    try
    {
        ocio_helper::apply_on_image<T>(_info.ocio_config_path, _info.width, _info.height, pixels);
        if (auto result = image_helper::write_image<T>(_info.save_path, _info.width, _info.height, pixels); !result)
        {
            std::println(std::cerr, "Write image {} failed: {}", _info.save_path.generic_string(), result.error());
        }
    }
    catch (const std::exception& _exception)
    {
        std::println(std::cerr, "Save image {} failed: {}", _info.save_path.generic_string(), _exception.what());
    }
}

}  // namespace

scene_manager::~scene_manager()
{
    app.wait_idle();
}

scene_manager::scene_manager(vulkan_application& _app, uint32_t _width, uint32_t _height)
    : app(_app)
    , light_manager(app.get_allocator(), *app.get_device(), recycle_bin, semaphore, graphic_queue, transfer_queue)
    , material_manager(app.get_allocator(), *app.get_physical_device(), *app.get_device(), recycle_bin, semaphore, graphic_queue, transfer_queue)
    , model_manager(app.get_allocator(), *app.get_physical_device(), *app.get_device(), recycle_bin, semaphore, graphic_queue, transfer_queue, material_manager)
{
    semaphore.create(*app.get_device());
    recycle_bin.create(&semaphore);

    graphic_queue.create(*app.get_device(), app.get_physical_device().get_queue_index(vk::QueueFlagBits::eGraphics));
    compute_queue.create(*app.get_device(), app.get_physical_device().get_queue_index(vk::QueueFlagBits::eCompute));
    transfer_queue.create(*app.get_device(), app.get_physical_device().get_queue_index(vk::QueueFlagBits::eTransfer));

    managers.emplace_back(pro::make_proxy_view<manager_base>(material_manager));
    managers.emplace_back(pro::make_proxy_view<manager_base>(model_manager));
    managers.emplace_back(pro::make_proxy_view<manager_base>(light_manager));

    active_camera.set_position(glm::vec3(0.f, 0.f, 0.f));
    active_camera.set_direction(glm::vec3(0.f, 0.f, -1.f));
    active_camera.set_world_up(glm::vec3(0.f, 1.f, 0.f));


    commandbuffers = vulkan_commandbuffer::create(*app.get_device(),
                                                  vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(),
                                                                                vk::CommandBufferLevel::ePrimary,
                                                                                vulkan_common::MAX_FRAMES_IN_FLIGHT),
                                                  &graphic_queue, &semaphore);

    skybox_image = create(std::type_identity<scene_image>{},
                          std::filesystem::path(TEXTURES_PATH) / u8"cracked ground.hdr", true, sampler_type::sky_box);

    scene_renders.insert_or_assign(render_mode::rasterization,
                                   pro::make_proxy<manager_render, scene_rasterization_render>(
                                       app.get_allocator(), *app.get_physical_device(), *app.get_device(),
                                       *app.get_pipeline_cache(), recycle_bin, semaphore, graphic_queue, transfer_queue,
                                       model_manager, material_manager, light_manager, render_output, color_format));

    scene_renders.insert_or_assign(render_mode::ray_tracing,
                                   pro::make_proxy<manager_render, scene_raytracing_render>(
                                       app.get_allocator(), *app.get_physical_device(), *app.get_device(),
                                       *app.get_pipeline_cache(), recycle_bin, semaphore, graphic_queue, transfer_queue,
                                       model_manager, material_manager, light_manager, render_output));

    // default use raytracing pipeline
    active_render = scene_renders.at(render_mode::ray_tracing);

    resize(_width, _height);
}

void scene_manager::resize(uint32_t _width, uint32_t _height)
{
    width  = _width;
    height = _height;

    const std::array queue_array = {graphic_queue.get_index()};

    // recreate render_output
    recycle_bin.retire(std::move(render_output), "scene manager old render output.");
    vk::ImageCreateInfo render_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(width, height, 1), 1, 1,
                                          vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
                                          vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
                                              | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage,
                                          vk::SharingMode::eExclusive, queue_array);
    vk::ImageViewCreateInfo render_view_info({}, {}, vk::ImageViewType::e2D, color_format, {},
                                             vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
    render_output.create(app.get_allocator(), *app.get_device(), render_image_info, render_view_info,
                         vma::MemoryUsage::eAutoPreferDevice, {}, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f), "scene_render");

    active_render->resize(width, height);
    is_dirty        = true;
    is_render_dirty = true;
}

void scene_manager::update()
{
    if (!is_dirty)
    {
        return;
    }

    const bool any_dirty = std::ranges::fold_left(managers, false, [this](bool b, auto& manager) {
        return manager->update(waited_infos) || b;
    });

    if (any_dirty || is_render_dirty)
    {
        active_render->update();
    }
    else
    {
        active_render->reset_accumulation();
    }

    skybox_index = material_manager.get_texture_index(skybox_image).value_or(std::numeric_limits<uint32_t>::max());

    is_dirty        = false;
    is_render_dirty = false;
}

vk::SemaphoreSubmitInfo scene_manager::render(bool _need_capture)
{
    recycle_bin.release();

    vulkan_commandbuffer& commandbuffer = commandbuffers.at(current_frame);
    commandbuffer.begin_record({});

    commandbuffer.add_waited_info(std::move(waited_infos));  // waited_infos clear here

    active_render->render(active_camera, *commandbuffer, skybox_index);

    commandbuffer.end_record();
    commandbuffer.submit();

    auto submit_info = commandbuffer.get_submit_info();

    if (_need_capture)
    {
        submit_info = capture_frame(submit_info);
    }

    current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;

    return submit_info;
}

void scene_manager::handle(int _key, int /*_scancode*/, int _action, int /*_mods*/)
{
    if (_action != GLFW_PRESS)
    {
        return;
    }

    switch (_key)
    {
        case GLFW_KEY_F5:
            try
            {
                active_render->recreate();
                active_render->update();
            }
            catch (const std::exception& _error)
            {
                std::println(std::cerr, "recreate failed, keep pipeline: {}", _error.what());
            }
            break;
        case GLFW_KEY_F6:
            if (material_manager.reload_textures(waited_infos))
            {
                need_material_update();
                need_model_update();
            }
            break;
        case GLFW_KEY_J:
        {
            // 调试热键:切换环境光(辐照度 + 环境 NEE)。
            // 复用已有的 DEBUG_DISABLE_AMBIENT 位,不需要新增 proxy 约定;
            // 用途是量出环境光的性能开销(窗口标题里的 FPS 就是读数)。
            // 注意:本函数开头已过滤 `_action != GLFW_PRESS`,这里无需再判。
            const uint32_t ambient_bit = static_cast<uint32_t>(pbr_debug_flags::disable_ambient);
            const uint32_t flags       = get_debug_flags();
            set_debug_flags((flags & ambient_bit) != 0u ? (flags & ~ambient_bit) : (flags | ambient_bit));
            break;
        }
        default:
            break;
    }
}

void scene_manager::need_update() noexcept
{
    is_dirty = true;
    light_manager.need_update();
    material_manager.need_update();
    model_manager.need_update();
}

void scene_manager::need_camera_update() noexcept
{
    is_dirty = true;
}

void scene_manager::need_light_update() noexcept
{
    is_dirty = true;
    light_manager.need_update();
}

void scene_manager::need_material_update() noexcept
{
    is_dirty = true;
    material_manager.need_update();
}

void scene_manager::need_model_update() noexcept
{
    is_dirty = true;
    model_manager.need_update();
}

void scene_manager::set_render_mode(render_mode _mode)
{
    active_render = scene_renders.at(_mode);

    // force resize and update
    active_render->resize(width, height);
    is_dirty        = true;
    is_render_dirty = true;
}

bool scene_manager::get_need_update() const noexcept
{
    return is_dirty;
}

void scene_manager::set_debug_flags(uint32_t _flags) noexcept
{
    active_render->set_debug_flags(_flags);
    // 调试参数改变会改变成像结果 → 置脏,下一帧经 update() 重置 RT 时域累积,
    // 否则新旧画面会混叠,能量实验读到的不是干净结果。
    is_dirty = true;
}

// 三个 getter 不能加 const:active_render 是 proxy_view,其约定成员在 facade 里
// 声明为非 const,const 成员函数里调用会 "转换丢失限定符"。
uint32_t scene_manager::get_debug_flags() noexcept
{
    return active_render->get_debug_flags();
}

void scene_manager::set_debug_mat_override(float _metallic, float _roughness) noexcept
{
    active_render->set_debug_mat_override(_metallic, _roughness);
    is_dirty = true;
}

float scene_manager::get_debug_force_metallic() noexcept
{
    return active_render->get_debug_force_metallic();
}

float scene_manager::get_debug_force_roughness() noexcept
{
    return active_render->get_debug_force_roughness();
}

scene_camera& scene_manager::get_active_camera() noexcept
{
    return active_camera;
}

vulkan_image& scene_manager::get_render_image() noexcept
{
    return render_output;
}

std::shared_ptr<scene_light> scene_manager::create(std::type_identity<scene_light>)
{
    return light_manager.create();
}

std::shared_ptr<scene_model> scene_manager::create(std::type_identity<scene_model>, const std::filesystem::path& _model_name)
{
    return model_manager.create(_model_name);
}

std::shared_ptr<scene_model> scene_manager::create_procedural_sphere(float _radius, uint32_t _segments, uint32_t _rings)
{
    is_dirty = true;
    return model_manager.create_procedural_sphere(_radius, _segments, _rings);
}

std::shared_ptr<scene_material> scene_manager::create(std::type_identity<scene_material>)
{
    return material_manager.create();
}

std::shared_ptr<scene_image> scene_manager::create(std::type_identity<scene_image>,
                                                   const std::filesystem::path& _font_path,
                                                   uint32_t                     _font_size,
                                                   std::wstring_view            _characters,
                                                   uint32_t                     _padding)
{
    return material_manager.create(_font_path, _font_size, _characters, waited_infos, _padding);
}

std::shared_ptr<scene_image> scene_manager::create(std::type_identity<scene_image>,
                                                   const std::filesystem::path& _image_path,
                                                   bool                         _is_hdr,
                                                   sampler_type                 _type)
{
    return material_manager.create(_image_path, _is_hdr, _type, waited_infos);
}

vk::SemaphoreSubmitInfo scene_manager::capture_frame(const vk::SemaphoreSubmitInfo& _waited_info)
{
    const auto [image_width, image_height] = render_output.get_extent();
    const VkFormat image_format            = static_cast<VkFormat>(render_output.get_format());

    const bool is_float_half = vkuFormatIsSFLOAT(image_format) && vkuFormatIs16bit(image_format);
    const bool is_uint8_t    = vkuFormatIs8bit(image_format) && vkuFormatIsUINT(image_format);
    if (!is_float_half && !is_uint8_t)
    {
        throw std::runtime_error("Unsupported format!");
    }

    vulkan_buffer staging_buffer;
    const auto wait_value = vulkan_common::download_image(app.get_allocator(), *app.get_device(), recycle_bin, semaphore, graphic_queue,
                                                          transfer_queue, _waited_info, render_output, staging_buffer);

    const auto now        = std::chrono::system_clock::now();
    const auto now_second = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::seconds>(now));

    std::filesystem::path save_path = std::format("{}{:%Y_%m_%d_%H_%M_%S}", CAPTURES_PATH, now_second);
    save_path.replace_extension(is_float_half ? ".exr" : ".png");

    const std::string ocio_config_path = std::string(OCIOS_PATH) + "studio-config-all-views-v4.0.0_aces-v2.0_ocio-v2.5.ocio";

    const size_t pixel_count = static_cast<size_t>(image_width) * image_height * vkuFormatComponentCount(image_format);

    image_save_info info{.staging_buffer   = std::move(staging_buffer),
                         .semaphore        = &semaphore,
                         .wait_value       = wait_value.value,
                         .save_path        = save_path,
                         .ocio_config_path = ocio_config_path,
                         .width            = image_width,
                         .height           = image_height,
                         .pixel_count      = pixel_count};

    if (is_float_half)
    {
        save_thread = std::jthread([info = std::move(info)]() mutable { save_frame<half>(std::move(info)); });
    }
    else
    {
        save_thread = std::jthread([info = std::move(info)]() mutable { save_frame<uint8_t>(std::move(info)); });
    }

    return wait_value;
}
