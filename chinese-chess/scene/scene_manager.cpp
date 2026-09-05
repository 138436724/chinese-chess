#include "scene_manager.h"

#include "scene_rasterization_render.h"
#include "scene_rayquery_render.h"
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
#include <string>
#include <utility>
#include <vulkan/utility/vk_format_utils.h>

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

    scene_renders.insert_or_assign(render_mode::ray_query,
                                   pro::make_proxy<manager_render, scene_rayquery_render>(
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
                                              | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage /*for ray tracing*/,
                                          vk::SharingMode::eExclusive, queue_array);
    vk::ImageViewCreateInfo render_view_info({}, {}, vk::ImageViewType::e2D, color_format, {},
                                             vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
    render_output.create(app.get_allocator(), *app.get_device(), render_image_info, render_view_info,
                         vma::MemoryUsage::eGpuOnly, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f), "scene_render");

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

vk::SemaphoreSubmitInfo scene_manager::render()
{
    recycle_bin.release();

    vulkan_commandbuffer& commandbuffer = commandbuffers.at(current_frame);
    commandbuffer.begin_record({});

    commandbuffer.add_waited_info(std::move(waited_infos));  // waited_infos clear here

    active_render->render(active_camera, *commandbuffer, skybox_index);

    commandbuffer.end_record();
    commandbuffer.submit();

    current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;

    return commandbuffer.get_submit_info();
}

void scene_manager::handle(int _glfw_key)
{
    switch (_glfw_key)
    {
        case GLFW_KEY_F5:
            active_render->recreate();
            active_render->update();
            break;
        case GLFW_KEY_F6:
            if (material_manager.reload_textures(waited_infos))
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

void scene_manager::save_image()
{
    const auto [image_width, image_height] = render_output.get_extent();
    const VkFormat image_format            = static_cast<VkFormat>(render_output.get_format());

    vulkan_buffer staging_buffer;
    const auto wait_value = vulkan_common::download_image(app.get_allocator(), *app.get_device(), recycle_bin, semaphore,
                                                          graphic_queue, transfer_queue, render_output, staging_buffer);

    semaphore.wait(wait_value.value);

    staging_buffer.invalidate();

    const auto now        = std::chrono::system_clock::now();
    const auto now_second = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::seconds>(now));

    std::filesystem::path save_path = std::format("{}{:%Y_%m_%d_%H_%M_%S}", CAPTURES_PATH, now_second);

    const std::string ocio_config_path = std::string(OCIOS_PATH) + "studio-config-all-views-v3.0.0_aces-v2.0_ocio-v2.4.ocio";

    const size_t pixel_count = static_cast<size_t>(image_width) * image_height * vkuFormatComponentCount(image_format);
    if (vkuFormatIsSFLOAT(image_format) && vkuFormatIs16bit(image_format))
    {
        save_path.replace_extension(".exr");
        std::vector<half> image_data(static_cast<half*>(staging_buffer.get_buffer_address().hostAddress),
                                     static_cast<half*>(staging_buffer.get_buffer_address().hostAddress) + pixel_count);
        ocio_helper::apply_on_image<half>(ocio_config_path, image_width, image_height, image_data);
        if (auto result = image_helper::write_image<half>(save_path, image_width, image_height, image_data); !result)
        {
            std::println(std::cerr, "write image{} failed: {}", save_path.generic_string(), result.error());
        }
    }
    else if (vkuFormatIs8bit(image_format) && vkuFormatIsUINT(image_format))
    {
        save_path.replace_extension(".png");
        std::vector<uint8_t> image_data(static_cast<uint8_t*>(staging_buffer.get_buffer_address().hostAddress),
                                        static_cast<uint8_t*>(staging_buffer.get_buffer_address().hostAddress) + pixel_count);
        ocio_helper::apply_on_image<uint8_t>(ocio_config_path, image_width, image_height, image_data);
        if (auto result = image_helper::write_image<uint8_t>(save_path, image_width, image_height, image_data); !result)
        {
            std::println(std::cerr, "write_image{} failed: {}", save_path.generic_string(), result.error());
        }
    }
    else
    {
        throw std::runtime_error("Unsupported format!");
    }
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
