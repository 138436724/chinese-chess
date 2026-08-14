#include "scene_material_manager.h"

#include "tools/font_loader.h"
#include "tools/image_helper.h"
#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <ranges>
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
                                                            const std::wstring&                   _characters,
                                                            std::vector<vk::SemaphoreSubmitInfo>& _waited_infos,
                                                            uint32_t                              _padding)
{
    // find in cache
    if (auto iter = std::ranges::find_if(images_cache,
                                         [&](const auto& s) {
                                             return s.first
                                                    == _font_path / std::to_wstring(_font_size) / _characters
                                                           / std::to_wstring(_padding);
                                         });
        iter != images_cache.end())
    {
        if (!iter->second.expired())
        {
            return std::shared_ptr<scene_image>(iter->second);
        }
    }

    // load font and transition to image
    const auto image = std::make_shared<scene_image>();
    image->type      = sampler_type::font;

    std::vector<character_info> fonts_info              = font_loader::load_font(_font_path, _font_size, _characters);
    uint32_t                    max_bearing_height_up   = 0;
    uint32_t                    max_bearing_height_down = 0;
    uint32_t                    all_width               = 0;
    size_t                      all_size                = 0;
    for (auto& _font_info : fonts_info)
    {
        max_bearing_height_up   = std::max(max_bearing_height_up, _font_info.bearing_height);
        max_bearing_height_down = std::max(max_bearing_height_down, _font_info.height - _font_info.bearing_height);
        all_width += _font_info.advance + 2 * _padding;

        // transfer only queue need bufferOffset has to be a multiple of 4, or need VK_KHR_maintenance11
        _font_info.buffer.resize(vulkan_common::align_up(_font_info.buffer.size(), 4), 0);
        all_size += _font_info.buffer.size();
    }
    const uint32_t all_height = max_bearing_height_up + max_bearing_height_down + 2 * _padding;

    // merge the font image data
    size_t                            buffer_offset = 0;
    int32_t                           width_offset  = 0;
    std::vector<uint8_t>              font_data;
    std::vector<vk::BufferImageCopy2> copy_infos;

    font_data.reserve(all_size);
    copy_infos.reserve(fonts_info.size());

    for (const auto& _font_info : fonts_info)
    {
        font_data.append_range(_font_info.buffer | std::views::as_rvalue);

        copy_infos.emplace_back(vk::BufferImageCopy2(
            buffer_offset, _font_info.width, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
            vk::Offset3D(width_offset + _font_info.bearing_width, max_bearing_height_up - _font_info.bearing_height + _padding, 0),
            vk::Extent3D(_font_info.width, _font_info.height, 1)));

        buffer_offset += _font_info.buffer.size();
        width_offset += _font_info.advance + 2 * _padding;
    }

    // create image and upload
    _waited_infos.push_back(vulkan_common::upload_image(allocator, device, recycle_bin, semaphore, graphic_queue,
                                                        transfer_queue, vk::ImageType::e2D, vk::ImageViewType::e2D,
                                                        vk::Format::eR8Unorm, vk::Extent3D(all_width, all_height, 1),
                                                        image->image, font_data, copy_infos, "font-atlas"));

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
    if (auto iter = std::ranges::find_if(images_cache, [&](const auto& s) { return s.first == _image_path; });
        iter != images_cache.end())
    {
        if (!iter->second.expired())
        {
            return std::shared_ptr<scene_image>(iter->second);
        }
    }

    const auto image = std::make_shared<scene_image>();
    image->type      = _type;

    if (_is_hdr)
    {
        const vk::Format image_format = vk::Format::eR16G16B16A16Sfloat;
        const auto       image_data =
            image_helper::read_image<half>(_image_path, vkuFormatComponentCount(static_cast<VkFormat>(image_format)));

        _waited_infos.push_back(vulkan_common::upload_image(
            allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue, vk::ImageType::e2D,
            vk::ImageViewType::e2D, image_format, vk::Extent3D(image_data.width, image_data.height, 1), image->image,
            std::span(reinterpret_cast<const uint8_t*>(image_data.buffer.data()),
                      image_data.buffer.size() * sizeof(image_data.buffer.front())),
            {}, "material_texture"));
    }
    else
    {
        const vk::Format image_format = vk::Format::eR8G8B8A8Unorm;
        const auto       image_data =
            image_helper::read_image<uint8_t>(_image_path, vkuFormatComponentCount(static_cast<VkFormat>(image_format)));

        _waited_infos.push_back(vulkan_common::upload_image(allocator, device, recycle_bin, semaphore, graphic_queue,
                                                            transfer_queue, vk::ImageType::e2D, vk::ImageViewType::e2D,
                                                            image_format, vk::Extent3D(image_data.width, image_data.height, 1),
                                                            image->image, image_data.buffer, {}, "material_texture"));
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

    auto iter = std::ranges::partition(images, [](const auto& sp) static { return sp.use_count() == 1; });
    if (const auto removed_image_count = std::distance(images.begin(), iter.begin()); removed_image_count != 0)
    {
        std::vector<std::shared_ptr<scene_image>> retire_images;
        retire_images.reserve(removed_image_count);
        std::ranges::move(images.begin(), iter.begin(), std::back_inserter(retire_images));
        recycle_bin.retire(std::move(retire_images), "scene material manager unused images.");
        images.erase(images.begin(), iter.begin());

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

const vk::raii::Sampler& scene_material_manager::get_sampler(sampler_type _type) const noexcept
{
    switch (_type)
    {
        case sampler_type::font:
            return font_sampler.get_sampler();
        case sampler_type::sky_box:
            return skybox_sampler.get_sampler();
        default:  // diffuse/color/normal/roughness/metallic 配置相同，共用 diffuse
            return diffuse_sampler.get_sampler();
    }
}

std::vector<vk::DescriptorImageInfo> scene_material_manager::get_descriptor_info() const
{
    return images | std::views::transform([this](const auto& sp) {
               return vk::DescriptorImageInfo(get_sampler(sp->type), sp->image.get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);
           })
           | std::ranges::to<std::vector>();
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
