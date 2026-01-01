export module chess_piece;

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
import model_loader;

export class chess_piece : public scene_node
{
public:
	chess_piece() = default;
	virtual ~chess_piece() = default;

	void create(const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format) override;
	void resize(const vulkan_application* _app, uint32_t _width, uint32_t _height) override;
	void update(const scene_camera* _camera) override;
	void render(const vk::raii::CommandBuffer& _commandbuffer) override;

private:
	vk::raii::DescriptorPool descriptor_pool = nullptr;
	vulkan_pipeline pipeline; // todo maybe static

	std::vector<model_vertex> vertices;
	vulkan_buffer vertices_buffer;

	std::vector<uint32_t> indices;
	vulkan_buffer indices_buffer;

	std::vector<vk::raii::DescriptorSet> descriptor_sets;

	uint32_t current_frame = 0;

	struct UBO
	{
		alignas(16) glm::mat4x4 model;
		alignas(16) glm::mat4x4 view;
		alignas(16) glm::mat4x4 proj;
		alignas(16) glm::vec3 camera_pos;
		alignas(16) glm::vec3 piece_color;
	};
	std::vector<vulkan_buffer> ubos;
	vulkan_image font_image;
	vk::raii::Sampler font_sampler = nullptr;

protected:
	inline static const float board_unit_distance = 0.25f;
	static glm::vec2 transform_location(bool _use_color, bool _piece_color, uint8_t _x, uint8_t _y);

	bool use_color = true; // true is red and false is black
	bool piece_color = true; // true is red and false is black
	std::wstring piece_name = L"兵";
	glm::u8vec2 piece_location = glm::u8vec2(1, 3);
	glm::vec2 model_location = transform_location(use_color, piece_color, piece_location.x, piece_location.y);

public:
	void set_piece_location(const glm::u8vec2& _location);

	virtual void set_piece_color(bool _use_color, bool _piece_color) = 0;
};