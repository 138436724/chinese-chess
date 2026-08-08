#pragma once

#include <span>
#include <vulkan/vulkan_raii.hpp>

class vulkan_pipeline
{
public:
    vulkan_pipeline()                                  = default;
    ~vulkan_pipeline()                                 = default;
    vulkan_pipeline(const vulkan_pipeline&)            = delete;
    vulkan_pipeline& operator=(const vulkan_pipeline&) = delete;
    vulkan_pipeline(vulkan_pipeline&& _other) noexcept;
    vulkan_pipeline& operator=(vulkan_pipeline&& _other) noexcept;

    void create(const vk::raii::Device&                                    _device,
                const std::span<const vk::DescriptorSetLayoutBinding>      _descriptor_set_layout_bindings,
                const std::span<const vk::PushConstantRange>               _push_constant,
                const std::span<const vk::VertexInputBindingDescription>   _binding_description,
                const std::span<const vk::VertexInputAttributeDescription> _attribute_descriptions,
                const std::span<const vk::PipelineShaderStageCreateInfo>   _shader_stages,
                vk::PrimitiveTopology                                      _topology_type,
                vk::PolygonMode                                            _polygon_mode,
                vk::CullModeFlags                                          _cull_mode,
                vk::FrontFace                                              _front_face,
                vk::SampleCountFlagBits                                    _multisample_count,
                vk::Bool32                                                 _use_depth,
                const std::span<const vk::Format>&                         _color_formats,
                vk::Format                                                 _depth_format);

    void create(const vk::raii::Device&                                       _device,
                const std::span<const vk::DescriptorSetLayoutBinding>         _descriptor_set_layout_bindings,
                const std::span<const vk::PushConstantRange>                  _push_constant,
                const std::span<const vk::PipelineShaderStageCreateInfo>      _shader_stages,
                const std::span<const vk::RayTracingShaderGroupCreateInfoKHR> _shader_groups,
                uint32_t                                                      _max_depth);

    [[nodiscard]] const vk::raii::DescriptorSetLayout& get_descriptor_set_layout() const noexcept;
    [[nodiscard]] const vk::raii::PipelineLayout&      get_pipeline_layout() const noexcept;
    [[nodiscard]] const vk::raii::Pipeline&            get_pipeline() const noexcept;

private:
    vk::raii::DescriptorSetLayout descriptor_set_layout = nullptr;
    vk::raii::PipelineLayout      pipeline_layout       = nullptr;
    vk::raii::Pipeline            pipeline              = nullptr;
};
