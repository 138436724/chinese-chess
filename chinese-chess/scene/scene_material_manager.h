#pragma once

#include "scene_material.h"
#include "vulkan_core/vulkan_application.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

class scene_material_manager
{
public:
    scene_material_manager(vulkan_application& _app, vulkan_recycle_bin& _recycle_bin, vulkan_queue& _graphic_queue, vulkan_queue& _transfer_queue);
    ~scene_material_manager() = default;

    std::shared_ptr<scene_material> create();
    std::shared_ptr<scene_image>    create(const std::filesystem::path&          _font_path,
                                           uint32_t                              _font_size,
                                           const std::wstring&                   _characters,
                                           std::vector<vk::SemaphoreSubmitInfo>& _wait_info);
    std::shared_ptr<scene_image>    create(const std::filesystem::path&          _image_path,
                                           bool                                  _is_hdr,
                                           std::vector<vk::SemaphoreSubmitInfo>& _wait_info);
    void                            update(std::vector<vk::SemaphoreSubmitInfo>& _wait_info) noexcept;
    void                            clear() noexcept;

    const vulkan_buffer& get_ssbo_buffer() const noexcept;

    std::optional<uint32_t> get_material_index(const std::weak_ptr<scene_material>& _material) const noexcept;
    std::optional<uint32_t> get_texture_index(const std::weak_ptr<scene_image>& _texture) const noexcept;
    std::vector<vk::DescriptorImageInfo> get_descriptor_info(const std::span<vk::Sampler> _samplers) const;

private:
    void update_ssbo(std::vector<vk::SemaphoreSubmitInfo>& _wait_info) noexcept;

    vulkan_application& app;
    vulkan_recycle_bin& recycle_bin;

    vulkan_queue& graphic_queue;
    vulkan_queue& transfer_queue;

    std::vector<std::weak_ptr<scene_material>>                            materials;
    std::vector<std::weak_ptr<scene_image>>                               images;  // need order
    std::unordered_map<std::filesystem::path, std::weak_ptr<scene_image>> images_cache;

    vulkan_buffer ssbo;
};
