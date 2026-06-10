#include "scene_model_manager.h"
#include <algorithm>
#include <ranges>

scene_model_manager::scene_model_manager(vulkan_application* _app)
	:app(_app)
{
}

std::shared_ptr<scene_model> scene_model_manager::create_node(const std::u8string& _model_path)
{
	// find in cache
	if (auto iter = std::ranges::find_if(models_cache, [&_model_path](const auto& s) {return s.first == _model_path; }); iter != models_cache.end())
	{
		if (!std::get<0>(iter->second).expired() && !std::get<1>(iter->second).expired())
		{
			auto node = std::make_shared<scene_model>(std::shared_ptr<model_infomation>(std::get<0>(iter->second)), std::shared_ptr<vulkan_acceleration_structure>(std::get<1>(iter->second)));
			nodes.emplace_back(node);
			return node;
		}
	}

	// begin commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), app->get_device(), app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue()).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

	// read vertex and index data from model file
	auto model = std::make_shared<model_infomation>();
	models.emplace_back(model);
	if (!MODEL_LOADER.load_model(_model_path, model->vertices, model->indices))
	{
		throw std::runtime_error("read model failed!");
	}

	reload_buffer(commandbuffer);

	// create blas
	auto blas = std::make_shared<vulkan_acceleration_structure>();
	blas->create_bottom_level_acceleration_structure(app->get_physical_device(), app->get_device(), *commandbuffer,
		static_cast<uint32_t>(model->vertices.size()), vertices_buffer.get_buffer_address().deviceAddress + model->vertex_offset,
		static_cast<uint32_t>(model->indices.size()), indices_buffer.get_buffer_address().deviceAddress + model->index_offset);

	// submit commandbuffer
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);


	models_cache.emplace(_model_path, std::make_tuple(std::weak_ptr<model_infomation>(model), std::weak_ptr<vulkan_acceleration_structure>(blas)));
	auto node = std::make_shared<scene_model>(model, blas);
	nodes.emplace_back(node);
	return node;
}

void scene_model_manager::clear_unused_nodes(bool _need_reload/* = true*/) noexcept
{
	std::erase_if(nodes, [](const auto& p)
		{
			return p.expired();
		});

	std::erase_if(models, [](const auto& p)
		{
			return p.expired();
		});

	std::erase_if(models_cache, [](const auto& p)
		{
			return std::get<0>(p.second).expired();
		});


	if (_need_reload)
	{
		if (models.empty())
		{
			vertices_buffer.clear();
			indices_buffer.clear();
		}
		else
		{
			// begin commandbuffer
			vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), app->get_device(), app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue()).front());
			commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

			reload_buffer(commandbuffer);

			// submit commandbuffer
			commandbuffer.end_record();
			commandbuffer.submit({}, {}, true);
		}
	}
}

void scene_model_manager::reload_buffer(vulkan_commandbuffer& _commandbuffer) noexcept
{
	// recreate vertex buffer
	vk::DeviceSize vertices_size = std::ranges::fold_left(models, static_cast<size_t>(0), [](size_t s, const auto& p)
		{
			const auto& sp = p.lock();
			sp->vertex_offset = s;
			return s + sp->vertices.size() * sizeof(sp->vertices.front());
		});

	vulkan_buffer vertices_staging_buffer;
	vertices_staging_buffer.create(app->get_physical_device(), app->get_device(), vertices_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

	std::ranges::for_each(models, [&](const auto& p)
		{
			const auto& sp = p.lock();
			memcpy(static_cast<uint8_t*>(vertices_staging_buffer.get_buffer_address().hostAddress) + sp->vertex_offset, sp->vertices.data(), sp->vertices.size() * sizeof(sp->vertices.front()));
		});

	vertices_buffer.create(app->get_physical_device(), app->get_device(), vertices_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);
	vulkan_buffer::copy_buffer_to_buffer(*_commandbuffer, vertices_staging_buffer.get_buffer(), vertices_buffer.get_buffer(), vk::BufferCopy2(0, 0, vertices_size));
	_commandbuffer.add_staging_buffer(std::move(vertices_staging_buffer));


	// recreate index buffer
	vk::DeviceSize indices_size = std::ranges::fold_left(models, static_cast<size_t>(0), [](size_t s, const auto& p)
		{
			const auto& sp = p.lock();
			sp->index_offset = s;
			return s + sp->indices.size() * sizeof(sp->indices.front());
		});

	vulkan_buffer indices_staging_buffer;
	indices_staging_buffer.create(app->get_physical_device(), app->get_device(), indices_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

	std::ranges::for_each(models, [&](const auto& p)
		{
			const auto& sp = p.lock();
			memcpy(static_cast<uint8_t*>(indices_staging_buffer.get_buffer_address().hostAddress) + sp->index_offset, sp->indices.data(), sp->indices.size() * sizeof(sp->indices.front()));
		});

	indices_buffer.create(app->get_physical_device(), app->get_device(), indices_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);
	vulkan_buffer::copy_buffer_to_buffer(*_commandbuffer, indices_staging_buffer.get_buffer(), indices_buffer.get_buffer(), vk::BufferCopy2(0, 0, indices_size));
	_commandbuffer.add_staging_buffer(std::move(indices_staging_buffer));
}

const vulkan_buffer& scene_model_manager::get_vertices_buffer() const noexcept
{
	return vertices_buffer;
}

const vulkan_buffer& scene_model_manager::get_indices_buffer() const noexcept
{
	return indices_buffer;
}
