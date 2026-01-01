module scene_manager;

import chess_board;
import chess_board_line;
//import chess_piece;
import piece_general;
import piece_guard;
import piece_elephant;
import piece_horse;
import piece_chariot;
import piece_cannon;
import piece_pawn;

void scene_manager::create(vulkan_application* _app, uint32_t _width, uint32_t _height)
{
	app = _app;
	commandbuffers = app->create_commandbuffers(vk::QueueFlagBits::eGraphics, vulkan_common::MAX_FRAMES_IN_FLIGHT);

	active_camera.set_position(glm::vec3(0.f, 0.f, 1.1f));
	active_camera.set_direction(glm::vec3(0.f, 0.f, -1.f));
	active_camera.set_world_up(glm::vec3(0.f, 1.f, 0.f));


	nodes.emplace_back(std::make_unique<chess_board>());
	nodes.emplace_back(std::make_unique<chess_board_line>());


	const bool use_red = true;
	// 帅
	auto red_general = std::make_unique<piece_general>();
	red_general->set_piece_color(use_red, true);
	red_general->set_piece_location(glm::u8vec2(5, 0));
	nodes.push_back(std::move(red_general));

	auto black_general = std::make_unique<piece_general>();
	black_general->set_piece_color(use_red, false);
	black_general->set_piece_location(glm::u8vec2(5, 0));
	nodes.push_back(std::move(black_general));

	// 士
	auto red_guard1 = std::make_unique<piece_guard>();
	red_guard1->set_piece_color(use_red, true);
	red_guard1->set_piece_location(glm::u8vec2(4, 0));
	nodes.push_back(std::move(red_guard1));

	auto red_guard2 = std::make_unique<piece_guard>();
	red_guard2->set_piece_color(use_red, true);
	red_guard2->set_piece_location(glm::u8vec2(6, 0));
	nodes.push_back(std::move(red_guard2));

	auto black_guard1 = std::make_unique<piece_guard>();
	black_guard1->set_piece_color(use_red, false);
	black_guard1->set_piece_location(glm::u8vec2(4, 0));
	nodes.push_back(std::move(black_guard1));

	auto black_guard2 = std::make_unique<piece_guard>();
	black_guard2->set_piece_color(use_red, false);
	black_guard2->set_piece_location(glm::u8vec2(6, 0));
	nodes.push_back(std::move(black_guard2));

	// 象
	auto red_elephant1 = std::make_unique<piece_elephant>();
	red_elephant1->set_piece_color(use_red, true);
	red_elephant1->set_piece_location(glm::u8vec2(3, 0));
	nodes.push_back(std::move(red_elephant1));

	auto red_elephant2 = std::make_unique<piece_elephant>();
	red_elephant2->set_piece_color(use_red, true);
	red_elephant2->set_piece_location(glm::u8vec2(7, 0));
	nodes.push_back(std::move(red_elephant2));

	auto black_elephant1 = std::make_unique<piece_elephant>();
	black_elephant1->set_piece_color(use_red, false);
	black_elephant1->set_piece_location(glm::u8vec2(3, 0));
	nodes.push_back(std::move(black_elephant1));

	auto black_elephant2 = std::make_unique<piece_elephant>();
	black_elephant2->set_piece_color(use_red, false);
	black_elephant2->set_piece_location(glm::u8vec2(7, 0));
	nodes.push_back(std::move(black_elephant2));

	// 马
	auto red_horse1 = std::make_unique<piece_horse>();
	red_horse1->set_piece_color(use_red, true);
	red_horse1->set_piece_location(glm::u8vec2(2, 0));
	nodes.push_back(std::move(red_horse1));

	auto red_horse2 = std::make_unique<piece_horse>();
	red_horse2->set_piece_color(use_red, true);
	red_horse2->set_piece_location(glm::u8vec2(8, 0));
	nodes.push_back(std::move(red_horse2));

	auto black_horse1 = std::make_unique<piece_horse>();
	black_horse1->set_piece_color(use_red, false);
	black_horse1->set_piece_location(glm::u8vec2(2, 0));
	nodes.push_back(std::move(black_horse1));

	auto black_horse2 = std::make_unique<piece_horse>();
	black_horse2->set_piece_color(use_red, false);
	black_horse2->set_piece_location(glm::u8vec2(8, 0));
	nodes.push_back(std::move(black_horse2));

	// 車
	auto red_chariot1 = std::make_unique<piece_chariot>();
	red_chariot1->set_piece_color(use_red, true);
	red_chariot1->set_piece_location(glm::u8vec2(1, 0));
	nodes.push_back(std::move(red_chariot1));

	auto red_chariot2 = std::make_unique<piece_chariot>();
	red_chariot2->set_piece_color(use_red, true);
	red_chariot2->set_piece_location(glm::u8vec2(9, 0));
	nodes.push_back(std::move(red_chariot2));

	auto black_chariot1 = std::make_unique<piece_chariot>();
	black_chariot1->set_piece_color(use_red, false);
	black_chariot1->set_piece_location(glm::u8vec2(1, 0));
	nodes.push_back(std::move(black_chariot1));

	auto black_chariot2 = std::make_unique<piece_chariot>();
	black_chariot2->set_piece_color(use_red, false);
	black_chariot2->set_piece_location(glm::u8vec2(9, 0));
	nodes.push_back(std::move(black_chariot2));

	// 炮
	auto red_cannon1 = std::make_unique<piece_cannon>();
	red_cannon1->set_piece_color(use_red, true);
	red_cannon1->set_piece_location(glm::u8vec2(2, 2));
	nodes.push_back(std::move(red_cannon1));

	auto red_cannon2 = std::make_unique<piece_cannon>();
	red_cannon2->set_piece_color(use_red, true);
	red_cannon2->set_piece_location(glm::u8vec2(8, 2));
	nodes.push_back(std::move(red_cannon2));

	auto black_cannon1 = std::make_unique<piece_cannon>();
	black_cannon1->set_piece_color(use_red, false);
	black_cannon1->set_piece_location(glm::u8vec2(2, 2));
	nodes.push_back(std::move(black_cannon1));

	auto black_cannon2 = std::make_unique<piece_cannon>();
	black_cannon2->set_piece_color(use_red, false);
	black_cannon2->set_piece_location(glm::u8vec2(8, 2));
	nodes.push_back(std::move(black_cannon2));

	// 兵
	auto red_pawn1 = std::make_unique<piece_pawn>();
	red_pawn1->set_piece_color(use_red, true);
	red_pawn1->set_piece_location(glm::u8vec2(1, 3));
	nodes.push_back(std::move(red_pawn1));

	auto red_pawn2 = std::make_unique<piece_pawn>();
	red_pawn2->set_piece_color(use_red, true);
	red_pawn2->set_piece_location(glm::u8vec2(3, 3));
	nodes.push_back(std::move(red_pawn2));

	auto red_pawn3 = std::make_unique<piece_pawn>();
	red_pawn3->set_piece_color(use_red, true);
	red_pawn3->set_piece_location(glm::u8vec2(5, 3));
	nodes.push_back(std::move(red_pawn3));

	auto red_pawn4 = std::make_unique<piece_pawn>();
	red_pawn4->set_piece_color(use_red, true);
	red_pawn4->set_piece_location(glm::u8vec2(7, 3));
	nodes.push_back(std::move(red_pawn4));

	auto red_pawn5 = std::make_unique<piece_pawn>();
	red_pawn5->set_piece_color(use_red, true);
	red_pawn5->set_piece_location(glm::u8vec2(9, 3));
	nodes.push_back(std::move(red_pawn5));

	auto black_pawn1 = std::make_unique<piece_pawn>();
	black_pawn1->set_piece_color(use_red, false);
	black_pawn1->set_piece_location(glm::u8vec2(1, 3));
	nodes.push_back(std::move(black_pawn1));

	auto black_pawn2 = std::make_unique<piece_pawn>();
	black_pawn2->set_piece_color(use_red, false);
	black_pawn2->set_piece_location(glm::u8vec2(3, 3));
	nodes.push_back(std::move(black_pawn2));

	auto black_pawn3 = std::make_unique<piece_pawn>();
	black_pawn3->set_piece_color(use_red, false);
	black_pawn3->set_piece_location(glm::u8vec2(5, 3));
	nodes.push_back(std::move(black_pawn3));

	auto black_pawn4 = std::make_unique<piece_pawn>();
	black_pawn4->set_piece_color(use_red, false);
	black_pawn4->set_piece_location(glm::u8vec2(7, 3));
	nodes.push_back(std::move(black_pawn4));

	auto black_pawn5 = std::make_unique<piece_pawn>();
	black_pawn5->set_piece_color(use_red, false);
	black_pawn5->set_piece_location(glm::u8vec2(9, 3));
	nodes.push_back(std::move(black_pawn5));


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

	app->wait_idle();
	app->resize(width, height);

	// camera projection
	constexpr float camera_height = 1.3f;
	active_camera.set_ortho_projection(-camera_height * width / height, camera_height * width / height, -camera_height, camera_height, 1.f, 0.f);

	// render_output
	vk::ImageCreateInfo render_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(_width, _height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo render_view_info({}, {}, vk::ImageViewType::e2D, color_format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	render_output.create(app->get_physical_device(), app->get_device(), render_image_info, render_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));

	// msaa color
	vk::ImageCreateInfo color_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(_width, _height, 1), 1, 1, vulkan_common::MASS_SAMPLE_COUNT, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eColorAttachment /*| vk::ImageUsageFlagBits::eTransferSrc*/, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo color_view_info({}, {}, vk::ImageViewType::e2D, color_format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	color_image.create(app->get_physical_device(), app->get_device(), color_image_info, color_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.41015625f, 0.234375f, 0.0859375f, 1.f));

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

void scene_manager::render()
{
	update();


	vk::Result result = vk::Result::eSuccess;
	vk::Semaphore waited_semaphore = nullptr;

	try
	{
		auto res = app->acquire_next_image();
		result = res.first;
		waited_semaphore = res.second;
	}
	catch (vk::OutOfDateKHRError e)
	{
#ifndef NDEBUG
		std::println("{}", e.what());
#endif // !NDEBUG
		return;
	}
	catch (std::system_error)
	{
		throw std::runtime_error("failed to present swap chain image!");
	}

	auto& commandbuffer = commandbuffers.at(current_frame);

	commandbuffer.begin_record({});

	std::vector<vk::ImageMemoryBarrier2> begin_barrier;
	begin_barrier.emplace_back(color_image.transition_to_layout(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	begin_barrier.emplace_back(render_output.transition_to_layout(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	begin_barrier.emplace_back(depth_image.transition_to_layout(vk::ImageLayout::eDepthStencilAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1)));
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
		_node->render((*commandbuffer));
	}


	(*commandbuffer).endRendering();

	app->show_image_on_swapchain((*commandbuffer), render_output);

	commandbuffer.end_record();

	commandbuffer.submit({ vk::SemaphoreSubmitInfo(waited_semaphore, {}, vk::PipelineStageFlagBits2::eColorAttachmentOutput) }, { vk::SemaphoreSubmitInfo(*(app->get_swapchain().get_current_waited_semaphore()), {}, vk::PipelineStageFlagBits2::eColorAttachmentOutput) }, false);

	try
	{
		result = app->present_image();
	}
	catch (vk::OutOfDateKHRError e)
	{
#ifndef NDEBUG
		std::println("{}", e.what());
#endif // !NDEBUG
		return;
	}
	catch (std::system_error)
	{
		throw std::runtime_error("failed to present swap chain image!");
	}

	current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;
}