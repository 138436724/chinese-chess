#pragma once

#include "scene_camera.h"
#include "scene_material_manager.h"
#include "scene_node_manager.h"
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


	struct push_constant
	{
		alignas(16) glm::vec3 camera_origin;
		union
		{
			alignas(16) glm::mat4x4 proj_matrix;
			alignas(16) glm::mat4x4 proj_inv_matrix;
		};
		union
		{
			alignas(16) glm::mat4x4 view_matrix;
			alignas(16) glm::mat4x4 view_inv_matrix;
		};
		alignas(16) glm::vec3 camera_direction;
	};

	struct model_data
	{
		alignas(16) glm::mat4 model_matrix = glm::mat4(1.f); // std430 layout
		alignas(8) uint32_t material_index = std::numeric_limits<uint32_t>::max();
		alignas(8) vk::DeviceAddress vertex_address = 0;
		alignas(8) vk::DeviceAddress index_address = 0;
	};

	struct material_data
	{
		alignas(8) glm::vec3 background_color = glm::vec3(1.f, 1.f, 1.f);
		alignas(8) glm::vec3 foreground_color = glm::vec3(1.f, 1.f, 1.f);
		uint32_t texture_index = std::numeric_limits<uint32_t>::max();
	};

	bool is_dirty = true;

	uint32_t width = 0;
	uint32_t height = 0;

	uint32_t current_frame = 0;
	vulkan_application* app = nullptr;

	std::vector<vulkan_commandbuffer> commandbuffers;

	std::unique_ptr<scene_node_manager> node_manager;
	std::unique_ptr<scene_material_manager> material_manager;

	scene_camera active_camera;
	vk::raii::Sampler image_sampler = nullptr;
	std::vector<std::shared_ptr<scene_node>> models;
	std::vector<std::shared_ptr<scene_material>> materials;

	vulkan_buffer model_ubo_buffer;
	vulkan_buffer material_ubo_buffer;

	vk::Format color_format = vk::Format::eUndefined;
	vulkan_image render_output;


	// only rasterization
	vulkan_image raster_color_image;
	vulkan_image raster_depth_image;
	vulkan_pipeline raster_pipeline;
	vk::raii::DescriptorPool raster_descriptor_pool = nullptr;
	std::vector<vk::raii::DescriptorSet> raster_descriptor_sets;
	vulkan_buffer raster_draw_commands;


	// only ray tracing
	vulkan_pipeline rt_pipeline;
	vk::raii::DescriptorPool rt_descriptor_pool = nullptr;
	std::vector<vk::raii::DescriptorSet> rt_descriptor_sets;
	vulkan_shader_binding_table rt_sbt;
	std::vector<vk::AccelerationStructureInstanceKHR> rt_instances;
	std::vector<vulkan_acceleration_structure> rt_tlas;
};
