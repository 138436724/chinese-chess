#include "vulkan_pipeline.h"

#include "tools/shader_compiler.h"

#include <filesystem>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] std::pair<vk::raii::ShaderModule, std::vector<vk::PipelineShaderStageCreateInfo>> compile_shader(
    const vk::raii::Device&                  _device,
    const std::filesystem::path&             _shader_path,
    const std::span<const shader_stage_info> _shader_stages)
{
    std::vector<std::string_view> entry_names =
        _shader_stages | std::views::transform(&shader_stage_info::name) | std::ranges::to<std::vector>();

    const auto spirv_code = SHADER_COMPILER.compile_shader_to_spv(_shader_path, entry_names);
    if (!spirv_code)
    {
        throw std::runtime_error(spirv_code.error());
    }

    auto shader_module =
        vk::raii::ShaderModule(_device, vk::ShaderModuleCreateInfo({}, spirv_code->size() * sizeof(char),
                                                                   reinterpret_cast<const uint32_t*>(spirv_code->data())));

    auto shader_stages = _shader_stages | std::views::transform([&shader_module](const auto& stage) {
                             return vk::PipelineShaderStageCreateInfo({}, stage.stage, shader_module, stage.name.data());
                         })
                         | std::ranges::to<std::vector>();


    return std::make_pair(std::move(shader_module), std::move(shader_stages));
}

}  // namespace

vulkan_pipeline::vulkan_pipeline(vulkan_pipeline&& _other) noexcept
    : descriptor_set_layout(std::exchange(_other.descriptor_set_layout, nullptr))
    , pipeline_layout(std::exchange(_other.pipeline_layout, nullptr))
    , pipeline(std::exchange(_other.pipeline, nullptr))
{
}

vulkan_pipeline& vulkan_pipeline::operator=(vulkan_pipeline&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(descriptor_set_layout, _other.descriptor_set_layout);
        std::ranges::swap(pipeline_layout, _other.pipeline_layout);
        std::ranges::swap(pipeline, _other.pipeline);
    }
    return *this;
}

void vulkan_pipeline::create(const vk::raii::Device&                                    _device,
                             const vk::raii::PipelineCache&                             _pipeline_cache,
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
                             vk::Format                                                 _depth_format)
{
    descriptor_set_layout =
        vk::raii::DescriptorSetLayout(_device, vk::DescriptorSetLayoutCreateInfo({}, _descriptor_set_layout_bindings));

    pipeline_layout =
        vk::raii::PipelineLayout(_device, vk::PipelineLayoutCreateInfo({}, *(descriptor_set_layout), _push_constant, nullptr));

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info({}, _binding_description, _attribute_descriptions, nullptr);
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly({}, _topology_type, vk::False, nullptr);
    const vk::PipelineViewportStateCreateInfo      viewport_state({}, 1, nullptr, 1, nullptr, nullptr);

    const vk::PipelineRasterizationStateCreateInfo rasterizer({}, vk::False, vk::False, _polygon_mode, _cull_mode,
                                                              _front_face, vk::False, 0.f, 0.f, 1.f, 1.f, nullptr);
    const vk::PipelineMultisampleStateCreateInfo   multisampling({}, _multisample_count, vk::False);
    const vk::PipelineDepthStencilStateCreateInfo  depth_stencil({}, _use_depth, _use_depth, vk::CompareOp::eLess,
                                                                 vk::False, vk::False);

    const vk::PipelineColorBlendAttachmentState color_blend_attachment(
        vk::False, vk::BlendFactor::eZero, vk::BlendFactor::eZero, vk::BlendOp::eAdd, vk::BlendFactor::eZero,
        vk::BlendFactor::eZero, vk::BlendOp::eAdd,
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB
            | vk::ColorComponentFlagBits::eA);

    const std::vector<vk::PipelineColorBlendAttachmentState> color_blend_attachments(_color_formats.size(), color_blend_attachment);
    const vk::PipelineColorBlendStateCreateInfo color_blending({}, vk::False, vk::LogicOp::eCopy, color_blend_attachments);

    constexpr std::array                     dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic_state_info({}, dynamic_states, nullptr);

    const vk::StructureChain<vk::GraphicsPipelineCreateInfo, vk::PipelineRenderingCreateInfo> pipeline_info(
        vk::GraphicsPipelineCreateInfo({}, _shader_stages, &vertex_input_info, &input_assembly, nullptr,
                                       &viewport_state, &rasterizer, &multisampling, &depth_stencil, &color_blending,
                                       &dynamic_state_info, pipeline_layout, nullptr, 0, nullptr, 0),
        vk::PipelineRenderingCreateInfo({}, _color_formats, _depth_format, vk::Format::eUndefined));

    pipeline = vk::raii::Pipeline(_device, _pipeline_cache, pipeline_info.get());
}

