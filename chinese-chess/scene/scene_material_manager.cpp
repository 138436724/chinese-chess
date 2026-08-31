#include "scene_material_manager.h"

#include "scene_material.h"
#include "tools/file_watcher.h"
#include "tools/font_loader.h"
#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <algorithm>
#include <format>
#include <iostream>
#include <iterator>
#include <limits>
#include <print>
#include <ranges>
#include <string>
#include <utility>
#include <vulkan/utility/vk_format_utils.h>

namespace {

struct material_data
{
    alignas(16) glm::vec3 background_color = glm::vec3(1.f, 1.f, 1.f);
    uint32_t texture_index                 = std::numeric_limits<uint32_t>::max();
    alignas(16) glm::vec3 foreground_color = glm::vec3(1.f, 1.f, 1.f);
    float roughness                        = 0.5f;
    alignas(16) float metallic             = 0.0f;
    float opacity                          = 1.0f;
    float ior                              = 1.5f;
    float transmission                     = 0.0f;
};

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

}  // namespace

scene_material_manager::scene_material_manager(const vma::raii::Allocator&     _allocator,
                                               const vk::raii::PhysicalDevice& _physical_device,
                                               const vk::raii::Device&         _device,
                                               vulkan_recycle_bin&             _recycle_bin,
                                               vulkan_semaphore&               _semaphore,
                                               const vulkan_queue&             _graphic_queue,
                                               const vulkan_queue&             _transfer_queue)
    : allocator(_allocator)
    , physical_device(_physical_device)
    , device(_device)
    , recycle_bin(_recycle_bin)
    , semaphore(_semaphore)
    , graphic_queue(_graphic_queue)
    , transfer_queue(_transfer_queue)
{
    diffuse_sampler.create(_physical_device, _device, sampler_type::diffuse);
    font_sampler.create(_physical_device, _device, sampler_type::font);
    skybox_sampler.create(_physical_device, _device, sampler_type::sky_box);

    compressed_hdr_format =
        vulkan_common::find_supported_format(_physical_device, {vk::Format::eBc6HUfloatBlock}, vk::ImageTiling::eOptimal,
                                             vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferDst)
            .value_or(vk::Format::eUndefined);

    compressed_ldr_format =
        vulkan_common::find_supported_format(_physical_device, {vk::Format::eBc7UnormBlock}, vk::ImageTiling::eOptimal,
                                             vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferDst)
            .value_or(vk::Format::eUndefined);

    compressed_font_format =
        vulkan_common::find_supported_format(_physical_device, {vk::Format::eBc4UnormBlock, vk::Format::eEacR11UnormBlock},
                                             vk::ImageTiling::eOptimal,
                                             vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferDst)
            .value_or(vk::Format::eUndefined);
}

std::shared_ptr<scene_material> scene_material_manager::create()
{
    auto material = std::make_shared<scene_material>();
    materials.emplace_back(material);
    is_dirty = true;
    return material;
}

