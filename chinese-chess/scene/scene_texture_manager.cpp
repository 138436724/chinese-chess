/**
 * @file scene_texture_manager.cpp
 * @brief 贴图管理器的实现：KTX2 侧车压缩 / 读取、压缩格式转码上传、字形图集合成、引用回收与绑定重排。
 * @details 构造函数探测 BC6H 与 BC7 支持；create 先查 (usage, source) 缓存，未命中才上传。
 *          上传优先走 .ktx2 侧车：源未修改时直接读，否则重新压缩并写侧车，再转码到设备支持的格式。
 *          字形图集在 CPU 侧按覆盖度合成彩色 RGBA（BC7 可用时压缩），源路径是拼出来的虚拟路径。
 *          所有上传令牌都追加到调用方的 waited_infos，本文件不等待 GPU。
 */

#include "scene_texture_manager.h"

#include "scene_data_type.h"
#include "tools/file_watcher.h"
#include "tools/font_loader.h"
#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_recycle_bin.h"
#include "vulkan_core/vulkan_submission_scope.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/utility/vk_format_utils.h>

namespace {

[[nodiscard]] std::expected<ktx2_texture_ptr, std::string> read_and_compress_to_ktx2(const std::filesystem::path& _source_path, bool _is_hdr)
{
    if (_is_hdr)
    {
        auto image = image_helper::read_image<half>(_source_path, 4);
        if (!image)
        {
            return std::unexpected(image.error());
        }
        return image_helper::compress_to_ktx2(*image, true);
    }
    else
    {
        auto image = image_helper::read_image<uint8_t>(_source_path, 4);
        if (!image)
        {
            return std::unexpected(image.error());
        }
        return image_helper::compress_to_ktx2(*image, false);
    }
}

// must before upload_ktx2 transcode
void write_ktx2(const std::filesystem::path& _ktx2_path, const ktx2_texture_ptr& _ktx2)
{
    if (auto result = image_helper::write_ktx2(_ktx2_path, _ktx2); !result)
    {
        std::println(std::cerr, "write_ktx2 {} failed: {}", _ktx2_path.generic_string(), result.error());
    }
}

[[nodiscard]] sampler_create_info sampler_info_for(texture_usage _usage) noexcept
{
    switch (_usage)
    {
        case texture_usage::color:
            return sampler_create_info{};
        case texture_usage::environment:
            return sampler_create_info{.mag_filter        = vk::Filter::eLinear,
                                       .min_filter        = vk::Filter::eLinear,
                                       .mipmap_mode       = vk::SamplerMipmapMode::eLinear,
                                       .address_mode_u    = vk::SamplerAddressMode::eClampToEdge,
                                       .address_mode_v    = vk::SamplerAddressMode::eClampToEdge,
                                       .address_mode_w    = vk::SamplerAddressMode::eClampToEdge,
                                       .enable_anisotropy = true};
        case texture_usage::glyph:
            return sampler_create_info{.mag_filter        = vk::Filter::eLinear,
                                       .min_filter        = vk::Filter::eLinear,
                                       .mipmap_mode       = vk::SamplerMipmapMode::eNearest,
                                       .address_mode_u    = vk::SamplerAddressMode::eClampToEdge,
                                       .address_mode_v    = vk::SamplerAddressMode::eClampToEdge,
                                       .address_mode_w    = vk::SamplerAddressMode::eClampToEdge,
                                       .enable_anisotropy = false,
                                       .min_lod           = 0.f,
                                       .max_lod           = 0.f};
        default:
            std::unreachable();
    }
}

}  // namespace

/** @brief 探测 BC6H / BC7 的采样与传输目标支持，不支持时分别记为 eUndefined。 */
scene_texture_manager::scene_texture_manager(const vma::raii::Allocator&     _allocator,
                                             const vk::raii::PhysicalDevice& _physical_device,
                                             const vk::raii::Device&         _device,
                                             vulkan_recycle_bin&             _recycle_bin,
                                             const vulkan_queue&             _graphic_queue,
                                             const vulkan_queue&             _transfer_queue)
    : allocator(_allocator)
    , physical_device(_physical_device)
    , device(_device)
    , recycle_bin(_recycle_bin)
    , graphic_queue(_graphic_queue)
    , transfer_queue(_transfer_queue)
{
    compressed_hdr_format =
        vulkan_common::find_supported_format(_physical_device, {vk::Format::eBc6HUfloatBlock}, vk::ImageTiling::eOptimal,
                                             vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferDst)
            .value_or(vk::Format::eUndefined);

    compressed_ldr_format =
        vulkan_common::find_supported_format(_physical_device, {vk::Format::eBc7UnormBlock}, vk::ImageTiling::eOptimal,
                                             vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferDst)
            .value_or(vk::Format::eUndefined);
}

