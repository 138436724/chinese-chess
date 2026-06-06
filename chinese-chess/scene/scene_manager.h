#pragma once

#include "chess_board.h"
#include "chess_board_line.h"
#include "chess_manager.h"
#include "scene_camera.h"
#include "scene_material_manager.h"
#include "scene_node_manager.h"
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
	const vulkan_commandbuffer& render(bool _use_ray_tracing);
	void destroy();

	void need_update();

	std::weak_ptr<scene_node> create_node(const std::u8string& _model_name);
	void remove_node(const std::weak_ptr<scene_node>& _node) noexcept;

	std::weak_ptr<scene_material> create_material(const std::wstring& _characters);
	void remove_material(const std::weak_ptr<scene_material>& _material) noexcept;

	chess_manager* get_piece_manager() const noexcept;
	vulkan_image& get_render_image() noexcept;

private:
	// for rasterization
	void create_rasterization();
	void resize_rasterization();
	void update_rasterization();
	void render_rasterization(const vk::raii::CommandBuffer& _commandbuffer) noexcept;

	// for ray tracing
	void create_ray_tracing();
	void resize_ray_tracing();
	void update_ray_tracing();
	void render_ray_tracing(const vk::raii::CommandBuffer& _commandbuffer) noexcept;

	struct PushConstant
	{
		alignas(16) glm::vec3 cameraOrigin;
		alignas(16) glm::mat4x4 projInvMatrix;
		alignas(16) glm::mat4x4 viewInvMatrix;
		alignas(16) glm::vec3 cameraDirection;
	};

	vk::Format color_format = vk::Format::eUndefined;

	vulkan_image color_image;
	vulkan_image depth_image;
	vulkan_image render_output;

	scene_camera active_camera;

	std::vector<std::unique_ptr<scene_node_old>> nodes;
	chess_manager* piece_manager = nullptr;
	chess_board* board_ = nullptr;
	chess_board_line* board_line_ = nullptr;

	std::unique_ptr<scene_cubemap> cubemap = nullptr;



	// new ======================================================

	struct model_data
	{
		alignas(8) glm::mat4 model_matrix = glm::mat4(1.f);
		alignas(8) uint32_t material_index = std::numeric_limits<uint32_t>::max();
		alignas(8) vk::DeviceAddress vertex_address = 0;
		alignas(8) vk::DeviceAddress index_address = 0;
		uint32_t _padding[2];  // 匹配std430 StructuredBuffer
	};

	struct material_data
	{
		alignas(8) glm::vec3 background_color = glm::vec3(1.f, 1.f, 1.f);
		alignas(8) glm::vec3 foreground_color = glm::vec3(1.f, 1.f, 1.f);
		uint32_t texture_index = std::numeric_limits<uint32_t>::max();
	};

	vulkan_buffer model_ubo_buffer;
	vulkan_buffer material_ubo_buffer;

	bool is_dirty = true;

	uint32_t width = 0;
	uint32_t height = 0;

	uint32_t current_frame = 0;
	vulkan_application* app = nullptr;

	std::vector<vulkan_commandbuffer> commandbuffers;

	std::unique_ptr<scene_node_manager> node_manager;
	std::unique_ptr<scene_material_manager> material_manager;

	vk::raii::Sampler image_sampler = nullptr;
	std::vector<std::shared_ptr<scene_node>> models;
	std::vector<std::shared_ptr<scene_material>> materials;

	vulkan_pipeline rt_pipeline;
	vk::raii::DescriptorPool rt_descriptor_pool = nullptr;
	std::vector<vk::raii::DescriptorSet> rt_descriptor_sets;
	vulkan_shader_binding_table rt_sbt;
	std::vector<vk::AccelerationStructureInstanceKHR> rt_instances;
	std::vector<vulkan_acceleration_structure> rt_tlas;
};
