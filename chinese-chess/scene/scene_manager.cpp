#include "scene_manager.h"

#include "scene_data_type.h"
#include "scene_rasterization_render.h"
#include "scene_rayquery_render.h"
#include "scene_raytracing_render.h"
#include "tools/image_helper.h"
#include "tools/ocio_helper.h"
#include "vulkan_core/vulkan_application.h"
#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_data_type.h"

#include <GLFW/glfw3.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

    const std::span<T> pixels(static_cast<T*>(_info.staging_buffer.get_buffer_address().hostAddress), _info.pixel_count);

    try
    {
        ocio_helper::apply_on_image<T>(_info.ocio_config_path, _info.width, _info.height, pixels);
        if (const auto result = image_helper::write_image<T>(_info.save_path, _info.width, _info.height, pixels); !result)
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
    , texture_manager(app.get_allocator(), *app.get_physical_device(), *app.get_device(), recycle_bin, semaphore, graphic_queue, transfer_queue)
    , material_manager(app.get_allocator(), *app.get_device(), recycle_bin, semaphore, graphic_queue, transfer_queue, texture_manager)
    , model_manager(app.get_allocator(), *app.get_physical_device(), *app.get_device(), recycle_bin, semaphore, graphic_queue, transfer_queue, material_manager)
{
    semaphore.create(*app.get_device());
    recycle_bin.create(&semaphore);

    graphic_queue.create(*app.get_device(), app.get_physical_device().get_queue_index(vk::QueueFlagBits::eGraphics));
    compute_queue.create(*app.get_device(), app.get_physical_device().get_queue_index(vk::QueueFlagBits::eCompute));
    transfer_queue.create(*app.get_device(), app.get_physical_device().get_queue_index(vk::QueueFlagBits::eTransfer));

    active_camera.set_position(glm::vec3(0.f, 0.f, 0.f));
    active_camera.set_direction(glm::vec3(0.f, 0.f, -1.f));
    active_camera.set_world_up(glm::vec3(0.f, 1.f, 0.f));


    commandbuffers = vulkan_commandbuffer::create(*app.get_device(),
                                                  vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(),
                                                                                vk::CommandBufferLevel::ePrimary,
                                                                                vulkan_common::MAX_FRAMES_IN_FLIGHT),
                                                  &graphic_queue, &semaphore);


    scene_renders.insert_or_assign(render_mode::rasterization,
                                   pro::make_proxy<manager_render, scene_rasterization_render>(
                                       app.get_allocator(), *app.get_physical_device(), *app.get_device(),
                                       *app.get_pipeline_cache(), recycle_bin, semaphore, graphic_queue, transfer_queue, model_manager,
                                       material_manager, light_manager, texture_manager, render_output, color_format));

    scene_renders.insert_or_assign(render_mode::ray_tracing,
                                   pro::make_proxy<manager_render, scene_raytracing_render>(
                                       app.get_allocator(), *app.get_physical_device(), *app.get_device(),
                                       *app.get_pipeline_cache(), recycle_bin, semaphore, graphic_queue, transfer_queue,
                                       model_manager, material_manager, light_manager, texture_manager, render_output));

    scene_renders.insert_or_assign(render_mode::ray_query,
                                   pro::make_proxy<manager_render, scene_rayquery_render>(
                                       app.get_allocator(), *app.get_physical_device(), *app.get_device(),
                                       *app.get_pipeline_cache(), recycle_bin, semaphore, graphic_queue, transfer_queue,
                                       model_manager, material_manager, light_manager, texture_manager, render_output));

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
    const vk::ImageCreateInfo render_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(width, height, 1), 1,
                                                1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
                                                vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
                                                    | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage,
                                                vk::SharingMode::eExclusive, queue_array);
    const vk::ImageViewCreateInfo render_view_info({}, {}, vk::ImageViewType::e2D, color_format, {},
                                                   vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
    render_output.create(app.get_allocator(), *app.get_device(), render_image_info, render_view_info,
                         vma::MemoryUsage::eAutoPreferDevice, {}, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f), "scene_render");

    active_render->resize(width, height);
    is_dirty               = true;
    need_descriptor_update = true;
}

void scene_manager::update()
{
    if (!is_dirty)
    {
        return;
    }

    const auto texture_result = texture_manager.update();
    if (texture_result.layout_moved)
    {
        material_manager.need_update();
    }

#ifndef NDEBUG
    bool dangling = false;
    for (const auto handle : material_manager.live_texture_references())
    {
        dangling = dangling || (handle.is_valid() && texture_manager.get(handle) == nullptr);
    }
    dangling = dangling || (environment.image.is_valid() && texture_manager.get(environment.image) == nullptr);

    if (dangling)
    {
        throw std::runtime_error("Texture handle must use retain() and release(), but somewhere not!");
    }
#endif  // !NDEBUG

    const bool material_changed = material_manager.update(waited_infos);
    const bool model_changed    = model_manager.update(waited_infos);
    const bool light_changed    = light_manager.update(waited_infos);

    if (texture_result.needs_redraw || material_changed || model_changed || light_changed || need_descriptor_update)
    {
        active_render->update();
    }
    else
    {
        active_render->reset_accumulation();
    }

    is_dirty               = false;
    need_descriptor_update = false;
}

