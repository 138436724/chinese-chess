#include "scene_manager.h"
#include "tools/font_loader.h"
#include "tools/image_helper.h"
#include "tools/shader_compiler.h"
#include "vulkan_core/vulkan_common.h"
#include <ranges>
#include <string>
#include <unordered_set>

enum class stage_indices :uint32_t
{
	ray_gen,
	miss,
	miss_shadow,
	closest_hit,
	anyhit_shadow,
	shader_group_max_count
};

void scene_manager::create(vulkan_application* _app, uint32_t _width, uint32_t _height)
{
	app = _app;

	material_manager = std::make_unique<scene_material_manager>(app);
	model_manager = std::make_unique<scene_model_manager>(app, material_manager.get());
	light_manager = std::make_unique<scene_light_manager>(app);


	// create sampler
	vk::PhysicalDeviceProperties properties = app->get_physical_device().getProperties();
	vk::SamplerCreateInfo sampler_info({}, vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear,
		vk::SamplerAddressMode::eClampToBorder, vk::SamplerAddressMode::eClampToBorder, vk::SamplerAddressMode::eClampToBorder,
		0.f, vk::True, properties.limits.maxSamplerAnisotropy, vk::False, vk::CompareOp::eAlways, 0.f, 1.f,
		vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);
	image_sampler = vk::raii::Sampler(app->get_device(), sampler_info);


	active_camera.set_position(glm::vec3(0.f, 0.f, 0.f));
	active_camera.set_direction(glm::vec3(0.f, 0.f, -1.f));
	active_camera.set_world_up(glm::vec3(0.f, 1.f, 0.f));


	commandbuffers = vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::eSecondary, vulkan_common::MAX_FRAMES_IN_FLIGHT), app->get_device(), app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue());


	color_format = vk::Format::eR16G16B16A16Sfloat;

	create_rasterization();
	create_ray_tracing();


	resize(_width, _height);
}

