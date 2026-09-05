#include "scene_model_manager.h"

#include "scene_material_manager.h"
#include "scene_model.h"
#include "tools/model_loader.h"
#include "vulkan_core/vulkan_commandbuffer.h"
#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>
#include <span>

namespace {
struct model_data  // scalar layout
{
    glm::mat4         model_matrix   = glm::mat4(1.f);
    uint32_t          material_index = std::numeric_limits<uint32_t>::max();
    vk::DeviceAddress vertex_address = 0;
    vk::DeviceAddress index_address  = 0;
};
}  // namespace

scene_model_manager::scene_model_manager(const vma::raii::Allocator&     _allocator,
                                         const vk::raii::PhysicalDevice& _physical_device,
                                         const vk::raii::Device&         _device,
                                         vulkan_recycle_bin&             _recycle_bin,
                                         vulkan_semaphore&               _semaphore,
                                         const vulkan_queue&             _graphic_queue,
                                         const vulkan_queue&             _transfer_queue,
                                         scene_material_manager&         _material_manager) noexcept
    : allocator(_allocator)
    , physical_device(_physical_device)
    , device(_device)
    , recycle_bin(_recycle_bin)
    , semaphore(_semaphore)
    , graphic_queue(_graphic_queue)
    , transfer_queue(_transfer_queue)
    , material_manager(_material_manager)
{
}

std::shared_ptr<scene_model> scene_model_manager::create(const std::filesystem::path& _model_path)
{
    // find in cache
    if (auto iter = models_cache.find(_model_path); iter != models_cache.end())
    {
        if (!iter->second.expired())
        {
            auto model        = std::make_shared<scene_model>();
            model->model_info = std::shared_ptr<model_information>(iter->second);
            models.emplace_back(model);
            is_dirty = true;
            return model;
        }
    }

    // read vertex and index data from model file
    auto mesh         = std::make_shared<model_information>();
    auto loaded_model = model_loader::load_model(_model_path);
    if (!loaded_model)
    {
        throw std::runtime_error(std::format("Failed to load model {}: {}", _model_path.generic_string(), loaded_model.error()));
    }

    mesh->vertices = std::move(loaded_model->vertices);
    mesh->indices  = std::move(loaded_model->indices);

    meshes.emplace_back(mesh);
    models_cache.emplace(_model_path, std::weak_ptr<model_information>(mesh));

    auto model        = std::make_shared<scene_model>();
    model->model_info = mesh;
    models.emplace_back(model);

    is_dirty     = true;
    meshes_dirty = true;
    return model;
}