/**
 * @brief 先查 (usage, source) 缓存，未命中再上传；成功后插入槽表并置两个脏标志。
 * @note 上传失败返回无效句柄；异常展开时由 vulkan_submission_scope 先停提交再排空已发布令牌。
 */
texture_handle scene_texture_manager::create(const std::filesystem::path&          _image_path,
                                             texture_usage                         _usage,
                                             std::vector<vulkan_submission_token>& _waited_infos)
{
    const texture_key key{_usage, _image_path};
    if (const auto iter = texture_cache.find(key); iter != texture_cache.end())
    {
        if (textures.is_alive(iter->second))
        {
            return iter->second;
        }
        texture_cache.erase(iter);
    }

    ensure_sampler(_usage);

    texture_slot slot{.image           = vulkan_image{},
                      .data            = scene_texture_data{.source = _image_path, .usage = _usage},
                      .binding         = scene_handle<texture_tag>::npos,
                      .reference_count = 0};

    const vulkan_submission_scope failure_scope(graphic_queue, transfer_queue);
    if (auto result = upload_texture_data(_image_path, _usage, slot.image, _waited_infos); !result)
    {
        std::println(std::cerr, "create texture {} failed: {}", _image_path.generic_string(), result.error());
        return {};
    }

    const auto handle = textures.insert(std::move(slot));
    texture_cache.emplace(key, handle);

    is_dirty     = true;
    layout_dirty = true;

    return handle;
}

/**
 * @brief 合成彩色字形图集：按覆盖度把前景 / 背景色混合成 RGBA，再按可用格式压缩上传。
 * @note 压缩路径的图集宽高按 4 对齐，四周额外留 4 texel 背景色；去重键包含前后景色。
 * @note BC7 压缩或压缩上传失败时回退未压缩 RGBA8；字体加载失败直接返回无效句柄。
 */