void scene_manager::resize(uint32_t _width, uint32_t _height)
{
	width = _width;
	height = _height;

	is_dirty = true;

	// render_output
	vk::ImageCreateInfo render_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(width, height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage/*for ray tracing*/, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo render_view_info({}, {}, vk::ImageViewType::e2D, color_format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	render_output.create(app->get_physical_device(), app->get_device(), render_image_info, render_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));

	resize_rasterization();
	resize_ray_tracing();
}

void scene_manager::update()
{
	if (!is_dirty) [[likely]]
	{
		return;
	}

	is_dirty = false;
	material_manager->update(commandbuffers.at(static_cast<size_t>(current_frame - 1 + vulkan_common::MAX_FRAMES_IN_FLIGHT) % vulkan_common::MAX_FRAMES_IN_FLIGHT));
	model_manager->update(commandbuffers.at(static_cast<size_t>(current_frame - 1 + vulkan_common::MAX_FRAMES_IN_FLIGHT) % vulkan_common::MAX_FRAMES_IN_FLIGHT));
	light_manager->update(commandbuffers.at(static_cast<size_t>(current_frame - 1 + vulkan_common::MAX_FRAMES_IN_FLIGHT) % vulkan_common::MAX_FRAMES_IN_FLIGHT));

	update_rasterization();
	update_ray_tracing();
}

const vulkan_commandbuffer& scene_manager::render()
{
	vulkan_commandbuffer& commandbuffer = commandbuffers.at(current_frame);
	commandbuffer.begin_record({});

	if (!use_ray_tracing)
	{
		render_rasterization(*commandbuffer);
	}
	else
	{
		render_ray_tracing(*commandbuffer);
	}

	commandbuffer.end_record();

	current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;

	return commandbuffer;
}

void scene_manager::destroy()
{
	material_manager->clear();
	model_manager->clear();
	light_manager->clear();
}

void scene_manager::need_update() noexcept
{
	is_dirty = true;
}

std::shared_ptr<scene_model> scene_manager::create_model(const std::u8string& _model_name)
{
	is_dirty = true;
	return model_manager->create(std::u8string(MODELS_PATH) + _model_name);
}

void scene_manager::remove_model(const std::weak_ptr<scene_model>& _model) noexcept
{
	is_dirty = true;
	model_manager->remove(_model);
}

std::shared_ptr<scene_material> scene_manager::create_material(const std::wstring& _characters)
{
	is_dirty = true;
	return material_manager->create(std::u8string(FONTS_PATH) + u8"LXGWWenKaiGB-Medium.ttf", static_cast<uint32_t>(height / 9.0 * 2), _characters);
}

void scene_manager::remove_material(const std::weak_ptr<scene_material>& _material) noexcept
{
	is_dirty = true;
	material_manager->remove(_material);
}

std::shared_ptr<scene_light> scene_manager::create_light(light_type _type) noexcept
{
	is_dirty = true;
	return light_manager->create(_type);
}

void scene_manager::remove_light(const std::weak_ptr<scene_light>& _light) noexcept
{
	is_dirty = true;
	light_manager->remove(_light);
}

void scene_manager::set_use_ray_tracing(bool _use_ray_tracing) noexcept
{
	is_dirty = true;
	use_ray_tracing = _use_ray_tracing;
}

scene_camera& scene_manager::get_active_camera() noexcept
{
	return active_camera;
}

vulkan_image& scene_manager::get_render_image() noexcept
{
	return render_output;
}

void scene_manager::create_rasterization()
{
	// pipeline
	std::array bindings{
		vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex, nullptr),
		vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eFragment, nullptr),
		vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eCombinedImageSampler, 1024, vk::ShaderStageFlagBits::eFragment, nullptr)
	};

	vk::PushConstantRange push_constant(vk::ShaderStageFlagBits::eVertex, 0, sizeof(scene_manager::push_constant));

	auto binding = model_vertex::get_binding_description();
	auto attribute = model_vertex::get_attribute_descriptions<model_vertex_type::position, model_vertex_type::uv>();

	auto spirv_code = SHADER_COMPILER.compile_shader_to_spv(std::u8string(SHADERS_PATH) + u8"rasterization.slang", { VERT_ENTYR_NAME, FRAG_ENTYR_NAME });
	if (spirv_code.empty())
	{
		throw std::runtime_error("compile .spv failed!");
	}

	vk::raii::ShaderModule shaderModule(app->get_device(), vk::ShaderModuleCreateInfo({}, spirv_code.size() * sizeof(char), reinterpret_cast<const uint32_t*>(spirv_code.data())));
	std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = {
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, shaderModule, VERT_ENTYR_NAME.data()),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, shaderModule, FRAG_ENTYR_NAME.data()),
	};

	raster_pipeline.create(app->get_device(), bindings, std::span(&push_constant, 1), std::span(&binding, 1), attribute, shader_stages,
		vk::PrimitiveTopology::eTriangleList, vk::PolygonMode::eFill, vk::CullModeFlagBits::eBack, vk::FrontFace::eCounterClockwise,
		vulkan_common::MSAA_SAMPLE_COUNT, vk::True, std::span(&color_format, 1), vulkan_common::DEPTH_FORMAT);
}

void scene_manager::resize_rasterization()
{
	// msaa color
	vk::ImageCreateInfo color_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(width, height, 1), 1, 1, vulkan_common::MSAA_SAMPLE_COUNT, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eColorAttachment, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo color_view_info({}, {}, vk::ImageViewType::e2D, color_format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	raster_color_image.create(app->get_physical_device(), app->get_device(), color_image_info, color_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));

	// depth
	vk::ImageCreateInfo depth_image_info({}, vk::ImageType::e2D, vulkan_common::DEPTH_FORMAT, vk::Extent3D(width, height, 1), 1, 1, vulkan_common::MSAA_SAMPLE_COUNT, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eDepthStencilAttachment, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo depth_view_info({}, {}, vk::ImageViewType::e2D, vulkan_common::DEPTH_FORMAT, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth, {}, 1, 0, 1), nullptr);
	raster_depth_image.create(app->get_physical_device(), app->get_device(), depth_image_info, depth_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearDepthStencilValue(1.f, 0));
}

