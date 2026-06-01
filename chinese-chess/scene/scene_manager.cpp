#include "chess_board.h"
#include "chess_board_line.h"
#include "scene_manager.h"
#include "skybox/scene_skybox.h"
#include "tools/image_helper.h"
#include "tools/shader_compiler.h"
#include "vulkan_core/vulkan_common.h"
#include <ranges>
#include <string>

void scene_manager::create(vulkan_application* _app, uint32_t _width, uint32_t _height)
{
	app = _app;

	color_format = vk::Format::eR16G16B16A16Sfloat;

	active_camera.set_position(glm::vec3(0.f, 0.f, 1.1f));
	active_camera.set_direction(glm::vec3(0.f, 0.f, -1.f));
	active_camera.set_world_up(glm::vec3(0.f, 1.f, 0.f));


	//// sort by draw index
	//// skybox first
	//cubemap = std::make_unique<scene_cubemap>();
	//cubemap->create(app, std::u8string(TEXTURES_PATH) + u8"干裂地面.hdr");

	//auto skybox = std::make_unique<scene_skybox>();
	//skybox->set_cubemap(cubemap.get());
	//nodes.emplace_back(std::move(skybox));

	auto board = std::make_unique<chess_board>();
	board_ = board.get();
	nodes.emplace_back(std::move(board));

	auto board_line = std::make_unique<chess_board_line>();
	board_line_ = board_line.get();
	nodes.emplace_back(std::move(board_line));

	auto pieces = std::make_unique<chess_manager>();
	piece_manager = pieces.get();
	nodes.emplace_back(std::move(pieces));


	for (auto& _node : nodes)
	{
		_node->create(app, vulkan_common::MSAA_SAMPLE_COUNT, color_format, vulkan_common::DEPTH_FORMAT);
	}

	commandbuffers = vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(_app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::eSecondary, vulkan_common::MAX_FRAMES_IN_FLIGHT), _app->get_device(), _app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue());

	resize(_width, _height);

	tlas.resize(vulkan_common::MAX_FRAMES_IN_FLIGHT);


	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(_app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), _app->get_device(), _app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue()).front());

	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);


	// create all shaders
	enum class StageIndices
	{
		eRaygen,
		eMiss,
		eClosestHit,
		eShaderGroupCount
	};

	std::array bindings{
		vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eAccelerationStructureKHR, 1, vk::ShaderStageFlagBits::eAll, nullptr),
		vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eAll, nullptr),
		vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eAll, nullptr), // 棋盘字体纹理 (2D)
		vk::DescriptorSetLayoutBinding(3, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eAll, nullptr), // 棋子字体纹理 (2D Array)
	};

	vk::PushConstantRange push_constant(vk::ShaderStageFlagBits::eAll, 0, sizeof(scene_manager::PushConstant));

	auto spirv_code = SHADER_COMPILER.compile_shader_to_spv(std::u8string(SHADERS_PATH) + u8"ray_tracing.slang", { "rgenMain", "rmissMain", "rchitMain" });
	if (spirv_code.empty())
	{
		throw std::runtime_error("compile .spv failed!");
	}
	vk::raii::ShaderModule shaderModule(_app->get_device(), vk::ShaderModuleCreateInfo({}, spirv_code.size() * sizeof(char), reinterpret_cast<const uint32_t*>(spirv_code.data())));

	std::array<vk::PipelineShaderStageCreateInfo, static_cast<size_t>(StageIndices::eShaderGroupCount)> shader_stages = {
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eRaygenKHR, shaderModule, "rgenMain"),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eMissKHR, shaderModule, "rmissMain"),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eClosestHitKHR, shaderModule, "rchitMain"),
	};

	std::vector<vk::RayTracingShaderGroupCreateInfoKHR> shader_groups = {
		vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eGeneral, static_cast<uint32_t>(StageIndices::eRaygen)),
		vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eGeneral, static_cast<uint32_t>(StageIndices::eMiss)),
		vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup, vk::ShaderUnusedKHR, static_cast<uint32_t>(StageIndices::eClosestHit)),
	};

	auto props = app->get_physical_device().getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceRayTracingPipelinePropertiesKHR, vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
	const auto& properties = props.get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
	pipeline.create(app->get_device(), bindings, std::span(&push_constant, 1), shader_stages, shader_groups, std::max(3u, properties.maxRayRecursionDepth));


	// create shader binding table
	vulkan_buffer sbt_staging_buffer;
	sbt.create(app->get_physical_device(), app->get_device(), *commandbuffer, pipeline.get_pipeline(), static_cast<uint32_t>(shader_stages.size()), sbt_staging_buffer);

	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);
}

