/**
 * @file vulkan_pipeline.h
 * @brief 图形 / 光追 / 计算三类 Vulkan 管线的一体化 RAII 封装：描述符集布局、管线布局、管线一起建。
 * @details 三个 create_from_shader 重载分别走图形、光追、计算路径，都先用 SHADER_COMPILER 编译 .slang
 *          （命中着色器缓存时复用 SPIR-V）。图形路径先编译再建布局，光追与计算路径先建布局再编译。
 *          所有句柄由 vk::raii 持有：移动构造清空源对象，移动赋值交换双方资源。
 *          调用方通过临时对象构造新管线，再安排旧资源回收；本类不提供整个热重载流程的回滚保证。
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vulkan/vulkan_raii.hpp>

/**
 * @brief 一个着色器入口：Slang 入口名 + 它对应的 Vulkan stage。
 * @details create_from_shader 把所有 name 当作入口名列表交给 SHADER_COMPILER 一次性编译，
 *          编译出的单个 shader module 再按 stage 复用到 vk::PipelineShaderStageCreateInfo 数组里。
 */
struct shader_stage_info
{
    /** @brief 入口名（Slang 函数名）；须以 NUL 结尾且在创建期间存活，因为 Vulkan 直接使用 name.data()。 */
    std::string_view        name;
    /** @brief 该入口对应的 Vulkan stage，决定它在 vk::PipelineShaderStageCreateInfo 里的位置。 */
    vk::ShaderStageFlagBits stage;
};

/**
 * @brief 单条 Vulkan 管线的 RAII 包装：描述符集布局 + 管线布局 + 管线本体。
 * @details 三种创建路径都把布局与管线一起建好，所以 create 之后 get_descriptor_set_layout() 与
 *          get_pipeline_layout() 立刻可用于分配描述符集。create 中途抛异常时成员按赋值顺序留下部分状态
 *          （布局可能已建、管线仍为空），因此调用方一律「新建临时对象再 move 替换」，不要原地重建。
 *          管线是设备级不可变对象，创建后不再修改；push constant 由调用方以 span 给出，当前调用点恒传空。
 */
class vulkan_pipeline
{
public:
    /** @brief 空对象：三个句柄均为 nullptr，直到 create / create_from_shader 成功。 */
    vulkan_pipeline()                                  = default;
    /** @brief 析构：由 vk::raii 释放管线与两个布局；调用方须保证 GPU 已不再使用它们。 */
    ~vulkan_pipeline()                                 = default;
    /** @brief 禁用拷贝：管线与布局句柄都是独占所有权。 */
    vulkan_pipeline(const vulkan_pipeline&)            = delete;
    /** @brief 禁用拷贝赋值：同上，句柄不可共享。 */
    vulkan_pipeline& operator=(const vulkan_pipeline&) = delete;
    /** @brief 移动构造：三个 RAII 句柄用 std::exchange 搬走，源对象被置空。 */
    vulkan_pipeline(vulkan_pipeline&& _other) noexcept;
    /** @brief 移动赋值：与源对象逐成员 std::ranges::swap，相当于接管对方句柄并把自己的旧句柄交给它。 */
    vulkan_pipeline& operator=(vulkan_pipeline&& _other) noexcept;

