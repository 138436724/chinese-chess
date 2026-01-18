module;

#include <glm/gtc/matrix_transform.hpp>

module chess_board;

import std;
import vulkan_commandbuffer;
import font_loader;
import shader_compiler;

void chess_board::create(GLFWwindow* _window, const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format)
{
	// begin a commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(_app->create_commandbuffers(vk::QueueFlagBits::eGraphics, 1).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);


	// descriptor pool
	std::array pool_size{
		vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, vulkan_common::MAX_FRAMES_IN_FLIGHT),
		vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, vulkan_common::MAX_FRAMES_IN_FLIGHT),
	};
	vk::DescriptorPoolCreateInfo pool_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, vulkan_common::MAX_FRAMES_IN_FLIGHT, pool_size);
	descriptor_pool = vk::raii::DescriptorPool(_app->get_device(), pool_info);


	// pipeline
	std::array bindings{
		vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex, nullptr),
		vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment, nullptr),
	};

	auto binding = model_vertex::get_binding_description();
	auto attribute = model_vertex::get_attribute_descriptions();

	auto spirv_code = shader_compiler::get_shader_compiler().compile_shader_to_spv(std::string(SHADERS_PATH) + "chess_board.slang", { "vertMain", "fragMain" });
	if (spirv_code.empty())
	{
		throw std::runtime_error("compile .spv failed!");
	}

	vk::raii::ShaderModule shaderModule(_app->get_device(), vk::ShaderModuleCreateInfo({}, spirv_code.size() * sizeof(char), reinterpret_cast<const uint32_t*>(spirv_code.data())));
	std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = {
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, shaderModule, "vertMain"),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, shaderModule, "fragMain"),
	};

	pipeline.create_pipeline(_app->get_device(), bindings, {}, std::span(&binding, 1), attribute, shader_stages,
		vk::PrimitiveTopology::eTriangleList, vk::PolygonMode::eFill, vk::CullModeFlagBits::eBack, vk::FrontFace::eCounterClockwise,
		_multisample_count, vk::True, std::span(&_color_formats, 1), _depth_format);


	// vertex and index buffer
	if (!model_loader::get_model_loader().load_model(std::string(MODELS_PATH) + "chess_board.glb", vertices, indices))
	{
		throw std::runtime_error("read model failed!");
	}

	vk::DeviceSize vertices_size = sizeof(vertices.front()) * vertices.size();
	vertices_buffer.create(_app->get_physical_device(), _app->get_device(), vertices_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer, vk::MemoryPropertyFlagBits::eDeviceLocal);

	vulkan_buffer vertices_staging_buffer;
	vertices_staging_buffer.create(_app->get_physical_device(), _app->get_device(), vertices_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

	memcpy(vertices_staging_buffer.get_buffer_address(), vertices.data(), vertices_size);
	vulkan_buffer::copy_buffer_to_buffer((*commandbuffer), vertices_staging_buffer.get_buffer(), vertices_buffer.get_buffer(), 0, 0, vertices_size);

	vk::DeviceSize indices_size = sizeof(indices.front()) * indices.size();
	indices_buffer.create(_app->get_physical_device(), _app->get_device(), indices_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer, vk::MemoryPropertyFlagBits::eDeviceLocal);

	vulkan_buffer indices_staging_buffer;
	indices_staging_buffer.create(_app->get_physical_device(), _app->get_device(), indices_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

	memcpy(indices_staging_buffer.get_buffer_address(), indices.data(), indices_size);
	vulkan_buffer::copy_buffer_to_buffer((*commandbuffer), indices_staging_buffer.get_buffer(), indices_buffer.get_buffer(), 0, 0, indices_size);


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
		buffer.create(_app->get_physical_device(), _app->get_device(), sizeof(chess_board::UBO), vk::BufferUsageFlagBits::eUniformBuffer, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		ubos.push_back(std::move(buffer));
	}


	// commandbuffer submit
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);
}

