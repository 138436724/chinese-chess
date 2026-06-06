#pragma once

#include "chess_piece.h"
#include "scene_camera.h"
#include "scene_node.h"
#include "tools/record_loader.h"

class chess_manager : public scene_node_old
{
public:
	chess_manager() = default;
	~chess_manager()  override = default;

	void create(const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format) override;
	void resize(const vulkan_application* _app, uint32_t _width, uint32_t _height) override;
	void update(const scene_camera* _camera) noexcept override;
	void render(const vk::raii::CommandBuffer& _commandbuffer) noexcept override;
	void destroy() noexcept override;

	std::vector<vk::AccelerationStructureInstanceKHR> get_all_blas_info() const noexcept override;

	const vulkan_image& get_font_image() const noexcept { return font_image; }
	const vk::raii::Sampler& get_font_sampler() const noexcept { return font_sampler; }

	bool load_record(const std::filesystem::path& _record_path);
	void set_now_record_index(uint32_t _index) noexcept;

	all_board_state capture_board_state();
	void restore_board_state(const all_board_state& _state);

private:
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

	uint32_t alive_piece_num = 0;

	vulkan_image font_image;
	vk::raii::Sampler font_sampler = nullptr;

	uint32_t now_record_index = 0;
	std::vector<all_board_state> board_state = { RECORD_LOADER.get_init_all_borad() };
	std::array<std::array<chess_piece, 16>, 2> all_pieces;
};
