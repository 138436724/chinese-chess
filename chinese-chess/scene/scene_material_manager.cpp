#include "scene_material_manager.h"

#include "tools/font_loader.h"
#include "tools/image_helper.h"
#include "vulkan_core/vulkan_common.h"

#include <ranges>
#include <vulkan/utility/vk_format_utils.h>

struct material_data
{
    alignas(16) glm::vec3 background_color = glm::vec3(1.f, 1.f, 1.f);
    uint32_t texture_index                 = std::numeric_limits<uint32_t>::max();
    alignas(16) glm::vec3 foreground_color = glm::vec3(1.f, 1.f, 1.f);
    float roughness                        = 0.5f;
    alignas(16) float metallic             = 0.0f;
};

scene_material_manager::scene_material_manager(vulkan_application& _app,
                                               vulkan_recycle_bin& _recycle_bin,
                                               vulkan_queue&       _graphic_queue,
                                               vulkan_queue&       _transfer_queue)
    : app(_app)
    , recycle_bin(_recycle_bin)
    , graphic_queue(_graphic_queue)
    , transfer_queue(_transfer_queue)
{
}

std::shared_ptr<scene_material> scene_material_manager::create()
{
    auto material = std::make_shared<scene_material>();
    materials.emplace_back(material);
    return material;
}

std::shared_ptr<scene_image> scene_material_manager::create(const std::filesystem::path&          _font_path,
                                                            uint32_t                              _font_size,
                                                            const std::wstring&                   _characters,
                                                            std::vector<vk::SemaphoreSubmitInfo>& _wait_info)
{
    // find in cache
    if (auto iter = std::ranges::find_if(images_cache,
                                         [&](const auto& s) {
                                             return s.first == _font_path / std::to_wstring(_font_size) / _characters;
                                         });
        iter != images_cache.end())
    {
        if (!iter->second.expired())
        {
            return std::shared_ptr<scene_image>(iter->second);
        }
    }

    // load font and transition to image
    auto image = std::make_shared<scene_image>();

    std::vector<character_info> fonts_info              = FONT_LOADER.load_font(_font_path, _font_size, _characters);
    uint32_t                    max_bearing_height_up   = 0;
    uint32_t                    max_bearing_height_down = 0;
    uint32_t                    all_width               = 0;
    size_t                      all_size                = 0;
    for (auto& _font_info : fonts_info)
    {
        max_bearing_height_up   = std::max(max_bearing_height_up, _font_info.bearing_height);
        max_bearing_height_down = std::max(max_bearing_height_down, _font_info.height - _font_info.bearing_height);
        all_width += _font_info.advance;

        // transfer only queue need bufferOffset has to be a multiple of 4, or need VK_KHR_maintenance11
        _font_info.buffer.resize(vulkan_common::align_up(_font_info.buffer.size(), 4), 0);
        all_size += _font_info.buffer.size();
    }
    uint32_t all_height = max_bearing_height_up + max_bearing_height_down;

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
            vk::Offset3D(width_offset + _font_info.bearing_width, max_bearing_height_up - _font_info.bearing_height, 0),
            vk::Extent3D(_font_info.width, _font_info.height, 1)));

        buffer_offset += _font_info.buffer.size();
        width_offset += _font_info.advance;
    }

    // create image and upload
    _wait_info.push_back(vulkan_common::upload_image(app.get_allocator(), *app.get_device(), recycle_bin,
                                                     app.get_semaphore_ptr(), graphic_queue, transfer_queue,
                                                     vk::ImageType::e2D, vk::ImageViewType::e2D, vk::Format::eR8Unorm,
                                                     vk::Extent3D(all_width, all_height, 1), *image, font_data, copy_infos));

    images.emplace_back(image);
    images_cache.emplace(_font_path / std::to_wstring(_font_size) / _characters, std::weak_ptr<scene_image>(image));
    return image;
}

