#pragma once

#include "scene_data_type.h"
#include "scene_handle.h"
#include "scene_handle_map.h"
#include "vulkan_core/vulkan_acceleration_structure.h"
#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_submission_token.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

class vulkan_recycle_bin;
class vulkan_queue;
class scene_material_manager;

class scene_model_manager
{
public:
    scene_model_manager(const vma::raii::Allocator&     _allocator,
                        const vk::raii::PhysicalDevice& _physical_device,
                        const vk::raii::Device&         _device,
                        vulkan_recycle_bin&             _recycle_bin,
                        const vulkan_queue&             _graphic_queue,
                        const vulkan_queue&             _transfer_queue,
                        scene_material_manager&         _material_manager) noexcept;
    ~scene_model_manager() = default;

    [[nodiscard]] model_handle      create(const std::filesystem::path& _model_path);
    [[nodiscard]] scene_model_data* get(model_handle _handle) noexcept;
    [[nodiscard]] bool              destroy(model_handle _handle) noexcept;

    [[nodiscard]] bool update(std::vector<vulkan_submission_token>& _waited_infos);

    void need_update() noexcept;

    [[nodiscard]] size_t                               get_models_size() const noexcept;
    [[nodiscard]] const vulkan_buffer&                 get_vertices_buffer() const noexcept;
    [[nodiscard]] const vulkan_buffer&                 get_indices_buffer() const noexcept;
    [[nodiscard]] const vulkan_acceleration_structure& get_tlas() const noexcept;
    [[nodiscard]] const vulkan_buffer&                 get_draw_commands() const noexcept;
    [[nodiscard]] const vulkan_buffer&                 get_ssbo_buffer() const noexcept;

private:
    struct scene_mesh_data
    {
        std::vector<vulkan_model_vertex> vertices;
        std::vector<uint32_t>            indices;
        size_t                           vertex_offset   = 0;
        size_t                           index_offset    = 0;
        uint32_t                         reference_count = 0;
        vulkan_acceleration_structure    blas;
    };

    void update_meshes(std::vector<vulkan_submission_token>& _waited_infos);
    void update_tlas(std::vector<vulkan_submission_token>& _waited_infos);
    void update_draw_commands(std::vector<vulkan_submission_token>& _waited_infos);
    void update_ssbo(std::vector<vulkan_submission_token>& _waited_infos);

    const vma::raii::Allocator&     allocator;
    const vk::raii::PhysicalDevice& physical_device;
    const vk::raii::Device&         device;
    vulkan_recycle_bin&             recycle_bin;
    const vulkan_queue&             graphic_queue;
    const vulkan_queue&             transfer_queue;
    scene_material_manager&         material_manager;

    bool is_dirty     = true;
    bool meshes_dirty = true;

    scene_handle_map<scene_mesh_data, mesh_tag>            meshes;  // submit to gpu in slot order
    scene_handle_map<scene_model_data, model_tag>          models;
    std::unordered_map<std::filesystem::path, mesh_handle> mesh_cache;

    vulkan_buffer vertices_buffer;
    vulkan_buffer indices_buffer;

    size_t                        tlas_instance_count = 0;
    vulkan_buffer                 tlas_scratch_buffer;
    vulkan_acceleration_structure tlas;

    vulkan_buffer draw_commands;

    vulkan_buffer ssbo;
};
