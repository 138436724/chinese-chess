export module piece_base;

import <glm/ext/matrix_float4x4.hpp>;
import std;
import glm;
import vulkan_hpp;
import vulkan_image;
import vulkan_buffer;
import vulkan_application;
import scene_camera;

export class piece_base
{
public:
	piece_base() = default;
	virtual ~piece_base() = default;

	virtual void create(const vulkan_application* _app, bool _use_color, bool _piece_color);
	void resize(const vulkan_application* _app, const vk::raii::DescriptorSetLayout& _descriptor_set_layout, uint32_t _width, uint32_t _height);
	void update(const scene_camera* _camera);
	void render(const vk::raii::CommandBuffer& _commandbuffer, const vk::raii::PipelineLayout& _pipeline_layout);

	virtual glm::u8vec2 move_by(wchar_t _direction, uint8_t _move_number) = 0;
	virtual std::pair<wchar_t, uint8_t> move_to(const glm::u8vec2& _location) = 0;

	void set_is_on_board(bool _is_on_board);
	void set_piece_location(const glm::u8vec2& _location);

	bool get_is_on_board() const;
	bool get_piece_color() const;
	wchar_t get_piece_name() const;
	glm::u8vec2 get_piece_location() const;

protected:
	vk::raii::DescriptorPool descriptor_pool = nullptr;
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

	bool is_on_board = true;
	bool use_color; // true is red and false is black
	bool piece_color; // true is red and false is black
	wchar_t piece_name;
	glm::u8vec2 piece_location;
	glm::vec2 model_location;

	inline static const float board_unit_distance = 0.25f;
	static glm::vec2 transform_location(bool _use_color, bool _piece_color, uint8_t _x, uint8_t _y);
};