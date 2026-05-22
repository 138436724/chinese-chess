#include "chess_manager.h"
#include "tools/font_loader.h"
#include "tools/shader_compiler.h"
#include "vulkan_core/vulkan_common.h"
#include <algorithm>
#include <ranges>
#include <unordered_set>

void chess_manager::create(const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format)
{
	constexpr PIECE_COLOR use_red = PIECE_COLOR::RED;
	// 帅
	auto red_general = chess_piece();
	red_general.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::GENERAL);

	auto black_general = chess_piece();
	black_general.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::GENERAL);

	// 士
	auto red_guard1 = chess_piece();
	red_guard1.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::GUARD);

	auto red_guard2 = chess_piece();
	red_guard2.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::GUARD);

	auto black_guard1 = chess_piece();
	black_guard1.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::GUARD);

	auto black_guard2 = chess_piece();
	black_guard2.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::GUARD);

	// 象
	auto red_elephant1 = chess_piece();
	red_elephant1.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::ELEPHANT);

	auto red_elephant2 = chess_piece();
	red_elephant2.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::ELEPHANT);

	auto black_elephant1 = chess_piece();
	black_elephant1.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::ELEPHANT);

	auto black_elephant2 = chess_piece();
	black_elephant2.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::ELEPHANT);

	// 马
	auto red_horse1 = chess_piece();
	red_horse1.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::HORSE);

	auto red_horse2 = chess_piece();
	red_horse2.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::HORSE);

	auto black_horse1 = chess_piece();
	black_horse1.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::HORSE);

	auto black_horse2 = chess_piece();
	black_horse2.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::HORSE);

	// 車
	auto red_chariot1 = chess_piece();
	red_chariot1.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::CHARIOT);

	auto red_chariot2 = chess_piece();
	red_chariot2.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::CHARIOT);

	auto black_chariot1 = chess_piece();
	black_chariot1.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::CHARIOT);

	auto black_chariot2 = chess_piece();
	black_chariot2.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::CHARIOT);

	// 炮
	auto red_cannon1 = chess_piece();
	red_cannon1.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::CANNON);

	auto red_cannon2 = chess_piece();
	red_cannon2.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::CANNON);

	auto black_cannon1 = chess_piece();
	black_cannon1.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::CANNON);

	auto black_cannon2 = chess_piece();
	black_cannon2.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::CANNON);

	// 兵
	auto red_pawn1 = chess_piece();
	red_pawn1.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::PAWN);

	auto red_pawn2 = chess_piece();
	red_pawn2.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::PAWN);

	auto red_pawn3 = chess_piece();
	red_pawn3.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::PAWN);

	auto red_pawn4 = chess_piece();
	red_pawn4.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::PAWN);

	auto red_pawn5 = chess_piece();
	red_pawn5.create(use_red, PIECE_COLOR::RED, PIECE_TYPE::PAWN);

	auto black_pawn1 = chess_piece();
	black_pawn1.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::PAWN);

	auto black_pawn2 = chess_piece();
	black_pawn2.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::PAWN);

	auto black_pawn3 = chess_piece();
	black_pawn3.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::PAWN);

	auto black_pawn4 = chess_piece();
	black_pawn4.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::PAWN);

	auto black_pawn5 = chess_piece();
	black_pawn5.create(use_red, PIECE_COLOR::BLACK, PIECE_TYPE::PAWN);


	all_pieces.at(static_cast<size_t>(PIECE_COLOR::BLACK)) = {
		std::move(black_general),
		std::move(black_guard1), std::move(black_guard2),
		std::move(black_elephant1),std::move(black_elephant2),
		std::move(black_horse1),std::move(black_horse2),
		std::move(black_chariot1),std::move(black_chariot2),
		std::move(black_cannon1),std::move(black_cannon2),
		std::move(black_pawn1),std::move(black_pawn2),std::move(black_pawn3),std::move(black_pawn4),std::move(black_pawn5)
	};

	all_pieces.at(static_cast<size_t>(PIECE_COLOR::RED)) = {
		std::move(red_general),
		std::move(red_guard1), std::move(red_guard2),
		std::move(red_elephant1),std::move(red_elephant2),
		std::move(red_horse1),std::move(red_horse2),
		std::move(red_chariot1),std::move(red_chariot2),
		std::move(red_cannon1),std::move(red_cannon2),
		std::move(red_pawn1),std::move(red_pawn2),std::move(red_pawn3),std::move(red_pawn4),std::move(red_pawn5)
	};

	restore_board_state(board_state.front());


	// begin a commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(_app->get_queue(vk::QueueFlagBits::eGraphics).get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), &(_app->get_device()), &(_app->get_queue(vk::QueueFlagBits::eGraphics).get_queue())).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);


	// create sampler
	vk::PhysicalDeviceProperties properties = _app->get_physical_device().getProperties();
	vk::SamplerCreateInfo sampler_info({}, vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear,
		vk::SamplerAddressMode::eClampToBorder, vk::SamplerAddressMode::eClampToBorder, vk::SamplerAddressMode::eClampToBorder,
		0.f, vk::True, properties.limits.maxSamplerAnisotropy, vk::False, vk::CompareOp::eAlways, 0.f, 1.f,
		vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);
	font_sampler = vk::raii::Sampler(_app->get_device(), sampler_info);


	// pipeline
	std::array bindings{
		vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex, nullptr),
		vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment, nullptr),
	};

	auto binding = model_vertex::get_binding_description();
	auto attribute = model_vertex::get_attribute_descriptions<model_vertex_type::position, model_vertex_type::uv>();

	auto spirv_code = SHADER_COMPILER.compile_shader_to_spv(std::u8string(SHADERS_PATH) + u8"chess_manager.slang", { std::string(VERT_ENTYR_NAME), std::string(FRAG_ENTYR_NAME) });
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
		vk::PrimitiveTopology::eTriangleList, vk::PolygonMode::eFill, vk::CullModeFlagBits::eBack, vk::FrontFace::eCounterClockwise,
		_multisample_count, vk::True, std::span(&_color_formats, 1), _depth_format);


	// vertex and index buffer
	if (!MODEL_LOADER.load_model(std::u8string(MODELS_PATH) + u8"chess_piece.glb", vertices, indices))
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
	for (uint32_t i = 0; i < vulkan_common::MAX_FRAMES_IN_FLIGHT; i++)
	{
		vulkan_buffer buffer;
		buffer.create(_app->get_physical_device(), _app->get_device(), sizeof(chess_manager::UBOS), vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		ubos.push_back(std::move(buffer));
	}

	blas.create_bottom_level_accelerration_structure(_app->get_physical_device(), _app->get_device(), *commandbuffer,
		static_cast<uint32_t>(vertices.size()), vertices_buffer.get_buffer_address().deviceAddress, static_cast<uint32_t>(indices.size()), indices_buffer.get_buffer_address().deviceAddress);

	// commandbuffer submit
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);
}

