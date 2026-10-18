/**
 * @file scene_manager.cpp
 * @brief 场景编排器的实现：初始化顺序、脏门控更新、命令录制与异步截图。
 * @details 构造函数按 texture -> material -> model 的依赖顺序建 manager，再建三套渲染器；
 *          update() 先重抛后台截图异常，再依次 update 四个 manager，据其结果决定重建渲染器资源还是只清累积；
 *          render() 录制本帧命令、合并 waited_infos 依赖并提交，_need_capture 时接上回读链。
 *          截图回读与写盘分离：主线程只发起 download_image 并保存 token，写盘在 jthread 里完成。
 */
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
#include "vulkan_core/vulkan_submission_scope.h"

#include <GLFW/glfw3.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vulkan/utility/vk_format_utils.h>

namespace {

/**
 * @brief 一次截图回读的上下文：staging 缓冲、完成 token 与写盘参数。
 * @details 由主线程构造并交给后台线程；析构时 wait_for_cleanup 排空 token，确保缓冲不被提前释放。
 */
struct image_save_info
{
    vulkan_buffer         staging_buffer;
    vulkan_submission_token      token;
    std::filesystem::path save_path;
    std::filesystem::path ocio_config_path;
    uint32_t              width       = 0;
    uint32_t              height      = 0;
    size_t                pixel_count = 0;