void vulkan_pipeline::create_from_shader(const vk::raii::Device&        _device,
                                         const vk::raii::PipelineCache& _pipeline_cache,
                                         const std::span<const vk::DescriptorSetLayoutBinding> _descriptor_set_layout_bindings,
                                         const std::span<const vk::PushConstantRange>             _push_constant,
                                         const std::span<const vk::VertexInputBindingDescription> _binding_description,
                                         const std::span<const vk::VertexInputAttributeDescription> _attribute_descriptions,
                                         const std::filesystem::path&             _shader_path,
                                         const std::span<const shader_stage_info> _shader_stages,
                                         vk::PrimitiveTopology                    _topology_type,
                                         vk::PolygonMode                          _polygon_mode,
                                         vk::CullModeFlags                        _cull_mode,
                                         vk::FrontFace                            _front_face,
                                         vk::SampleCountFlagBits                  _multisample_count,
                                         vk::Bool32                               _use_depth,
                                         const std::span<const vk::Format>&       _color_formats,
                                         vk::Format                               _depth_format)
{
    const auto& [shader_module, shader_stages] = compile_shader(_device, _shader_path, _shader_stages);
    create(_device, _pipeline_cache, _descriptor_set_layout_bindings, _push_constant, _binding_description,
           _attribute_descriptions, shader_stages, _topology_type, _polygon_mode, _cull_mode, _front_face,
           _multisample_count, _use_depth, _color_formats, _depth_format);
}

void vulkan_pipeline::create_from_shader(const vk::raii::Device&        _device,
                                         const vk::raii::PipelineCache& _pipeline_cache,
                                         const std::span<const vk::DescriptorSetLayoutBinding> _descriptor_set_layout_bindings,
                                         const std::span<const vk::PushConstantRange>                  _push_constant,
                                         const std::filesystem::path&                                  _shader_path,
                                         const std::span<const shader_stage_info>                      _shader_stages,
                                         const std::span<const vk::RayTracingShaderGroupCreateInfoKHR> _shader_groups,
                                         uint32_t                                                      _max_depth)
{
    descriptor_set_layout =
        vk::raii::DescriptorSetLayout(_device, vk::DescriptorSetLayoutCreateInfo({}, _descriptor_set_layout_bindings));

    pipeline_layout =
        vk::raii::PipelineLayout(_device, vk::PipelineLayoutCreateInfo({}, *(descriptor_set_layout), _push_constant, nullptr));

    const auto& [shader_module, shader_stages] = compile_shader(_device, _shader_path, _shader_stages);
    const vk::RayTracingPipelineCreateInfoKHR pipeline_info({}, shader_stages, _shader_groups, _max_depth, {}, {}, {}, pipeline_layout);
    pipeline = vk::raii::Pipeline(_device, nullptr, _pipeline_cache, pipeline_info);
}

void vulkan_pipeline::create_from_shader(const vk::raii::Device&        _device,
                                         const vk::raii::PipelineCache& _pipeline_cache,
                                         const std::span<const vk::DescriptorSetLayoutBinding> _descriptor_set_layout_bindings,
                                         const std::span<const vk::PushConstantRange> _push_constant,
                                         const std::filesystem::path&                 _shader_path,
                                         const std::span<const shader_stage_info>     _shader_stages)
{
    descriptor_set_layout =
        vk::raii::DescriptorSetLayout(_device, vk::DescriptorSetLayoutCreateInfo({}, _descriptor_set_layout_bindings, {}));

    pipeline_layout =
        vk::raii::PipelineLayout(_device, vk::PipelineLayoutCreateInfo({}, *descriptor_set_layout, _push_constant, nullptr));

    const auto& [shader_module, shader_stages] = compile_shader(_device, _shader_path, _shader_stages);
    const vk::ComputePipelineCreateInfo pipeline_info({}, shader_stages.front(), pipeline_layout, nullptr, 0);
    pipeline = vk::raii::Pipeline(_device, _pipeline_cache, pipeline_info);
}

const vk::raii::DescriptorSetLayout& vulkan_pipeline::get_descriptor_set_layout() const noexcept
{
    return descriptor_set_layout;
}

const vk::raii::PipelineLayout& vulkan_pipeline::get_pipeline_layout() const noexcept
{
    return pipeline_layout;
}

const vk::raii::Pipeline& vulkan_pipeline::get_pipeline() const noexcept
{
    return pipeline;
}