void scene_manager::update_rasterization()
{
	// update draw commands
	auto draw_commands = model_manager->get_models()
		| std::views::transform([](const auto& m)
			{
				return vk::DrawIndexedIndirectCommand(static_cast<uint32_t>(m->model_info->indices.size()), 1, static_cast<uint32_t>(m->model_info->index_offset / sizeof(uint32_t)), static_cast<uint32_t>(m->model_info->vertex_offset / sizeof(model_vertex)), 0);
			})
		| std::ranges::to<std::vector>();


	// begin a commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), app->get_device(), app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue()).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

	vk::DeviceSize draw_commands_size = sizeof(draw_commands.front()) * draw_commands.size();
	raster_draw_commands.create(app->get_physical_device(), app->get_device(), draw_commands_size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);

	vulkan_buffer draw_commands_staging_buffer;
	draw_commands_staging_buffer.create(app->get_physical_device(), app->get_device(), draw_commands_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

	memcpy(draw_commands_staging_buffer.get_buffer_address().hostAddress, draw_commands.data(), draw_commands_size);
	vulkan_buffer::copy_buffer_to_buffer(*commandbuffer, draw_commands_staging_buffer.get_buffer(), raster_draw_commands.get_buffer(), vk::BufferCopy2(0, 0, draw_commands_size));

	// commandbuffer submit
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);


	// update descriptor pool
	raster_descriptor_sets.clear();

	std::array pool_size = {
		vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 2),
		vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 2),
		vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 2048)
	};

	vk::DescriptorPoolCreateInfo pool_create_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet | vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind, 2, pool_size);
	raster_descriptor_pool = vk::raii::DescriptorPool(app->get_device(), pool_create_info);

	// descriptor set
	std::vector<vk::DescriptorSetLayout> layouts(2, raster_pipeline.get_descriptor_set_layout());
	auto alloc_info = vk::DescriptorSetAllocateInfo(raster_descriptor_pool, layouts);
	raster_descriptor_sets = app->get_device().allocateDescriptorSets(alloc_info);

	std::ranges::for_each(raster_descriptor_sets | std::views::enumerate, [&](const auto& _pair)
		{
			const auto& [index, descriptor_set] = _pair;
			std::vector<vk::WriteDescriptorSet> write_sets;

			vk::DescriptorBufferInfo model_buffer_info(model_manager->get_ssbo_buffer().get_buffer(), 0, vk::WholeSize);
			write_sets.emplace_back(vk::WriteDescriptorSet(descriptor_set, 0, {}, vk::DescriptorType::eStorageBuffer, {}, model_buffer_info));

			vk::DescriptorBufferInfo material_buffer_info(material_manager->get_ssbo_buffer().get_buffer(), 0, vk::WholeSize);
			write_sets.emplace_back(vk::WriteDescriptorSet(descriptor_set, 1, {}, vk::DescriptorType::eStorageBuffer, {}, material_buffer_info));

			std::array sampler = { *image_sampler };
			auto material_sets = material_manager->get_descriptor_info(sampler)
				| std::views::enumerate
				| std::views::transform([&descriptor_set](const auto& _pair)
					{
						const auto& [index, image_info] = _pair;
						return vk::WriteDescriptorSet(descriptor_set, 2, static_cast<uint32_t>(index), vk::DescriptorType::eCombinedImageSampler, image_info, {});
					});
			std::ranges::move(material_sets, std::back_inserter(write_sets));

			app->get_device().updateDescriptorSets(write_sets, {});
		});
}

