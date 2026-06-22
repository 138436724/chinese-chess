#include "scene_material_manager.h"
#include "tools/font_loader.h"
#include <ranges>

scene_material_manager::scene_material_manager(vulkan_application* _app)
	: app(_app)
{
}

std::shared_ptr<scene_material> scene_material_manager::create(const std::u8string& _font_path, uint32_t _font_size, const std::wstring& _characters)
{
	// find in cache
	if (auto iter = std::ranges::find_if(images_cache, [&](const auto& s) {return std::get<0>(s.first) == _characters && std::get<1>(s.first) == _font_size; }); iter != images_cache.end())
	{
		if (!iter->second.expired())
		{
			auto material = std::make_shared<scene_material>();
			material->alpha_map = std::shared_ptr<vulkan_image>(iter->second);
			materials.emplace_back(material);
			return material;
		}
	}

	// begin a commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), app->get_device(), app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue()).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);


	// load font and transition to image
	std::vector<character_info> fonts_info = FONT_LOADER.load_font(_font_path, _font_size, _characters);
	uint32_t max_bearing_height_up = 0, max_bearing_height_down = 0, all_width = 0;
	for (const auto& _font_info : fonts_info)
	{
		max_bearing_height_up = std::max(max_bearing_height_up, _font_info.bearing_height);
		max_bearing_height_down = std::max(max_bearing_height_down, _font_info.height - _font_info.bearing_height);
		all_width += _font_info.advance;
	}
	uint32_t all_height = max_bearing_height_up + max_bearing_height_down;

	auto font_image = std::make_shared<vulkan_image>();
	vk::ImageCreateInfo font_image_info({}, vk::ImageType::e2D, vk::Format::eR8Unorm, vk::Extent3D(all_width, all_height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo font_view_info({}, {}, vk::ImageViewType::e2D, vk::Format::eR8Unorm, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	font_image->create(app->get_physical_device(), app->get_device(), font_image_info, font_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));

	std::vector<vk::ImageMemoryBarrier2> begin_barrier;
	begin_barrier.emplace_back(font_image->set_layout(vk::ImageLayout::eTransferDstOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));

	uint32_t width_offset = 0;
	for (const auto& _font_info : fonts_info)
	{
		vulkan_buffer stage_buffer;
		stage_buffer.create(app->get_physical_device(), app->get_device(), static_cast<size_t>(_font_info.width) * _font_info.height, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		memcpy(stage_buffer.get_buffer_address().hostAddress, _font_info.buffer.data(), _font_info.buffer.size());

		vk::Offset3D copy_offset(width_offset + _font_info.bearing_width, max_bearing_height_up - _font_info.bearing_height, 0);
		vulkan_buffer::copy_buffer_to_image(*commandbuffer, stage_buffer.get_buffer(), font_image->get_image(), vk::BufferImageCopy2(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), copy_offset, vk::Extent3D(_font_info.width, _font_info.height, 1)));
		width_offset += _font_info.advance;

		commandbuffer.add_staging_buffer(std::move(stage_buffer));
	}

	std::vector<vk::ImageMemoryBarrier2> end_barrier;
	end_barrier.emplace_back(font_image->set_layout(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, end_barrier));

	images.emplace_back(font_image);
	images_cache.emplace(std::make_tuple(_characters, _font_size), std::weak_ptr<vulkan_image>(font_image));

	auto material = std::make_shared<scene_material>();
	material->alpha_map = font_image;
	materials.emplace_back(material);


	// commandbuffer submit
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);

	return material;
}

void scene_material_manager::remove(const std::weak_ptr<scene_material>& _material) noexcept
{
	std::owner_less<void> cmp;
	std::erase_if(images, [&](const auto& p)
		{
			return !cmp(p, _material.lock()->alpha_map) && !cmp(_material.lock()->alpha_map, p);
		});
	std::erase_if(materials, [&](const auto& p)
		{
			return !cmp(p, _material) && !cmp(_material, p);
		});
}

void scene_material_manager::update(vulkan_commandbuffer& _commandbuffer) noexcept
{
	std::erase_if(materials, [](const auto& p)
		{
			return !p;
		});

	std::erase_if(images, [](const auto& p)
		{
			return p.expired();
		});

	std::erase_if(images_cache, [](const auto& p)
		{
			return p.second.expired();
		});


	_commandbuffer.add_staging_buffer(std::move(ssbo));


	// begin a commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), app->get_device(), app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue()).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

	update_ssbo(commandbuffer);

	// commandbuffer submit
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);
}

