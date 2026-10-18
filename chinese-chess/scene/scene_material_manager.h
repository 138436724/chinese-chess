#pragma once

#include "scene_data_type.h"
#include "scene_handle.h"
#include "scene_handle_map.h"
#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_submission_token.h"

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <vector>

class vulkan_recycle_bin;
class vulkan_queue;
class scene_texture_manager;

class scene_material_manager
{
public:
    scene_material_manager(const vma::raii::Allocator& _allocator,
                           const vk::raii::Device&     _device,
                           vulkan_recycle_bin&         _recycle_bin,
                           const vulkan_queue&         _graphic_queue,
                           const vulkan_queue&         _transfer_queue,
                           scene_texture_manager&      _texture_manager) noexcept;
    ~scene_material_manager() = default;

    [[nodiscard]] material_handle      create();
    [[nodiscard]] scene_material_data* get(material_handle _handle) noexcept;
    [[nodiscard]] bool                 destroy(material_handle _handle) noexcept;

    [[nodiscard]] bool update(std::vector<vulkan_submission_token>& _waited_infos);

    void need_update() noexcept;

    void set_texture(material_handle _handle, texture_channel _channel, texture_handle _texture);

    [[nodiscard]] size_t               get_materials_size() const noexcept;
    [[nodiscard]] const vulkan_buffer& get_ssbo_buffer() const noexcept;
    [[nodiscard]] uint32_t             get_material_binding(material_handle _handle) const noexcept;

    [[nodiscard]] auto live_texture_references() const noexcept
    {
        return materials.live()
               | std::views::transform([](const auto& _entry) -> const auto& { return _entry.value.data.textures; })
               | std::views::join;
    }

private:
    struct material_entry
    {
        scene_material_data data;
        uint32_t            binding = scene_handle<material_tag>::npos;
    };

    void update_ssbo(std::vector<vulkan_submission_token>& _waited_infos);

    const vma::raii::Allocator& allocator;
    const vk::raii::Device&     device;
    vulkan_recycle_bin&         recycle_bin;
    const vulkan_queue&         graphic_queue;
    const vulkan_queue&         transfer_queue;
    scene_texture_manager&      texture_manager;

    bool is_dirty = true;

    scene_handle_map<material_entry, material_tag> materials;
    vulkan_buffer                                  ssbo;
};