void scene_manager::render_rasterization(const vk::raii::CommandBuffer& _commandbuffer) noexcept
{
	std::vector<vk::ImageMemoryBarrier2> begin_barrier;
	begin_barrier.emplace_back(render_output.set_layout(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	begin_barrier.emplace_back(raster_color_image.set_layout(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	begin_barrier.emplace_back(raster_depth_image.set_layout(vk::ImageLayout::eDepthStencilAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1)));
	_commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));


	vk::RenderingAttachmentInfo colorAttachmentInfo(raster_color_image.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::ResolveModeFlagBits::eAverage,
		render_output.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, raster_color_image.get_clear_value());
	vk::RenderingAttachmentInfo depthAttachmentInfo(raster_depth_image.get_imageview(), vk::ImageLayout::eDepthStencilAttachmentOptimal, vk::ResolveModeFlagBits::eNone,
		{}, vk::ImageLayout::eUndefined, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare, raster_depth_image.get_clear_value());

	vk::RenderingInfo renderingInfo({}, vk::Rect2D({ 0, 0 }, { static_cast<uint32_t>(width), static_cast<uint32_t>(height) }), 1, {}, colorAttachmentInfo, &depthAttachmentInfo, nullptr, nullptr);

	_commandbuffer.setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f));
	_commandbuffer.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(width, height)));

	_commandbuffer.beginRendering(renderingInfo);


	_commandbuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, raster_pipeline.get_pipeline());
	_commandbuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, raster_pipeline.get_pipeline_layout(), 0, *(raster_descriptor_sets.at(current_frame)), nullptr);

	// Push constant with camera
	scene_manager::push_constant pc{
		active_camera.get_projection_matrix(),
		active_camera.get_view_matrix(),
	};
	_commandbuffer.pushConstants2(vk::PushConstantsInfo(raster_pipeline.get_pipeline_layout(), vk::ShaderStageFlagBits::eVertex, 0, sizeof(scene_manager::push_constant), &pc));

	_commandbuffer.bindVertexBuffers(0, *(model_manager->get_vertices_buffer().get_buffer()), vk::DeviceSize(0));
	_commandbuffer.bindIndexBuffer(*(model_manager->get_indices_buffer().get_buffer()), vk::DeviceSize(0), vk::IndexType::eUint32);
	_commandbuffer.drawIndexedIndirect(raster_draw_commands.get_buffer(), 0, static_cast<uint32_t>(model_manager->get_models().size()), sizeof(vk::DrawIndexedIndirectCommand));

	_commandbuffer.endRendering();
}

void scene_manager::create_ray_tracing()
{
	// create pipeline
	std::array bindings{
		vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eAccelerationStructureKHR, 1, vk::ShaderStageFlagBits::eAll, nullptr),
		vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eAll, nullptr),
		vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr),
		vk::DescriptorSetLayoutBinding(3, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr),
		vk::DescriptorSetLayoutBinding(4, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr),
		vk::DescriptorSetLayoutBinding(5, vk::DescriptorType::eCombinedImageSampler, 1024, vk::ShaderStageFlagBits::eAll, nullptr),
	};

	vk::PushConstantRange push_constant(vk::ShaderStageFlagBits::eAll, 0, sizeof(scene_manager::push_constant));

	auto spirv_code = SHADER_COMPILER.compile_shader_to_spv(std::u8string(SHADERS_PATH) + u8"ray_tracing.slang", { RAY_GEN_ENTYR_NAME, RAY_MISS_ENTYR_NAME, RAY_SHADOW_MISS_ENTYR_NAME, RAY_CLOSEST_HIT_ENTRY_NAME, RAY_SHADOW_ANY_HIT_ENTYR_NAME });
	if (spirv_code.empty())
	{
		throw std::runtime_error("compile .spv failed!");
	}
	vk::raii::ShaderModule shaderModule(app->get_device(), vk::ShaderModuleCreateInfo({}, spirv_code.size() * sizeof(char), reinterpret_cast<const uint32_t*>(spirv_code.data())));

	std::array<vk::PipelineShaderStageCreateInfo, static_cast<size_t>(stage_indices::shader_group_max_count)> shader_stages = {
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eRaygenKHR, shaderModule, RAY_GEN_ENTYR_NAME.data()),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eMissKHR, shaderModule, RAY_MISS_ENTYR_NAME.data()),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eMissKHR, shaderModule, RAY_SHADOW_MISS_ENTYR_NAME.data()),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eClosestHitKHR, shaderModule, RAY_CLOSEST_HIT_ENTRY_NAME.data()),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eAnyHitKHR, shaderModule, RAY_SHADOW_ANY_HIT_ENTYR_NAME.data()),
	};

	std::vector<vk::RayTracingShaderGroupCreateInfoKHR> shader_groups = {
		// Group 0: Ray generation (general)
		vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eGeneral, static_cast<uint32_t>(stage_indices::ray_gen)),
		// Group 1: Primary miss (general)
		vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eGeneral, static_cast<uint32_t>(stage_indices::miss)),
		// Group 2: Shadow miss (general)
		vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eGeneral, static_cast<uint32_t>(stage_indices::miss_shadow)),
		// Group 3: Primary hit group (triangles)
		vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup, vk::ShaderUnusedKHR, static_cast<uint32_t>(stage_indices::closest_hit)),
		// Group 4: Shadow hit group (triangles, any-hit only)
		vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup, vk::ShaderUnusedKHR, vk::ShaderUnusedKHR, static_cast<uint32_t>(stage_indices::anyhit_shadow)),
	};

	auto props = app->get_physical_device().getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceRayTracingPipelinePropertiesKHR, vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
	const auto& properties = props.get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
	rt_pipeline.create(app->get_device(), bindings, std::span(&push_constant, 1), shader_stages, shader_groups, std::min(9u, properties.maxRayRecursionDepth));


	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), app->get_device(), app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue()).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

	// create shader binding table (now needs 5 groups)
	rt_sbt.create(app->get_physical_device(), app->get_device(), commandbuffer, rt_pipeline.get_pipeline(), static_cast<uint32_t>(shader_stages.size()));

	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);
}

