module chess_pieces;

import <glm/gtc/matrix_transform.hpp>;
import std;
import vulkan_commandbuffer;
import piece_general;
import piece_guard;
import piece_elephant;
import piece_horse;
import piece_chariot;
import piece_cannon;
import piece_pawn;
import record_loader;
import font_loader;
import shader_compiler;

void chess_pieces::create(const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format)
{
	const bool use_red = true;
	// 帅
	auto red_general = std::make_unique<piece_general>();
	red_general->create(_app, use_red, true);
	red_general->set_piece_location(glm::u8vec2(5, 0));

	auto black_general = std::make_unique<piece_general>();
	black_general->create(_app, use_red, false);
	black_general->set_piece_location(glm::u8vec2(5, 0));

	// 士
	auto red_guard1 = std::make_unique<piece_guard>();
	red_guard1->create(_app, use_red, true);
	red_guard1->set_piece_location(glm::u8vec2(4, 0));

	auto red_guard2 = std::make_unique<piece_guard>();
	red_guard2->create(_app, use_red, true);
	red_guard2->set_piece_location(glm::u8vec2(6, 0));

	auto black_guard1 = std::make_unique<piece_guard>();
	black_guard1->create(_app, use_red, false);
	black_guard1->set_piece_location(glm::u8vec2(4, 0));

	auto black_guard2 = std::make_unique<piece_guard>();
	black_guard2->create(_app, use_red, false);
	black_guard2->set_piece_location(glm::u8vec2(6, 0));

	// 象
	auto red_elephant1 = std::make_unique<piece_elephant>();
	red_elephant1->create(_app, use_red, true);
	red_elephant1->set_piece_location(glm::u8vec2(3, 0));

	auto red_elephant2 = std::make_unique<piece_elephant>();
	red_elephant2->create(_app, use_red, true);
	red_elephant2->set_piece_location(glm::u8vec2(7, 0));

	auto black_elephant1 = std::make_unique<piece_elephant>();
	black_elephant1->create(_app, use_red, false);
	black_elephant1->set_piece_location(glm::u8vec2(3, 0));

	auto black_elephant2 = std::make_unique<piece_elephant>();
	black_elephant2->create(_app, use_red, false);
	black_elephant2->set_piece_location(glm::u8vec2(7, 0));

	// 马
	auto red_horse1 = std::make_unique<piece_horse>();
	red_horse1->create(_app, use_red, true);
	red_horse1->set_piece_location(glm::u8vec2(2, 0));

	auto red_horse2 = std::make_unique<piece_horse>();
	red_horse2->create(_app, use_red, true);
	red_horse2->set_piece_location(glm::u8vec2(8, 0));

	auto black_horse1 = std::make_unique<piece_horse>();
	black_horse1->create(_app, use_red, false);
	black_horse1->set_piece_location(glm::u8vec2(2, 0));

	auto black_horse2 = std::make_unique<piece_horse>();
	black_horse2->create(_app, use_red, false);
	black_horse2->set_piece_location(glm::u8vec2(8, 0));

	// 車
	auto red_chariot1 = std::make_unique<piece_chariot>();
	red_chariot1->create(_app, use_red, true);
	red_chariot1->set_piece_location(glm::u8vec2(1, 0));

	auto red_chariot2 = std::make_unique<piece_chariot>();
	red_chariot2->create(_app, use_red, true);
	red_chariot2->set_piece_location(glm::u8vec2(9, 0));

	auto black_chariot1 = std::make_unique<piece_chariot>();
	black_chariot1->create(_app, use_red, false);
	black_chariot1->set_piece_location(glm::u8vec2(1, 0));

	auto black_chariot2 = std::make_unique<piece_chariot>();
	black_chariot2->create(_app, use_red, false);
	black_chariot2->set_piece_location(glm::u8vec2(9, 0));

	// 炮
	auto red_cannon1 = std::make_unique<piece_cannon>();
	red_cannon1->create(_app, use_red, true);
	red_cannon1->set_piece_location(glm::u8vec2(2, 2));

	auto red_cannon2 = std::make_unique<piece_cannon>();
	red_cannon2->create(_app, use_red, true);
	red_cannon2->set_piece_location(glm::u8vec2(8, 2));

	auto black_cannon1 = std::make_unique<piece_cannon>();
	black_cannon1->create(_app, use_red, false);
	black_cannon1->set_piece_location(glm::u8vec2(2, 2));

	auto black_cannon2 = std::make_unique<piece_cannon>();
	black_cannon2->create(_app, use_red, false);
	black_cannon2->set_piece_location(glm::u8vec2(8, 2));

	// 兵
	auto red_pawn1 = std::make_unique<piece_pawn>();
	red_pawn1->create(_app, use_red, true);
	red_pawn1->set_piece_location(glm::u8vec2(1, 3));

	auto red_pawn2 = std::make_unique<piece_pawn>();
	red_pawn2->create(_app, use_red, true);
	red_pawn2->set_piece_location(glm::u8vec2(3, 3));

	auto red_pawn3 = std::make_unique<piece_pawn>();
	red_pawn3->create(_app, use_red, true);
	red_pawn3->set_piece_location(glm::u8vec2(5, 3));

	auto red_pawn4 = std::make_unique<piece_pawn>();
	red_pawn4->create(_app, use_red, true);
	red_pawn4->set_piece_location(glm::u8vec2(7, 3));

	auto red_pawn5 = std::make_unique<piece_pawn>();
	red_pawn5->create(_app, use_red, true);
	red_pawn5->set_piece_location(glm::u8vec2(9, 3));

	auto black_pawn1 = std::make_unique<piece_pawn>();
	black_pawn1->create(_app, use_red, false);
	black_pawn1->set_piece_location(glm::u8vec2(1, 3));

	auto black_pawn2 = std::make_unique<piece_pawn>();
	black_pawn2->create(_app, use_red, false);
	black_pawn2->set_piece_location(glm::u8vec2(3, 3));

	auto black_pawn3 = std::make_unique<piece_pawn>();
	black_pawn3->create(_app, use_red, false);
	black_pawn3->set_piece_location(glm::u8vec2(5, 3));

	auto black_pawn4 = std::make_unique<piece_pawn>();
	black_pawn4->create(_app, use_red, false);
	black_pawn4->set_piece_location(glm::u8vec2(7, 3));

	auto black_pawn5 = std::make_unique<piece_pawn>();
	black_pawn5->create(_app, use_red, false);
	black_pawn5->set_piece_location(glm::u8vec2(9, 3));


	red = {
		std::move(red_general),
		std::move(red_guard1), std::move(red_guard2),
		std::move(red_elephant1),std::move(red_elephant2),
		std::move(red_horse1),std::move(red_horse2),
		std::move(red_chariot1),std::move(red_chariot2),
		std::move(red_cannon1),std::move(red_cannon2),
		std::move(red_pawn1),std::move(red_pawn2),std::move(red_pawn3),std::move(red_pawn4),std::move(red_pawn5)
	};

	black = {
		std::move(black_general),
		std::move(black_guard1), std::move(black_guard2),
		std::move(black_elephant1),std::move(black_elephant2),
		std::move(black_horse1),std::move(black_horse2),
		std::move(black_chariot1),std::move(black_chariot2),
		std::move(black_cannon1),std::move(black_cannon2),
		std::move(black_pawn1),std::move(black_pawn2),std::move(black_pawn3),std::move(black_pawn4),std::move(black_pawn5)
	};


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
	if (!model_loader::get_model_loader().load_model_file(std::string(MODELS_PATH) + "chess_pieces.glb", vertices, indices))
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
	for (auto& _piece : red)
	{
		_piece->resize(_app, pipeline.get_descriptor_set_layout(), _width, _height);
	}
	for (auto& _piece : black)
	{
		_piece->resize(_app, pipeline.get_descriptor_set_layout(), _width, _height);
	}
}

