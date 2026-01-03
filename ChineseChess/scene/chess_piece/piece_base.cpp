module piece_base;

import <glm/gtc/matrix_transform.hpp>;
import vulkan_common;
import font_loader;

void piece_base::create(const vulkan_application* _app, bool _use_color, bool _piece_color)
{
	use_color = _use_color;
	piece_color = _piece_color;


	// descriptor pool
	std::array pool_size{
		vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, vulkan_common::MAX_FRAMES_IN_FLIGHT),
		vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, vulkan_common::MAX_FRAMES_IN_FLIGHT),
	};
	vk::DescriptorPoolCreateInfo pool_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, vulkan_common::MAX_FRAMES_IN_FLIGHT, pool_size);
	descriptor_pool = vk::raii::DescriptorPool(_app->get_device(), pool_info);


	// create sampler
	vk::PhysicalDeviceProperties properties = _app->get_physical_device().getProperties();
	vk::SamplerCreateInfo sampler_info({}, vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear,
		vk::SamplerAddressMode::eClampToBorder, vk::SamplerAddressMode::eClampToBorder, vk::SamplerAddressMode::eClampToBorder,
		0.f, vk::True, properties.limits.maxSamplerAnisotropy, vk::False, vk::CompareOp::eAlways, 0.f, 1.f,
		vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);
	font_sampler = vk::raii::Sampler(_app->get_device(), sampler_info);


	// uniform buffer
	ubos.clear();
	for (uint32_t i = 0; i < vulkan_common::MAX_FRAMES_IN_FLIGHT; i++)
	{
		vulkan_buffer buffer;
		buffer.create(_app->get_physical_device(), _app->get_device(), sizeof(piece_base::UBO), vk::BufferUsageFlagBits::eUniformBuffer, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		ubos.push_back(std::move(buffer));
	}
}

void piece_base::resize(const vulkan_application* _app, const vk::raii::DescriptorSetLayout& _descriptor_set_layout, uint32_t _width, uint32_t _height)
{
	// begin a commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(_app->create_commandbuffers(vk::QueueFlagBits::eGraphics, 1).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);


	// load font and transition to image
	constexpr float font_resolution = 1.5f;
	character_info font_info = std::move(font_loader::get_font_loader().load_font(std::string(FONTS_PATH) + "LXGWWenKaiGB-Medium.ttf", static_cast<uint32_t>(_height / 9 * font_resolution), std::wstring(1, piece_name)).front());

	vk::ImageCreateInfo font_image_info({}, vk::ImageType::e2D, vk::Format::eR8Unorm, vk::Extent3D(font_info.advance, font_info.height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo font_view_info({}, {}, vk::ImageViewType::e2D, vk::Format::eR8Unorm, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	font_image.create(_app->get_physical_device(), _app->get_device(), font_image_info, font_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));

	std::vector<vk::ImageMemoryBarrier2> begin_barrier;
	begin_barrier.emplace_back(font_image.transition_to_layout(vk::ImageLayout::eTransferDstOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));


	vulkan_buffer stage_buffer;
	stage_buffer.create(_app->get_physical_device(), _app->get_device(), font_info.width * font_info.height, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	memcpy(stage_buffer.get_buffer_address(), font_info.buffer.data(), font_info.buffer.size());

	vk::Offset3D copy_offset(font_info.bearing_width, 0, 0);
	vulkan_buffer::copy_buffer_to_image(*commandbuffer, stage_buffer.get_buffer(), font_image.get_image(), vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), copy_offset, vk::Extent3D(font_info.width, font_info.height, 1));


	std::vector<vk::ImageMemoryBarrier2> end_barrier;
	end_barrier.emplace_back(font_image.transition_to_layout(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, end_barrier));


	// commandbuffer submit
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);


	// descriptor set
	std::vector<vk::DescriptorSetLayout> layouts(vulkan_common::MAX_FRAMES_IN_FLIGHT, _descriptor_set_layout);
	vk::DescriptorSetAllocateInfo alloc_info(descriptor_pool, layouts);

	descriptor_sets.clear();
	descriptor_sets = _app->get_device().allocateDescriptorSets(alloc_info);

	for (size_t i = 0; i < vulkan_common::MAX_FRAMES_IN_FLIGHT; i++)
	{
		vk::DescriptorBufferInfo buffer_info(ubos.at(i).get_buffer(), 0, sizeof(piece_base::UBO));
		vk::DescriptorImageInfo image_info(font_sampler, font_image.get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);

		std::array descriptorWrite{
			vk::WriteDescriptorSet(descriptor_sets.at(i), 0, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &buffer_info),
			vk::WriteDescriptorSet(descriptor_sets.at(i), 1, 0, 1, vk::DescriptorType::eCombinedImageSampler, &image_info, nullptr),
		};

		_app->get_device().updateDescriptorSets(descriptorWrite, {});
	}
}

void piece_base::update(const scene_camera* _camera)
{
	if (is_on_board)
	{
		piece_base::UBO ubo_(glm::translate(glm::mat4(1.f), glm::vec3(model_location, 0.3f)), _camera->get_view_matrix(), _camera->get_projection_matrix(), _camera->get_position(), glm::vec3(0.f, 0.f, 0.f));
		if (piece_color)
		{
			ubo_.piece_color = std::move(glm::vec3(1.f, 0.f, 0.f));
		}
		memcpy(ubos.at(current_frame).get_buffer_address(), &ubo_, sizeof(piece_base::UBO));
	}
}

void piece_base::render(const vk::raii::CommandBuffer& _commandbuffer, const vk::raii::PipelineLayout& _pipeline_layout)
{
	if (is_on_board)
	{
		_commandbuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, _pipeline_layout, 0, *(descriptor_sets.at(current_frame)), nullptr);

		current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;
	}
}