void scene_manager::resize_ray_tracing()
{
}

void scene_manager::update_ray_tracing()
{
	// reset path tracing accumulation
	rt_frame_index = 0;

	// generate tlas
	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), app->get_device(), app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue()).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

	// create top level acceleration structure
	auto rt_instances = model_manager->get_models()
		| std::views::transform([](const auto& m)
			{
				return m->get_blas_instance();
			})
		| std::ranges::to<std::vector>();

	vk::DeviceSize instance_buffer_size = sizeof(rt_instances.front()) * rt_instances.size();
	rt_tlas.resize(vulkan_common::MAX_FRAMES_IN_FLIGHT);
	std::ranges::for_each(rt_tlas, [&](auto& _tlas)
		{
			vulkan_buffer instance_staging_buffer;
			instance_staging_buffer.create(app->get_physical_device(), app->get_device(), instance_buffer_size, vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal);

			vulkan_buffer staging_buffer;
			staging_buffer.create(app->get_physical_device(), app->get_device(), instance_buffer_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

			memcpy(staging_buffer.get_buffer_address().hostAddress, rt_instances.data(), instance_buffer_size);
			vulkan_buffer::copy_buffer_to_buffer(*commandbuffer, staging_buffer.get_buffer(), instance_staging_buffer.get_buffer(), vk::BufferCopy2(0, 0, instance_buffer_size));

			_tlas.create_top_level_acceleration_structure(app->get_physical_device(), app->get_device(), *commandbuffer, static_cast<uint32_t>(rt_instances.size()), instance_staging_buffer.get_buffer_address().deviceAddress);

			commandbuffer.add_staging_buffer(std::move(instance_staging_buffer));
			commandbuffer.add_staging_buffer(std::move(staging_buffer));
		});


	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);


	// update descriptor pool
	rt_descriptor_sets.clear();

	std::array pool_size = {
		vk::DescriptorPoolSize(vk::DescriptorType::eAccelerationStructureKHR, 2),
		vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, 2),
		vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 6),
		vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 2048)
	};

	vk::DescriptorPoolCreateInfo pool_create_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet | vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind, 2, pool_size);
	rt_descriptor_pool = vk::raii::DescriptorPool(app->get_device(), pool_create_info);

	// descriptor set
	std::vector<vk::DescriptorSetLayout> layouts(2, rt_pipeline.get_descriptor_set_layout());
	auto alloc_info = vk::DescriptorSetAllocateInfo(rt_descriptor_pool, layouts);
	rt_descriptor_sets = app->get_device().allocateDescriptorSets(alloc_info);

	std::ranges::for_each(rt_descriptor_sets | std::views::enumerate, [&](const auto& _pair)
		{
			const auto& [index, descriptor_set] = _pair;
			std::vector<vk::WriteDescriptorSet> write_sets;

			vk::DescriptorBufferInfo as_buffer_info(rt_tlas.at(index).get_buffer(), 0, sizeof(vk::AccelerationStructureInstanceKHR) * rt_instances.size());
			vk::StructureChain<vk::WriteDescriptorSet, vk::WriteDescriptorSetAccelerationStructureKHR> as_write_set(
				vk::WriteDescriptorSet(descriptor_set, 0, {}, vk::DescriptorType::eAccelerationStructureKHR, {}, as_buffer_info),
				vk::WriteDescriptorSetAccelerationStructureKHR(*(rt_tlas.at(index).get_acceleration_structure())));
			write_sets.push_back(as_write_set.get());

			vk::DescriptorImageInfo storage_image_info(nullptr, render_output.get_imageview(), vk::ImageLayout::eGeneral);
			write_sets.emplace_back(vk::WriteDescriptorSet(descriptor_set, 1, {}, vk::DescriptorType::eStorageImage, storage_image_info, {}));

			vk::DescriptorBufferInfo model_buffer_info(model_manager->get_ssbo_buffer().get_buffer(), 0, vk::WholeSize);
			write_sets.emplace_back(vk::WriteDescriptorSet(descriptor_set, 2, {}, vk::DescriptorType::eStorageBuffer, {}, model_buffer_info));

			vk::DescriptorBufferInfo material_buffer_info(material_manager->get_ssbo_buffer().get_buffer(), 0, vk::WholeSize);
			write_sets.emplace_back(vk::WriteDescriptorSet(descriptor_set, 3, {}, vk::DescriptorType::eStorageBuffer, {}, material_buffer_info));

			vk::DescriptorBufferInfo light_buffer_info(light_manager->get_ssbo_buffer().get_buffer(), 0, vk::WholeSize);
			write_sets.emplace_back(vk::WriteDescriptorSet(descriptor_set, 4, {}, vk::DescriptorType::eStorageBuffer, {}, light_buffer_info));

			std::array sampler = { *image_sampler };
			auto material_sets = material_manager->get_descriptor_info(sampler)
				| std::views::enumerate
				| std::views::transform([&descriptor_set](const auto& _pair)
					{
						const auto& [index, image_info] = _pair;
						return vk::WriteDescriptorSet(descriptor_set, 5, static_cast<uint32_t>(index), vk::DescriptorType::eCombinedImageSampler, image_info, {});
					});
			std::ranges::move(material_sets, std::back_inserter(write_sets));

			app->get_device().updateDescriptorSets(write_sets, {});
		});
}

