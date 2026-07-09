#include "scene_model_manager.h"

#include "vulkan_core/vulkan_common.h"

#include <algorithm>
#include <ranges>

struct model_data  // std430 layout
{
    alignas(16) glm::mat4 model_matrix          = glm::mat4(1.f);
    alignas(8) uint32_t material_index          = std::numeric_limits<uint32_t>::max();
    alignas(8) vk::DeviceAddress vertex_address = 0;
    alignas(8) vk::DeviceAddress index_address  = 0;
};

scene_model_manager::scene_model_manager(vulkan_application&     _app,
                                         vulkan_recycle_bin&     _recycle_bin,
                                         scene_material_manager& _manager,
                                         vulkan_queue&           _graphic_queue,
                                         vulkan_queue&           _computr_queue,
                                         vulkan_queue&           _transfer_queue)
    : app(_app)
    , recycle_bin(_recycle_bin)
    , manager(_manager)
    , graphic_queue(_graphic_queue)
    , computr_queue(_computr_queue)
    , transfer_queue(_transfer_queue)
{
}

std::shared_ptr<scene_model> scene_model_manager::create(const std::filesystem::path& _model_path)
{
    // find in cache
    if (auto iter = std::ranges::find_if(models_cache, [&_model_path](const auto& s) { return s.first == _model_path; });
        iter != models_cache.end())
    {
        if (!iter->second.expired())
        {
            auto model        = std::make_shared<scene_model>();
            model->model_info = std::shared_ptr<model_infomation>(iter->second);
            models.emplace_back(model);
            return model;
        }
    }

    // read vertex and index data from model file
    auto mesh = std::make_shared<model_infomation>();
    if (!MODEL_LOADER.load_model(_model_path, mesh->vertices, mesh->indices))
    {
        throw std::runtime_error("read model failed!");
    }

    meshs.emplace_back(mesh);
    models_cache.emplace(_model_path, std::weak_ptr<model_infomation>(mesh));

    auto model        = std::make_shared<scene_model>();
    model->model_info = mesh;
    models.emplace_back(model);

    return model;
}

void scene_model_manager::update(std::vector<vk::SemaphoreSubmitInfo>& _wait_info) noexcept
{
    std::erase_if(models, [](const auto& p) { return p.expired(); });
    std::erase_if(meshs, [](const auto& p) { return p.expired(); });
    std::erase_if(models_cache, [](const auto& p) { return p.second.expired(); });

    recycle_bin.retire(std::move(vertices_buffer));
    recycle_bin.retire(std::move(indices_buffer));
    recycle_bin.retire(std::move(ssbo));

    update_meshs(_wait_info);
    update_ssbo(_wait_info);
}

void scene_model_manager::clear() noexcept
{
    models.clear();
    meshs.clear();
    models_cache.clear();

    vertices_buffer.clear();
    indices_buffer.clear();
}

const std::vector<std::weak_ptr<scene_model>>& scene_model_manager::get_models() const noexcept
{
    return models;
}

const vulkan_buffer& scene_model_manager::get_vertices_buffer() const noexcept
{
    return vertices_buffer;
}

const vulkan_buffer& scene_model_manager::get_indices_buffer() const noexcept
{
    return indices_buffer;
}

const vulkan_buffer& scene_model_manager::get_ssbo_buffer() const noexcept
{
    return ssbo;
}

