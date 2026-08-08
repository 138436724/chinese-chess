#pragma once

#include "scene_model.h"
#include "vulkan_core/vulkan_buffer.h"

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

class vulkan_recycle_bin;
class vulkan_semaphore;
class vulkan_queue;
class scene_material_manager;

class scene_model_manager
{
public:
    scene_model_manager(const vma::raii::Allocator&     _allocator,
                        const vk::raii::PhysicalDevice& _physical_device,
                        const vk::raii::Device&         _device,
                        vulkan_recycle_bin&             _recycle_bin,
                        vulkan_semaphore&               _semaphore,
                        const vulkan_queue&             _graphic_queue,
                        const vulkan_queue&             _compute_queue,
                        const vulkan_queue&             _transfer_queue,
                        scene_material_manager&         _material_manager) noexcept;
    ~scene_model_manager() = default;

    [[nodiscard]] std::shared_ptr<scene_model> create(const std::filesystem::path& _model_path);
    void                                       update(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos);
    void                                       clear();

    [[nodiscard]] size_t                               get_models_size() const noexcept;
    [[nodiscard]] const vulkan_buffer&                 get_vertices_buffer() const noexcept;
    [[nodiscard]] const vulkan_buffer&                 get_indices_buffer() const noexcept;
    [[nodiscard]] const vulkan_acceleration_structure& get_tlas() const noexcept;
    [[nodiscard]] const vulkan_buffer&                 get_draw_commands() const noexcept;
    [[nodiscard]] const vulkan_buffer&                 get_ssbo_buffer() const noexcept;

private:
    void update_meshes(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos);
    void update_tlas(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos);
    void update_draw_commands(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos);
    void update_ssbo(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos);

    const vma::raii::Allocator&     allocator;
    const vk::raii::PhysicalDevice& physical_device;
    const vk::raii::Device&         device;
    vulkan_recycle_bin&             recycle_bin;
    vulkan_semaphore&               semaphore;
    const vulkan_queue&             graphic_queue;
    const vulkan_queue&             compute_queue;
    const vulkan_queue&             transfer_queue;
    scene_material_manager&         material_manager;

    std::vector<std::weak_ptr<scene_model>>                                     models;
    std::vector<std::weak_ptr<model_information>>                               meshes;        // submit to gpu in order
    std::unordered_map<std::filesystem::path, std::weak_ptr<model_information>> models_cache;  // no need order

    vulkan_buffer vertices_buffer;
    vulkan_buffer indices_buffer;

    vulkan_acceleration_structure tlas;
    vulkan_buffer                 draw_commands;

    vulkan_buffer ssbo;
};