void chess_board::resize(const vulkan_application* _app, uint32_t _width, uint32_t _height)
{
	// begin a commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(_app->create_commandbuffers(vk::QueueFlagBits::eGraphics, 1).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);


	// load font and transition to image
	constexpr float font_resolution = 2.f;
	std::vector<character_info> fonts_info = std::move(font_loader::get_font_loader().load_font(std::string(FONTS_PATH) + "LXGWWenKaiGB-Medium.ttf", static_cast<uint32_t>(_height / 9 * font_resolution), L"楚河汉界"));
	uint32_t max_bearing_height_up = 0, max_bearing_height_down = 0, all_width = 0;
	for (const auto& _font_info : fonts_info)
	{
		max_bearing_height_up = std::max(max_bearing_height_up, _font_info.bearing_height);
		max_bearing_height_down = std::max(max_bearing_height_down, _font_info.height - _font_info.bearing_height);
		all_width += _font_info.advance;
	}
	uint32_t all_height = max_bearing_height_up + max_bearing_height_down;

	vk::ImageCreateInfo font_image_info({}, vk::ImageType::e2D, vk::Format::eR8Unorm, vk::Extent3D(all_width, all_height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo font_view_info({}, {}, vk::ImageViewType::e2D, vk::Format::eR8Unorm, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	font_image.create(_app->get_physical_device(), _app->get_device(), font_image_info, font_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));

	std::vector<vk::ImageMemoryBarrier2> begin_barrier;
	begin_barrier.emplace_back(font_image.transition_to_layout(vk::ImageLayout::eTransferDstOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));

	std::vector<vulkan_buffer> all_stage_buffer;
	uint32_t width_offset = 0;
	for (const auto& _font_info : fonts_info)
	{
		vulkan_buffer stage_buffer;
		stage_buffer.create(_app->get_physical_device(), _app->get_device(), _font_info.width * _font_info.height, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		memcpy(stage_buffer.get_buffer_address(), _font_info.buffer.data(), _font_info.buffer.size());

		vk::Offset3D copy_offset(width_offset + _font_info.bearing_width, max_bearing_height_up - _font_info.bearing_height, 0);
		vulkan_buffer::copy_buffer_to_image(*commandbuffer, stage_buffer.get_buffer(), font_image.get_image(), vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), copy_offset, vk::Extent3D(_font_info.width, _font_info.height, 1));
		width_offset += _font_info.advance;

		all_stage_buffer.push_back(std::move(stage_buffer));
	}

	std::vector<vk::ImageMemoryBarrier2> end_barrier;
	end_barrier.emplace_back(font_image.transition_to_layout(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, end_barrier));


	// descriptor set
	std::vector<vk::DescriptorSetLayout> layouts(vulkan_common::MAX_FRAMES_IN_FLIGHT, *(pipeline.get_descriptor_set_layout()));
	vk::DescriptorSetAllocateInfo alloc_info(descriptor_pool, layouts);

	descriptor_sets.clear();
	descriptor_sets = _app->get_device().allocateDescriptorSets(alloc_info);

	for (size_t i = 0; i < vulkan_common::MAX_FRAMES_IN_FLIGHT; i++)
	{
		vk::DescriptorBufferInfo buffer_info(ubos.at(i).get_buffer(), 0, sizeof(chess_board::UBO));
		vk::DescriptorImageInfo image_info(font_sampler, font_image.get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);

		std::array descriptorWrite{
			vk::WriteDescriptorSet(descriptor_sets.at(i), 0, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &buffer_info),
			vk::WriteDescriptorSet(descriptor_sets.at(i), 1, 0, 1, vk::DescriptorType::eCombinedImageSampler, &image_info, nullptr),
		};

		_app->get_device().updateDescriptorSets(descriptorWrite, {});
	}


	// commandbuffer submit
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);
}

void chess_board::update(const scene_camera* _camera)
{
	chess_board::UBO ubo_(glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, 0.7f)), _camera->get_view_matrix(), _camera->get_projection_matrix(), _camera->get_position());
	memcpy(ubos.at(current_frame).get_buffer_address(), &ubo_, sizeof(chess_board::UBO));
}

void chess_board::render(const vk::raii::CommandBuffer& _commandbuffer)
{
	_commandbuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline());
	_commandbuffer.bindVertexBuffers(0, *(vertices_buffer.get_buffer()), vk::DeviceSize(0));
	_commandbuffer.bindIndexBuffer(*(indices_buffer.get_buffer()), vk::DeviceSize(0), vk::IndexType::eUint32);
	_commandbuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline_layout(), 0, *(descriptor_sets.at(current_frame)), nullptr);
	_commandbuffer.drawIndexed(static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);

	current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;
}

void chess_board::destroy()
{
}