bool scene_model_manager::update(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    if (std::erase_if(models,
                      [](const auto& p) static {
                          const auto sp = p.lock();
                          return !sp || sp->model_info == nullptr;
                      })
        != 0)
    {
        is_dirty = true;
    }


    const auto retires = std::ranges::stable_partition(meshes, [](const auto& sp) static { return sp.use_count() != 1; });
    meshes_dirty = meshes_dirty || !retires.empty();

    if (retires.begin() != meshes.end())
    {
        std::vector<std::shared_ptr<model_information>> retire_meshes(std::make_move_iterator(retires.begin()),
                                                                      std::make_move_iterator(retires.end()));
        recycle_bin.retire(std::move(retire_meshes), "scene model manager meshes blas.");
        meshes.erase(retires.begin(), retires.end());
        is_dirty = true;
    }

    std::erase_if(models_cache, [this](const auto& p) {
        const auto sp = p.second.lock();
        return !sp || std::ranges::find(meshes, sp) == meshes.end();
    });

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

void scene_model_manager::clear()
{
    models.clear();
    recycle_bin.retire(std::move(meshes), "scene model manager clear meshes.");
    models_cache.clear();
    is_dirty     = true;
    meshes_dirty = true;

    recycle_bin.retire(std::move(vertices_buffer), "scene model manager clear vertices buffer.");
    recycle_bin.retire(std::move(indices_buffer), "scene model manager clear indices buffer.");

    tlas_instance_count = 0;

    recycle_bin.retire(std::move(tlas_scratch_buffer), "scene model manager clear tlas scratch buffer.");
    recycle_bin.retire(std::move(tlas), "scene model manager clear tlas.");
    recycle_bin.retire(std::move(draw_commands), "scene model manager clear draw commands.");
    recycle_bin.retire(std::move(ssbo), "scene model manager clear ssbo.");
}

size_t scene_model_manager::get_models_size() const noexcept
{
    return models.size();
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

void scene_model_manager::need_update() noexcept
{
    is_dirty = true;
}

const vulkan_buffer& scene_model_manager::get_ssbo_buffer() const noexcept
{
    return ssbo;
}

void scene_model_manager::update_meshes(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    if (!meshes.empty())
    {
        vulkan_buffer old_vertices = std::move(vertices_buffer);
        vulkan_buffer old_indices  = std::move(indices_buffer);

        // recreate vertex buffer
        const vk::DeviceSize vertices_size =
            std::ranges::fold_left(meshes, static_cast<size_t>(0), [](size_t s, const auto& sp) static {
                sp->vertex_offset = s;
                return s + sp->vertices.size() * sizeof(sp->vertices.front());
            });

        std::vector<uint8_t> staging_vertex(vertices_size);
        std::ranges::for_each(meshes, [&](const auto& sp) {
            std::memcpy(staging_vertex.data() + sp->vertex_offset, sp->vertices.data(),
                        sp->vertices.size() * sizeof(sp->vertices.front()));
        });

        const auto vertex_upload = vulkan_common::upload_buffer(
            allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue, vertices_buffer,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer
                | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            staging_vertex, "model_vertex");
        _waited_infos.push_back(vertex_upload);


        // recreate index buffer
        const vk::DeviceSize indices_size =
            std::ranges::fold_left(meshes, static_cast<size_t>(0), [](size_t s, const auto& sp) static {
                sp->index_offset = s;
                return s + sp->indices.size() * sizeof(sp->indices.front());
            });

        std::vector<uint8_t> staging_index(indices_size);
        std::ranges::for_each(meshes, [&](const auto& sp) {
            std::memcpy(staging_index.data() + sp->index_offset, sp->indices.data(),
                        sp->indices.size() * sizeof(sp->indices.front()));
        });

        const auto index_upload = vulkan_common::upload_buffer(
            allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue, indices_buffer,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eStorageBuffer
                | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            staging_index, "model_index");
        _waited_infos.push_back(index_upload);

        recycle_bin.retire(std::move(old_vertices), "scene model manager meshes dirty old vertices buffer.");
        recycle_bin.retire(std::move(old_indices), "scene model manager meshes dirty old indices buffer.");


        vulkan_commandbuffer commandbuffer =
            std::move(vulkan_commandbuffer::create(device,
                                                   vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(),
                                                                                 vk::CommandBufferLevel::ePrimary, 1),
                                                   &graphic_queue, &semaphore)
                          .front());
        commandbuffer.add_waited_info(vertex_upload);
        commandbuffer.add_waited_info(index_upload);

        commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

        for (const auto& sp : meshes)
        {
            recycle_bin.retire(std::move(sp->blas_info), "scene model manager update mesh old blas info.");
            sp->blas_info       = vulkan_acceleration_structure();
            auto scratch_buffer = sp->blas_info.create_bottom_level_acceleration_structure(
                physical_device, device, allocator, *commandbuffer, static_cast<uint32_t>(sp->vertices.size()),
                vertices_buffer.get_buffer_address().deviceAddress + sp->vertex_offset,
                static_cast<uint32_t>(sp->indices.size()),
                indices_buffer.get_buffer_address().deviceAddress + sp->index_offset, graphic_queue.get_index());
            recycle_bin.retire(std::move(scratch_buffer), "scene model manager update mesh scratch buffer to create blas.");
        }

        commandbuffer.end_record();
        commandbuffer.submit();

        _waited_infos.push_back(commandbuffer.get_submit_info());
        recycle_bin.retire(std::move(commandbuffer), "scene model manager update mesh commandbuffer.");
    }
}

void scene_model_manager::update_tlas(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    if (models.empty())
    {
        // create a empty tlas
        recycle_bin.retire(std::move(tlas), "ray tracing old tlas.");
        recycle_bin.retire(std::move(tlas_scratch_buffer), "ray tracing old tlas scratch buffer.");

        vulkan_commandbuffer commandbuffer =
            std::move(vulkan_commandbuffer::create(device,
                                                   vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(),
                                                                                 vk::CommandBufferLevel::ePrimary, 1),
                                                   &graphic_queue, &semaphore)
                          .front());
        commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

        tlas_scratch_buffer = tlas.create_top_level_acceleration_structure(physical_device, device, allocator, *commandbuffer,
                                                                           0, {}, graphic_queue.get_index());

        tlas_instance_count = 0;

        commandbuffer.end_record();
        commandbuffer.submit();

        _waited_infos.push_back(commandbuffer.get_submit_info());

        recycle_bin.retire(std::move(commandbuffer), "ray tracing commandbuffer to create tlas.");

        return;
    }

    const auto instances = models
                           | std::views::transform([](const auto& p) static { return p.lock()->get_blas_instance(); })
                           | std::ranges::to<std::vector>();

    vulkan_commandbuffer commandbuffer = std::move(
        vulkan_commandbuffer::create(
            device, vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(), vk::CommandBufferLevel::ePrimary, 1),
            &graphic_queue, &semaphore)
            .front());
    commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    if (tlas_instance_count != 0 && instances.size() == tlas_instance_count)
    {
        vulkan_buffer tlas_instance_buffer;
        commandbuffer.add_waited_info(vulkan_common::upload_buffer(
            allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue, tlas_instance_buffer,
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR
                | vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eTransferDst,
            std::span(reinterpret_cast<const uint8_t*>(instances.data()), sizeof(instances.front()) * instances.size()),
            "tlas_instance"));

        tlas.update_top_level_acceleration_structure(*commandbuffer, static_cast<uint32_t>(instances.size()),
                                                     tlas_instance_buffer.get_buffer_address().deviceAddress,
                                                     tlas_scratch_buffer.get_buffer_address().deviceAddress);

        recycle_bin.retire(std::move(tlas_instance_buffer), "ray tracing old tlas instance buffer.");
    }
    else
    {
        recycle_bin.retire(std::move(tlas), "ray tracing old tlas.");
        recycle_bin.retire(std::move(tlas_scratch_buffer), "ray tracing old tlas scratch buffer.");

        vulkan_buffer tlas_instance_buffer;
        commandbuffer.add_waited_info(vulkan_common::upload_buffer(
            allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue, tlas_instance_buffer,
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR
                | vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eTransferDst,
            std::span(reinterpret_cast<const uint8_t*>(instances.data()), sizeof(instances.front()) * instances.size()),
            "tlas_instance"));

        tlas_scratch_buffer = tlas.create_top_level_acceleration_structure(
            physical_device, device, allocator, *commandbuffer, static_cast<uint32_t>(instances.size()),
            tlas_instance_buffer.get_buffer_address().deviceAddress, graphic_queue.get_index());

        recycle_bin.retire(std::move(tlas_instance_buffer), "ray tracing old tlas instance buffer.");
    }

    tlas_instance_count = instances.size();

    commandbuffer.end_record();
    commandbuffer.submit();

    _waited_infos.push_back(commandbuffer.get_submit_info());

    recycle_bin.retire(std::move(commandbuffer), "ray tracing commandbuffer to create tlas.");
}

