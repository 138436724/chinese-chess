#pragma once

#include "scene_material.h"
#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_sampler.h"

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

class vulkan_recycle_bin;
class vulkan_semaphore;
class vulkan_queue;

class scene_material_manager
{
public:
    scene_material_manager(const vma::raii::Allocator&     _allocator,
                           const vk::raii::PhysicalDevice& _physical_device,
                           const vk::raii::Device&         _device,
                           vulkan_recycle_bin&             _recycle_bin,
                           vulkan_semaphore&               _semaphore,
                           const vulkan_queue&             _graphic_queue,
                           const vulkan_queue&             _transfer_queue);
    ~scene_material_manager() = default;

    [[nodiscard]] std::shared_ptr<scene_material> create();
    [[nodiscard]] std::shared_ptr<scene_image>    create(const std::filesystem::path&          _font_path,
                                                         uint32_t                              _font_size,
                                                         const std::wstring&                   _characters,
                                                         std::vector<vk::SemaphoreSubmitInfo>& _waited_infos,
                                                         uint32_t                              _padding);
    [[nodiscard]] std::shared_ptr<scene_image>    create(const std::filesystem::path&          _image_path,
                                                         bool                                  _is_hdr,
                                                         sampler_type                          _type,
                                                         std::vector<vk::SemaphoreSubmitInfo>& _waited_infos);

    [[nodiscard]] bool update(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos);
    void               clear();

    void need_update() noexcept;

    [[nodiscard]] const vulkan_buffer& get_ssbo_buffer() const noexcept;

    [[nodiscard]] std::optional<uint32_t> get_material_index(const std::weak_ptr<scene_material>& _material) const noexcept;
    [[nodiscard]] std::optional<uint32_t> get_texture_index(const std::weak_ptr<scene_image>& _texture) const noexcept;
    [[nodiscard]] std::vector<vk::DescriptorImageInfo> get_descriptor_info() const;

private:
    [[nodiscard]] const vk::raii::Sampler& get_sampler(sampler_type _type) const noexcept;

    void update_ssbo(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos);

    const vma::raii::Allocator&     allocator;
    const vk::raii::PhysicalDevice& physical_device;
    const vk::raii::Device&         device;
    vulkan_recycle_bin&             recycle_bin;
    vulkan_semaphore&               semaphore;
    const vulkan_queue&             graphic_queue;
    const vulkan_queue&             transfer_queue;

    bool is_dirty = true;

    std::vector<std::weak_ptr<scene_material>>                            materials;
    std::vector<std::shared_ptr<scene_image>>                             images;  // need order
    std::unordered_map<std::filesystem::path, std::weak_ptr<scene_image>> images_cache;

    vulkan_sampler diffuse_sampler;
    vulkan_sampler font_sampler;
    vulkan_sampler skybox_sampler;

    vulkan_buffer ssbo;
};
