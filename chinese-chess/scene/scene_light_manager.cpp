#include "scene_light_manager.h"
#include <ranges>

enum class light_type :uint32_t
{
	directional,
	point,
	spot
};

struct light_data
{
	alignas(16) glm::vec3 color = glm::vec3(1.f);
	uint32_t active_type = static_cast<uint32_t>(light_type::directional);
	alignas(16) glm::vec3 direction = glm::vec3(0.f, -1.f, 0.f);
	float intensity = 1.f;
	alignas(16) glm::vec3 position = glm::vec3(0.f);
	float range = 10.f;
	alignas(16) float inner_cone_angle = glm::radians(15.f);
	float outer_cone_angle = glm::radians(30.f);
};

scene_light_manager::scene_light_manager(const vulkan_application* _app, const vulkan_queue* _transfer_queue)
	: app(_app),
	transfer_queue(_transfer_queue)
{
}

void scene_light_manager::update(vulkan_commandbuffer& _commandbuffer) noexcept
{
	std::erase_if(lights, [](const auto& p)
		{
			return p.expired();
		});


	_commandbuffer.add_staging_buffer(std::move(ssbo));


	// begin commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(transfer_queue->get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), app->get_device(), transfer_queue->get_queue()).front());
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

const std::vector<std::weak_ptr<scene_light>>& scene_light_manager::get_lights() const noexcept
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
					auto sp = p.lock();
					return std::visit([](auto& light)
						{
							using T = std::decay_t<decltype(light)>;

							if constexpr (std::is_same_v<T, directional_light>)
							{
								return light_data{
									.color = light.color,
									.active_type = static_cast<uint32_t>(light_type::directional),
									.direction = glm::normalize(light.direction),
									.intensity = light.intensity,
								};
							}
							else if constexpr (std::is_same_v<T, point_light>)
							{
								return light_data{
									.color = light.color,
									.active_type = static_cast<uint32_t>(light_type::point),
									.intensity = light.intensity,
									.position = light.position,
									.range = light.range,
								};
							}
							else if constexpr (std::is_same_v<T, spot_light>)
							{
								return light_data{
									.color = light.color,
									.active_type = static_cast<uint32_t>(light_type::spot),
									.direction = glm::normalize(light.direction),
									.intensity = light.intensity,
									.position = light.position,
									.range = light.range,
									.inner_cone_angle = light.inner_cone_angle,
									.outer_cone_angle = light.outer_cone_angle,
								};
							}
						}, *sp);
				})
			| std::ranges::to<std::vector>();

		vk::DeviceSize lights_ssbo_size = sizeof(lights_ssbo.front()) * lights_ssbo.size();
		ssbo.create(app->get_allocator(), app->get_device(), lights_ssbo_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);

		vulkan_buffer staging_buffer;
		staging_buffer.create(app->get_allocator(), app->get_device(), lights_ssbo_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

		memcpy(staging_buffer.get_buffer_address().hostAddress, lights_ssbo.data(), lights_ssbo_size);
		vulkan_buffer::copy_buffer_to_buffer(*_commandbuffer, staging_buffer.get_buffer(), ssbo.get_buffer(), vk::BufferCopy2(0, 0, lights_ssbo_size));
		_commandbuffer.add_staging_buffer(std::move(staging_buffer));
	}
}
