module;

#include <glm/gtc/matrix_transform.hpp>

module chess_pieces;

import std;
import vulkan_commandbuffer;
import piece_general;
import piece_guard;
import piece_elephant;
import piece_horse;
import piece_chariot;
import piece_cannon;
import piece_pawn;
import font_loader;
import shader_compiler;

void chess_pieces::create(const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format)
{
	constexpr bool use_red = true;
	// 帅
	auto red_general = std::make_unique<piece_general>();
	red_general->create(_app, use_red, true);

	auto black_general = std::make_unique<piece_general>();
	black_general->create(_app, use_red, false);

	// 士
	auto red_guard1 = std::make_unique<piece_guard>();
	red_guard1->create(_app, use_red, true);

	auto red_guard2 = std::make_unique<piece_guard>();
	red_guard2->create(_app, use_red, true);

	auto black_guard1 = std::make_unique<piece_guard>();
	black_guard1->create(_app, use_red, false);

	auto black_guard2 = std::make_unique<piece_guard>();
	black_guard2->create(_app, use_red, false);

	// 象
	auto red_elephant1 = std::make_unique<piece_elephant>();
	red_elephant1->create(_app, use_red, true);

	auto red_elephant2 = std::make_unique<piece_elephant>();
	red_elephant2->create(_app, use_red, true);

	auto black_elephant1 = std::make_unique<piece_elephant>();
	black_elephant1->create(_app, use_red, false);

	auto black_elephant2 = std::make_unique<piece_elephant>();
	black_elephant2->create(_app, use_red, false);

	// 马
	auto red_horse1 = std::make_unique<piece_horse>();
	red_horse1->create(_app, use_red, true);

	auto red_horse2 = std::make_unique<piece_horse>();
	red_horse2->create(_app, use_red, true);

	auto black_horse1 = std::make_unique<piece_horse>();
	black_horse1->create(_app, use_red, false);

	auto black_horse2 = std::make_unique<piece_horse>();
	black_horse2->create(_app, use_red, false);

	// 車
	auto red_chariot1 = std::make_unique<piece_chariot>();
	red_chariot1->create(_app, use_red, true);

	auto red_chariot2 = std::make_unique<piece_chariot>();
	red_chariot2->create(_app, use_red, true);

	auto black_chariot1 = std::make_unique<piece_chariot>();
	black_chariot1->create(_app, use_red, false);

	auto black_chariot2 = std::make_unique<piece_chariot>();
	black_chariot2->create(_app, use_red, false);

	// 炮
	auto red_cannon1 = std::make_unique<piece_cannon>();
	red_cannon1->create(_app, use_red, true);

	auto red_cannon2 = std::make_unique<piece_cannon>();
	red_cannon2->create(_app, use_red, true);

	auto black_cannon1 = std::make_unique<piece_cannon>();
	black_cannon1->create(_app, use_red, false);

	auto black_cannon2 = std::make_unique<piece_cannon>();
	black_cannon2->create(_app, use_red, false);

	// 兵
	auto red_pawn1 = std::make_unique<piece_pawn>();
	red_pawn1->create(_app, use_red, true);

	auto red_pawn2 = std::make_unique<piece_pawn>();
	red_pawn2->create(_app, use_red, true);

	auto red_pawn3 = std::make_unique<piece_pawn>();
	red_pawn3->create(_app, use_red, true);

	auto red_pawn4 = std::make_unique<piece_pawn>();
	red_pawn4->create(_app, use_red, true);

	auto red_pawn5 = std::make_unique<piece_pawn>();
	red_pawn5->create(_app, use_red, true);

	auto black_pawn1 = std::make_unique<piece_pawn>();
	black_pawn1->create(_app, use_red, false);

	auto black_pawn2 = std::make_unique<piece_pawn>();
	black_pawn2->create(_app, use_red, false);

	auto black_pawn3 = std::make_unique<piece_pawn>();
	black_pawn3->create(_app, use_red, false);

	auto black_pawn4 = std::make_unique<piece_pawn>();
	black_pawn4->create(_app, use_red, false);

	auto black_pawn5 = std::make_unique<piece_pawn>();
	black_pawn5->create(_app, use_red, false);

	all_pieces.at(0) = {
		std::move(black_general),
		std::move(black_guard1), std::move(black_guard2),
		std::move(black_elephant1),std::move(black_elephant2),
		std::move(black_horse1),std::move(black_horse2),
		std::move(black_chariot1),std::move(black_chariot2),
		std::move(black_cannon1),std::move(black_cannon2),
		std::move(black_pawn1),std::move(black_pawn2),std::move(black_pawn3),std::move(black_pawn4),std::move(black_pawn5)
	};

	all_pieces.at(1) = {
		std::move(red_general),
		std::move(red_guard1), std::move(red_guard2),
		std::move(red_elephant1),std::move(red_elephant2),
		std::move(red_horse1),std::move(red_horse2),
		std::move(red_chariot1),std::move(red_chariot2),
		std::move(red_cannon1),std::move(red_cannon2),
		std::move(red_pawn1),std::move(red_pawn2),std::move(red_pawn3),std::move(red_pawn4),std::move(red_pawn5)
	};

	restore_board_state(record_loader::init_board_state);

	// begin a commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(_app->create_commandbuffers(vk::QueueFlagBits::eGraphics, 1).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);


	// pipeline
	std::array bindings{
		vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex, nullptr),
		vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment, nullptr),
	};

	auto binding = model_vertex::get_binding_description();
	auto attribute = model_vertex::get_attribute_descriptions();

	auto spirv_code = shader_compiler::get_shader_compiler().compile_shader_to_spv(std::string(SHADERS_PATH) + "chess_pieces.slang", { "vertMain", "fragMain" });
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
	if (!model_loader::get_model_loader().load_model(std::string(MODELS_PATH) + "chess_pieces.glb", vertices, indices))
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


	// commandbuffer submit
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);
}