void chess_manager::resize(const vulkan_application* _app, uint32_t _width, uint32_t _height)
{
	// begin a commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(_app->get_queue(vk::QueueFlagBits::eGraphics).get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), &(_app->get_device()), &(_app->get_queue(vk::QueueFlagBits::eGraphics).get_queue())).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);


	// load font and transition to image
	constexpr std::wstring_view all_piece_names = L"帥仕相傌俥炮兵將士象馬車砲卒";
	constexpr float font_resolution = 1.5f;
	std::vector<character_info> fonts_info = FONT_LOADER.load_font(std::u8string(FONTS_PATH) + u8"LXGWWenKaiGB-Medium.ttf", static_cast<uint32_t>(_height / 9 * font_resolution), std::wstring(all_piece_names));
	uint32_t max_bearing_height_up = 0, max_bearing_height_down = 0, all_width = 0;
	for (const auto& _font_info : fonts_info)
	{
		max_bearing_height_up = std::max(max_bearing_height_up, _font_info.bearing_height);
		max_bearing_height_down = std::max(max_bearing_height_down, _font_info.height - _font_info.bearing_height);
		all_width = std::max(all_width, _font_info.advance);
	}
	uint32_t all_height = max_bearing_height_up + max_bearing_height_down;


	vk::ImageCreateInfo font_image_info({}, vk::ImageType::e2D, vk::Format::eR8Unorm, vk::Extent3D(all_width, all_height, 1), 1, static_cast<uint32_t>(all_piece_names.size()), vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo font_view_info({}, {}, vk::ImageViewType::e2DArray, vk::Format::eR8Unorm, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, static_cast<uint32_t>(all_piece_names.size())), nullptr);
	font_image.create(_app->get_physical_device(), _app->get_device(), font_image_info, font_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));

	std::vector<vk::ImageMemoryBarrier2> begin_barrier;
	begin_barrier.emplace_back(font_image.set_layout(vk::ImageLayout::eTransferDstOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, static_cast<uint32_t>(all_piece_names.size()))));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));

	std::vector<vulkan_buffer> all_stage_buffer;
	for (const auto& [_index, _font_info] : fonts_info | std::views::enumerate)
	{
		vulkan_buffer stage_buffer;
		stage_buffer.create(_app->get_physical_device(), _app->get_device(), _font_info.width * _font_info.height, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		memcpy(stage_buffer.get_buffer_address().hostAddress, _font_info.buffer.data(), _font_info.buffer.size());

		vk::Offset3D copy_offset((all_width - _font_info.advance) / 2 + _font_info.bearing_width, max_bearing_height_up - _font_info.bearing_height, 0);
		vulkan_buffer::copy_buffer_to_image(*commandbuffer, stage_buffer.get_buffer(), font_image.get_image(), vk::BufferImageCopy2(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, static_cast<uint32_t>(_index), 1), copy_offset, vk::Extent3D(_font_info.width, _font_info.height, 1)));

		all_stage_buffer.push_back(std::move(stage_buffer));
	}

	std::vector<vk::ImageMemoryBarrier2> end_barrier;
	end_barrier.emplace_back(font_image.set_layout(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, static_cast<uint32_t>(all_piece_names.size()))));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, end_barrier));


	// commandbuffer submit
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);


	descriptor.clear_descriptor_info();

	auto buffer_pool_info = ubos
		| std::views::transform([](const auto& _buffer) -> DescriptorBufferOrImageInfo
			{
				return vk::DescriptorBufferInfo(_buffer.get_buffer(), 0, sizeof(chess_manager::UBOS));
			})
		| std::ranges::to<std::vector>();
	descriptor.add_descriptor_info(vk::DescriptorType::eUniformBuffer, buffer_pool_info);

	auto image_pool_info = std::views::iota(0u, vulkan_common::MAX_FRAMES_IN_FLIGHT)
		| std::views::transform([&](const auto&) -> DescriptorBufferOrImageInfo
			{
				return vk::DescriptorImageInfo(font_sampler, font_image.get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);
			})
		| std::ranges::to<std::vector>();
	descriptor.add_descriptor_info(vk::DescriptorType::eCombinedImageSampler, image_pool_info);

	descriptor.update_descriptor_sets(_app->get_device(), vulkan_common::MAX_FRAMES_IN_FLIGHT, pipeline.get_descriptor_set_layout());
}