void scene_manager::render_ray_tracing(const vk::raii::CommandBuffer& _commandbuffer) noexcept
{
	std::vector<vk::ImageMemoryBarrier2> begin_barrier;
	begin_barrier.emplace_back(render_output.set_layout(vk::ImageLayout::eGeneral, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	_commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));

	_commandbuffer.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, rt_pipeline.get_pipeline());
	_commandbuffer.bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR, rt_pipeline.get_pipeline_layout(), 0, *(rt_descriptor_sets.at(current_frame)), nullptr);

	scene_manager::push_constant pc{
		glm::inverse(active_camera.get_projection_matrix()),
		glm::inverse(active_camera.get_view_matrix()),
		static_cast<uint32_t>(light_manager->get_lights().size()),
		rt_frame_index,
	};
	_commandbuffer.pushConstants2(vk::PushConstantsInfo(rt_pipeline.get_pipeline_layout(), vk::ShaderStageFlagBits::eAll, 0, sizeof(scene_manager::push_constant), &pc));

	_commandbuffer.traceRaysKHR(rt_sbt.get_raygen_region(), rt_sbt.get_miss_region(), rt_sbt.get_hit_region(), rt_sbt.get_callable_region(), width, height, 1);

	//vk::MemoryBarrier2 end_barrier(vk::PipelineStageFlagBits2::eRayTracingShaderKHR, vk::AccessFlagBits2::eShaderWrite, vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryRead);
	//_commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, end_barrier, {}, {}));

	rt_frame_index++;
}