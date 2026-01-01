module vulkan_pipeline;

vulkan_pipeline::vulkan_pipeline(vulkan_pipeline&& _other) noexcept
	:descriptor_set_layout(std::move(_other.descriptor_set_layout)),
	pipeline_layout(std::move(_other.pipeline_layout)),
	pipeline(std::move(_other.pipeline))
{
}

vulkan_pipeline& vulkan_pipeline::operator=(vulkan_pipeline&& _other) noexcept
{
	if (this != &_other)
	{
		std::swap(descriptor_set_layout, _other.descriptor_set_layout);
		std::swap(pipeline_layout, _other.pipeline_layout);
		std::swap(pipeline, _other.pipeline);
	}
	return *this;
}

void vulkan_pipeline::create_pipeline(const vk::raii::Device& _device,
	const std::span<vk::DescriptorSetLayoutBinding>& _descriptor_set_layout_bindings,
	const std::span<vk::PushConstantRange>& _push_constant,
	const std::span<vk::VertexInputBindingDescription>& _binding_description,
	const std::span<vk::VertexInputAttributeDescription>& _attribute_descriptions,
	const std::span<vk::PipelineShaderStageCreateInfo>& _shader_stages,
	vk::PrimitiveTopology _topology_type, vk::PolygonMode _polygon_mode,
	vk::CullModeFlags _cull_mode, vk::FrontFace _front_face,
	vk::SampleCountFlagBits _multisample_count, vk::Bool32 _use_depth,
	const std::span<vk::Format>& _color_formats, vk::Format _depth_format)
{
	descriptor_set_layout = vk::raii::DescriptorSetLayout(_device, vk::DescriptorSetLayoutCreateInfo({}, _descriptor_set_layout_bindings));

	vk::PipelineLayoutCreateInfo pipeline_layout_info({}, (*descriptor_set_layout), _push_constant, nullptr);
	pipeline_layout = vk::raii::PipelineLayout(_device, pipeline_layout_info);

	vk::PipelineVertexInputStateCreateInfo vertex_input_info({}, _binding_description, _attribute_descriptions, nullptr);
	vk::PipelineInputAssemblyStateCreateInfo input_assembly({}, _topology_type, vk::False, nullptr);
	vk::PipelineViewportStateCreateInfo viewport_state({}, 1, nullptr, 1, nullptr, nullptr);

	vk::PipelineRasterizationStateCreateInfo rasterizer({}, vk::False, vk::False, _polygon_mode, _cull_mode, _front_face, vk::False, 0.f, 0.f, 1.f, 1.f, nullptr);
	vk::PipelineMultisampleStateCreateInfo multisampling({}, _multisample_count, vk::False);
	vk::PipelineDepthStencilStateCreateInfo depth_stencil({}, _use_depth, _use_depth, vk::CompareOp::eLess, vk::False, vk::False);

	vk::PipelineColorBlendAttachmentState color_blend_attachment(vk::False, vk::BlendFactor::eZero, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
		vk::BlendFactor::eZero, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
		vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
	vk::PipelineColorBlendStateCreateInfo color_blending({}, vk::False, vk::LogicOp::eCopy, 1, &color_blend_attachment);

	std::array dynamic_states = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo dynamic_state_info({}, dynamic_states, nullptr);

	vk::StructureChain<vk::GraphicsPipelineCreateInfo, vk::PipelineRenderingCreateInfo> pipeline_info(
		vk::GraphicsPipelineCreateInfo({}, _shader_stages, &vertex_input_info, &input_assembly, nullptr, &viewport_state, &rasterizer,
			&multisampling, &depth_stencil, &color_blending, &dynamic_state_info, pipeline_layout, nullptr, 0, nullptr, 0),
		vk::PipelineRenderingCreateInfo({}, _color_formats, _depth_format, vk::Format::eUndefined));

	pipeline = vk::raii::Pipeline(_device, nullptr, pipeline_info.get());
}

const vk::raii::DescriptorSetLayout& vulkan_pipeline::get_descriptor_set_layout() const
{
	return descriptor_set_layout;
}

const vk::raii::PipelineLayout& vulkan_pipeline::get_pipeline_layout() const
{
	return pipeline_layout;
}

const vk::raii::Pipeline& vulkan_pipeline::get_pipeline() const
{
	return pipeline;
}