texture_handle scene_texture_manager::create(const std::filesystem::path&          _font_path,
                                             uint32_t                              _font_size,
                                             std::wstring_view                     _characters,
                                             uint32_t                              _padding,
                                             const glm::vec3&                      _foreground_color,
                                             const glm::vec3&                      _background_color,
                                             std::vector<vulkan_submission_token>& _waited_infos)
{
    const std::string color_key =
        std::format("fg{:.4f},{:.4f},{:.4f}_bg{:.4f},{:.4f},{:.4f}", _foreground_color.x, _foreground_color.y,
                    _foreground_color.z, _background_color.x, _background_color.y, _background_color.z);
    const std::filesystem::path virtual_path =
        _font_path / std::to_wstring(_font_size) / std::wstring(_characters) / std::to_wstring(_padding) / color_key;

    const texture_key key{texture_usage::glyph, virtual_path};
    if (const auto iter = texture_cache.find(key); iter != texture_cache.end())
    {
        if (textures.is_alive(iter->second))
        {
            return iter->second;
        }
        texture_cache.erase(iter);
    }

    ensure_sampler(texture_usage::glyph);

    auto fonts_info_result = font_loader::load_font(_font_path, _font_size, _characters);
    if (!fonts_info_result)
    {
        std::println(std::cerr, "create glyph atlas {} failed: {}", _font_path.generic_string(), fonts_info_result.error());
        return {};
    }

    auto&    fonts_info              = *fonts_info_result;
    uint32_t max_bearing_height_up   = 0;
    uint32_t max_bearing_height_down = 0;
    uint32_t all_width               = 0;

    for (const auto& _font_info : fonts_info)
    {
        max_bearing_height_up   = std::max(max_bearing_height_up, _font_info.bearing_height);
        max_bearing_height_down = std::max(max_bearing_height_down, _font_info.height - _font_info.bearing_height);
        all_width += _font_info.advance + 2 * _padding;
    }
    const uint32_t all_height = max_bearing_height_up + max_bearing_height_down + 2 * _padding;

    const bool         compressible   = compressed_ldr_format != vk::Format::eUndefined;
    constexpr uint32_t glyph_margin   = 4;
    const uint32_t     content_width  = all_width + 2 * glyph_margin;
    const uint32_t     content_height = all_height + 2 * glyph_margin;
    const uint32_t atlas_width = compressible ? static_cast<uint32_t>(vulkan_common::align_up(content_width, 4)) : content_width;
    const uint32_t atlas_height = compressible ? static_cast<uint32_t>(vulkan_common::align_up(content_height, 4)) : content_height;
    const int32_t offset_x = static_cast<int32_t>((atlas_width - all_width) / 2);
    const int32_t offset_y = static_cast<int32_t>((atlas_height - all_height) / 2);

    image_info<uint8_t> coverage{.width    = atlas_width,
                                 .height   = atlas_height,
                                 .channels = 1,
                                 .buffer   = std::vector<uint8_t>(static_cast<size_t>(atlas_width) * atlas_height, 0)};
    int32_t             width_offset = 0;
    for (auto& _font_info : fonts_info)
    {
        const image_info<uint8_t> glyph{
            .width = _font_info.width, .height = _font_info.height, .channels = 1, .buffer = std::move(_font_info.buffer)};

        if (auto result = image_helper::paste_image(
                glyph, coverage, offset_x + width_offset + _font_info.bearing_width,
                offset_y + static_cast<int32_t>(max_bearing_height_up - _font_info.bearing_height + _padding));
            !result)
        {
            std::println(std::cerr, "create glyph atlas {} failed: {}", _font_path.generic_string(), result.error());
            return {};
        }

        width_offset += _font_info.advance + 2 * _padding;
    }

    image_info<uint8_t> image{.width    = atlas_width,
                              .height   = atlas_height,
                              .channels = 4,
                              .buffer   = std::vector<uint8_t>(static_cast<size_t>(atlas_width) * atlas_height * 4)};
    for (size_t i = 0; i < coverage.buffer.size(); ++i)
    {
        const float     weight  = static_cast<float>(coverage.buffer.at(i)) / 255.f;
        const glm::vec3 rgb     = glm::mix(_background_color, _foreground_color, weight);
        image.buffer[i * 4 + 0] = static_cast<uint8_t>(std::lround(glm::clamp(rgb.r, 0.f, 1.f) * 255.f));
        image.buffer[i * 4 + 1] = static_cast<uint8_t>(std::lround(glm::clamp(rgb.g, 0.f, 1.f) * 255.f));
        image.buffer[i * 4 + 2] = static_cast<uint8_t>(std::lround(glm::clamp(rgb.b, 0.f, 1.f) * 255.f));
        image.buffer[i * 4 + 3] = 255;
    }

    texture_slot slot{.image           = vulkan_image{},
                      .data            = scene_texture_data{.source = virtual_path, .usage = texture_usage::glyph},
                      .binding         = scene_handle<texture_tag>::npos,
                      .reference_count = 0};

    const vulkan_submission_scope failure_scope(graphic_queue, transfer_queue);
    if (compressible)
    {
        if (auto ktx2 = image_helper::compress_to_ktx2(image, false); ktx2)
        {
            if (auto result = upload_ktx2(*ktx2, compressed_ldr_format, slot.image, _waited_infos, "font-atlas"); result)
            {
                const auto handle = textures.insert(std::move(slot));
                texture_cache.emplace(key, handle);

                is_dirty     = true;
                layout_dirty = true;

                return handle;
            }
            else
            {
                std::println(std::cerr, "upload font atlas ktx2 failed: {}", result.error());
            }
        }
        else
        {
            std::println(std::cerr, "compress font atlas failed: {}", ktx2.error());
        }
    }

    _waited_infos.push_back(vulkan_common::upload_image(
        allocator, device, recycle_bin, graphic_queue, transfer_queue, vk::ImageType::e2D,
        vk::ImageViewType::e2D, vk::Format::eR8G8B8A8Unorm, vk::Extent3D(atlas_width, atlas_height, 1), slot.image,
        vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eRayTracingShaderKHR | vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderSampledRead, image.buffer, "font-atlas"));

    const auto handle = textures.insert(std::move(slot));
    texture_cache.emplace(key, handle);

    is_dirty     = true;
    layout_dirty = true;

    return handle;
}

scene_texture_data* scene_texture_manager::get(texture_handle _handle) noexcept
{
    auto* slot = textures.get(_handle);
    return slot != nullptr ? &slot->data : nullptr;
}

/**
 * @brief 重新上传源文件并在原槽换图；glyph 图集与无效句柄直接拒绝。
 * @note 旧图按已提交前沿退休；只置 is_dirty，不置 layout_dirty，槽序与绑定保持不变。
 */