void scene_model_manager::update_meshs(std::vector<vk::SemaphoreSubmitInfo>& _wait_info) noexcept
{
    if (!meshs.empty())
    {
        // recreate vertex buffer
        vk::DeviceSize vertices_size = std::ranges::fold_left(meshs, static_cast<size_t>(0), [](size_t s, const auto& p) {
            auto sp           = p.lock();
            sp->vertex_offset = s;
            return s + sp->vertices.size() * sizeof(sp->vertices.front());
        });

        std::vector<uint8_t> staging_vertex(vertices_size);
        std::ranges::for_each(meshs, [&](const auto& p) {
            auto sp = p.lock();
            memcpy(staging_vertex.data() + sp->vertex_offset, sp->vertices.data(),
                   sp->vertices.size() * sizeof(sp->vertices.front()));
        });

        _wait_info.push_back(vulkan_common::upload_buffer(
            app.get_allocator(), *app.get_device(), recycle_bin, app.get_semaphore_ptr(), graphic_queue, transfer_queue, vertices_buffer,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer
                | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            staging_vertex));


        // recreate index buffer
        vk::DeviceSize indices_size = std::ranges::fold_left(meshs, static_cast<size_t>(0), [](size_t s, const auto& p) {
            auto sp          = p.lock();
            sp->index_offset = s;
            return s + sp->indices.size() * sizeof(sp->indices.front());
        });

        std::vector<uint8_t> staging_index(indices_size);
        std::ranges::for_each(meshs, [&](const auto& p) {
            auto sp = p.lock();
            memcpy(staging_index.data() + sp->index_offset, sp->indices.data(), sp->indices.size() * sizeof(sp->indices.front()));
        });

        _wait_info.push_back(vulkan_common::upload_buffer(
            app.get_allocator(), *app.get_device(), recycle_bin, app.get_semaphore_ptr(), graphic_queue, transfer_queue, indices_buffer,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eStorageBuffer
                | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            staging_index));


        // update blas todo use computr_queue build
        vulkan_commandbuffer commandbuffer =
            std::move(vulkan_commandbuffer::create(*app.get_device(),
                                                   vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(),
                                                                                 vk::CommandBufferLevel::ePrimary, 1),
                                                   &graphic_queue, app.get_semaphore_ptr())
                          .front());
        commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

        std::ranges::for_each(meshs, [&](const auto& p) {
            auto sp = p.lock();
            recycle_bin.retire(std::move(sp->blas_info));
            sp->blas_info       = vulkan_acceleration_structure();
            auto scratch_buffer = sp->blas_info.create_bottom_level_acceleration_structure(
                *app.get_physical_device(), *app.get_device(), app.get_allocator(), *commandbuffer,
                static_cast<uint32_t>(sp->vertices.size()), vertices_buffer.get_buffer_address().deviceAddress + sp->vertex_offset,
                static_cast<uint32_t>(sp->indices.size()),
                indices_buffer.get_buffer_address().deviceAddress + sp->index_offset, graphic_queue.get_index());
            recycle_bin.retire(std::move(scratch_buffer));
        });

        commandbuffer.end_record();
        commandbuffer.submit(false);

        _wait_info.push_back(commandbuffer.get_submit_info());
        recycle_bin.retire(std::move(commandbuffer));
    }
}

void scene_model_manager::update_ssbo(std::vector<vk::SemaphoreSubmitInfo>& _wait_info) noexcept
{
    if (!models.empty())
    {
        // update ssbo
        auto models_ssbo =
            models | std::views::transform([&](const auto& p) {
                auto sp = p.lock();
                return model_data{
                    .model_matrix = sp->model_matrix,
                    .material_index = manager.get_material_index(sp->material).value_or(std::numeric_limits<uint32_t>::max()),
                    .vertex_address = vertices_buffer.get_buffer_address().deviceAddress + sp->model_info->vertex_offset,
                    .index_address = indices_buffer.get_buffer_address().deviceAddress + sp->model_info->index_offset,
                };
            })
            | std::ranges::to<std::vector>();

        _wait_info.push_back(vulkan_common::upload_buffer(
            app.get_allocator(), *app.get_device(), recycle_bin, app.get_semaphore_ptr(), graphic_queue, transfer_queue, ssbo,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            std::span(reinterpret_cast<uint8_t*>(models_ssbo.data()), sizeof(models_ssbo.front()) * models_ssbo.size())));
    }
}
