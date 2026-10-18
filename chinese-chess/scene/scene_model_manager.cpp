/**
 * @file scene_model_manager.cpp
 * @brief 模型管理器的实现：网格去重与共享缓冲上传、BLAS/TLAS 构建或 refit、间接命令与 SSBO 上传。
 * @details 所有 GPU 更新都在 update() 的脏门控下完成：先按引用数回收零引用网格，再依次执行
 *          update_meshes → update_tlas → update_draw_commands → update_ssbo。每次上传 / 构建都
 *          提交一次性命令缓冲，并把返回的令牌追加到 waited_infos；旧资源一律先进回收站，
 *          由 scene_manager 在渲染前统一等待后释放。
 */

#include "scene_model_manager.h"
#include "vulkan_core/vulkan_submission_scope.h"

#include "scene_material_manager.h"
#include "tools/model_loader.h"
#include "vulkan_core/vulkan_commandbuffer.h"
#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <print>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

/** @brief 只保存引用，不分配 GPU 资源，因此构造不会与设备或队列交互。 */
scene_model_manager::scene_model_manager(const vma::raii::Allocator&     _allocator,
                                         const vk::raii::PhysicalDevice& _physical_device,
                                         const vk::raii::Device&         _device,
                                         vulkan_recycle_bin&             _recycle_bin,
                                         const vulkan_queue&             _graphic_queue,
                                         const vulkan_queue&             _transfer_queue,
                                         scene_material_manager&         _material_manager) noexcept
    : allocator(_allocator)
    , physical_device(_physical_device)
    , device(_device)
    , recycle_bin(_recycle_bin)
    , graphic_queue(_graphic_queue)
    , transfer_queue(_transfer_queue)
    , material_manager(_material_manager)
{
}

/**
 * @brief 加载模型并新建模型实例；网格按路径去重，加载失败返回无效句柄。
 * @note 网格插入即置 meshes_dirty，真正上传推迟到 update()；模型插入只置 is_dirty。
 */
model_handle scene_model_manager::create(const std::filesystem::path& _model_path)
{
    mesh_handle mesh;

    if (const auto iter = mesh_cache.find(_model_path); iter != mesh_cache.end() && meshes.is_alive(iter->second))
    {
        mesh = iter->second;
    }
    else
    {
        auto loaded_model = model_loader::load_model(_model_path);
        if (!loaded_model)
        {
            std::println(std::cerr, "load model {} failed: {}", _model_path.generic_string(), loaded_model.error());
            return {};
        }

        mesh = meshes.insert(
            scene_mesh_data{.vertices = std::move(loaded_model->vertices), .indices = std::move(loaded_model->indices)});
        mesh_cache.insert_or_assign(_model_path, mesh);
        meshes_dirty = true;
    }

    is_dirty = true;
    return models.insert(scene_model_data{.mesh = mesh});
}

scene_model_data* scene_model_manager::get(model_handle _handle) noexcept
{
    return models.get(_handle);
}

bool scene_model_manager::destroy(model_handle _handle) noexcept
{
    if (!models.erase(_handle))
    {
        return false;
    }

    is_dirty = true;
    return true;
}

/**
 * @brief 更新入口：先按引用数回收网格，再按脏标志重建 GPU 数据。
 * @note 网格引用数每轮都重算；只有网格集合变化才重建顶点 / 索引与 BLAS，其余按 is_dirty 决定。
 * @note 只有全部步骤成功才清脏标志，中途异常时保持脏状态，下一帧重试。
 */
bool scene_model_manager::update(std::vector<vulkan_submission_token>& _waited_infos)
{
    for (auto&& mesh : meshes.live())
    {
        mesh.value.reference_count = 0;
    }

    for (auto&& model : models.live())
    {
        if (auto* mesh = meshes.get(model.value.mesh); mesh != nullptr)
        {
            ++mesh->reference_count;
        }
    }

    const auto dead =
        meshes.live() | std::views::filter([](const auto& _entry) { return _entry.value.reference_count == 0; })
        | std::views::transform([](const auto& _entry) { return _entry.handle; }) | std::ranges::to<std::vector>();

    for (const auto handle : dead)
    {
        if (auto* slot = meshes.get(handle); slot != nullptr)
        {
            recycle_bin.retire_after_submitted(std::move(slot->blas), "scene model manager unused mesh blas.");
        }

        std::ignore = meshes.erase(handle);
        std::erase_if(mesh_cache, [handle](const auto& _pair) { return _pair.second == handle; });

        meshes_dirty = true;
        is_dirty     = true;
    }

    if (!is_dirty)
    {
        return false;
    }

    if (meshes_dirty)
    {
        update_meshes(_waited_infos);
    }

    update_tlas(_waited_infos);
    update_draw_commands(_waited_infos);
    update_ssbo(_waited_infos);

    meshes_dirty = false;
    is_dirty     = false;

    return true;
}

