#include "scene_light_manager.h"
#include <ranges>

scene_light_manager::scene_light_manager(vulkan_application* _app)
	: app(_app)
{
}

std::shared_ptr<scene_light> scene_light_manager::create(light_type _type) noexcept
{
	auto light = std::make_shared<scene_light>();
	light->active_type = _type;
	switch (_type)
	{
	case light_type::directional:
		light->light = directional_light();
		break;
	case light_type::point:
		light->light = point_light();
		break;
	case light_type::spot:
		light->light = spot_light();
		break;
	default:
		break;
	}
	lights.push_back(light);

	return light;
}

void scene_light_manager::remove(const std::weak_ptr<scene_light>& _light) noexcept
{
	std::owner_less<void> cmp;
	std::erase_if(lights, [&](const auto& p)
		{
			return !cmp(p, _light) && !cmp(_light, p);
		});
}

void scene_light_manager::update(vulkan_commandbuffer& _commandbuffer) noexcept
{
	std::erase_if(lights, [](const auto& p)
		{
			return !p;
		});


	_commandbuffer.add_staging_buffer(std::move(ssbo));


	// begin commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), app->get_device(), app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue()).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

	update_ssbo(commandbuffer);

	// submit commandbuffer
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);
}

void scene_light_manager::clear() noexcept
{
	lights.clear();
}

const vulkan_buffer& scene_light_manager::get_ssbo_buffer() const noexcept
{
	return ssbo;
}

const std::vector<std::shared_ptr<scene_light>>& scene_light_manager::get_lights() const noexcept
{
	return lights;
}

void scene_light_manager::update_ssbo(vulkan_commandbuffer& _commandbuffer) noexcept
{
	if (!lights.empty())
	{
		auto lights_ssbo = lights
			| std::views::transform([this](const auto& p)
				{
					switch (p->active_type)
					{
					case light_type::directional:
						return light_data{
							.color = std::get<directional_light>(p->light).color,
							.active_type = static_cast<uint32_t>(p->active_type),
							.direction = glm::normalize(std::get<directional_light>(p->light).direction),
							.intensity = std::get<directional_light>(p->light).intensity,
						};
					case light_type::point:
						return light_data{
							.color = std::get<point_light>(p->light).color,
							.active_type = static_cast<uint32_t>(p->active_type),
							.intensity = std::get<point_light>(p->light).intensity,
							.position = std::get<point_light>(p->light).position,
							.range = std::get<point_light>(p->light).range,
						};
					case light_type::spot:
						return light_data{
							.color = std::get<spot_light>(p->light).color,
							.active_type = static_cast<uint32_t>(p->active_type),
							.direction = glm::normalize(std::get<spot_light>(p->light).direction),
							.intensity = std::get<spot_light>(p->light).intensity,
							.position = std::get<spot_light>(p->light).position,
							.range = std::get<spot_light>(p->light).range,
							.inner_cone_angle = std::get<spot_light>(p->light).inner_cone_angle,
							.outer_cone_angle = std::get<spot_light>(p->light).outer_cone_angle,
						};
					default:
						break;
					}

					return light_data{
						.color = glm::vec3(1.f),
						.active_type = static_cast<uint32_t>(light_type::directional),
						.direction = glm::vec3(0.f, -1.f, 0.f),
						.intensity = 1.f,
					};
				})
			| std::ranges::to<std::vector>();

		vk::DeviceSize lights_ssbo_size = sizeof(lights_ssbo.front()) * lights_ssbo.size();
		ssbo.create(app->get_physical_device(), app->get_device(), lights_ssbo_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);

		vulkan_buffer staging_buffer;
		staging_buffer.create(app->get_physical_device(), app->get_device(), lights_ssbo_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

		memcpy(staging_buffer.get_buffer_address().hostAddress, lights_ssbo.data(), lights_ssbo_size);
		vulkan_buffer::copy_buffer_to_buffer(*_commandbuffer, staging_buffer.get_buffer(), ssbo.get_buffer(), vk::BufferCopy2(0, 0, lights_ssbo_size));
		_commandbuffer.add_staging_buffer(std::move(staging_buffer));
	}
}
