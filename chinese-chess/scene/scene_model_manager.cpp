#include "scene_model_manager.h"
#include <algorithm>
#include <ranges>

scene_model_manager::scene_model_manager(vulkan_application* _app, scene_material_manager* _manager)
	:app(_app),
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

void scene_model_manager::remove(const std::weak_ptr<scene_model>& _model) noexcept
{
	std::owner_less<void> cmp;
	std::erase_if(meshs, [&](const auto& p)
		{
			return !cmp(p, _model.lock()->model_info) && !cmp(_model.lock()->model_info, p);
		});
	std::erase_if(models, [&](const auto& p)
		{
			return !cmp(p, _model) && !cmp(_model, p);
		});
}

void scene_model_manager::update(vulkan_commandbuffer& _commandbuffer) noexcept
{
	std::erase_if(models, [](const auto& p)
		{
			return !p;
		});

	std::erase_if(meshs, [](const auto& p)
		{
			return !p;
		});

	std::erase_if(models_cache, [](const auto& p)
		{
			return p.second.expired();
		});


	_commandbuffer.add_staging_buffer(std::move(vertices_buffer));
	_commandbuffer.add_staging_buffer(std::move(indices_buffer));
	_commandbuffer.add_staging_buffer(std::move(ssbo));


	// begin commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), app->get_device(), app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue()).front());
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

const std::vector<std::shared_ptr<scene_model>>& scene_model_manager::get_models() const noexcept
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
				p->vertex_offset = s;
				return s + p->vertices.size() * sizeof(p->vertices.front());
			});

		vulkan_buffer vertices_staging_buffer;
		vertices_staging_buffer.create(app->get_physical_device(), app->get_device(), vertices_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

		std::ranges::for_each(meshs, [&](const auto& p)
			{
				memcpy(static_cast<uint8_t*>(vertices_staging_buffer.get_buffer_address().hostAddress) + p->vertex_offset, p->vertices.data(), p->vertices.size() * sizeof(p->vertices.front()));
			});

		vertices_buffer.create(app->get_physical_device(), app->get_device(), vertices_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);
		vulkan_buffer::copy_buffer_to_buffer(*_commandbuffer, vertices_staging_buffer.get_buffer(), vertices_buffer.get_buffer(), vk::BufferCopy2(0, 0, vertices_size));
		_commandbuffer.add_staging_buffer(std::move(vertices_staging_buffer));

		// recreate index buffer
		vk::DeviceSize indices_size = std::ranges::fold_left(meshs, static_cast<size_t>(0), [](size_t s, const auto& p)
			{
				p->index_offset = s;
				return s + p->indices.size() * sizeof(p->indices.front());
			});

		vulkan_buffer indices_staging_buffer;
		indices_staging_buffer.create(app->get_physical_device(), app->get_device(), indices_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

		std::ranges::for_each(meshs, [&](const auto& p)
			{
				memcpy(static_cast<uint8_t*>(indices_staging_buffer.get_buffer_address().hostAddress) + p->index_offset, p->indices.data(), p->indices.size() * sizeof(p->indices.front()));
			});

		indices_buffer.create(app->get_physical_device(), app->get_device(), indices_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);
		vulkan_buffer::copy_buffer_to_buffer(*_commandbuffer, indices_staging_buffer.get_buffer(), indices_buffer.get_buffer(), vk::BufferCopy2(0, 0, indices_size));
		_commandbuffer.add_staging_buffer(std::move(indices_staging_buffer));


		// update blas
		std::ranges::for_each(meshs, [&](const auto& p)
			{
				p->blas_info.create_bottom_level_acceleration_structure(app->get_physical_device(), app->get_device(), *_commandbuffer,
					static_cast<uint32_t>(p->vertices.size()), vertices_buffer.get_buffer_address().deviceAddress + p->vertex_offset,
					static_cast<uint32_t>(p->indices.size()), indices_buffer.get_buffer_address().deviceAddress + p->index_offset);
			});
	}
}

void scene_model_manager::update_ssbo(vulkan_commandbuffer& _commandbuffer) noexcept
{
	if (!models.empty())
	{
		// update ssbo
		auto models_ssbo = models
			| std::views::transform([&](const auto& m)
				{
					return model_data{
						.model_matrix = m->model_matrix,
						.material_index = manager->get_material_index(m->material).value_or(std::numeric_limits<uint32_t>::max()),
						.vertex_address = vertices_buffer.get_buffer_address().deviceAddress + m->model_info->vertex_offset,
						.index_address = indices_buffer.get_buffer_address().deviceAddress + m->model_info->index_offset,
					};
				})
			| std::ranges::to<std::vector>();

		vk::DeviceSize models_ssbo_size = sizeof(models_ssbo.front()) * models_ssbo.size();
		ssbo.create(app->get_physical_device(), app->get_device(), models_ssbo_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);

		vulkan_buffer staging_buffer;
		staging_buffer.create(app->get_physical_device(), app->get_device(), models_ssbo_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

		memcpy(staging_buffer.get_buffer_address().hostAddress, models_ssbo.data(), models_ssbo_size);
		vulkan_buffer::copy_buffer_to_buffer(*_commandbuffer, staging_buffer.get_buffer(), ssbo.get_buffer(), vk::BufferCopy2(0, 0, models_ssbo_size));
		_commandbuffer.add_staging_buffer(std::move(staging_buffer));
	}
}