void chess_manager::update(const scene_camera* _camera) noexcept
{
	auto pieces_to_render = all_pieces
		| std::views::all
		| std::views::join
		| std::views::filter([](const auto& _piece)
			{
				return _piece.get_is_on_board();
			})
		| std::views::transform([&](const auto& _piece)
			{
				chess_manager::UBO ubo_(glm::translate(glm::mat4(1.f), glm::vec3(_piece.get_model_location(), 0.3f)), _camera->get_view_matrix(), _camera->get_projection_matrix(), _camera->get_position(), glm::vec3(0.f, 0.f, 0.f), static_cast<uint32_t>(_piece.get_piece_type()) - 1);
				if (_piece.get_piece_color() == PIECE_COLOR::RED)
				{
					ubo_.piece_color = glm::vec3(1.f, 0.f, 0.f);
				}
				return ubo_;
			})
		| std::ranges::to<std::vector>();

	memcpy(ubos.at(current_frame).get_buffer_address().hostAddress, pieces_to_render.data(), pieces_to_render.size() * sizeof(chess_manager::UBO));

	alive_piece_num = static_cast<uint32_t>(pieces_to_render.size());
}

void chess_manager::render(const vk::raii::CommandBuffer& _commandbuffer) noexcept
{
	_commandbuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline());
	_commandbuffer.bindVertexBuffers(0, *(vertices_buffer.get_buffer()), vk::DeviceSize(0));
	_commandbuffer.bindIndexBuffer(*(indices_buffer.get_buffer()), vk::DeviceSize(0), vk::IndexType::eUint32);
	_commandbuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline_layout(), 0, *(descriptor.get_descriptor_sets().at(current_frame)), nullptr);
	_commandbuffer.drawIndexed(static_cast<uint32_t>(indices.size()), alive_piece_num, 0, 0, 0);

	current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;
}