void chess_pieces::resize(const vulkan_application* _app, uint32_t _width, uint32_t _height)
{
	for (auto& _pieces : all_pieces)
	{
		for (auto& _piece : _pieces)
		{
			_piece->resize(_app, pipeline.get_descriptor_set_layout(), _width, _height);
		}
	}
}

void chess_pieces::update(const scene_camera* _camera)
{
	for (auto& _pieces : all_pieces)
	{
		for (auto& _piece : _pieces)
		{
			_piece->update(_camera);
		}
	}
}

void chess_pieces::render(const vk::raii::CommandBuffer& _commandbuffer)
{
	_commandbuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline());
	_commandbuffer.bindVertexBuffers(0, *(vertices_buffer.get_buffer()), vk::DeviceSize(0));
	_commandbuffer.bindIndexBuffer(*(indices_buffer.get_buffer()), vk::DeviceSize(0), vk::IndexType::eUint32);

	for (auto& _pieces : all_pieces)
	{
		for (auto& _piece : _pieces)
		{
			if (_piece->get_is_on_board())
			{
				_piece->render(_commandbuffer, pipeline.get_pipeline_layout());
				_commandbuffer.drawIndexed(static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
			}
		}
	}
}

bool chess_pieces::load_record(const std::filesystem::path& _record_path)
{
	all_board_state = std::move(record_loader::get_record_loader().load_records(_record_path));
	return all_board_state.size() > 1;
}

void chess_pieces::parse_back()
{
	if (now_record_index == 0)
	{
		return;
	}

	now_record_index--;
	restore_board_state(all_board_state.at(now_record_index));

	return;
}

void chess_pieces::parse_next()
{
	if (now_record_index == all_board_state.size() - 1)
	{
		return;
	}

	now_record_index++;
	restore_board_state(all_board_state.at(now_record_index));

	return;
}

board_state chess_pieces::capture_board_state()
{
	board_state state;

	for (auto [_index, _pieces] : all_pieces | std::views::enumerate)
	{
		for (auto& _piece : _pieces)
		{
			if (_piece->get_is_on_board())
			{
				state.at(_index).name += record_loader::to_character(_piece->get_piece_name());
				const glm::u8vec2& position = _piece->get_piece_location();
				state.at(_index).position += (position.x << 4 | position.y & 0x0F);
			}
		}
	}

	return state;
}

void chess_pieces::restore_board_state(const board_state& _state)
{
	for (auto [_color, _pieces] : all_pieces | std::views::enumerate)
	{
		std::unordered_set<wchar_t> names;
		for (auto& _piece : _pieces)
		{
			_piece->set_is_on_board(false);
			names.insert(_piece->get_piece_name());
		}
		for (auto& _name : names)
		{
			auto pieces1 = record_loader::find_piece_by_name(static_cast<bool>(_color), _name, _state);
			auto pieces2 = find_piece_by_name(static_cast<bool>(_color), false, _name);

			for (const auto&& [_index, _value] : pieces1 | std::views::enumerate)
			{
				pieces2.at(_index)->set_is_on_board(true);

				uint8_t pos = _state.at(_color).position.at(_value);
				uint8_t pos_x = pos >> 4 & 0x0F;
				uint8_t pos_y = pos & 0x0F;
				pieces2.at(_index)->set_piece_location(glm::u8vec2(pos_x, pos_y));
			}
		}
	}
}

std::vector<piece_base*> chess_pieces::find_piece_by_name(bool _use_color, bool _only_on_borad, wchar_t _name)
{
	return all_pieces.at(static_cast<size_t>(_use_color))
		| std::views::filter([&](const auto& _piece)
			{
				return (!_only_on_borad || _piece->get_is_on_board()) && record_loader::to_character(_piece->get_piece_name()) == record_loader::to_character(_name);
			})
		| std::views::transform([](const auto& _piece) { return _piece.get(); })
		| std::ranges::to<std::vector>();
}

std::vector<piece_base*> chess_pieces::find_piece_on_x(bool _use_color, bool _only_on_borad, uint8_t _x)
{
	return all_pieces.at(static_cast<size_t>(_use_color))
		| std::views::filter([&](const auto& _piece)
			{
				return (!_only_on_borad || _piece->get_is_on_board()) && _piece->get_piece_location().x == _x;
			})
		| std::views::transform([](const auto& _piece) { return _piece.get(); })
		| std::ranges::to<std::vector>();
}

std::vector<piece_base*> chess_pieces::find_piece_on_y(bool _use_color, bool _only_on_borad, uint8_t _y)
{
	return all_pieces.at(static_cast<size_t>(_use_color))
		| std::views::filter([&](const auto& _piece)
			{
				return (!_only_on_borad || _piece->get_is_on_board()) && _piece->get_piece_location().y == _y;
			})
		| std::views::transform([](const auto& _piece) { return _piece.get(); })
		| std::ranges::to<std::vector>();
}

piece_base* chess_pieces::find_piece_on_x_y(bool _use_color, bool _only_on_borad, uint8_t _x, uint8_t _y)
{
	auto pieces = all_pieces.at(static_cast<size_t>(_use_color))
		| std::views::filter([&](const auto& _piece)
			{
				return (!_only_on_borad || _piece->get_is_on_board()) && _piece->get_piece_location().x == _x && _piece->get_piece_location().y == _y;
			})
		| std::views::transform([](const auto& _piece) { return _piece.get(); });

	return pieces.empty() ? nullptr : pieces.front();
}