std::shared_ptr<scene_image> scene_material_manager::create(const std::filesystem::path&          _image_path,
                                                            bool                                  _is_hdr,
                                                            std::vector<vk::SemaphoreSubmitInfo>& _wait_info)
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

    auto image = std::make_shared<scene_image>();

    if (_is_hdr)
    {
        vk::Format image_format = vk::Format::eR16G16B16A16Sfloat;
        auto       image_data =
            IMAGE_HELPER.read_image<half>(_image_path, vkuFormatComponentCount(static_cast<VkFormat>(image_format)));

        _wait_info.push_back(
            vulkan_common::upload_image(app.get_allocator(), *app.get_device(), recycle_bin, app.get_semaphore_ptr(),
                                        graphic_queue, transfer_queue, vk::ImageType::e2D, vk::ImageViewType::e2D,
                                        image_format, vk::Extent3D(image_data.width, image_data.height, 1), *image,
                                        std::span(reinterpret_cast<uint8_t*>(image_data.buffer.data()),
                                                  image_data.buffer.size() * sizeof(image_data.buffer.front())),
                                        {}));
    }
    else
    {
        vk::Format image_format = vk::Format::eR8G8B8A8Unorm;
        auto       image_data =
            IMAGE_HELPER.read_image<uint8_t>(_image_path, vkuFormatComponentCount(static_cast<VkFormat>(image_format)));

        _wait_info.push_back(
            vulkan_common::upload_image(app.get_allocator(), *app.get_device(), recycle_bin, app.get_semaphore_ptr(),
                                        graphic_queue, transfer_queue, vk::ImageType::e2D, vk::ImageViewType::e2D, image_format,
                                        vk::Extent3D(image_data.width, image_data.height, 1), *image, image_data.buffer, {}));
    }

    images.emplace_back(image);
    images_cache.emplace(_image_path, std::weak_ptr<scene_image>(image));
    return image;
}

void scene_material_manager::update(std::vector<vk::SemaphoreSubmitInfo>& _wait_info) noexcept
{
    std::erase_if(materials, [](const auto& p) { return p.expired(); });
    std::erase_if(images, [](const auto& p) { return p.expired(); });
    std::erase_if(images_cache, [](const auto& p) { return p.second.expired(); });

    recycle_bin.retire(std::move(ssbo));

    update_ssbo(_wait_info);
}

void scene_material_manager::clear() noexcept
{
    materials.clear();
    images.clear();
    images_cache.clear();
}

const vulkan_buffer& scene_material_manager::get_ssbo_buffer() const noexcept
{
    return ssbo;
}

std::optional<uint32_t> scene_material_manager::get_material_index(const std::weak_ptr<scene_material>& _material) const noexcept
{
    auto materials_with_index = materials | std::views::enumerate;

    std::owner_less<void> cmp;
    auto                  iter = std::ranges::find_if(materials_with_index, [&](const auto& p) {
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
    auto images_with_index = images | std::views::enumerate;

    std::owner_less<void> cmp;
    auto                  iter = std::ranges::find_if(images_with_index, [&](const auto& p) {
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

std::vector<vk::DescriptorImageInfo> scene_material_manager::get_descriptor_info(const std::span<vk::Sampler> _samplers) const
{
    if (_samplers.size() == 1)
    {
        return images | std::views::transform([&](const auto& image) {
                   return vk::DescriptorImageInfo(_samplers.front(), image.lock()->get_imageview(),
                                                  vk::ImageLayout::eShaderReadOnlyOptimal);
               })
               | std::ranges::to<std::vector>();
    }
    else if (_samplers.size() == materials.size())
    {
        return std::views::zip(_samplers, images) | std::views::transform([&](const auto& _pair) {
                   const auto& [sampler, image] = _pair;
                   return vk::DescriptorImageInfo(sampler, image.lock()->get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);
               })
               | std::ranges::to<std::vector>();
    }
    else
    {
        throw std::runtime_error("Must match size and generate!");
    }
}

void scene_material_manager::update_ssbo(std::vector<vk::SemaphoreSubmitInfo>& _wait_info) noexcept
{
    if (!materials.empty())
    {
        auto materials_ssbo =
            materials | std::views::transform([this](const auto& p) {
                auto sp = p.lock();
                return material_data{.background_color = sp->background_color,
                                     .texture_index =
                                         get_texture_index(sp->alpha_map).value_or(std::numeric_limits<uint32_t>::max()),
                                     .foreground_color = sp->foreground_color,
                                     .roughness        = sp->roughness,
                                     .metallic         = sp->metallic};
            })
            | std::ranges::to<std::vector>();

        _wait_info.push_back(vulkan_common::upload_buffer(
            app.get_allocator(), *app.get_device(), recycle_bin, app.get_semaphore_ptr(), graphic_queue, transfer_queue, ssbo,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            std::span(reinterpret_cast<uint8_t*>(materials_ssbo.data()), sizeof(materials_ssbo.front()) * materials_ssbo.size())));
    }
}
