#pragma once

#include "scene/chess_piece.h"
#include "scene/scene_camera.h"
#include "tools/model_loader.h"
#include "tools/record_loader.h"
#include "vulkan_core/vulkan_application.h"
#include "vulkan_core/vulkan_buffer.h"

class chess_manager
{
public:
	chess_manager() = default;
	virtual ~chess_manager() = default;

	void create(const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format);
	void resize(const vulkan_application* _app, uint32_t _width, uint32_t _height);
	void update(const scene_camera* _camera) noexcept;
	void render(const vk::raii::CommandBuffer& _commandbuffer) noexcept;
	void destroy() noexcept;

	bool load_record(const std::filesystem::path& _record_path);
	void set_now_record_index(uint32_t _index) noexcept;

	all_board_state capture_board_state();
	void restore_board_state(const all_board_state& _state);

private:
	vk::raii::DescriptorPool descriptor_pool = nullptr;
	vulkan_pipeline pipeline;

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
		uint32_t piece_type;
	};
	struct UBOS
	{
		UBO ubo[32];
	};
	std::vector<vulkan_buffer> ubos;

	uint32_t alive_piece_num = 0;

	vulkan_image font_images;
	vk::raii::Sampler font_sampler = nullptr;

	uint32_t now_record_index = 0;
	std::vector<all_board_state> board_state = { RECORD_LOADER.get_init_all_borad() };
	std::array<std::array<chess_piece, 16>, 2> all_pieces;
};