size_t scene_model_manager::get_models_size() const noexcept
{
    return models.live_size();
}

const vulkan_buffer& scene_model_manager::get_vertices_buffer() const noexcept
{
    return vertices_buffer;
}

const vulkan_buffer& scene_model_manager::get_indices_buffer() const noexcept
{
    return indices_buffer;
}

const vulkan_acceleration_structure& scene_model_manager::get_tlas() const noexcept
{
    return tlas;
}

const vulkan_buffer& scene_model_manager::get_draw_commands() const noexcept
{
    return draw_commands;
}

/** @brief 只置 SSBO 脏标志，不触发网格 / BLAS 重建。 */
void scene_model_manager::need_update() noexcept
{
    is_dirty = true;
}

const vulkan_buffer& scene_model_manager::get_ssbo_buffer() const noexcept
{
    return ssbo;
}

/**
 * @brief 重建共享顶点 / 索引缓冲并重建全部网格的 BLAS。
 * @note 两种缓冲都按网格槽序紧凑排布，字节偏移写回各槽；旧缓冲先退休再被新缓冲替换。
 * @note 先在 CPU 侧拼好整块 staging 再分两次 upload_buffer，最后用一次性命令缓冲录 BLAS 并提交。
 */
void scene_model_manager::update_meshes(std::vector<vulkan_submission_token>& _waited_infos)
{
    if (meshes.empty())
    {
        return;
    }

    vulkan_buffer old_vertices = std::move(vertices_buffer);
    vulkan_buffer old_indices  = std::move(indices_buffer);
    std::vector<vulkan_buffer> scratch_buffers;
    const vulkan_submission_scope failure_scope(graphic_queue, transfer_queue);

    size_t vertices_size = 0;
    for (auto&& mesh : meshes.live())
    {
        mesh.value.vertex_offset = vertices_size;
        vertices_size += mesh.value.vertices.size() * sizeof(vulkan_model_vertex);
    }

    std::vector<uint8_t> staging_vertex(vertices_size);
    for (auto&& mesh : meshes.live())
    {
        std::memcpy(staging_vertex.data() + mesh.value.vertex_offset, mesh.value.vertices.data(),
                    mesh.value.vertices.size() * sizeof(vulkan_model_vertex));
    }

    const auto vertex_upload = vulkan_common::upload_buffer(
        allocator, device, recycle_bin, graphic_queue, transfer_queue, vertices_buffer,
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer
            | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress,
        vk::PipelineStageFlagBits2::eVertexAttributeInput | vk::PipelineStageFlagBits2::eRayTracingShaderKHR
            | vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eAccelerationStructureReadKHR,
        staging_vertex, "model_vertex");
    _waited_infos.push_back(vertex_upload);


    // recreate index buffer
    size_t indices_size = 0;
    for (auto&& mesh : meshes.live())
    {
        mesh.value.index_offset = indices_size;
        indices_size += mesh.value.indices.size() * sizeof(uint32_t);
    }

    std::vector<uint8_t> staging_index(indices_size);
    for (auto&& mesh : meshes.live())
    {
        std::memcpy(staging_index.data() + mesh.value.index_offset, mesh.value.indices.data(),
                    mesh.value.indices.size() * sizeof(uint32_t));
    }

    const auto index_upload = vulkan_common::upload_buffer(
        allocator, device, recycle_bin, graphic_queue, transfer_queue, indices_buffer,
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eStorageBuffer
            | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress,
        vk::PipelineStageFlagBits2::eIndexInput | vk::PipelineStageFlagBits2::eRayTracingShaderKHR
            | vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        vk::AccessFlagBits2::eIndexRead | vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eAccelerationStructureReadKHR,
        staging_index, "model_index");
    _waited_infos.push_back(index_upload);

    recycle_bin.retire_after_submitted(std::move(old_vertices), "scene model manager meshes dirty old vertices buffer.");
    recycle_bin.retire_after_submitted(std::move(old_indices), "scene model manager meshes dirty old indices buffer.");


    vulkan_commandbuffer commandbuffer = std::move(
        vulkan_commandbuffer::create(
            device, vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(), vk::CommandBufferLevel::ePrimary, 1),
            &graphic_queue)
            .front());
    commandbuffer.add_dependency(vertex_upload);
    commandbuffer.add_dependency(index_upload);

    commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    scratch_buffers.reserve(meshes.live_size());

    for (auto&& entry : meshes.live())
    {
        scene_mesh_data& mesh = entry.value;
        recycle_bin.retire_after_submitted(std::move(mesh.blas), "scene model manager update mesh old blas info.");

        mesh.blas           = vulkan_acceleration_structure();
        auto scratch_buffer = mesh.blas.create_bottom_level_acceleration_structure(
            physical_device, device, allocator, *commandbuffer, static_cast<uint32_t>(mesh.vertices.size()),
            vertices_buffer.get_buffer_address().deviceAddress + mesh.vertex_offset, static_cast<uint32_t>(mesh.indices.size()),
            indices_buffer.get_buffer_address().deviceAddress + mesh.index_offset, graphic_queue.get_index());

        scratch_buffers.push_back(std::move(scratch_buffer));
    }

    commandbuffer.end_record();
    const auto build_token = commandbuffer.submit();

    // Scratch buffers are last read by the BLAS build above; retire them with
    // the token of that submission, never before it is submitted.
    for (auto& scratch_buffer : scratch_buffers)
    {
        recycle_bin.retire(std::move(scratch_buffer), build_token, "scene model manager update mesh scratch buffer to create blas.");
    }

    _waited_infos.push_back(build_token);
    recycle_bin.retire(std::move(commandbuffer), build_token, "scene model manager update mesh commandbuffer.");
}