void scene_model_manager::update_draw_commands(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    if (!models.empty())
    {
        const auto all_draw_commands =
            models | std::views::transform([](const auto& p) static {
                const auto sp = p.lock();
                if (sp)
                {
                    return vk::DrawIndexedIndirectCommand(
                        static_cast<uint32_t>(sp->model_info->indices.size()), sp->is_show ? 1u : 0u,
                        static_cast<uint32_t>(sp->model_info->index_offset / sizeof(uint32_t)),
                        static_cast<int32_t>(sp->model_info->vertex_offset / sizeof(model_vertex)), 0u);
                }
                else
                {
                    return vk::DrawIndexedIndirectCommand(0u, 0u, 0u, 0, 0u);
                }
            })
            | std::ranges::to<std::vector>();

        _waited_infos.push_back(vulkan_common::upload_buffer(
            allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue, draw_commands,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            std::span(reinterpret_cast<const uint8_t*>(all_draw_commands.data()),
                      sizeof(all_draw_commands.front()) * all_draw_commands.size()),
            "draw_commands"));
    }
}

void scene_model_manager::update_ssbo(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    if (!models.empty())
    {
        // update ssbo
        const auto models_ssbo =
            models | std::views::transform([&](const auto& p) {
                const auto sp = p.lock();
                return model_data{
                    .model_matrix = sp->model_matrix,
                    .material_index =
                        material_manager.get_material_index(sp->material).value_or(std::numeric_limits<uint32_t>::max()),
                    .vertex_address = vertices_buffer.get_buffer_address().deviceAddress + sp->model_info->vertex_offset,
                    .index_address = indices_buffer.get_buffer_address().deviceAddress + sp->model_info->index_offset,
                };
            })
            | std::ranges::to<std::vector>();

        _waited_infos.push_back(vulkan_common::upload_buffer(
            allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue, ssbo,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            std::span(reinterpret_cast<const uint8_t*>(models_ssbo.data()), sizeof(models_ssbo.front()) * models_ssbo.size()),
            "model_ssbo"));
    }
}
