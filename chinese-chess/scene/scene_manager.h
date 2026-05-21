#pragma once

#include "chess_manager.h"
#include "scene_camera.h"
#include "scene_node.h"
#include "skybox/scene_cubemap.h"

class scene_manager
{
public:
	scene_manager() = default;
	~scene_manager() = default;

	// in the scene, all object's pipeline only need one color format and one depth format
	void create(vulkan_application* _app, uint32_t _width, uint32_t _height);
	void resize(uint32_t _width, uint32_t _height);
	void update();
	void render(const vk::raii::CommandBuffer& _commandbuffer);
	void destroy();

	chess_manager* get_piece_manager() const noexcept;

	vulkan_image& get_render_image() noexcept;

private:
	vk::Format color_format = vk::Format::eUndefined;

	vulkan_application* app = nullptr;

	uint32_t width = 0;
	uint32_t height = 0;

	vulkan_image color_image;
	vulkan_image depth_image;
	vulkan_image render_output;

	scene_camera active_camera;

	std::vector<std::unique_ptr<scene_node>> nodes;
	chess_manager* piece_manager = nullptr;

	std::unique_ptr<scene_cubemap> cubemap = nullptr;

	vulkan_acceleration_structure tlas;
};