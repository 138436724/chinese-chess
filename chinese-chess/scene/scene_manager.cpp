#include "chess_board.h"
#include "chess_board_line.h"
#include "scene_manager.h"
#include "skybox/scene_skybox.h"
#include "tools/image_helper.h"
#include "vulkan_core/vulkan_common.h"

void scene_manager::create(vulkan_application* _app, uint32_t _width, uint32_t _height)
{
	app = _app;

	color_format = vk::Format::eR16G16B16A16Sfloat;
	//color_format = app->get_swapchain().get_format(); //todo may be need srgb format

	active_camera.set_position(glm::vec3(0.f, 0.f, 1.1f));
	active_camera.set_direction(glm::vec3(0.f, 0.f, -1.f));
	active_camera.set_world_up(glm::vec3(0.f, 1.f, 0.f));


	// sort by draw index
	// skybox first
	cubemap = std::make_unique<scene_cubemap>();
	cubemap->create(app, std::u8string(TEXTURES_PATH) + u8"干裂地面.hdr");

	auto skybox = std::make_unique<scene_skybox>();
	skybox->set_cubemap(cubemap.get());
	nodes.emplace_back(std::move(skybox));

	nodes.emplace_back(pro::make_proxy<scene_node, chess_board>());
	nodes.emplace_back(pro::make_proxy<scene_node, chess_board_line>());

	auto pieces = std::make_unique<chess_manager>();
	piece_manager = pieces.get();
	nodes.emplace_back(std::move(pieces));


	for (auto& _node : nodes)
	{
		_node->create(_app, vulkan_common::MASS_SAMPLE_COUNT, color_format, vulkan_common::DEPTH_FORMAT);
	}

	resize(_width, _height);
}

void scene_manager::resize(uint32_t _width, uint32_t _height)
{
	width = _width;
	height = _height;

	// camera projection
	constexpr float camera_height = 1.3f;
	active_camera.set_ortho_projection(-camera_height * width / height, camera_height * width / height, -camera_height, camera_height, 1.f, -10.f);

	// render_output
	vk::ImageCreateInfo render_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(_width, _height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo render_view_info({}, {}, vk::ImageViewType::e2D, color_format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	render_output.create(app->get_physical_device(), app->get_device(), render_image_info, render_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));

	// msaa color
	vk::ImageCreateInfo color_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(_width, _height, 1), 1, 1, vulkan_common::MASS_SAMPLE_COUNT, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eColorAttachment, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo color_view_info({}, {}, vk::ImageViewType::e2D, color_format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	color_image.create(app->get_physical_device(), app->get_device(), color_image_info, color_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));

	// depth
	vk::ImageCreateInfo depth_image_info({}, vk::ImageType::e2D, vulkan_common::DEPTH_FORMAT, vk::Extent3D(_width, _height, 1), 1, 1, vulkan_common::MASS_SAMPLE_COUNT, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eDepthStencilAttachment, vk::SharingMode::eExclusive, 0);
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
}

void scene_manager::render(const vk::raii::CommandBuffer& _commandbuffer)
{
	std::vector<vk::ImageMemoryBarrier2> begin_barrier;
	begin_barrier.emplace_back(color_image.set_layout(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	begin_barrier.emplace_back(render_output.set_layout(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	begin_barrier.emplace_back(depth_image.set_layout(vk::ImageLayout::eDepthStencilAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1)));
	_commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));


	vk::RenderingAttachmentInfo colorAttachmentInfo(color_image.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::ResolveModeFlagBits::eAverage,
		render_output.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, color_image.get_clear_value());
	vk::RenderingAttachmentInfo depthAttachmentInfo(depth_image.get_imageview(), vk::ImageLayout::eDepthStencilAttachmentOptimal, vk::ResolveModeFlagBits::eNone,
		{}, vk::ImageLayout::eUndefined, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare, depth_image.get_clear_value());

	vk::RenderingInfo renderingInfo({}, vk::Rect2D({ 0, 0 }, { static_cast<uint32_t>(width), static_cast<uint32_t>(height) }), 1, {}, colorAttachmentInfo, &depthAttachmentInfo, nullptr, nullptr);

	_commandbuffer.setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f));
	_commandbuffer.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(width, height)));

	_commandbuffer.beginRendering(renderingInfo);


	for (auto& _node : nodes)
	{
		_node->render(_commandbuffer);
	}

	_commandbuffer.endRendering();
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
