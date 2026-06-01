#include "scene_skybox.h"
#include "tools/shader_compiler.h"
#include "vulkan_core/vulkan_common.h"
#include <algorithm>
#include <ranges>

void scene_skybox::create(const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format)
{
	// begin a commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(_app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), _app->get_device(), _app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue()).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);


	// pipeline
	std::array bindings{
		vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex, nullptr),
		vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment, nullptr),
		vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment, nullptr),
	};

	auto binding = model_vertex::get_binding_description();
	auto attribute = model_vertex::get_attribute_descriptions<model_vertex_type::position>();

	auto spirv_code = SHADER_COMPILER.compile_shader_to_spv(std::u8string(SHADERS_PATH) + u8"scene_skybox.slang", { std::string(VERT_ENTYR_NAME), std::string(FRAG_ENTYR_NAME) });
	if (spirv_code.empty())
	{
		throw std::runtime_error("compile .spv failed!");
	}

	vk::raii::ShaderModule shaderModule(_app->get_device(), vk::ShaderModuleCreateInfo({}, spirv_code.size() * sizeof(char), reinterpret_cast<const uint32_t*>(spirv_code.data())));
	std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = {
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, shaderModule, VERT_ENTYR_NAME.data()),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, shaderModule, FRAG_ENTYR_NAME.data()),
	};

	pipeline.create(_app->get_device(), bindings, {}, std::span(&binding, 1), attribute, shader_stages,
		vk::PrimitiveTopology::eTriangleList, vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise,
		_multisample_count, vk::False, std::span(&_color_formats, 1), _depth_format);


	// vertex and index buffer
	if (!MODEL_LOADER.load_model(std::u8string(MODELS_PATH) + u8"scene_skybox.glb", vertices, indices))
	{
		throw std::runtime_error("read model failed!");
	}

	vk::DeviceSize vertices_size = sizeof(vertices.front()) * vertices.size();
	vertices_buffer.create(_app->get_physical_device(), _app->get_device(), vertices_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);

	vulkan_buffer vertices_staging_buffer;
	vertices_staging_buffer.create(_app->get_physical_device(), _app->get_device(), vertices_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

	memcpy(vertices_staging_buffer.get_buffer_address().hostAddress, vertices.data(), vertices_size);
	vulkan_buffer::copy_buffer_to_buffer((*commandbuffer), vertices_staging_buffer.get_buffer(), vertices_buffer.get_buffer(), vk::BufferCopy2(0, 0, vertices_size));

	vk::DeviceSize indices_size = sizeof(indices.front()) * indices.size();
	indices_buffer.create(_app->get_physical_device(), _app->get_device(), indices_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);

	vulkan_buffer indices_staging_buffer;
	indices_staging_buffer.create(_app->get_physical_device(), _app->get_device(), indices_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

	memcpy(indices_staging_buffer.get_buffer_address().hostAddress, indices.data(), indices_size);
	vulkan_buffer::copy_buffer_to_buffer((*commandbuffer), indices_staging_buffer.get_buffer(), indices_buffer.get_buffer(), vk::BufferCopy2(0, 0, indices_size));


	// uniform buffer
	ubos.clear();
	ubo_params.clear();
	for (uint32_t i = 0; i < vulkan_common::MAX_FRAMES_IN_FLIGHT; i++)
	{
		vulkan_buffer ubo_buffer;
		ubo_buffer.create(_app->get_physical_device(), _app->get_device(), sizeof(scene_skybox::UBO), vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		ubos.push_back(std::move(ubo_buffer));

		vulkan_buffer ubo_param_buffer;
		ubo_param_buffer.create(_app->get_physical_device(), _app->get_device(), sizeof(scene_skybox::UBOParams), vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		ubo_params.push_back(std::move(ubo_param_buffer));
	}


	// commandbuffer submit
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);
}

void scene_skybox::resize(const vulkan_application* _app, uint32_t _width, uint32_t _height)
{
	if (cubemap == nullptr)
	{
		throw std::runtime_error("Need Set Sampler Image Resource First!");
	}


	descriptor.clear_descriptor_info();

	auto buffer_pool_info = ubos
		| std::views::transform([](const auto& _buffer) -> DescriptorBufferOrImageInfo
			{
				return vk::DescriptorBufferInfo(_buffer.get_buffer(), 0, sizeof(scene_skybox::UBO));
			})
		| std::ranges::to<std::vector>();
	descriptor.add_descriptor_info(vk::DescriptorType::eUniformBuffer, std::move(buffer_pool_info));

	auto params_pool_info = ubo_params
		| std::views::transform([](const auto& _buffer) -> DescriptorBufferOrImageInfo
			{
				return vk::DescriptorBufferInfo(_buffer.get_buffer(), 0, sizeof(scene_skybox::UBOParams));
			})
		| std::ranges::to<std::vector>();
	descriptor.add_descriptor_info(vk::DescriptorType::eUniformBuffer, std::move(params_pool_info));

	auto image_pool_info = std::views::iota(0u, vulkan_common::MAX_FRAMES_IN_FLIGHT)
		| std::views::transform([&](const auto&) -> DescriptorBufferOrImageInfo
			{
				return vk::DescriptorImageInfo(cubemap->get_sampler(), cubemap->get_image().get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);
			})
		| std::ranges::to<std::vector>();
	descriptor.add_descriptor_info(vk::DescriptorType::eCombinedImageSampler, std::move(image_pool_info));

	descriptor.update_descriptor_sets(_app->get_device(), pipeline.get_descriptor_set_layout());
}

void scene_skybox::update(const scene_camera* _camera) noexcept
{
	scene_skybox::UBO ubo_(_camera->get_projection_matrix(), glm::mat4(glm::mat3(_camera->get_view_matrix())));
	memcpy(ubos.at(current_frame).get_buffer_address().hostAddress, &ubo_, sizeof(scene_skybox::UBO));

	scene_skybox::UBOParams ubo_params_;
	ubo_params_.exposure = 1.0f;
	ubo_params_.gamma = 1.0f;

	memcpy(ubo_params.at(current_frame).get_buffer_address().hostAddress, &ubo_params_, sizeof(scene_skybox::UBOParams));
}

void scene_skybox::render(const vk::raii::CommandBuffer& _commandbuffer) noexcept
{
	_commandbuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline());
	_commandbuffer.bindVertexBuffers(0, *(vertices_buffer.get_buffer()), vk::DeviceSize(0));
	_commandbuffer.bindIndexBuffer(indices_buffer.get_buffer(), vk::DeviceSize(0), vk::IndexType::eUint32);
	_commandbuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline_layout(), 0, *(descriptor.get_descriptor_sets().at(current_frame)), nullptr);
	_commandbuffer.drawIndexed(static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);

	current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;
}

void scene_skybox::destroy() noexcept
{
}

std::vector<vk::AccelerationStructureInstanceKHR> scene_skybox::get_all_blas_info() const noexcept
{
	return std::vector<vk::AccelerationStructureInstanceKHR>();
}

void scene_skybox::set_cubemap(scene_cubemap* _cubemap) noexcept
{
	cubemap = _cubemap;
}