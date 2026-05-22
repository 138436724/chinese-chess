#pragma once

#include <vulkan/vulkan_raii.hpp>

class vulkan_pipeline
{
public:
	vulkan_pipeline() = default;
	~vulkan_pipeline() = default;
	vulkan_pipeline(vulkan_pipeline&) = delete;
	vulkan_pipeline(vulkan_pipeline&& _other) noexcept;
	vulkan_pipeline& operator=(vulkan_pipeline&) = delete;
	vulkan_pipeline& operator=(vulkan_pipeline&& _other) noexcept;

	void create(const vk::raii::Device& _device,
		const std::span<vk::DescriptorSetLayoutBinding>& _descriptor_set_layout_bindings,
		const std::span<vk::PushConstantRange>& _push_constant,
		const std::span<vk::VertexInputBindingDescription>& _binding_description,
		const std::span<vk::VertexInputAttributeDescription>& _attribute_descriptions,
		const std::span<vk::PipelineShaderStageCreateInfo>& _shader_stages,
		vk::PrimitiveTopology _topology_type, vk::PolygonMode _polygon_mode,
		vk::CullModeFlags _cull_mode, vk::FrontFace _front_face,
		vk::SampleCountFlagBits _multisample_count, vk::Bool32 _use_depth,
		const std::span<vk::Format>& _color_formats, vk::Format _depth_format);

	void create(const vk::raii::Device& _device,
		const std::span<vk::DescriptorSetLayoutBinding>& _descriptor_set_layout_bindings,
		const std::span<vk::PushConstantRange>& _push_constant,
		const std::span<vk::PipelineShaderStageCreateInfo>& _shader_stages,
		const std::span<vk::RayTracingShaderGroupCreateInfoKHR>& _shader_groups,
		uint32_t _max_depth);

	const vk::raii::DescriptorSetLayout& get_descriptor_set_layout() const noexcept;
	const vk::raii::PipelineLayout& get_pipeline_layout() const noexcept;
	const vk::raii::Pipeline& get_pipeline() const noexcept;

private:
	vk::raii::DescriptorSetLayout descriptor_set_layout = nullptr;
	vk::raii::PipelineLayout pipeline_layout = nullptr;
	vk::raii::Pipeline pipeline = nullptr;
};