vk::SemaphoreSubmitInfo scene_manager::render(bool _need_capture)
{
    recycle_bin.release();

    vulkan_commandbuffer& commandbuffer = commandbuffers.at(current_frame);
    commandbuffer.begin_record({});

    commandbuffer.add_waited_info(std::move(waited_infos));  // waited_infos clear here

    const vulkan_environment_data environment_data{.environment_index = texture_manager.get_texture_binding(environment.image),
                                                   .environment_intensity = environment.intensity};
    active_render->render(active_camera, *commandbuffer, environment_data);

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
            if (texture_manager.reload_all_textures(waited_infos))
            {
                need_material_update();
                need_model_update();
            }
            break;
        default:
            break;
    }
}

void scene_manager::need_update() noexcept
{
    is_dirty = true;

    light_manager.need_update();
    texture_manager.need_update();
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

void scene_manager::need_environment_update() noexcept
{
    is_dirty = true;
    texture_manager.need_update();
}

light_handle scene_manager::create_light()
{
    is_dirty = true;
    return light_manager.create();
}

texture_handle scene_manager::create_texture(const std::filesystem::path& _image_path, texture_usage _usage)
{
    is_dirty = true;
    return texture_manager.create(_image_path, _usage, waited_infos);
}

texture_handle scene_manager::create_texture(const std::filesystem::path& _font_path,
                                             uint32_t                     _font_size,
                                             std::wstring_view            _characters,
                                             uint32_t                     _padding,
                                             const glm::vec3&             _foreground_color,
                                             const glm::vec3&             _background_color)
{
    is_dirty = true;
    return texture_manager.create(_font_path, _font_size, _characters, _padding, _foreground_color, _background_color, waited_infos);
}

material_handle scene_manager::create_material()
{
    is_dirty = true;
    model_manager.need_update();
    return material_manager.create();
}

model_handle scene_manager::create_model(const std::filesystem::path& _model_path)
{
    is_dirty = true;
    return model_manager.create(_model_path);
}

scene_light_data* scene_manager::get(light_handle _handle) noexcept
{
    return light_manager.get(_handle);
}

scene_texture_data* scene_manager::get(texture_handle _handle) noexcept
{
    return texture_manager.get(_handle);
}

scene_material_data* scene_manager::get(material_handle _handle) noexcept
{
    return material_manager.get(_handle);
}

scene_model_data* scene_manager::get(model_handle _handle) noexcept
{
    return model_manager.get(_handle);
}

void scene_manager::destroy(light_handle _handle) noexcept
{
    if (light_manager.destroy(_handle))
    {
        is_dirty = true;
    }
}

void scene_manager::destroy(material_handle _handle) noexcept
{
    if (material_manager.destroy(_handle))
    {
        is_dirty = true;
        model_manager.need_update();
    }
}

void scene_manager::destroy(model_handle _handle) noexcept
{
    if (model_manager.destroy(_handle))
    {
        is_dirty = true;
    }
}

void scene_manager::clear()
{
    light_manager.clear();
    texture_manager.clear();
    material_manager.clear();
    model_manager.clear();

    environment            = scene_environment_data{};
    is_dirty               = true;
    need_descriptor_update = true;
}

void scene_manager::set_material_texture(material_handle _handle, texture_channel _channel, texture_handle _texture) noexcept
{
    is_dirty = true;
    material_manager.set_texture(_handle, _channel, _texture);
}

void scene_manager::set_environment_image(texture_handle _image) noexcept
{
    texture_manager.release(environment.image);

    environment.image = _image;
    texture_manager.retain(_image);

    need_environment_update();
}

void scene_manager::set_render_mode(render_mode _mode)
{
    active_render = scene_renders.at(_mode);

    // force resize and update
    active_render->resize(width, height);
    is_dirty               = true;
    need_descriptor_update = true;
}

bool scene_manager::reload_texture(texture_handle _handle)
{
    if (!texture_manager.reload_texture(_handle, waited_infos))
    {
        return false;
    }

    need_model_update();
    return true;
}

bool scene_manager::get_need_update() const noexcept
{
    return is_dirty;
}

scene_camera& scene_manager::get_active_camera() noexcept
{
    return active_camera;
}

vulkan_image& scene_manager::get_render_image() noexcept
{
    return render_output;
}

scene_environment_data& scene_manager::get_environment() noexcept
{
    return environment;
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