bool scene_texture_manager::reload_texture(texture_handle _handle, std::vector<vulkan_submission_token>& _waited_infos)
{
    auto* slot = textures.get(_handle);
    if (slot == nullptr || slot->data.usage == texture_usage::glyph)
    {
        return false;
    }

    vulkan_image new_image;
    const vulkan_submission_scope failure_scope(graphic_queue, transfer_queue);
    if (auto result = upload_texture_data(slot->data.source, slot->data.usage, new_image, _waited_infos); !result)
    {
        std::println(std::cerr, "reload {} failed: {}", slot->data.source.generic_string(), result.error());
        return false;
    }

    recycle_bin.retire_after_submitted(std::exchange(slot->image, std::move(new_image)), "reloaded texture old image.");
    is_dirty = true;

    return true;
}

/**
 * @brief 遍历存活贴图，跳过 glyph、源文件不存在或哈希未变的项，逐张重载。
 */
bool scene_texture_manager::reload_all_textures(std::vector<vulkan_submission_token>& _waited_infos)
{
    bool reloaded = false;
    for (auto&& [handle, slot] : textures.live())
    {
        if (slot.data.usage == texture_usage::glyph)
        {
            continue;
        }

        std::error_code ec;
        if (!std::filesystem::exists(slot.data.source, ec) || ec)
        {
            continue;
        }
        if (!FILE_WATCHER.is_file_modified(slot.data.source))
        {
            continue;
        }

        reloaded = reload_texture(handle, _waited_infos) || reloaded;
    }
    return reloaded;
}

/** @brief 引用计数加一；句柄无效时静默忽略。 */
void scene_texture_manager::retain(texture_handle _handle) noexcept
{
    if (auto* slot = textures.get(_handle); slot != nullptr)
    {
        ++slot->reference_count;
    }
}

/**
 * @brief 引用计数减一；Debug 下计数已为 0 时抛异常，Release 下静默忽略。
 * @note 归零并不立即回收，交给下一次 update()。
 */
void scene_texture_manager::release(texture_handle _handle)
{
    auto* slot = textures.get(_handle);
    if (slot == nullptr)
    {
        return;
    }

    if (slot->reference_count > 0) [[likely]]
    {
        --slot->reference_count;
    }
#ifndef NDEBUG
    else
    {
        throw std::runtime_error("This texture release times greater than retain times");
    }
#endif  // !NDEBUG
}

/**
 * @brief 先回收零引用槽，再按槽序重排 bindless 绑定并返回两个标志。
 * @note 绑定数超过 MAX_BINDLESS_TEXTURES 时，超出部分记为 npos，Debug 下还会抛异常。
 * @note layout_dirty 在每次调用都被清掉，即使本次没有回收任何槽。
 */
scene_texture_manager::update_result scene_texture_manager::update()
{
    const auto dead =
        textures.live() | std::views::filter([](const auto& _entry) { return _entry.value.reference_count == 0; })
        | std::views::transform([](const auto& _entry) { return _entry.handle; }) | std::ranges::to<std::vector>();
    for (const auto handle : dead)
    {
        std::ignore = destroy(handle);
    }

    const bool layout_moved = layout_dirty;
    layout_dirty            = false;
    // 句柄的世代身份不变，但压紧后的 GPU binding 可能改变；调用方须据 layout_moved 重建材质绑定。

    if (!is_dirty)
    {
        return {.needs_redraw = false, .layout_moved = layout_moved};
    }

    uint32_t binding = 0;
    for (auto&& texture : textures.live())
    {
        if (binding < vulkan_common::MAX_BINDLESS_TEXTURES) [[likely]]
        {
            texture.value.binding = binding;
            ++binding;
        }
        else
        {
            texture.value.binding = scene_handle<texture_tag>::npos;
#ifndef NDEBUG
            throw std::runtime_error(std::format("Out of range, max count is {}!", vulkan_common::MAX_BINDLESS_TEXTURES));
#endif  // !NDEBUG
        }
    }
    texture_binding_count = binding;

    is_dirty = false;
    return {.needs_redraw = true, .layout_moved = layout_moved};
}

/** @brief 只置 is_dirty；layout_dirty 保持原值，材质 SSBO 不会因此重传。 */
void scene_texture_manager::need_update() noexcept
{
    is_dirty = true;
}

size_t scene_texture_manager::get_textures_size() const noexcept
{
    return texture_binding_count;
}