void scene_manager::resize(uint32_t _width, uint32_t _height)
{
	width = _width;
	height = _height;

	// camera projection
	constexpr float camera_height = 1.3f;
	active_camera.set_ortho_projection(-camera_height * width / height, camera_height * width / height, -camera_height, camera_height, 1.f, -10.f);

	// render_output
	vk::ImageCreateInfo render_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(_width, _height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage/*for ray tracing*/, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo render_view_info({}, {}, vk::ImageViewType::e2D, color_format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	render_output.create(app->get_physical_device(), app->get_device(), render_image_info, render_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));

	// msaa color
	vk::ImageCreateInfo color_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(_width, _height, 1), 1, 1, vulkan_common::MSAA_SAMPLE_COUNT, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eColorAttachment, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo color_view_info({}, {}, vk::ImageViewType::e2D, color_format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	color_image.create(app->get_physical_device(), app->get_device(), color_image_info, color_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));

	// depth
	vk::ImageCreateInfo depth_image_info({}, vk::ImageType::e2D, vulkan_common::DEPTH_FORMAT, vk::Extent3D(_width, _height, 1), 1, 1, vulkan_common::MSAA_SAMPLE_COUNT, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eDepthStencilAttachment, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo depth_view_info({}, {}, vk::ImageViewType::e2D, vulkan_common::DEPTH_FORMAT, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth, {}, 1, 0, 1), nullptr);
	depth_image.create(app->get_physical_device(), app->get_device(), depth_image_info, depth_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearDepthStencilValue(1.f, 0));


	for (auto& _node : nodes)
	{
		_node->resize(app, _width, _height);
	}
}

void scene_manager::update()
{
	for (auto& _node : nodes)
	{
		_node->update(&active_camera);
	}

	// create top level acceleration structure
	tlas_instances = nodes
		| std::views::transform([](const auto& n) { return n->get_all_blas_info(); })
		| std::views::join
		| std::ranges::to<std::vector>();

	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), app->get_device(), app->get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue()).front());

	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

	vk::DeviceSize instance_buffer_size = sizeof(vk::AccelerationStructureInstanceKHR) * tlas_instances.size();

	vulkan_buffer instance_staging_buffer;
	instance_staging_buffer.create(app->get_physical_device(), app->get_device(), instance_buffer_size, vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal);

	vulkan_buffer staging_buffer;
	staging_buffer.create(app->get_physical_device(), app->get_device(), instance_buffer_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

	memcpy(staging_buffer.get_buffer_address().hostAddress, tlas_instances.data(), instance_buffer_size);
	vulkan_buffer::copy_buffer_to_buffer(*commandbuffer, staging_buffer.get_buffer(), instance_staging_buffer.get_buffer(), vk::BufferCopy2(0, 0, instance_buffer_size));

	tlas.at(current_frame).create_top_level_acceleration_structure(app->get_physical_device(), app->get_device(), *commandbuffer, static_cast<uint32_t>(tlas_instances.size()), instance_staging_buffer.get_buffer_address().deviceAddress);

	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);
}

const vulkan_commandbuffer& scene_manager::render()
{
	const vulkan_commandbuffer& commandbuffer = commandbuffers.at(current_frame);
	commandbuffer.begin_record({});


	std::vector<vk::ImageMemoryBarrier2> begin_barrier;
	begin_barrier.emplace_back(color_image.set_layout(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	begin_barrier.emplace_back(render_output.set_layout(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	begin_barrier.emplace_back(depth_image.set_layout(vk::ImageLayout::eDepthStencilAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));


	vk::RenderingAttachmentInfo colorAttachmentInfo(color_image.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::ResolveModeFlagBits::eAverage,
		render_output.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, color_image.get_clear_value());
	vk::RenderingAttachmentInfo depthAttachmentInfo(depth_image.get_imageview(), vk::ImageLayout::eDepthStencilAttachmentOptimal, vk::ResolveModeFlagBits::eNone,
		{}, vk::ImageLayout::eUndefined, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare, depth_image.get_clear_value());

	vk::RenderingInfo renderingInfo({}, vk::Rect2D({ 0, 0 }, { static_cast<uint32_t>(width), static_cast<uint32_t>(height) }), 1, {}, colorAttachmentInfo, &depthAttachmentInfo, nullptr, nullptr);

	(*commandbuffer).setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f));
	(*commandbuffer).setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(width, height)));

	(*commandbuffer).beginRendering(renderingInfo);


	for (auto& _node : nodes)
	{
		_node->render(*commandbuffer);
	}

	(*commandbuffer).endRendering();

	commandbuffer.end_record();

	current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;

	return commandbuffer;
}

