#pragma once

#include "chess_board.h"
#include "chess_board_line.h"
#include "chess_manager.h"
#include "scene_camera.h"
#include "scene_node.h"
#include "skybox/scene_cubemap.h"
#include "vulkan_core/vulkan_shader_binding_table.h"

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

    void ray_tracing_render(const vk::raii::CommandBuffer& _commandbuffer);

private:
    struct PushConstant
    {
		alignas(16) glm::vec3 cameraOrigin;
        alignas(16) glm::mat4x4 projInvMatrix;
        alignas(16) glm::mat4x4 viewInvMatrix;
		alignas(16) glm::vec3 cameraDirection;
    };

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
	chess_board* board_ = nullptr;
	chess_board_line* board_line_ = nullptr;

	std::unique_ptr<scene_cubemap> cubemap = nullptr;

	uint32_t current_frame = 0;

	// ray tracing
	std::vector<vk::AccelerationStructureInstanceKHR> tlas_instances;
	std::vector<vulkan_acceleration_structure> tlas;

	vulkan_pipeline pipeline;

	vulkan_shader_binding_table sbt;

	// 光追附加资源
	vulkan_buffer instance_data_buffer;   // InstanceData 数组
	vk::raii::DescriptorPool descriptor_pool = nullptr;
};