uint32_t scene_texture_manager::get_texture_binding(texture_handle _handle) const noexcept
{
    const auto* slot = textures.get(_handle);
    return slot ? slot->binding : scene_handle<texture_tag>::npos;
}

/**
 * @brief 遍历已绑定槽位，按槽序把 sampler / view / 布局包成 DescriptorImageInfo。
 * @note 返回向量下标即紧凑绑定；binding 为 npos 的槽被过滤掉。
 */
std::vector<vk::DescriptorImageInfo> scene_texture_manager::get_descriptor_info() const
{
    return textures.live() | std::views::filter([](const auto& _entry) {
               return _entry.value.binding != scene_handle<texture_tag>::npos;
           })
           | std::views::transform([this](const auto& _entry) {
                 return vk::DescriptorImageInfo(get_sampler(_entry.value.data.usage), _entry.value.image.get_imageview(),
                                                vk::ImageLayout::eShaderReadOnlyOptimal);
             })
           | std::ranges::to<std::vector>();
}

/**
 * @brief 图像进回收站、擦除槽并清理缓存，随后置 is_dirty 与 layout_dirty。
 * @note erase 的失败结果被忽略，因此无论句柄是否有效都返回 true。
 */
bool scene_texture_manager::destroy(texture_handle _handle)
{
    if (auto* slot = textures.get(_handle); slot != nullptr)
    {
        recycle_bin.retire_after_submitted(std::move(slot->image), "scene texture manager unused texture.");
    }

    std::ignore = textures.erase(_handle);
    std::erase_if(texture_cache, [_handle](const auto& _pair) { return _pair.second == _handle; });

    is_dirty     = true;
    layout_dirty = true;

    return true;
}

/**
 * @brief 优先读 / 生成 .ktx2 侧车并转码上传，压缩路径不可用时回退未压缩 RGBA16F / RGBA8。
 * @note 侧车存在但读取失败时打印错误并继续走未压缩回退；两条路径都只把令牌推进 _waited_infos。
 */
std::expected<void, std::string> scene_texture_manager::upload_texture_data(const std::filesystem::path& _source,
                                                                            texture_usage                _usage,
                                                                            vulkan_image&                _image,
                                                                            std::vector<vulkan_submission_token>& _waited_infos) const
{
    const bool       is_hdr        = (_usage == texture_usage::environment);
    const auto       target_format = is_hdr ? compressed_hdr_format : compressed_ldr_format;
    ktx2_texture_ptr ktx2{nullptr, ktxTexture2_Destroy};

    std::filesystem::path ktx2_path = _source;
    ktx2_path += ".ktx2";

    // has support compress format
    if (target_format != vk::Format::eUndefined)
    {
        if (std::filesystem::exists(ktx2_path) && !FILE_WATCHER.is_file_modified(_source))
        {
            if (auto result = image_helper::read_ktx2(ktx2_path); result)
            {
                ktx2 = std::move(*result);
            }
            else
            {
                std::println(std::cerr, "read ktx2 {} failed: {}", ktx2_path.generic_string(), result.error());
            }
        }
        else if (auto result = read_and_compress_to_ktx2(_source, is_hdr); result)
        {
            ktx2 = std::move(*result);
            write_ktx2(ktx2_path, ktx2);
        }
        else
        {
            std::println(std::cerr, "compress_to_ktx2 {} failed: {}", _source.generic_string(), result.error());
        }
    }

    if (ktx2)
    {
        if (auto result = upload_ktx2(ktx2, target_format, _image, _waited_infos, "material_texture"); result)
        {
            return {};
        }
        else
        {
            std::println(std::cerr, "upload ktx2 {} failed: {}", ktx2_path.generic_string(), result.error());
        }
    }

    if (is_hdr)
    {
        constexpr vk::Format image_format = vk::Format::eR16G16B16A16Sfloat;
        const auto           image_data_result =
            image_helper::read_image<half>(_source, vkuFormatComponentCount(static_cast<VkFormat>(image_format)));
        if (!image_data_result)
        {
            return std::unexpected(image_data_result.error());
        }

        const auto& image_data = *image_data_result;
        _waited_infos.push_back(vulkan_common::upload_image(
            allocator, device, recycle_bin, graphic_queue, transfer_queue, vk::ImageType::e2D,
            vk::ImageViewType::e2D, image_format, vk::Extent3D(image_data.width, image_data.height, 1), _image,
            vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eRayTracingShaderKHR
                | vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderSampledRead,
            std::span(reinterpret_cast<const uint8_t*>(image_data.buffer.data()),
                      image_data.buffer.size() * sizeof(decltype(image_data.buffer)::value_type)),
            "material_texture"));
    }
    else
    {
        constexpr vk::Format image_format = vk::Format::eR8G8B8A8Unorm;
        const auto           image_data_result =
            image_helper::read_image<uint8_t>(_source, vkuFormatComponentCount(static_cast<VkFormat>(image_format)));
        if (!image_data_result)
        {
            return std::unexpected(image_data_result.error());
        }

        const auto& image_data = *image_data_result;
        _waited_infos.push_back(vulkan_common::upload_image(
            allocator, device, recycle_bin, graphic_queue, transfer_queue, vk::ImageType::e2D,
            vk::ImageViewType::e2D, image_format, vk::Extent3D(image_data.width, image_data.height, 1), _image,
            vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eRayTracingShaderKHR
                | vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderSampledRead, image_data.buffer, "material_texture"));
    }

    return {};
}

