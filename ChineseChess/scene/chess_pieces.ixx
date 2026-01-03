export module chess_pieces;

import <glm/ext/matrix_float4x4.hpp>;
import std;
import glm;
import vulkan_hpp;
import vulkan_image;
import vulkan_buffer;
import vulkan_common;
import vulkan_pipeline;
import vulkan_application;
import scene_node;
import scene_camera;
import piece_base;
import model_loader;

export class chess_pieces : public scene_node
{
public:
	chess_pieces() = default;
	virtual ~chess_pieces() = default;

	void create(const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format) override;
	void resize(const vulkan_application* _app, uint32_t _width, uint32_t _height) override;
	void update(const scene_camera* _camera) override;
	void render(const vk::raii::CommandBuffer& _commandbuffer) override;

	bool load_record(const std::filesystem::path& _record_path);
	void parse_next();

	std::vector<piece_base*> find_piece_by_name(bool _use_color, wchar_t _name);

	std::vector<piece_base*> find_piece_on_x(bool _use_color, uint8_t _x);
	std::vector<piece_base*> find_piece_on_y(bool _use_color, uint8_t _y);
	piece_base* find_piece_on_x_y(bool _use_color, uint8_t _x, uint8_t _y);

	static bool character_is_digit_number(wchar_t _character);
	static bool character_is_chinese_number(wchar_t _character);
	static bool character_is_number(wchar_t _character);
	static uint8_t character_to_number(wchar_t _character);

	static wchar_t character_map(wchar_t _character);

private:
	vulkan_pipeline pipeline;

	std::vector<model_vertex> vertices;
	vulkan_buffer vertices_buffer;

	std::vector<uint32_t> indices;
	vulkan_buffer indices_buffer;

	std::array<std::unique_ptr<piece_base>, 16> red, black;

	uint32_t now_record_index = 0;
	std::vector<std::array<wchar_t, 4>> all_records;
};