void scene_material_manager::clear() noexcept
{
	materials.clear();
	images.clear();
	images_cache.clear();
}

const vulkan_buffer& scene_material_manager::get_ssbo_buffer() const noexcept
{
	return ssbo;
}

std::optional<uint32_t> scene_material_manager::get_material_index(const std::weak_ptr<scene_material>& _material) const noexcept
{
	auto materials_with_index = materials | std::views::enumerate;

	std::owner_less<void> cmp;
	auto iter = std::ranges::find_if(materials_with_index, [&](const auto& p)
		{
			return !cmp(std::get<1>(p), _material) && !cmp(_material, std::get<1>(p));
		});

	if (iter != materials_with_index.end())
	{
		return static_cast<uint32_t>(std::get<0>(*iter));
	}
	else
	{
		return std::nullopt;
	}
}

std::optional<uint32_t> scene_material_manager::get_texture_index(const std::weak_ptr<vulkan_image>& _texture) const noexcept
{
	auto images_with_index = images | std::views::enumerate;

	std::owner_less<void> cmp;
	auto iter = std::ranges::find_if(images_with_index, [&](const auto& p)
		{
			return !cmp(std::get<1>(p), _texture) && !cmp(_texture, std::get<1>(p));
		});

	if (iter != images_with_index.end())
	{
		return static_cast<uint32_t>(std::get<0>(*iter));
	}
	else
	{
		return std::nullopt;
	}
}

std::vector<vk::DescriptorImageInfo> scene_material_manager::get_descriptor_info(const std::span<vk::Sampler> _samplers) const
{
	if (_samplers.size() == 1)
	{
		return images
			| std::views::transform([&](const auto& image)
				{
					return vk::DescriptorImageInfo(_samplers.front(), image.lock()->get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);
				})
			| std::ranges::to<std::vector>();
	}
	else if (_samplers.size() == materials.size())
	{
		return std::views::zip(_samplers, images)
			| std::views::transform([&](const auto& _pair)
				{
					const auto& [sampler, image] = _pair;
					return vk::DescriptorImageInfo(sampler, image.lock()->get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);
				})
			| std::ranges::to<std::vector>();
	}
	else
	{
		throw std::runtime_error("Must match size and generate!");
	}
	return {};
}

void scene_material_manager::update_ssbo(vulkan_commandbuffer& _commandbuffer) noexcept
{
	if (!materials.empty())
	{
		auto materials_ssbo = materials
			| std::views::transform([this](const auto& p)
				{
					return material_data{
						.background_color = p->background_color,
						.texture_index = get_texture_index(p->alpha_map).value_or(std::numeric_limits<uint32_t>::max()),
						.foreground_color = p->foreground_color,
						.roughness = p->roughness,
						.metallic = p->metallic
					};
				})
			| std::ranges::to<std::vector>();

		vk::DeviceSize materials_ssbo_size = sizeof(materials_ssbo.front()) * materials_ssbo.size();
		ssbo.create(app->get_physical_device(), app->get_device(), materials_ssbo_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);

		vulkan_buffer staging_buffer;
		staging_buffer.create(app->get_physical_device(), app->get_device(), materials_ssbo_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

		memcpy(staging_buffer.get_buffer_address().hostAddress, materials_ssbo.data(), materials_ssbo_size);
		vulkan_buffer::copy_buffer_to_buffer(*_commandbuffer, staging_buffer.get_buffer(), ssbo.get_buffer(), vk::BufferCopy2(0, 0, materials_ssbo_size));
		_commandbuffer.add_staging_buffer(std::move(staging_buffer));
	}
}
