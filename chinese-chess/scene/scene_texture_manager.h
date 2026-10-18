#pragma once

#include "scene_data_type.h"
#include "scene_handle.h"
#include "scene_handle_map.h"
#include "tools/image_helper.h"
#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_sampler.h"
#include "vulkan_core/vulkan_submission_token.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class vulkan_recycle_bin;
class vulkan_queue;

class scene_texture_manager
{
public:
    scene_texture_manager(const vma::raii::Allocator&     _allocator,
                          const vk::raii::PhysicalDevice& _physical_device,
                          const vk::raii::Device&         _device,
                          vulkan_recycle_bin&             _recycle_bin,
                          const vulkan_queue&             _graphic_queue,
                          const vulkan_queue&             _transfer_queue);
    ~scene_texture_manager() = default;

    [[nodiscard]] texture_handle      create(const std::filesystem::path&          _image_path,
                                             texture_usage                         _usage,
                                             std::vector<vulkan_submission_token>& _waited_infos);
    [[nodiscard]] texture_handle      create(const std::filesystem::path&          _font_path,
                                             uint32_t                              _font_size,
                                             std::wstring_view                     _characters,
                                             uint32_t                              _padding,
                                             const glm::vec3&                      _foreground_color,
                                             const glm::vec3&                      _background_color,
                                             std::vector<vulkan_submission_token>& _waited_infos);
    [[nodiscard]] scene_texture_data* get(texture_handle _handle) noexcept;

    [[nodiscard]] bool reload_texture(texture_handle _handle, std::vector<vulkan_submission_token>& _waited_infos);
    [[nodiscard]] bool reload_all_textures(std::vector<vulkan_submission_token>& _waited_infos);

    void retain(texture_handle _handle) noexcept;
    void release(texture_handle _handle);

    struct update_result
    {
        bool needs_redraw = false;
        bool layout_moved = false;
    };

    [[nodiscard]] update_result update();

    void need_update() noexcept;

    [[nodiscard]] size_t                               get_textures_size() const noexcept;
    [[nodiscard]] uint32_t                             get_texture_binding(texture_handle _handle) const noexcept;
    [[nodiscard]] std::vector<vk::DescriptorImageInfo> get_descriptor_info() const;

private:
    struct texture_slot
    {
        vulkan_image       image;
        scene_texture_data data;
        uint32_t           binding         = scene_handle<texture_tag>::npos;
        uint32_t           reference_count = 0;
    };

    using texture_key = std::pair<texture_usage, std::filesystem::path>;

    [[nodiscard]] bool destroy(texture_handle _handle);

    [[nodiscard]] std::expected<void, std::string> upload_texture_data(const std::filesystem::path& _source,
                                                                       texture_usage                _usage,
                                                                       vulkan_image&                _image,
                                                                       std::vector<vulkan_submission_token>& _waited_infos) const;
    [[nodiscard]] std::expected<void, std::string> upload_ktx2(const ktx2_texture_ptr&               _ktx2,
                                                               vk::Format                            _target_format,
                                                               vulkan_image&                         _image,
                                                               std::vector<vulkan_submission_token>& _waited_infos,
                                                               const std::string&                    _name) const;

    void                      ensure_sampler(texture_usage _usage);
    [[nodiscard]] vk::Sampler get_sampler(texture_usage _usage) const;

    const vma::raii::Allocator&     allocator;
    const vk::raii::PhysicalDevice& physical_device;
    const vk::raii::Device&         device;
    vulkan_recycle_bin&             recycle_bin;
    const vulkan_queue&             graphic_queue;
    const vulkan_queue&             transfer_queue;

    bool is_dirty     = true;
    bool layout_dirty = false;

    size_t                                            texture_binding_count = 0;
    scene_handle_map<texture_slot, texture_tag>       textures;
    std::map<texture_key, texture_handle>             texture_cache;
    std::unordered_map<texture_usage, vulkan_sampler> samplers;

    vk::Format compressed_hdr_format = vk::Format::eBc6HUfloatBlock;
    vk::Format compressed_ldr_format = vk::Format::eBc7UnormBlock;
};