const vulkan_commandbuffer& scene_manager::ray_tracing_render()
{
	const vulkan_commandbuffer& commandbuffer = commandbuffers.at(current_frame);
	commandbuffer.begin_record({});


	std::vector<vk::ImageMemoryBarrier2> begin_barrier;
	begin_barrier.emplace_back(render_output.set_layout(vk::ImageLayout::eGeneral, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));

	(*commandbuffer).bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, pipeline.get_pipeline());

	// Build push descriptors (4 bindings): AS, storage image, font images[2], font samplers[2]
	std::vector<vk::WriteDescriptorSet> write_sets;

	// Binding 0: Acceleration structure
	vk::DescriptorBufferInfo as_buffer_info(tlas.at(current_frame).get_buffer(), 0, sizeof(vk::AccelerationStructureInstanceKHR) * tlas_instances.size());
	vk::StructureChain<vk::WriteDescriptorSet, vk::WriteDescriptorSetAccelerationStructureKHR> as_write_set(
		vk::WriteDescriptorSet(nullptr, 0, {}, vk::DescriptorType::eAccelerationStructureKHR, {}, as_buffer_info),
		vk::WriteDescriptorSetAccelerationStructureKHR(*(tlas.at(current_frame).get_acceleration_structure())));
	write_sets.push_back(as_write_set.get());

	// Binding 1: Storage image (render output)
	vk::DescriptorImageInfo storage_image_info(nullptr, render_output.get_imageview(), vk::ImageLayout::eGeneral);
	write_sets.emplace_back(vk::WriteDescriptorSet(nullptr, 1, {}, vk::DescriptorType::eStorageImage, storage_image_info, {}));

	// Binding 2: 棋盘字体纹理 (2D)
	vk::DescriptorImageInfo board_font_image_info(*board_->get_font_sampler(), *board_->get_font_image().get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);
	write_sets.emplace_back(vk::WriteDescriptorSet(nullptr, 2, {}, vk::DescriptorType::eCombinedImageSampler, board_font_image_info, {}, {}));

	// Binding 3: 棋子字体纹理 (2D Array)
	vk::DescriptorImageInfo piece_font_image_info(*piece_manager->get_font_sampler(), *piece_manager->get_font_image().get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);
	write_sets.emplace_back(vk::WriteDescriptorSet(nullptr, 3, {}, vk::DescriptorType::eCombinedImageSampler, piece_font_image_info, {}, {}));

	(*commandbuffer).pushDescriptorSet(vk::PipelineBindPoint::eRayTracingKHR, pipeline.get_pipeline_layout(), 0, write_sets);

	// Push constant with camera + device addresses for raw-buffer-load
	scene_manager::PushConstant push_constant{
		active_camera.get_position(),
		glm::inverse(active_camera.get_projection_matrix()),
		glm::inverse(active_camera.get_view_matrix()),
		active_camera.get_direction(),
		board_->get_vertices_device_address(),
		board_->get_indices_device_address(),
		board_line_->get_vertices_device_address(),
		board_line_->get_indices_device_address(),
		piece_manager->get_vertices_device_address(),
		piece_manager->get_indices_device_address(),
	};
	(*commandbuffer).pushConstants2(vk::PushConstantsInfo(pipeline.get_pipeline_layout(), vk::ShaderStageFlagBits::eAll, 0, sizeof(scene_manager::PushConstant), &push_constant));

	// Ray trace
	const auto& extent = app->get_swapchain().get_extent();
	(*commandbuffer).traceRaysKHR(sbt.get_raygen_region(), sbt.get_miss_region(), sbt.get_hit_region(), sbt.get_callable_region(), extent.width, extent.height, 1);

	// Barrier: ray tracing writes complete → ready for tonemapping read
	vk::MemoryBarrier2 barrier(vk::PipelineStageFlagBits2::eRayTracingShaderKHR, vk::AccessFlagBits2::eShaderWrite, vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryRead);
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, barrier, {}, {}));

	commandbuffer.end_record();

	current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;

	return commandbuffer;
}

void scene_manager::destroy()
{
	for (auto& _node : nodes)
	{
		_node->destroy();
	}
}

chess_manager* scene_manager::get_piece_manager() const noexcept
{
	return piece_manager;
}

vulkan_image& scene_manager::get_render_image() noexcept
{
	return render_output;
}