/**
 * @brief 按目标格式把 KTX2 转码（BC6H / BC7 / BC4 / EAC_R11）并只上传 mip0。
 * @note 目标格式不受支持、转码失败、取不到 mip0 偏移或没有 mip0 数据时返回错误串。
 */
std::expected<void, std::string> scene_texture_manager::upload_ktx2(const ktx2_texture_ptr& _ktx2,
                                                                    vk::Format              _target_format,
                                                                    vulkan_image&           _image,
                                                                    std::vector<vulkan_submission_token>& _waited_infos,
                                                                    const std::string&                    _name) const
{
    if (static_cast<vk::Format>(_ktx2->vkFormat) != _target_format)
    {
        ktx_transcode_fmt_e transcode_format;
        if (_target_format == vk::Format::eBc6HUfloatBlock)
        {
            transcode_format = KTX_TTF_BC6HU_RGB;
        }
        else if (_target_format == vk::Format::eBc7UnormBlock)
        {
            transcode_format = KTX_TTF_BC7_RGBA;
        }
        else if (_target_format == vk::Format::eBc4UnormBlock)
        {
            transcode_format = KTX_TTF_BC4_R;
        }
        else if (_target_format == vk::Format::eEacR11UnormBlock)
        {
            transcode_format = KTX_TTF_ETC2_EAC_R11;
        }
        else
        {
            return std::unexpected(std::format("upload_ktx2: unsupported target format {}.", vk::to_string(_target_format)));
        }

        if (const auto error = ktxTexture2_TranscodeBasis(_ktx2.get(), transcode_format, 0); error != KTX_SUCCESS)
        {
            return std::unexpected(std::format("ktxTexture2_TranscodeBasis failed: {}.", ktxErrorString(error)));
        }
    }

    ktx_size_t offset = 0;
    if (const auto error = ktxTexture2_GetImageOffset(_ktx2.get(), 0, 0, 0, &offset); error != KTX_SUCCESS)
    {
        return std::unexpected(std::format("ktxTexture2_GetImageOffset failed: {}.", ktxErrorString(error)));
    }

    // only mip0
    if (offset >= _ktx2->dataSize)
    {
        return std::unexpected("ktx2 has no mip0 data.");
    }

    const auto* data = reinterpret_cast<const uint8_t*>(_ktx2->pData) + offset;
    const auto  size = _ktx2->dataSize - offset;

    _waited_infos.push_back(vulkan_common::upload_image(
        allocator, device, recycle_bin, graphic_queue, transfer_queue, vk::ImageType::e2D,
        vk::ImageViewType::e2D, _target_format, vk::Extent3D(_ktx2->baseWidth, _ktx2->baseHeight, 1), _image,
        vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eRayTracingShaderKHR | vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderSampledRead, std::span(data, size), _name));

    return {};
}

/** @brief 每种用途只建一个采样器；已存在时直接返回。 */
void scene_texture_manager::ensure_sampler(texture_usage _usage)
{
    if (samplers.contains(_usage))
    {
        return;
    }

    vulkan_sampler sampler;
    sampler.create(physical_device, device, sampler_info_for(_usage), "scene sampler");

    samplers.emplace(_usage, std::move(sampler));
}

/** @brief 从用途表取采样器；缺失时抛 std::runtime_error（正常路径不会发生）。 */
vk::Sampler scene_texture_manager::get_sampler(texture_usage _usage) const
{
    if (const auto iter = samplers.find(_usage); iter != samplers.end())
    {
        return *(iter->second);
    }
    throw std::runtime_error("No matching sampler exists!");
}