/**
 * @brief 重建或 refit TLAS。
 * @note 上次实例数非零且本次不变时只上传新实例并 refit（复用 scratch），否则重建结构并重新分配 scratch。
 * @note 没有模型时建 0 实例 TLAS；实例的 mask 由 is_show 决定（0xFF / 0）。
 */
void scene_model_manager::update_tlas(std::vector<vulkan_submission_token>& _waited_infos)
{
    if (models.empty())
    {
        // create a empty tlas
        recycle_bin.retire_after_submitted(std::move(tlas), "ray tracing old tlas.");
        recycle_bin.retire_after_submitted(std::move(tlas_scratch_buffer), "ray tracing old tlas scratch buffer.");

        vulkan_commandbuffer commandbuffer =
            std::move(vulkan_commandbuffer::create(device,
                                                   vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(),
                                                                                 vk::CommandBufferLevel::ePrimary, 1),
                                                   &graphic_queue)
                          .front());
        commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

        tlas_scratch_buffer = tlas.create_top_level_acceleration_structure(physical_device, device, allocator, *commandbuffer,
                                                                           0, {}, graphic_queue.get_index());

        tlas_instance_count = 0;

        commandbuffer.end_record();
        const auto build_token = commandbuffer.submit();

        _waited_infos.push_back(build_token);

        recycle_bin.retire(std::move(commandbuffer), build_token, "ray tracing commandbuffer to create tlas.");

        return;
    }

    const auto instances =
        models.live()
        | std::views::filter([this](const auto& _entry) { return meshes.get(_entry.value.mesh) != nullptr; })
        | std::views::transform([this](const auto& _entry) {
              const scene_model_data& model = _entry.value;
              const auto*             mesh  = meshes.get(model.mesh);
              return vk::AccelerationStructureInstanceKHR(vulkan_common::glm_matrix_to_vulkan(model.model_matrix), 0u,
                                                          model.is_show ? 0xFF : 0, 0u, {}, mesh->blas.get_address());
          })
        | std::ranges::to<std::vector>();

    vulkan_commandbuffer commandbuffer = std::move(
        vulkan_commandbuffer::create(
            device, vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(), vk::CommandBufferLevel::ePrimary, 1),
            &graphic_queue)
            .front());
    commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    vulkan_buffer tlas_instance_buffer;
    const vulkan_submission_scope failure_scope(graphic_queue, transfer_queue);

    if (tlas_instance_count != 0 && instances.size() == tlas_instance_count)
    {
        commandbuffer.add_dependency(vulkan_common::upload_buffer(
            allocator, device, recycle_bin, graphic_queue, transfer_queue, tlas_instance_buffer,
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR
                | vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eTransferDst,
            vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR, vk::AccessFlagBits2::eAccelerationStructureReadKHR,
            std::span(reinterpret_cast<const uint8_t*>(instances.data()),
                      sizeof(decltype(instances)::value_type) * instances.size()),
            "tlas_instance"));

        tlas.update_top_level_acceleration_structure(*commandbuffer, static_cast<uint32_t>(instances.size()),
                                                     tlas_instance_buffer.get_buffer_address().deviceAddress,
                                                     tlas_scratch_buffer.get_buffer_address().deviceAddress);
    }
    else
    {
        recycle_bin.retire_after_submitted(std::move(tlas), "ray tracing old tlas.");
        recycle_bin.retire_after_submitted(std::move(tlas_scratch_buffer), "ray tracing old tlas scratch buffer.");

        commandbuffer.add_dependency(vulkan_common::upload_buffer(
            allocator, device, recycle_bin, graphic_queue, transfer_queue, tlas_instance_buffer,
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR
                | vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eTransferDst,
            vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR, vk::AccessFlagBits2::eAccelerationStructureReadKHR,
            std::span(reinterpret_cast<const uint8_t*>(instances.data()),
                      sizeof(decltype(instances)::value_type) * instances.size()),
            "tlas_instance"));

        tlas_scratch_buffer = tlas.create_top_level_acceleration_structure(
            physical_device, device, allocator, *commandbuffer, static_cast<uint32_t>(instances.size()),
            tlas_instance_buffer.get_buffer_address().deviceAddress, graphic_queue.get_index());
    }

    tlas_instance_count = instances.size();

    commandbuffer.end_record();
    const auto build_token = commandbuffer.submit();

    // Instance buffer and command buffer are last read by this submission.
    recycle_bin.retire(std::move(tlas_instance_buffer), build_token, "ray tracing tlas instance buffer.");
    _waited_infos.push_back(build_token);

    recycle_bin.retire(std::move(commandbuffer), build_token, "ray tracing commandbuffer to create tlas.");
}

