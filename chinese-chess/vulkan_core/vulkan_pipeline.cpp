/**
 * @file vulkan_pipeline.cpp
 * @brief vulkan_pipeline 的实现：着色器编译 + 图形 / 光追 / 计算三条创建路径。
 * @details 匿名命名空间的 compile_shader 负责「入口名列表 → shader module + stage 数组」，编译错误
 *          以 std::runtime_error 上抛，成功时 module 由返回值持有且只在调用者栈上存活。
 *          三个 create_from_shader 只是把「编译」与「建布局 / 管线」的顺序拼起来，不做额外缓存。
 *          每个句柄都在构造成功后才赋给成员，所以抛异常最多留下「布局已建、管线为空」的部分状态。
 */

#include "vulkan_pipeline.h"

#include "tools/shader_compiler.h"

#include <array>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/**
 * @brief 编译一个 .slang 的全部入口，返回 shader module 与按 stage 拼好的创建信息数组。
 * @note 入口名从 _shader_stages 的 name 字段收集（顺序即数组顺序），module 由调用方以 const 引用接住；
 *       编译结果来自 SHADER_COMPILER，命中缓存时这里不会真的调用 Slang。
 */
[[nodiscard]] std::pair<vk::raii::ShaderModule, std::vector<vk::PipelineShaderStageCreateInfo>> compile_shader(
    const vk::raii::Device&                  _device,
    const std::filesystem::path&             _shader_path,
    const std::span<const shader_stage_info> _shader_stages)
{
    const std::vector<std::string_view> entry_names =
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

/** @brief 移动构造：逐成员 std::exchange，把三个 RAII 句柄从源对象搬空并置空源对象。 */
vulkan_pipeline::vulkan_pipeline(vulkan_pipeline&& _other) noexcept
    : descriptor_set_layout(std::exchange(_other.descriptor_set_layout, nullptr))
    , pipeline_layout(std::exchange(_other.pipeline_layout, nullptr))
    , pipeline(std::exchange(_other.pipeline, nullptr))
{
}

/** @brief 移动赋值：与源对象逐成员 std::ranges::swap，旧句柄被换到源对象、由它的析构释放。 */
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

/**
 * @brief 建图形管线：按序建描述符集布局、管线布局，再以动态渲染的 StructureChain 建管线。
 * @note 顶点输入 / 输入装配 / 光栅 / 多重采样 / 深度模板 / 颜色混合全部在这里固定；viewport 与 scissor
 *       只声明为动态状态，具体矩形由命令缓冲设置。深度测试与深度写同取 _use_depth，比较固定 eLess
 *       （配合 GLM_FORCE_DEPTH_ZERO_TO_ONE 的 0..1 深度范围）。颜色附件个数取自 _color_formats.size()。
 */
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

/**
 * @brief 编译后建图形管线：先拿到 shader module 与 stage 数组，再把其余参数原样转给 create。
 * @note 结构化绑定用 const 引用接住 compile_shader 的返回值，module 在本函数返回前一直有效；
 *       编译失败时 std::runtime_error 直接从 compile_shader 冒出去，不会进入 create。
 */
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

/**
 * @brief 建光追管线：先建布局，再编译，最后以 vk::RayTracingPipelineCreateInfoKHR 建管线。
 * @note 这里直接把 _shader_stages 与 _shader_groups 交给驱动，组与入口的对应关系由调用方保证；
 *       vk::raii::Pipeline 的 deferredOperation 参数传 nullptr ⇒ 同步创建，不产生延迟操作对象。
 */
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

/**
 * @brief 建计算管线：先建布局与编译，再以 shader_stages.front() 作为 compute stage 建管线。
 * @note 只取第一个入口，因此 _shader_stages 必须把 compute 入口放最前（Ray Query 分支只有一个入口）。
 */
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

/** @brief 返回 create 建好的描述符集布局；未创建时是空句柄。 */
const vk::raii::DescriptorSetLayout& vulkan_pipeline::get_descriptor_set_layout() const noexcept
{
    return descriptor_set_layout;
}

/** @brief 返回与描述符集布局一起建好的管线布局；未创建时是空句柄。 */
const vk::raii::PipelineLayout& vulkan_pipeline::get_pipeline_layout() const noexcept
{
    return pipeline_layout;
}

/** @brief 返回管线本体；未创建时是空句柄，绑定前调用方需自行确保已创建。 */
const vk::raii::Pipeline& vulkan_pipeline::get_pipeline() const noexcept
{
    return pipeline;
}
