module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <imgui.h>

export module ui;

import <cstdint>;
import std;
import vulkan_hpp;
import vulkan_image;
import vulkan_pipeline;
import vulkan_application;
import vulkan_commandbuffer;
import scene_node;
import chess_pieces;

export class ui :public scene_node
{
public:
	ui() = default;
	~ui() = default;

	void create(GLFWwindow* _window, const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format) override;
	void resize(const vulkan_application* _app, uint32_t _width, uint32_t _height) override;
	void update(const scene_camera* _camera) override;
	void render(const vk::raii::CommandBuffer& _commandbuffer) override;
	void destroy() override;

	void render_end();

	void set_chess_manager(chess_pieces* _manager);

private:
	void load_records(const std::filesystem::path& _record_path);

	vk::raii::DescriptorPool descriptor_pool = nullptr;
	std::array<vk::Format, 1> color_format;
	vk::PipelineRenderingCreateInfo create_info;
	ImDrawData* draw_data = nullptr;

	chess_pieces* manager = nullptr;

	int selected_index = 0;
	std::vector<std::u8string> all_records;
	std::vector<const char*> all_records_c_str;
};