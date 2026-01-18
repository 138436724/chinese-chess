module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

export module scene_manager;

import <cstdint>;
import std;
import vulkan_hpp;
import vulkan_image;
import vulkan_common;
import vulkan_application;
import vulkan_commandbuffer;
import scene_node;
import scene_camera;
import chess_pieces;
import ui;

export class scene_manager
{
public:
	scene_manager() = default;
	~scene_manager() = default;

	// in the scene, all object's pipeline only need one color format and one depth format
	void create(GLFWwindow* _window, vulkan_application* _app, uint32_t _width, uint32_t _height);
	void resize(uint32_t _width, uint32_t _height);
	void update();
	void render(bool _save = false);
	void destroy();

	chess_pieces* get_piece_manager() const;

private:
	const vk::Format color_format = vk::Format::eR16G16B16A16Sfloat;

	vulkan_application* app = nullptr;
	uint32_t current_frame = 0;
	std::vector<vulkan_commandbuffer> commandbuffers;

	uint32_t width = 0;
	uint32_t height = 0;

	vulkan_image color_image;
	vulkan_image depth_image;
	vulkan_image render_output;

	scene_camera active_camera;

	std::vector<std::unique_ptr<scene_node>> nodes;

	chess_pieces* piece_manager = nullptr;
	ui* ui_manager = nullptr;
};