    ~image_save_info() { token.wait_for_cleanup(); }
};

/**
 * @brief 后台线程主体：等 token、invalidate 映射内存、做 OCIO 变换并写盘。
 * @tparam T 像素类型，16 位浮点写 EXR，8 位无符号整数写 PNG。
 * @note 除等待 token 与 invalidate 外的异常都在本函数内打印，不再向上抛。
 */
template <typename T>
void save_frame(image_save_info& _info)
{
    _info.token.semaphore->wait(_info.token.value);
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

/**
 * @brief 析构前先做设备级等待，确保在飞命令与后台截图都结束。
 * @note save_thread 的 join 发生在成员析构阶段，晚于这里的 wait_idle_for_cleanup。
 */
scene_manager::~scene_manager()
{
    app.wait_idle_for_cleanup();
}

/**
 * @brief 按依赖顺序初始化四个 manager 与三套渲染器，注册回收站队列并建输出图像。
 * @note 成员初始化列表决定 manager 的构造顺序：texture 先于 material、material 先于 model。
 * @note failure_scope 覆盖构造体内的工作（三套渲染器、命令缓冲、输出图像）；失败时先停提交再排空已发布 token。
 */
scene_manager::scene_manager(vulkan_application& _app, uint32_t _width, uint32_t _height)
    : app(_app)
    , graphic_queue(app.get_graphic_queue())
    , transfer_queue(app.get_transfer_queue())
    , light_manager(app.get_allocator(), *app.get_device(), recycle_bin, graphic_queue, transfer_queue)
    , texture_manager(app.get_allocator(), *app.get_physical_device(), *app.get_device(), recycle_bin, graphic_queue, transfer_queue)
    , material_manager(app.get_allocator(), *app.get_device(), recycle_bin, graphic_queue, transfer_queue, texture_manager)
    , model_manager(app.get_allocator(), *app.get_physical_device(), *app.get_device(), recycle_bin, graphic_queue, transfer_queue, material_manager)
{
    const vulkan_submission_scope failure_scope(graphic_queue, transfer_queue);
    // The application owns the actual VkQueue objects; scene work shares their
    // completion timeline so tokens from both owners are directly comparable.
    recycle_bin.add_queue(graphic_queue);
    recycle_bin.add_queue(transfer_queue);

    active_camera.set_position(glm::vec3(0.f, 0.f, 0.f));
    active_camera.set_direction(glm::vec3(0.f, 0.f, -1.f));
    active_camera.set_world_up(glm::vec3(0.f, 1.f, 0.f));


    commandbuffers = vulkan_commandbuffer::create(*app.get_device(),
                                                  vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(),
                                                                                vk::CommandBufferLevel::ePrimary,
                                                                                vulkan_common::MAX_FRAMES_IN_FLIGHT),
                                                  &graphic_queue);


    scene_renders.insert_or_assign(render_mode::rasterization,
                                   pro::make_proxy<manager_render, scene_rasterization_render>(
                                       app.get_allocator(), *app.get_physical_device(), *app.get_device(),
                                       *app.get_pipeline_cache(), recycle_bin, graphic_queue, transfer_queue, model_manager,
                                       material_manager, light_manager, texture_manager, render_output, color_format));

    scene_renders.insert_or_assign(render_mode::ray_tracing,
                                   pro::make_proxy<manager_render, scene_raytracing_render>(
                                       app.get_allocator(), *app.get_physical_device(), *app.get_device(),
                                       *app.get_pipeline_cache(), recycle_bin, graphic_queue, transfer_queue,
                                       model_manager, material_manager, light_manager, texture_manager, render_output));

    scene_renders.insert_or_assign(render_mode::ray_query,
                                   pro::make_proxy<manager_render, scene_rayquery_render>(
                                       app.get_allocator(), *app.get_physical_device(), *app.get_device(),
                                       *app.get_pipeline_cache(), recycle_bin, graphic_queue, transfer_queue,
                                       model_manager, material_manager, light_manager, texture_manager, render_output));

    // default use raytracing pipeline
    active_render = scene_renders.at(render_mode::ray_tracing);

    resize(_width, _height);
}

/**
 * @brief 退休旧输出图像，按新尺寸重建 render_output，再调整活跃渲染器。
 * @note 只 resize 当前活跃渲染器；另外两个在切过去时靠 need_descriptor_update 重写描述符。
 * @note 新图像固定 eColorAttachment / eSampled / eTransferSrc / eStorage，供渲染、合成与截图共用。
 */
void scene_manager::resize(uint32_t _width, uint32_t _height)
{
    width  = _width;
    height = _height;

    const std::array queue_array = {graphic_queue.get_index()};

    // recreate render_output
    recycle_bin.retire_after_submitted(std::move(render_output), "scene manager old render output.");
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

/**
 * @brief 脏门控更新：先重抛后台截图异常，再按序更新四个 manager 并决定渲染器动作。
 * @note 贴图 layout_moved 会连带置材质脏；相机-only 变化走 reset_accumulation，不重写描述符。
 * @note 任何 manager 报告变化或 need_descriptor_update 置位时调 active_render->update()。
 */
void scene_manager::update()
{
    {
        const std::lock_guard lock(save_error_mutex);
        if (save_error) std::rethrow_exception(save_error);
    }
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

/**
 * @brief 录制本帧场景命令并提交，必要时接上截图回读链。
 * @note 开头的 update() 覆盖本帧稍后切换的渲染模式；waited_infos 在本帧转成依赖后清空。
 * @note 回读会把返回 token 换成回读末条命令的 token，合成因此要等整条回读完成。
 */
vulkan_submission_token scene_manager::render(uint32_t frame_slot, bool _need_capture)
{
    // Also prepares a mode selected after an earlier update in this frame.
    update();
    recycle_bin.release();

    vulkan_commandbuffer& commandbuffer = commandbuffers.at(frame_slot);
    commandbuffer.begin_record({});

    commandbuffer.add_dependency(waited_infos);
    waited_infos.clear();

    const vulkan_environment_data environment_data{.environment_index = texture_manager.get_texture_binding(environment.image),
                                                   .environment_intensity = environment.intensity};
    active_render->render(active_camera, *commandbuffer, environment_data, frame_slot);

    commandbuffer.end_record();
    auto token = commandbuffer.submit();

    if (_need_capture)
    {
        token = capture_frame(token);
    }

    return token;
}

/**
 * @brief 处理窗口按键：F5 重建活跃管线、F6 重载全部贴图。
 * @note 非 GLFW_PRESS 一律忽略；F5 的异常只打印，旧管线继续可用。
 */
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

/**
 * @brief 置全脏：场景标志与四个 manager 的脏标志一起置位。
 * @note 下一帧会重传全部 SSBO 并重建描述符。
 */
void scene_manager::need_update() noexcept
{
    is_dirty = true;

    light_manager.need_update();
    texture_manager.need_update();
    material_manager.need_update();
    model_manager.need_update();
}

/**
 * @brief 只置场景脏，不碰任何 manager。
 * @note 相机矩阵每帧直接写 UBO，所以相机变化不需要重传场景数据或重写描述符。
 */
void scene_manager::need_camera_update() noexcept
{
    is_dirty = true;
}

/**
 * @brief 置场景脏与灯光脏，下一帧重传灯光 SSBO。
 */
void scene_manager::need_light_update() noexcept
{
    is_dirty = true;
    light_manager.need_update();
}

/**
 * @brief 置场景脏与材质脏，下一帧重排 binding 并重传材质 SSBO。
 */
void scene_manager::need_material_update() noexcept
{
    is_dirty = true;
    material_manager.need_update();
}

/**
 * @brief 置场景脏与模型脏，下一帧重建实例缓冲与 TLAS。
 */
void scene_manager::need_model_update() noexcept
{
    is_dirty = true;
    model_manager.need_update();
}

/**
 * @brief 置场景脏与贴图脏。
 * @note 环境强度每帧在 render 里直接进入 GPU 数据，这里只让贴图管理器重算紧凑 binding。
 */
void scene_manager::need_environment_update() noexcept
{
    is_dirty = true;
    texture_manager.need_update();
}

/**
 * @brief 新建默认灯，置场景脏并返回句柄。
 */
light_handle scene_manager::create_light()
{
    is_dirty = true;
    return light_manager.create();
}

/**
 * @brief 从图片文件建贴图，失败时返回无效句柄。
 * @note 上传 token 追加进 waited_infos，下一次 render 时转成命令依赖。
 */
texture_handle scene_manager::create_texture(const std::filesystem::path& _image_path, texture_usage _usage)
{
    is_dirty = true;
    return texture_manager.create(_image_path, _usage, waited_infos);
}

/**
 * @brief 用字体与前景/背景色合成字形图集贴图，失败时返回无效句柄。
 * @note 图集按覆盖度合成后展开成 RGBA，属于普通 LDR 贴图；上传 token 同样进 waited_infos。
 */
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

/**
 * @brief 新建默认材质，同时置模型脏以覆盖紧凑 binding 的变化。
 */
material_handle scene_manager::create_material()
{
    is_dirty = true;
    model_manager.need_update();
    return material_manager.create();
}

/**
 * @brief 从模型文件建实例，网格按路径在 model_manager 内复用。
 * @note 加载失败时 model_manager 返回无效句柄并打印错误，这里原样透传。
 */
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

/**
 * @brief 销毁灯，成功时置场景脏。
 */
void scene_manager::destroy(light_handle _handle) noexcept
{
    if (light_manager.destroy(_handle))
    {
        is_dirty = true;
    }
}

/**
 * @brief 销毁材质，成功时置场景与模型脏（后续材质的紧凑 binding 会前移）。
 */
void scene_manager::destroy(material_handle _handle) noexcept
{
    if (material_manager.destroy(_handle))
    {
        is_dirty = true;
        model_manager.need_update();
    }
}

/**
 * @brief 销毁模型实例，成功时置场景脏；网格由引用计数决定是否回收。
 */
void scene_manager::destroy(model_handle _handle) noexcept
{
    if (model_manager.destroy(_handle))
    {
        is_dirty = true;
    }
}

/**
 * @brief 转发贴图通道设置，并置场景脏。
 * @note 引用计数的增减全部发生在 material_manager::set_texture 内。
 */
void scene_manager::set_material_texture(material_handle _handle, texture_channel _channel, texture_handle _texture) noexcept
{
    is_dirty = true;
    material_manager.set_texture(_handle, _channel, _texture);
}

/**
 * @brief 更换环境贴图：先 release 旧句柄，再写入并 retain 新句柄，最后置环境脏。
 * @note 必须经本函数写入，否则引用计数与 environment.image 会不一致。
 */
void scene_manager::set_environment_image(texture_handle _image) noexcept
{
    texture_manager.release(environment.image);

    environment.image = _image;
    texture_manager.retain(_image);

    need_environment_update();
}

/**
 * @brief 切换活跃渲染器，强制按当前尺寸 resize 并置脏。
 * @note need_descriptor_update 保证新后端不会继续引用旧输出图像的 imageview。
 */
void scene_manager::set_render_mode(render_mode _mode)
{
    active_render = scene_renders.at(_mode);

    // force resize and update
    active_render->resize(width, height);
    is_dirty               = true;
    need_descriptor_update = true;
}

/**
 * @brief 重载单张贴图，失败时保留旧图并返回 false。
 * @note 成功时置模型脏，因为模型 SSBO 里的贴图绑定可能需要更新。
 */
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

/**
 * @brief 三段式回读 render_output，并起后台线程做 OCIO 变换与写盘。
 * @note 先按输出格式决定 EXR 还是 PNG；路径由 CAPTURES_PATH 与本地时间拼出。
 * @note 重新赋值 save_thread 会先 join 上一次作业，因此连续截图会短暂阻塞主线程。
 */
vulkan_submission_token scene_manager::capture_frame(vulkan_submission_token _waited_info)
{
    const auto [image_width, image_height] = render_output.get_extent();
    const VkFormat image_format            = static_cast<VkFormat>(render_output.get_format());

    const bool is_float_half = vkuFormatIsSFLOAT(image_format) && vkuFormatIs16bit(image_format);
    const bool is_uint8_t    = vkuFormatIs8bit(image_format) && vkuFormatIsUINT(image_format);
    if (!is_float_half && !is_uint8_t)
    {
        throw std::runtime_error("Unsupported format!");
    }

    // Allocate ownership before submitting. A failing path/thread construction
    // still retains the readback buffer until its published token completes.
    auto info = std::make_shared<image_save_info>();
    info->token = vulkan_common::download_image(app.get_allocator(), *app.get_device(), recycle_bin, graphic_queue,
                                               transfer_queue, _waited_info, render_output, info->staging_buffer);
    const auto token = info->token;

    const auto now        = std::chrono::system_clock::now();
    const auto now_second = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::seconds>(now));

    std::filesystem::path save_path = std::format("{}{:%Y_%m_%d_%H_%M_%S}", CAPTURES_PATH, now_second);
    save_path.replace_extension(is_float_half ? ".exr" : ".png");

    const std::string ocio_config_path = std::string(OCIOS_PATH) + "studio-config-all-views-v4.0.0_aces-v2.0_ocio-v2.5.ocio";

    const size_t pixel_count = static_cast<size_t>(image_width) * image_height * vkuFormatComponentCount(image_format);

    info->save_path = std::move(save_path);
    info->ocio_config_path = ocio_config_path;
    info->width = image_width;
    info->height = image_height;
    info->pixel_count = pixel_count;


    save_thread = std::jthread([this, info = std::move(info), is_float_half]() {
        try
        {
            if (is_float_half) save_frame<half>(*info);
            else save_frame<uint8_t>(*info);
        }
        catch (...)
        {
            std::fputs("Screenshot readback failed; rendering will stop.\n", stderr);
            const std::lock_guard lock(save_error_mutex);
            save_error = std::current_exception();
        }
    });


    return token;
}