void chess_pieces::update(const scene_camera* _camera)
{
	for (auto& _piece : red)
	{
		_piece->update(_camera);
	}
	for (auto& _piece : black)
	{
		_piece->update(_camera);
	}
}

void chess_pieces::render(const vk::raii::CommandBuffer& _commandbuffer)
{
	_commandbuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline());
	_commandbuffer.bindVertexBuffers(0, *(vertices_buffer.get_buffer()), vk::DeviceSize(0));
	_commandbuffer.bindIndexBuffer(*(indices_buffer.get_buffer()), vk::DeviceSize(0), vk::IndexType::eUint32);

	for (auto& _piece : red)
	{
		if (_piece->get_is_on_board())
		{
			_piece->render(_commandbuffer, pipeline.get_pipeline_layout());
			_commandbuffer.drawIndexed(static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
		}
	}
	for (auto& _piece : black)
	{
		if (_piece->get_is_on_board())
		{
			_piece->render(_commandbuffer, pipeline.get_pipeline_layout());
			_commandbuffer.drawIndexed(static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
		}
	}
}

bool chess_pieces::load_record(const std::filesystem::path& _record_path)
{
	all_records = record_loader::get_record_loader().load_record(_record_path);
	return !all_records.empty();
}

void chess_pieces::parse_next()
{
	if (now_record_index < 0)
	{
		return;
	}
	if (now_record_index >= all_records.size())
	{
		return;
	}

	const std::array<wchar_t, 4>& _record = all_records.at(now_record_index);
	now_record_index++;

	// find piece
	piece_base* now_piece = nullptr;

	if (character_is_number(_record.at(1)))
	{
		std::vector<piece_base*> pieces_on_x = find_piece_on_x(character_is_chinese_number(_record.at(3)), character_to_number(_record.at(1)));
		auto iter = std::ranges::find_if(pieces_on_x,
			[&](const auto& _piece) {
				return character_map(_piece->get_piece_name()) == character_map(_record.at(0));
			});

		if (iter != pieces_on_x.end())
		{
			now_piece = *iter;
		}
	}
	else
	{
		std::vector<piece_base*> pieces = find_piece_by_name(character_is_chinese_number(_record.at(3)), _record.at(1));
		if (_record.at(0) == L'前')
		{
			auto iter = std::ranges::max_element(pieces, {}, [](const auto& _piece) { return _piece->get_piece_location().y; });
			if (iter != pieces.end())
			{
				now_piece = *iter;
			}
		}
		else if (_record.at(0) == L'后')
		{
			auto iter = std::ranges::min_element(pieces, {}, [](const auto& _piece) { return _piece->get_piece_location().y; });
			if (iter != pieces.end())
			{
				now_piece = *iter;
			}
		}
	}


	if (now_piece == nullptr)
	{
		return;
	}


	// move
	glm::u8vec2 new_location = now_piece->move_by(_record.at(2), character_to_number(_record.at(3)));

	piece_base* piece = find_piece_on_x_y(!now_piece->get_piece_color(), 10 - new_location.x, 9 - new_location.y); // 红方和黑方的Y是相反的
	if (piece != nullptr)
	{
		piece->set_is_on_board(false);
	}

	now_piece->set_piece_location(new_location);
}

std::vector<piece_base*> chess_pieces::find_piece_by_name(bool _use_color, wchar_t _name)
{
	if (_use_color)
	{
		return red
			| std::views::filter([&](const auto& _piece)
				{
					return _piece->get_is_on_board() && character_map(_piece->get_piece_name()) == character_map(_name);
				})
			| std::views::transform([](const auto& _piece) { return _piece.get(); })
			| std::ranges::to<std::vector>();
	}
	else
	{
		return black
			| std::views::filter([&](const auto& _piece)
				{
					return _piece->get_is_on_board() && character_map(_piece->get_piece_name()) == character_map(_name);
				})
			| std::views::transform([](const auto& _piece) { return _piece.get(); })
			| std::ranges::to<std::vector>();
	}
}

std::vector<piece_base*> chess_pieces::find_piece_on_x(bool _use_color, uint8_t _x)
{
	if (_use_color)
	{
		return red
			| std::views::filter([&](const auto& _piece)
				{
					return _piece->get_is_on_board() && _piece->get_piece_location().x == _x;
				})
			| std::views::transform([](const auto& _piece) { return _piece.get(); })
			| std::ranges::to<std::vector>();
	}
	else
	{
		return black
			| std::views::filter([&](const auto& _piece)
				{
					return _piece->get_is_on_board() && _piece->get_piece_location().x == _x;
				})
			| std::views::transform([](const auto& _piece) { return _piece.get(); })
			| std::ranges::to<std::vector>();
	}
}

std::vector<piece_base*> chess_pieces::find_piece_on_y(bool _use_color, uint8_t _y)
{
	if (_use_color)
	{
		return red
			| std::views::filter([&](const auto& _piece)
				{
					return _piece->get_is_on_board() && _piece->get_piece_location().y == _y;
				})
			| std::views::transform([](const auto& _piece) { return _piece.get(); })
			| std::ranges::to<std::vector>();
	}
	else
	{
		return black
			| std::views::filter([&](const auto& _piece)
				{
					return _piece->get_is_on_board() && _piece->get_piece_location().y == _y;
				})
			| std::views::transform([](const auto& _piece) { return _piece.get(); })
			| std::ranges::to<std::vector>();
	}
}

piece_base* chess_pieces::find_piece_on_x_y(bool _use_color, uint8_t _x, uint8_t _y)
{
	if (_use_color)
	{
		auto pieces = red
			| std::views::filter([&](const auto& _piece)
				{
					return _piece->get_is_on_board() && _piece->get_piece_location().x == _x && _piece->get_piece_location().y == _y;
				})
			| std::views::transform([](const auto& _piece) { return _piece.get(); });

		if (!pieces.empty())
		{
			return pieces.front();
		}
	}
	else
	{
		auto pieces = black
			| std::views::filter([&](const auto& _piece)
				{
					return _piece->get_is_on_board() && _piece->get_piece_location().x == _x && _piece->get_piece_location().y == _y;
				})
			| std::views::transform([](const auto& _piece) { return _piece.get(); });

		if (!pieces.empty())
		{
			return pieces.front();
		}
	}

	return nullptr;
}

bool chess_pieces::character_is_digit_number(wchar_t _character)
{
	switch (_character)
	{
	case L'1':
	case L'１':
	case L'2':
	case L'２':
	case L'3':
	case L'３':
	case L'4':
	case L'４':
	case L'5':
	case L'５':
	case L'6':
	case L'６':
	case L'7':
	case L'７':
	case L'8':
	case L'８':
	case L'9':
	case L'９':
		return true;
	default:
		return false;
	}
}

bool chess_pieces::character_is_chinese_number(wchar_t _character)
{
	switch (_character)
	{
	case L'一':
	case L'二':
	case L'三':
	case L'四':
	case L'五':
	case L'六':
	case L'七':
	case L'八':
	case L'九':
		return true;
	default:
		return false;
	}
}

bool chess_pieces::character_is_number(wchar_t _character)
{
	return character_is_digit_number(_character) || character_is_chinese_number(_character);
}

uint8_t chess_pieces::character_to_number(wchar_t _character)
{
	switch (_character)
	{
	case L'1':
	case L'１':
	case L'一':
		return 1;
	case L'2':
	case L'２':
	case L'二':
		return 2;
	case L'3':
	case L'３':
	case L'三':
		return 3;
	case L'4':
	case L'４':
	case L'四':
		return 4;
	case L'5':
	case L'５':
	case L'五':
		return 5;
	case L'6':
	case L'６':
	case L'六':
		return 6;
	case L'7':
	case L'７':
	case L'七':
		return 7;
	case L'8':
	case L'８':
	case L'八':
		return 8;
	case L'9':
	case L'９':
	case L'九':
		return 9;
	default:
		return 0;
	}
}

wchar_t chess_pieces::character_map(wchar_t _character)
{
	switch (_character)
	{
	case L'帅':
	case L'帥':
		return L'帅';
	case L'将':
	case L'將':
		return L'将';
	case L'士':
	case L'仕':
		return L'士';
	case L'相':
	case L'象':
		return L'象';
	case L'马':
	case L'馬':
	case L'傌':
		return L'马';
	case L'车':
	case L'車':
	case L'俥':
		return L'车';
	case L'炮':
	case L'砲':
		return L'炮';
	case L'兵':
		return L'兵';
	case L'卒':
		return L'卒';
	default:
		return L'\0';
	}
}