std::shared_ptr<scene_image> scene_material_manager::create(const std::filesystem::path&          _font_path,
                                                            uint32_t                              _font_size,
                                                            std::wstring_view                     _characters,
                                                            std::vector<vk::SemaphoreSubmitInfo>& _waited_infos,
                                                            uint32_t                              _padding)
{
    // find in cache
    const auto cache_key = _font_path / std::to_wstring(_font_size) / std::wstring(_characters) / std::to_wstring(_padding);
    if (auto iter = images_cache.find(cache_key); iter != images_cache.end())
    {
        if (!iter->second.expired())
        {
            return std::shared_ptr<scene_image>(iter->second);
        }
    }

    // load font and transition to image
    const auto image = std::make_shared<scene_image>();
    image->type      = sampler_type::font;

    auto fonts_info_result = font_loader::load_font(_font_path, _font_size, _characters);
    if (!fonts_info_result)
    {
        throw std::runtime_error(fonts_info_result.error());
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

    const bool compressible = compressed_font_format != vk::Format::eUndefined;
    const uint32_t atlas_width = compressible ? static_cast<uint32_t>(vulkan_common::align_up(all_width, 4)) : all_width;
    const uint32_t atlas_height = compressible ? static_cast<uint32_t>(vulkan_common::align_up(all_height, 4)) : all_height;
    const uint32_t atlas_channels = compressible ? 4 : 1;
    const int32_t  offset_x       = compressible ? static_cast<int32_t>((atlas_width - all_width) / 2) : 0;
    const int32_t  offset_y       = compressible ? static_cast<int32_t>((atlas_height - all_height) / 2) : 0;

    image_info<uint8_t> full_image{.width    = atlas_width,
                                   .height   = atlas_height,
                                   .channels = atlas_channels,
                                   .buffer = std::vector<uint8_t>(static_cast<size_t>(atlas_width) * atlas_height * atlas_channels, 0)};

    int32_t width_offset = 0;
    for (auto& _font_info : fonts_info)
    {
        const image_info<uint8_t> glyph{
            .width = _font_info.width, .height = _font_info.height, .channels = 1, .buffer = std::move(_font_info.buffer)};

        if (auto result = image_helper::paste_image(
                glyph, full_image, offset_x + width_offset + _font_info.bearing_width,
                offset_y + static_cast<int32_t>(max_bearing_height_up - _font_info.bearing_height + _padding));
            !result)
        {
            throw std::runtime_error(result.error());
        }

        width_offset += _font_info.advance + 2 * _padding;
    }

    if (compressible)
    {
        if (auto ktx2 = image_helper::compress_to_ktx2(full_image, false); ktx2)
        {
            if (auto result = upload_ktx2(*ktx2, compressed_font_format, image->image, _waited_infos, "font-atlas"); result)
            {
                images.emplace_back(image);
                images_cache.emplace(_font_path / std::to_wstring(_font_size) / _characters / std::to_wstring(_padding),
                                     std::weak_ptr<scene_image>(image));
                is_dirty = true;
                return image;
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

    // default upload R8
    image_info<uint8_t> r8_fallback;
    const auto*         upload_buffer = &full_image.buffer;
    if (full_image.channels != 1)
    {
        auto r8 = image_helper::convert_channels(full_image, 1);
        if (!r8)
        {
            throw std::runtime_error(r8.error());
        }
        r8_fallback   = std::move(*r8);
        upload_buffer = &r8_fallback.buffer;
    }
    _waited_infos.push_back(vulkan_common::upload_image(allocator, device, recycle_bin, semaphore, graphic_queue,
                                                        transfer_queue, vk::ImageType::e2D, vk::ImageViewType::e2D,
                                                        vk::Format::eR8Unorm, vk::Extent3D(atlas_width, atlas_height, 1),
                                                        image->image, *upload_buffer, "font-atlas"));

    images.emplace_back(image);
    images_cache.emplace(_font_path / std::to_wstring(_font_size) / _characters / std::to_wstring(_padding),
                         std::weak_ptr<scene_image>(image));
    is_dirty = true;
    return image;
}

std::shared_ptr<scene_image> scene_material_manager::create(const std::filesystem::path&          _image_path,
                                                            bool                                  _is_hdr,
                                                            sampler_type                          _type,
                                                            std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    // find in cache
    if (auto iter = images_cache.find(_image_path); iter != images_cache.end())
    {
        if (!iter->second.expired())
        {
            return std::shared_ptr<scene_image>(iter->second);
        }
    }

    const auto image = std::make_shared<scene_image>();
    image->type      = _type;

    const auto       target_format = _is_hdr ? compressed_hdr_format : compressed_ldr_format;
    ktx2_texture_ptr ktx2{nullptr, ktxTexture2_Destroy};

    std::filesystem::path ktx2_path = _image_path;
    ktx2_path += ".ktx2";

    // has support compress format
    if (target_format != vk::Format::eUndefined)
    {
        if (std::filesystem::exists(ktx2_path) && !FILE_WATCHER.is_file_modified(_image_path))
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
        else if (auto result = read_and_compress_to_ktx2(_image_path, _is_hdr); result)
        {
            ktx2 = std::move(*result);
            write_ktx2(ktx2_path, ktx2);
        }
        else
        {
            std::println(std::cerr, "compress_to_ktx2 {} failed: {}", _image_path.generic_string(), result.error());
        }
    }

    if (ktx2)
    {
        if (auto result = upload_ktx2(ktx2, target_format, image->image, _waited_infos, "material_texture"); result)
        {
            images.emplace_back(image);
            images_cache.emplace(_image_path, std::weak_ptr<scene_image>(image));
            is_dirty = true;
            return image;
        }
        else
        {
            std::println(std::cerr, "upload ktx2 {} failed: {}", ktx2_path.generic_string(), result.error());
        }
    }

    if (_is_hdr)
    {
        constexpr vk::Format image_format = vk::Format::eR16G16B16A16Sfloat;
        auto                 image_data_result =
            image_helper::read_image<half>(_image_path, vkuFormatComponentCount(static_cast<VkFormat>(image_format)));
        if (!image_data_result)
        {
            throw std::runtime_error(image_data_result.error());
        }

        const auto& image_data = *image_data_result;
        _waited_infos.push_back(vulkan_common::upload_image(
            allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue, vk::ImageType::e2D,
            vk::ImageViewType::e2D, image_format, vk::Extent3D(image_data.width, image_data.height, 1), image->image,
            std::span(reinterpret_cast<const uint8_t*>(image_data.buffer.data()),
                      image_data.buffer.size() * sizeof(image_data.buffer.front())),
            "material_texture"));
    }
    else
    {
        constexpr vk::Format image_format = vk::Format::eR8G8B8A8Unorm;
        auto                 image_data_result =
            image_helper::read_image<uint8_t>(_image_path, vkuFormatComponentCount(static_cast<VkFormat>(image_format)));
        if (!image_data_result)
        {
            throw std::runtime_error(image_data_result.error());
        }

        const auto& image_data = *image_data_result;
        _waited_infos.push_back(vulkan_common::upload_image(allocator, device, recycle_bin, semaphore, graphic_queue,
                                                            transfer_queue, vk::ImageType::e2D, vk::ImageViewType::e2D,
                                                            image_format, vk::Extent3D(image_data.width, image_data.height, 1),
                                                            image->image, image_data.buffer, "material_texture"));
    }

    images.emplace_back(image);
    images_cache.emplace(_image_path, std::weak_ptr<scene_image>(image));
    is_dirty = true;
    return image;
}

bool scene_material_manager::update(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    if (std::erase_if(materials, [](const auto& p) static { return p.expired(); }) != 0)
    {
        is_dirty = true;
    }

    const auto retires = std::ranges::stable_partition(images, [](const auto& sp) static { return sp.use_count() != 1; });
    if (retires.begin() != images.end())
    {
        std::vector<std::shared_ptr<scene_image>> retire_images(std::make_move_iterator(retires.begin()),
                                                                std::make_move_iterator(retires.end()));
        recycle_bin.retire(std::move(retire_images), "scene material manager unused images.");
        images.erase(retires.begin(), retires.end());
        is_dirty = true;
    }

    std::erase_if(images_cache, [](const auto& p) static { return p.second.expired(); });

    if (!is_dirty)
    {
        return false;
    }

    recycle_bin.retire(std::move(ssbo), "scene material manager old ssbo.");
    update_ssbo(_waited_infos);
    is_dirty = false;

    return true;
}

void scene_material_manager::clear()
{
    materials.clear();
    recycle_bin.retire(std::move(images), "scene material manager clear images.");
    images_cache.clear();
    recycle_bin.retire(std::move(ssbo), "scene material manager clear ssbo.");
    is_dirty = true;
}

void scene_material_manager::need_update() noexcept
{
    is_dirty = true;
}

const vulkan_buffer& scene_material_manager::get_ssbo_buffer() const noexcept
{
    return ssbo;
}

std::optional<uint32_t> scene_material_manager::get_material_index(const std::weak_ptr<scene_material>& _material) const noexcept
{
    const auto materials_with_index = materials | std::views::enumerate;

    const std::owner_less<void> cmp;
    const auto                  iter = std::ranges::find_if(materials_with_index, [&](const auto& p) {
        return !cmp(std::get<1>(p), _material) && !cmp(_material, std::get<1>(p));
    });

    if (iter != materials_with_index.end())
    {
        return static_cast<uint32_t>(std::get<0>(*iter));
    }
    else
    {
        return std::nullopt;
    }
}

std::optional<uint32_t> scene_material_manager::get_texture_index(const std::weak_ptr<scene_image>& _texture) const noexcept
{
    const auto images_with_index = images | std::views::enumerate;

    const std::owner_less<void> cmp;
    const auto                  iter = std::ranges::find_if(images_with_index, [&](const auto& p) {
        return !cmp(std::get<1>(p), _texture) && !cmp(_texture, std::get<1>(p));
    });

    if (iter != images_with_index.end())
    {
        return static_cast<uint32_t>(std::get<0>(*iter));
    }
    else
    {
        return std::nullopt;
    }
}

std::vector<vk::DescriptorImageInfo> scene_material_manager::get_descriptor_info() const
{
    return images | std::views::transform([this](const auto& sp) {
               return vk::DescriptorImageInfo(get_sampler(sp->type), sp->image.get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);
           })
           | std::ranges::to<std::vector>();
}

bool scene_material_manager::reload_textures(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    bool reloaded = false;
    for (const auto& [source_path, image_weak] : images_cache)
    {
        std::error_code ec;
        if (!std::filesystem::exists(source_path, ec) || ec)
        {
            continue;
        }

        const auto sp = image_weak.lock();
        if (!sp)
        {
            continue;
        }

        if (!FILE_WATCHER.is_file_modified(source_path))
        {
            continue;
        }

        vulkan_image                     new_image;
        const bool                       is_hdr        = sp->image.get_format() == vk::Format::eR16G16B16A16Sfloat
                                                         || sp->image.get_format() == vk::Format::eBc6HUfloatBlock;
        const auto                       target_format = is_hdr ? compressed_hdr_format : compressed_ldr_format;
        std::expected<void, std::string> load_result   = std::unexpected("Unknown reload failure.");

        if (target_format != vk::Format::eUndefined)
        {
            if (auto compressed = read_and_compress_to_ktx2(source_path, is_hdr); compressed)
            {
                std::filesystem::path ktx2_path = source_path;
                ktx2_path += ".ktx2";
                write_ktx2(ktx2_path, *compressed);  // 同步写 UASTC sidecar（转码前）
                load_result = upload_ktx2(*compressed, target_format, new_image, _waited_infos, "material texture reload");
            }
            else
            {
                load_result = std::unexpected(compressed.error());
            }
        }
        else if (is_hdr)
        {
            auto image_data = image_helper::read_image<half>(source_path, 4);
            if (image_data)
            {
                _waited_infos.push_back(
                    vulkan_common::upload_image(allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue,
                                                vk::ImageType::e2D, vk::ImageViewType::e2D, vk::Format::eR16G16B16A16Sfloat,
                                                vk::Extent3D(image_data->width, image_data->height, 1), new_image,
                                                std::span(reinterpret_cast<const uint8_t*>(image_data->buffer.data()),
                                                          image_data->buffer.size() * sizeof(image_data->buffer.front())),
                                                "material texture reload"));
                load_result = {};
            }
            else
            {
                load_result = std::unexpected(image_data.error());
            }
        }
        else
        {
            auto image_data = image_helper::read_image<uint8_t>(source_path, 4);
            if (image_data)
            {
                _waited_infos.push_back(
                    vulkan_common::upload_image(allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue,
                                                vk::ImageType::e2D, vk::ImageViewType::e2D, vk::Format::eR8G8B8A8Unorm,
                                                vk::Extent3D(image_data->width, image_data->height, 1), new_image,
                                                image_data->buffer, "material texture reload"));
                load_result = {};
            }
            else
            {
                load_result = std::unexpected(image_data.error());
            }
        }

        if (load_result)
        {
            recycle_bin.retire(std::exchange(sp->image, std::move(new_image)), "reloaded texture old image.");
            reloaded = true;
        }
        else
        {
            std::println(std::cerr, "reload {} failed: {}", source_path.generic_string(), load_result.error());
        }
    }
    return reloaded;
}

std::expected<void, std::string> scene_material_manager::upload_ktx2(const ktx2_texture_ptr& _ktx2,
                                                                     vk::Format              _target_format,
                                                                     vulkan_image&           _image,
                                                                     std::vector<vk::SemaphoreSubmitInfo>& _waited_infos,
                                                                     const std::string& _name) const
{
    if (static_cast<vk::Format>(_ktx2->vkFormat) != _target_format)
    {
        ktx_transcode_fmt_e transcode_format;
        if (_target_format == vk::Format::eBc6HUfloatBlock)
        {
            transcode_format = KTX_TTF_BC6HU;
        }
        else if (_target_format == vk::Format::eBc7UnormBlock)
        {
            transcode_format = KTX_TTF_BC7_RGBA;
        }
        else if (_target_format == vk::Format::eBc4UnormBlock)
        {
            transcode_format = KTX_TTF_BC4_R;  // 灰度（桌面）：单通道 BC4
        }
        else if (_target_format == vk::Format::eEacR11UnormBlock)
        {
            transcode_format = KTX_TTF_ETC2_EAC_R11;  // 灰度（移动端）：EAC R11
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
        allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue, vk::ImageType::e2D, vk::ImageViewType::e2D,
        _target_format, vk::Extent3D(_ktx2->baseWidth, _ktx2->baseHeight, 1), _image, std::span(data, size), _name));

    return {};
}

void scene_material_manager::update_ssbo(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    if (!materials.empty())
    {
        const auto materials_ssbo =
            materials | std::views::transform([this](const auto& p) {
                const auto sp = p.lock();
                return material_data{.background_color = sp->background_color,
                                     .texture_index =
                                         get_texture_index(sp->alpha_map).value_or(std::numeric_limits<uint32_t>::max()),
                                     .foreground_color = sp->foreground_color,
                                     .roughness        = sp->roughness,
                                     .metallic         = sp->metallic,
                                     .opacity          = sp->opacity,
                                     .ior              = sp->ior,
                                     .transmission     = sp->transmission};
            })
            | std::ranges::to<std::vector>();

        _waited_infos.push_back(vulkan_common::upload_buffer(
            allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue, ssbo,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            std::span(reinterpret_cast<const uint8_t*>(materials_ssbo.data()),
                      sizeof(materials_ssbo.front()) * materials_ssbo.size()),
            "material_ssbo"));
    }
}

const vk::raii::Sampler& scene_material_manager::get_sampler(sampler_type _type) const noexcept
{
    switch (_type)
    {
        case sampler_type::font:
            return *font_sampler;
        case sampler_type::sky_box:
            return *skybox_sampler;
        default:  // diffuse/color/normal/roughness/metallic all same
            return *diffuse_sampler;
    }
}