    /**
     * @brief 用已编译好的 shader stage 建图形管线，同时建描述符集布局与管线布局。
     * @param _device 创建管线与布局的逻辑设备。
     * @param _pipeline_cache 管线缓存；未命中时驱动照常编译，只损失启动速度。
     * @param _descriptor_set_layout_bindings 描述符集布局的 binding 数组；binding 号须唯一，不要求按升序排列。
     * @param _push_constant push constant 范围数组；渲染器不推送常量，当前恒传空。
     * @param _binding_description 顶点缓冲绑定描述，与 _attribute_descriptions 组成顶点输入状态。
     * @param _attribute_descriptions 顶点属性描述，location 必须与着色器输入位置一致。
     * @param _shader_stages 已编译的 stage 数组，shader module 的生命周期必须覆盖本调用。
     * @param _topology_type 图元拓扑（三角形列表等）。
     * @param _polygon_mode 多边形模式；当前所有调用点都传 eFill。
     * @param _cull_mode 面剔除模式；前向 PBR 用背面剔除，天空盒与合成关闭。
     * @param _front_face 正面绕序，与模型的三角形绕序一致。
     * @param _multisample_count 光栅化采样数，必须与颜色 / 深度附件的采样数一致。
     * @param _use_depth 同时作为 depthTestEnable 与 depthWriteEnable；比较固定 eLess，不写模板。
     * @param _color_formats 动态渲染的颜色附件格式数组，个数决定 blend attachment 的个数。
     * @param _depth_format 深度附件格式；不写深度时也不会用到，但要与渲染目标一致。
     * @throws vk::SystemError 任一 Vulkan 对象创建失败时由 vk::raii 抛出；此时对象可能停在部分创建状态。
     * @note 走 vk::PipelineRenderingCreateInfo（动态渲染），因此没有 render pass 对象；
     *       viewport / scissor 是动态状态，必须由命令缓冲在录制时设置。颜色混合恒关闭
     *       （eZero / eZero / eAdd，RGBA 全写），scene 与 UI 的合成由 vulkan_blend_image.slang 在着色器里做。
     */
    void create(const vk::raii::Device&                                    _device,
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
                vk::Format                                                 _depth_format);

    /**
     * @brief 编译 .slang 后建图形管线：按 _shader_stages 收集入口名编译，再转调 create。
     * @param _device 创建 shader module、布局与管线的逻辑设备。
     * @param _pipeline_cache 管线缓存，透传给 create。
     * @param _descriptor_set_layout_bindings 描述符集布局的 binding 数组。
     * @param _push_constant push constant 范围数组，当前恒为空。
     * @param _binding_description 顶点缓冲绑定描述。
     * @param _attribute_descriptions 顶点属性描述。
     * @param _shader_path .slang 源文件路径，相对运行目录（chinese-chess/）。
     * @param _shader_stages 入口名 + stage 列表；全部入口在同一次编译里产出。
     * @param _topology_type 图元拓扑。
     * @param _polygon_mode 多边形模式。
     * @param _cull_mode 面剔除模式。
     * @param _front_face 正面绕序。
     * @param _multisample_count 光栅化采样数，须与附件一致。
     * @param _use_depth 是否开启深度测试与深度写。
     * @param _color_formats 动态渲染的颜色附件格式数组。
     * @param _depth_format 深度附件格式。
     * @throws std::runtime_error 着色器编译失败；错误串直接来自 SHADER_COMPILER（含 Slang 诊断）。
     * @throws vk::SystemError 布局或管线创建失败。
     * @note shader module 只活到本函数返回：管线创建完成后驱动不再需要它，因此不缓存。
     */
    void create_from_shader(const vk::raii::Device&                                    _device,
                            const vk::raii::PipelineCache&                             _pipeline_cache,
                            const std::span<const vk::DescriptorSetLayoutBinding>      _descriptor_set_layout_bindings,
                            const std::span<const vk::PushConstantRange>               _push_constant,
                            const std::span<const vk::VertexInputBindingDescription>   _binding_description,
                            const std::span<const vk::VertexInputAttributeDescription> _attribute_descriptions,
                            const std::filesystem::path&                               _shader_path,
                            const std::span<const shader_stage_info>                   _shader_stages,
                            vk::PrimitiveTopology                                      _topology_type,
                            vk::PolygonMode                                            _polygon_mode,
                            vk::CullModeFlags                                          _cull_mode,
                            vk::FrontFace                                              _front_face,
                            vk::SampleCountFlagBits                                    _multisample_count,
                            vk::Bool32                                                 _use_depth,
                            const std::span<const vk::Format>&                         _color_formats,
                            vk::Format                                                 _depth_format);