void chess_manager::destroy() noexcept
{
}

std::vector<vk::AccelerationStructureInstanceKHR> chess_manager::get_all_blas_info() const noexcept
{
	return all_pieces
		| std::views::all
		| std::views::join
		| std::views::filter([](const auto& _piece)
			{
				return _piece.get_is_on_board();
			})
		| std::views::transform([&](const auto& _piece)
			{
				return vk::AccelerationStructureInstanceKHR(vulkan_common::glm_matrix_to_vulkan(glm::translate(glm::mat4(1.f), glm::vec3(_piece.get_model_location(), 0.3f))), 0, 0xFF, 0, vk::GeometryInstanceFlagBitsKHR::eTriangleCullDisable, blas.get_address());
			})
		| std::ranges::to<std::vector>();
}

bool chess_manager::load_record(const std::filesystem::path& _record_path)
{
	board_state = RECORD_LOADER.load_record(_record_path);
	return board_state.size() > 1;
}

void chess_manager::set_now_record_index(uint32_t _index) noexcept
{
	if (0 <= _index && _index < board_state.size())
	{
		now_record_index = _index;
		restore_board_state(board_state.at(now_record_index));
	}
}

all_board_state chess_manager::capture_board_state()
{
	all_board_state state;

	for (auto [_color, _pieces] : all_pieces | std::views::enumerate)
	{
		for (auto& _piece : _pieces)
		{
			if (_piece.get_is_on_board())
			{
				state.at(_color).emplace_back(piece_state(_piece.get_piece_type(), _piece.get_piece_location().first, _piece.get_piece_location().second));
			}
		}
	}

	return state;
}

void chess_manager::restore_board_state(const all_board_state& _state)
{
	for (auto [_color, _pieces] : all_pieces | std::views::enumerate)
	{
		// find type
		std::unordered_set<PIECE_TYPE> piece_types;
		for (auto& _piece : _pieces)
		{
			_piece.set_is_on_board(false);
			piece_types.insert(_piece.get_piece_type());
		}

		// set location
		for (auto& _piece_type : piece_types)
		{
			auto pieces1 = _state.at(_color)
				| std::views::filter([&](const auto& _piece) { return _piece.piece_type == _piece_type; });

			auto pieces2 = all_pieces.at(_color)
				| std::views::filter([&](const auto& _piece) { return  _piece.get_piece_type() == _piece_type; });

			std::ranges::for_each(std::views::zip(pieces1, pieces2),
				[](auto&& _pair) {
					auto& [_piece1, _piece2] = _pair;
					_piece2.set_is_on_board(true);
					_piece2.set_piece_location(std::make_pair(_piece1.x, _piece1.y));
				});
		}
	}
}