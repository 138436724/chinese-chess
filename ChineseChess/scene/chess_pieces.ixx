module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

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
import record_loader;

export class chess_pieces : public scene_node
{
public:
	chess_pieces() = default;
	virtual ~chess_pieces() = default;

	void create(GLFWwindow* _window, const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format) override;
	void resize(const vulkan_application* _app, uint32_t _width, uint32_t _height) override;
	void update(const scene_camera* _camera) override;
	void render(const vk::raii::CommandBuffer& _commandbuffer) override;
	void destroy() override;

	bool load_record(const std::filesystem::path& _record_path);
	void parse_back();
	void parse_next();
	void set_now_record_index(uint32_t _index);

	board_state capture_board_state();
	void restore_board_state(const board_state& _state);

	// _on_board true is find only on board, false is find all chess
	std::vector<piece_base*> find_piece_by_name(bool _use_color, bool _only_on_borad, wchar_t _name);
	std::vector<piece_base*> find_piece_on_x(bool _use_color, bool _only_on_borad, uint8_t _x);
	std::vector<piece_base*> find_piece_on_y(bool _use_color, bool _only_on_borad, uint8_t _y);
	piece_base* find_piece_on_x_y(bool _use_color, bool _only_on_borad, uint8_t _x, uint8_t _y);

private:
	vulkan_pipeline pipeline;

	std::vector<model_vertex> vertices;
	vulkan_buffer vertices_buffer;

	std::vector<uint32_t> indices;
	vulkan_buffer indices_buffer;

	uint32_t now_record_index = 0;
	std::vector<board_state> all_board_state = { record_loader::init_board_state };
	std::array<std::array<std::unique_ptr<piece_base>, 16>, 2> all_pieces;
};