/**
 * @brief 生成间接绘制命令：索引数 / 实例数 / 首索引 / 顶点偏移全部来自网格槽的字节偏移。
 * @note 实例数只取 0 或 1；空场景直接返回，避免上传一条空缓冲。
 */
void scene_model_manager::update_draw_commands(std::vector<vulkan_submission_token>& _waited_infos)
{
    if (models.empty())
    {
        return;
    }

    const auto all_draw_commands =
        models.live() | std::views::transform([this](const auto& _entry) {
            const scene_model_data& model = _entry.value;
            const auto*             mesh  = meshes.get(model.mesh);
            if (mesh == nullptr)
            {
                return vk::DrawIndexedIndirectCommand(0u, 0u, 0u, 0, 0u);
            }
            return vk::DrawIndexedIndirectCommand(static_cast<uint32_t>(mesh->indices.size()), model.is_show ? 1u : 0u,
                                                  static_cast<uint32_t>(mesh->index_offset / sizeof(uint32_t)),
                                                  static_cast<int32_t>(mesh->vertex_offset / sizeof(vulkan_model_vertex)), 0u);
        })
        | std::ranges::to<std::vector>();

    _waited_infos.push_back(vulkan_common::upload_buffer(
        allocator, device, recycle_bin, graphic_queue, transfer_queue, draw_commands,
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
        vk::PipelineStageFlagBits2::eDrawIndirect, vk::AccessFlagBits2::eIndirectCommandRead,
        std::span(reinterpret_cast<const uint8_t*>(all_draw_commands.data()),
                  sizeof(decltype(all_draw_commands)::value_type) * all_draw_commands.size()),
        "draw_commands"));
}

/**
 * @brief 生成 vulkan_model_data 数组：模型矩阵 + 材质紧凑绑定 + 顶点 / 索引设备地址。
 * @note 地址取共享缓冲的 deviceAddress，所以缓冲重建后这些地址自动更新。
 * @note 材质绑定变化由 scene_manager 的 need_material_update + model_manager.need_update 驱动。
 */
void scene_model_manager::update_ssbo(std::vector<vulkan_submission_token>& _waited_infos)
{
    if (models.empty())
    {
        return;
    }

    const vk::DeviceAddress vertices_address = vertices_buffer.get_buffer_address().deviceAddress;
    const vk::DeviceAddress indices_address  = indices_buffer.get_buffer_address().deviceAddress;

    const auto models_ssbo = models.live() | std::views::transform([this, vertices_address, indices_address](const auto& _entry) {
                                 const scene_model_data& model = _entry.value;
                                 const auto*             mesh  = meshes.get(model.mesh);
                                 return vulkan_model_data{
                                     .model_matrix   = model.model_matrix,
                                     .material_index = material_manager.get_material_binding(model.material),
                                     .vertex_address = mesh != nullptr ? vertices_address + mesh->vertex_offset : 0,
                                     .index_address  = mesh != nullptr ? indices_address + mesh->index_offset : 0,
                                 };
                             })
                             | std::ranges::to<std::vector>();

    _waited_infos.push_back(vulkan_common::upload_buffer(
        allocator, device, recycle_bin, graphic_queue, transfer_queue, ssbo,
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
        vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader
            | vk::PipelineStageFlagBits2::eRayTracingShaderKHR | vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderRead,
        std::span(reinterpret_cast<const uint8_t*>(models_ssbo.data()),
                  sizeof(decltype(models_ssbo)::value_type) * models_ssbo.size()),
        "model_ssbo"));
}