void piece_base::set_is_on_board(bool _is_on_board)
{
	is_on_board = _is_on_board;
}

void piece_base::set_piece_location(const glm::u8vec2& _location)
{
	piece_location = _location;
	model_location = transform_location(use_color, piece_color, piece_location.x, piece_location.y);
}

bool piece_base::get_is_on_board() const
{
	return is_on_board;
}

bool piece_base::get_piece_color() const
{
	return piece_color;
}

wchar_t piece_base::get_piece_name() const
{
	return piece_name;
}

glm::u8vec2 piece_base::get_piece_location() const
{
	return piece_location;
}

glm::vec2 piece_base::transform_location(bool _use_color, bool _piece_color, uint8_t _x, uint8_t _y)
{
	// 棋盘中心为坐标的(0, 0)点，而右下和左上作为双方棋子的定位原点
	glm::vec2 location;
	if (_use_color)
	{
		if (_piece_color)
		{
			// 红方红子从右到左是一到九，先将_x映射到坐标对应的位置，然后-1计算格子数
			location.x = static_cast<float>(10 - _x - 1);
			location.y = static_cast<float>(9 - _y);
		}
		else
		{
			// 红方黑子从左到右是1到9，先将_x映射到坐标对应的位置，然后-1计算格子数
			location.x = static_cast<float>(_x - 1);
			location.y = static_cast<float>(_y);
		}
	}
	else
	{
		if (_piece_color)
		{
			// 黑方红子从左到右是一到九，先将_x映射到坐标对应的位置，然后-1计算格子数
			location.x = static_cast<float>(_x - 1);
			location.y = static_cast<float>(_y);
		}
		else
		{
			// 黑方黑子从右到左是1到9，先将_x映射到坐标对应的位置，然后-1计算格子数
			location.x = static_cast<float>(10 - _x - 1);
			location.y = static_cast<float>(9 - _y);
		}
	}

	location = (location - glm::vec2(4, 4.5)) * board_unit_distance;
	return location;
}