module;

#include <glm/gtc/matrix_transform.hpp>

module chess_board_line;

import std;
import vulkan_commandbuffer;
import font_loader;
import shader_compiler;

void chess_board_line::create(GLFWwindow* _window, const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format)
{
	// begin a commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(_app->create_commandbuffers(vk::QueueFlagBits::eGraphics, 1).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);


	// descriptor pool
	std::array pool_size{
		vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, vulkan_common::MAX_FRAMES_IN_FLIGHT),
	};
	vk::DescriptorPoolCreateInfo pool_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, vulkan_common::MAX_FRAMES_IN_FLIGHT, pool_size);
	descriptor_pool = vk::raii::DescriptorPool(_app->get_device(), pool_info);


	// pipeline
	std::array bindings{
		vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex, nullptr),
	};

	auto binding = model_vertex::get_binding_description();
	auto attribute = model_vertex::get_attribute_descriptions();

	auto spirv_code = shader_compiler::get_shader_compiler().compile_shader_to_spv(std::string(SHADERS_PATH) + "chess_board_line.slang", { "vertMain", "fragMain" });
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
	if (!model_loader::get_model_loader().load_model(std::string(MODELS_PATH) + "chess_board_line.glb", vertices, indices))
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


	// uniform buffer
	ubos.clear();
	for (uint32_t i = 0; i < vulkan_common::MAX_FRAMES_IN_FLIGHT; i++)
	{
		vulkan_buffer buffer;
		buffer.create(_app->get_physical_device(), _app->get_device(), sizeof(chess_board_line::UBO), vk::BufferUsageFlagBits::eUniformBuffer, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		ubos.push_back(std::move(buffer));
	}


	// commandbuffer submit
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);
}

void chess_board_line::resize(const vulkan_application* _app, uint32_t _width, uint32_t _height)
{
	// descriptor set
	std::vector<vk::DescriptorSetLayout> layouts(vulkan_common::MAX_FRAMES_IN_FLIGHT, *(pipeline.get_descriptor_set_layout()));
	vk::DescriptorSetAllocateInfo alloc_info(descriptor_pool, layouts);

	descriptor_sets.clear();
	descriptor_sets = _app->get_device().allocateDescriptorSets(alloc_info);

	for (size_t i = 0; i < vulkan_common::MAX_FRAMES_IN_FLIGHT; i++)
	{
		vk::DescriptorBufferInfo buffer_info(ubos.at(i).get_buffer(), 0, sizeof(chess_board_line::UBO));

		std::array descriptorWrite{
			vk::WriteDescriptorSet(descriptor_sets.at(i), 0, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &buffer_info),
		};

		_app->get_device().updateDescriptorSets(descriptorWrite, {});
	}
}

void chess_board_line::update(const scene_camera* _camera)
{
	chess_board_line::UBO ubo_(glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, 0.5f)), _camera->get_view_matrix(), _camera->get_projection_matrix(), _camera->get_position());
	memcpy(ubos.at(current_frame).get_buffer_address(), &ubo_, sizeof(chess_board_line::UBO));
}

void chess_board_line::render(const vk::raii::CommandBuffer& _commandbuffer)
{
	_commandbuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline());
	_commandbuffer.bindVertexBuffers(0, *(vertices_buffer.get_buffer()), vk::DeviceSize(0));
	_commandbuffer.bindIndexBuffer(*(indices_buffer.get_buffer()), vk::DeviceSize(0), vk::IndexType::eUint32);
	_commandbuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline_layout(), 0, *(descriptor_sets.at(current_frame)), nullptr);
	_commandbuffer.drawIndexed(static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);

	current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;
}

void chess_board_line::destroy()
{
}