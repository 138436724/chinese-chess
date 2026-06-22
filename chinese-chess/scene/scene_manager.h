#pragma once

#include "scene_camera.h"
#include "scene_material_manager.h"
#include "scene_model_manager.h"
#include "scene_light_manager.h"
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
	const vulkan_commandbuffer& render();
	void destroy();

	void need_update() noexcept;

	std::shared_ptr<scene_model> create_model(const std::filesystem::path& _model_name);
	void remove_model(const std::weak_ptr<scene_model>& _model) noexcept;
	std::shared_ptr<scene_material> create_material(const std::wstring& _characters);
	void remove_material(const std::weak_ptr<scene_material>& _material) noexcept;
	std::shared_ptr<scene_light> create_light(light_type _type) noexcept;
	void remove_light(const std::weak_ptr<scene_light>& _light) noexcept;

	void set_use_ray_tracing(bool _use_ray_tracing) noexcept;

	scene_camera& get_active_camera() noexcept;
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
		alignas(16) glm::mat4x4 proj_or_inv_matrix;
		alignas(16) glm::mat4x4 view_or_inv_matrix;
		alignas(16) uint32_t light_count = 0;
		uint32_t frame_index = 0;
	};

	bool is_dirty = true;

	bool use_ray_tracing = true;

	uint32_t width = 0;
	uint32_t height = 0;

	uint32_t current_frame = 0;
	vulkan_application* app = nullptr;

	std::vector<vulkan_commandbuffer> commandbuffers;

	std::unique_ptr<scene_model_manager> model_manager;
	std::unique_ptr<scene_material_manager> material_manager;
	std::unique_ptr<scene_light_manager> light_manager;

	scene_camera active_camera;

	vk::raii::Sampler image_sampler = nullptr;

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
	uint32_t rt_frame_index = 0;
	vulkan_pipeline rt_pipeline;
	vk::raii::DescriptorPool rt_descriptor_pool = nullptr;
	std::vector<vk::raii::DescriptorSet> rt_descriptor_sets;
	vulkan_shader_binding_table rt_sbt;
	std::vector<vulkan_acceleration_structure> rt_tlas;
};