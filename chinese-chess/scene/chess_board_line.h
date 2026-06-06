#pragma once

#include "scene_node.h"

class chess_board_line : public scene_node_old
{
public:
	chess_board_line() = default;
	~chess_board_line() override = default;

	void create(const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format) override;
	void resize(const vulkan_application* _app, uint32_t _width, uint32_t _height) override;
	void update(const scene_camera* _camera) noexcept override;
	void render(const vk::raii::CommandBuffer& _commandbuffer) noexcept override;
	void destroy() noexcept override;

	std::vector<vk::AccelerationStructureInstanceKHR> get_all_blas_info() const noexcept override;

private:
	struct UBO
	{
		alignas(16) glm::mat4x4 model;
		alignas(16) glm::mat4x4 view;
		alignas(16) glm::mat4x4 proj;
		alignas(16) glm::vec3 camera_pos;
	};
};