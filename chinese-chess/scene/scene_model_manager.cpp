#include "scene_model_manager.h"
#include <algorithm>
#include <ranges>

struct model_data
{
	alignas(16) glm::mat4 model_matrix = glm::mat4(1.f); // std430 layout
	alignas(8) uint32_t material_index = std::numeric_limits<uint32_t>::max();
	alignas(8) vk::DeviceAddress vertex_address = 0;
	alignas(8) vk::DeviceAddress index_address = 0;
};

scene_model_manager::scene_model_manager(const vulkan_application* _app, const vulkan_queue* _transfer_queue, const scene_material_manager* _manager)
	:app(_app),
	transfer_queue(_transfer_queue),
	manager(_manager)
{
}

std::shared_ptr<scene_model> scene_model_manager::create(const std::filesystem::path& _model_path)
{
	// find in cache
	if (auto iter = std::ranges::find_if(models_cache, [&_model_path](const auto& s) {return s.first == _model_path; }); iter != models_cache.end())
	{
		if (!iter->second.expired())
		{
			auto model = std::make_shared<scene_model>();
			model->model_info = std::shared_ptr<model_infomation>(iter->second);
			models.emplace_back(model);
			return model;
		}
	}

	// read vertex and index data from model file
	auto mesh = std::make_shared<model_infomation>();
	meshs.emplace_back(mesh);
	if (!MODEL_LOADER.load_model(_model_path, mesh->vertices, mesh->indices))
	{
		throw std::runtime_error("read model failed!");
	}

	models_cache.emplace(_model_path, std::weak_ptr<model_infomation>(mesh));

	auto model = std::make_shared<scene_model>();
	model->model_info = mesh;
	models.emplace_back(model);

	return model;
}

void scene_model_manager::update(vulkan_commandbuffer& _commandbuffer) noexcept
{
	std::erase_if(models, [](const auto& p)
		{
			return p.expired();
		});

	std::erase_if(meshs, [](const auto& p)
		{
			return p.expired();
		});

	std::erase_if(models_cache, [](const auto& p)
		{
			return p.second.expired();
		});


	_commandbuffer.add_staging_buffer(std::move(vertices_buffer));
	_commandbuffer.add_staging_buffer(std::move(indices_buffer));
	_commandbuffer.add_staging_buffer(std::move(ssbo));


	// begin commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(transfer_queue->get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), app->get_device(), transfer_queue->get_queue()).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

	update_meshs(commandbuffer);
	update_ssbo(commandbuffer);

	// commandbuffer submit
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);
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

void scene_model_manager::update_meshs(vulkan_commandbuffer& _commandbuffer) noexcept
{
	if (!meshs.empty())
	{
		// recreate vertex buffer
		vk::DeviceSize vertices_size = std::ranges::fold_left(meshs, static_cast<size_t>(0), [](size_t s, const auto& p)
			{
				auto sp = p.lock();
				sp->vertex_offset = s;
				return s + sp->vertices.size() * sizeof(sp->vertices.front());
			});

		vulkan_buffer vertices_staging_buffer;
		vertices_staging_buffer.create(app->get_allocator(), app->get_device(), vertices_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

		std::ranges::for_each(meshs, [&](const auto& p)
			{
				auto sp = p.lock();
				memcpy(static_cast<uint8_t*>(vertices_staging_buffer.get_buffer_address().hostAddress) + sp->vertex_offset, sp->vertices.data(), sp->vertices.size() * sizeof(sp->vertices.front()));
			});

		vertices_buffer.create(app->get_allocator(), app->get_device(), vertices_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);
		vulkan_buffer::copy_buffer_to_buffer(*_commandbuffer, vertices_staging_buffer.get_buffer(), vertices_buffer.get_buffer(), vk::BufferCopy2(0, 0, vertices_size));
		_commandbuffer.add_staging_buffer(std::move(vertices_staging_buffer));

		// recreate index buffer
		vk::DeviceSize indices_size = std::ranges::fold_left(meshs, static_cast<size_t>(0), [](size_t s, const auto& p)
			{
				auto sp = p.lock();
				sp->index_offset = s;
				return s + sp->indices.size() * sizeof(sp->indices.front());
			});

		vulkan_buffer indices_staging_buffer;
		indices_staging_buffer.create(app->get_allocator(), app->get_device(), indices_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

		std::ranges::for_each(meshs, [&](const auto& p)
			{
				auto sp = p.lock();
				memcpy(static_cast<uint8_t*>(indices_staging_buffer.get_buffer_address().hostAddress) + sp->index_offset, sp->indices.data(), sp->indices.size() * sizeof(sp->indices.front()));
			});

		indices_buffer.create(app->get_allocator(), app->get_device(), indices_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);
		vulkan_buffer::copy_buffer_to_buffer(*_commandbuffer, indices_staging_buffer.get_buffer(), indices_buffer.get_buffer(), vk::BufferCopy2(0, 0, indices_size));
		_commandbuffer.add_staging_buffer(std::move(indices_staging_buffer));


		// update blas
		std::ranges::for_each(meshs, [&](const auto& p)
			{
				auto sp = p.lock();
				sp->blas_info.create_bottom_level_acceleration_structure(app->get_physical_device(), app->get_device(), app->get_allocator(), *_commandbuffer,
					static_cast<uint32_t>(sp->vertices.size()), vertices_buffer.get_buffer_address().deviceAddress + sp->vertex_offset,
					static_cast<uint32_t>(sp->indices.size()), indices_buffer.get_buffer_address().deviceAddress + sp->index_offset);
			});
	}
}

void scene_model_manager::update_ssbo(vulkan_commandbuffer& _commandbuffer) noexcept
{
	if (!models.empty())
	{
		// update ssbo
		auto models_ssbo = models
			| std::views::transform([&](const auto& p)
				{
					auto sp = p.lock();
					return model_data{
						.model_matrix = sp->model_matrix,
						.material_index = manager->get_material_index(sp->material).value_or(std::numeric_limits<uint32_t>::max()),
						.vertex_address = vertices_buffer.get_buffer_address().deviceAddress + sp->model_info->vertex_offset,
						.index_address = indices_buffer.get_buffer_address().deviceAddress + sp->model_info->index_offset,
					};
				})
			| std::ranges::to<std::vector>();

		vk::DeviceSize models_ssbo_size = sizeof(models_ssbo.front()) * models_ssbo.size();
		ssbo.create(app->get_allocator(), app->get_device(), models_ssbo_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);

		vulkan_buffer staging_buffer;
		staging_buffer.create(app->get_allocator(), app->get_device(), models_ssbo_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

		memcpy(staging_buffer.get_buffer_address().hostAddress, models_ssbo.data(), models_ssbo_size);
		vulkan_buffer::copy_buffer_to_buffer(*_commandbuffer, staging_buffer.get_buffer(), ssbo.get_buffer(), vk::BufferCopy2(0, 0, models_ssbo_size));
		_commandbuffer.add_staging_buffer(std::move(staging_buffer));
	}
}