    /**
     * @brief 编译 .slang 后建光追管线：直接以 vk::RayTracingPipelineCreateInfoKHR 创建，布局在此一并建好。
     * @param _device 创建 shader module、布局与管线的逻辑设备。
     * @param _pipeline_cache 管线缓存。
     * @param _descriptor_set_layout_bindings 描述符集布局的 binding 数组（AS / 输出图 / UBO / bindless）。
     * @param _push_constant push constant 范围数组，当前恒为空。
     * @param _shader_path 光追 .slang 源文件路径。
     * @param _shader_stages 光追入口列表；当前调用点使用 raygen、两个 miss、closest hit 与 shadow any-hit。
     * @param _shader_groups 着色器组数组，下标即 SBT 组号；组内 shader 索引引用 _shader_stages，不要求组与入口一一对应。
     * @param _max_depth 最大递归深度，不得超过设备上限；当前 raygen 循环积分器传 1。
     * @throws std::runtime_error 着色器编译失败。
     * @throws vk::SystemError 布局或光追管线创建失败。
     * @note 走 vk::raii::Pipeline 的 deferred 重载但 deferredOperation 传 nullptr ⇒ 同步创建、不走延迟编译。
     *       SBT 要另用 vulkan_shader_binding_table::create 从这条管线取组句柄，本函数不建 SBT。
     */
    void create_from_shader(const vk::raii::Device&                               _device,
                            const vk::raii::PipelineCache&                        _pipeline_cache,
                            const std::span<const vk::DescriptorSetLayoutBinding> _descriptor_set_layout_bindings,
                            const std::span<const vk::PushConstantRange>          _push_constant,
                            const std::filesystem::path&                          _shader_path,
                            const std::span<const shader_stage_info>              _shader_stages,
                            const std::span<const vk::RayTracingShaderGroupCreateInfoKHR> _shader_groups,
                            uint32_t                                                      _max_depth);

    /**
     * @brief 编译 .slang 后建计算管线：只取 _shader_stages 的第一个入口作为 compute stage。
     * @param _device 创建 shader module、布局与管线的逻辑设备。
     * @param _pipeline_cache 管线缓存。
     * @param _descriptor_set_layout_bindings 描述符集布局的 binding 数组（Ray Query 分支的 SSBO / 输出图）。
     * @param _push_constant push constant 范围数组，当前恒为空。
     * @param _shader_path 计算 .slang 源文件路径。
     * @param _shader_stages 非空入口列表，首项必须为 compute；所有入口都会参与编译，但创建管线只用 front()。
     * @throws std::runtime_error 着色器编译失败。
     * @throws vk::SystemError 布局或计算管线创建失败。
     * @note 计算管线没有顶点输入与光栅状态，dispatch 需要的描述符集布局已在此建好。
     */
    void create_from_shader(const vk::raii::Device&                               _device,
                            const vk::raii::PipelineCache&                        _pipeline_cache,
                            const std::span<const vk::DescriptorSetLayoutBinding> _descriptor_set_layout_bindings,
                            const std::span<const vk::PushConstantRange>          _push_constant,
                            const std::filesystem::path&                          _shader_path,
                            const std::span<const shader_stage_info>              _shader_stages);

    /**
     * @brief 取描述符集布局，供 allocateDescriptorSets 使用。
     * @return 布局句柄的引用；create 之前是 nullptr，引用在下一次 create / 移动之前有效。
     */
    [[nodiscard]] const vk::raii::DescriptorSetLayout& get_descriptor_set_layout() const noexcept;
    /**
     * @brief 取管线布局，供 bindDescriptorSets 与 push constant 使用。
     * @return 管线布局句柄的引用；只含一个 set（上面的布局）加调用方给的 push constant 范围。
     */
    [[nodiscard]] const vk::raii::PipelineLayout&      get_pipeline_layout() const noexcept;
    /**
     * @brief 取管线本体，用于 bindPipeline、光追的 traceRays 与 SBT 组句柄查询。
     * @return 管线句柄的引用；create 之前是 nullptr。
     */
    [[nodiscard]] const vk::raii::Pipeline&            get_pipeline() const noexcept;

private:
    /** @brief 描述符集布局：binding 来自 create 的 _descriptor_set_layout_bindings。 */
    vk::raii::DescriptorSetLayout descriptor_set_layout = nullptr;
    /** @brief 管线布局：单个 set 加调用方给的 push constant 范围。 */
    vk::raii::PipelineLayout      pipeline_layout       = nullptr;
    /** @brief 管线本体：图形 / 光追 / 计算三者之一，由最后一次成功的 create 决定。 */
    vk::raii::Pipeline            pipeline              = nullptr;
};
