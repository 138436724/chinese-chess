/**
 * @file scene_raytracing_render.h
 * @brief 硬件光追参考实现：RT 管线 + 五组 SBT，traceRaysKHR 把结果写进 render_output。
 * @details 入口与组合固定：rayGenMain、rayMissMain、rayShadowMissMain、rayClosestHitMain、
 *          rayShadowAnyHitMain；0/1/2 组是 general（raygen + 两个 miss），3 组是 triangles
 *          closest-hit，4 组是只带 any-hit 的阴影组。描述符四个 binding 与 Ray Query 分支相同：
 *          0 TLAS、1 输出 storage image、2 每帧 UBO、3 bindless 贴图数组。render() 把 UBO 写进
 *          _frame_slot 槽后把图转到 eGeneral，再 traceRaysKHR(width, height, 1)。
 */

#pragma once

#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_data_type.h"
#include "vulkan_core/vulkan_pipeline.h"
#include "vulkan_core/vulkan_shader_binding_table.h"

#include <cstdint>
#include <utility>
#include <vector>
#include <vulkan-memory-allocator-hpp/vk_mem_alloc_raii.hpp>
#include <vulkan/vulkan_raii.hpp>

class scene_camera;
class vulkan_recycle_bin;
class vulkan_queue;
class vulkan_image;
class scene_model_manager;
class scene_material_manager;
class scene_light_manager;
class scene_texture_manager;

/**
 * @brief 光追渲染器：持有 RT 管线、SBT、每帧 UBO 与描述符池 / 集，输出写外部 render_output。
 * @details create_pipeline_and_sbt() 在同一处建管线与 SBT，并用一条一次性命令缓冲把 staging
 *          拷进 SBT 缓冲；该命令缓冲与 staging 按提交令牌退休，不需要调用方等待。
 *          SBT 与管线必须成对替换，F5 时两者都先建新的再退休旧的。
 *          render_output 只是引用；类不提交帧命令，也不等待 GPU。
 */
class scene_raytracing_render
{
public:
    /**
     * @brief 建每帧 UBO、RT 管线与配套 SBT，保存各管理器与输出图的引用。
     * @param _allocator VMA 分配器，UBO 与 SBT 缓冲都由它分配。
     * @param _physical_device 物理设备，查 UBO 对齐与光追管线属性。
     * @param _device 逻辑设备，创建缓冲、描述符、管线与 SBT。
     * @param _pipeline_cache 管线缓存，交给 create_from_shader。
     * @param _recycle_bin 延迟回收站，SBT 构建的 staging / 命令缓冲与 F5 换下的资源按令牌销毁。
     * @param _graphic_queue 图形队列，SBT 拷贝与记录它的 queue family 用。
     * @param _transfer_queue 传输队列，upload_buffer 的 staging 拷贝与队列族转移走它。
     * @param _model_manager 模型管理器，提供 TLAS 与模型 SSBO 地址。
     * @param _material_manager 材质管理器，提供材质 SSBO 地址与材质数。
     * @param _light_manager 灯光管理器，提供灯光 SSBO 地址与灯光数。
     * @param _texture_manager 贴图管理器，update_descriptor 从这里取 bindless 采样器数组。
     * @param _render_output 渲染输出图（外部持有），以 storage image 形式绑定。
     * @throws std::runtime_error 五个入口编译失败。
     * @throws vk::SystemError UBO、管线、命令缓冲或 SBT 创建失败。
     * @note 全部引用都必须比本对象活得久；构造后 width / height 为 0，要先 resize() 再 render()。
     */
    scene_raytracing_render(const vma::raii::Allocator&     _allocator,
                            const vk::raii::PhysicalDevice& _physical_device,
                            const vk::raii::Device&         _device,
                            const vk::raii::PipelineCache&  _pipeline_cache,
                            vulkan_recycle_bin&             _recycle_bin,
                            const vulkan_queue&             _graphic_queue,
                            const vulkan_queue&             _transfer_queue,
                            const scene_model_manager&      _model_manager,
                            const scene_material_manager&   _material_manager,
                            const scene_light_manager&      _light_manager,
                            const scene_texture_manager&    _texture_manager,
                            vulkan_image&                   _render_output);
    /**
     * @brief 默认析构：UBO、管线、SBT 与描述符按成员声明逆序直接归还，不等待 GPU。
     * @note 调用方必须保证没有在飞帧仍在使用这些资源。
     */
    ~scene_raytracing_render() = default;

    /**
     * @brief 记录新的渲染尺寸；不创建任何图像（输出图由 scene_manager 持有）。
     * @param _width 新宽度，像素，直接作为 traceRaysKHR 的宽度。
     * @param _height 新高度，像素，直接作为 traceRaysKHR 的高度。
     * @note 尺寸为 0 时仍会 traceRays，但不会有像素被写入。
     */
    void resize(uint32_t _width, uint32_t _height);
    /**
     * @brief 清零累积帧号并重建描述符集。
     * @note 新描述符集意味着新的 TLAS / 输出图绑定，累积必须从第 0 帧重新开始。
     */
    void update();
    /**
     * @brief 写 UBO 槽、把输出图转到 eGeneral、绑定管线与 SBT 后 traceRaysKHR，末尾补一条 barrier。
     * @param _camera 当前相机，提供逆投影 / 逆视图矩阵与曝光。
     * @param _commandbuffer 已 begin_record 的图形命令缓冲；本函数只录制，不 end 也不 submit。
     * @param _environment 环境数据（bindless 槽 + 强度），写进 UBO。
     * @param _frame_slot 帧槽下标，决定 UBO 的哪个槽；必须小于 MAX_FRAMES_IN_FLIGHT。
     * @note 依赖 update() 建好的 descriptor_sets，且至少有 _frame_slot + 1 个。
     * @note sbt 的四个 region 必须来自与当前 pipeline 配对的那次创建。
     */
    void render(const scene_camera& _camera, const vk::raii::CommandBuffer& _commandbuffer, const vulkan_environment_data& _environment, uint32_t _frame_slot);
    /**
     * @brief 重建 RT 管线与 SBT（F5 热重载）。
     * @throws std::runtime_error 着色器编译失败；旧管线与旧 SBT 尚未退休，仍可继续使用。
     * @throws vk::SystemError 管线、命令缓冲或 SBT 创建失败；旧对象同样保持不变。
     * @note 先把管线与 SBT 成对建好，成功后才把旧的一对搬进回收站。
     */
    void recreate();
    /**
     * @brief 把累积帧号清零，使下一帧从第 0 个样本重新累积。
     */
    void reset_accumulation() noexcept;

private:
    /**
     * @brief 建 RT 管线与配套 SBT，并用一次性命令缓冲完成 SBT 缓冲拷贝。
     * @return 新管线与 SBT 的 pair；两者必须一起替换，不能只换其一。
     * @throws std::runtime_error 五个入口编译失败。
     * @throws vk::SystemError 管线、命令缓冲或 SBT 缓冲创建失败。
     * @note 本函数内部提交 SBT 拷贝命令，把 staging 与命令缓冲按该提交的令牌退休；
     *       管线与 SBT 返回给调用方，调用方负责替换。
     */
    [[nodiscard]] std::pair<vulkan_pipeline, vulkan_shader_binding_table> create_pipeline_and_sbt();
    /**
     * @brief 重建描述符池与 MAX_FRAMES_IN_FLIGHT 个描述符集。
     * @note 0 号写 TLAS（StructureChain），1 号写输出 storage image，2 号写 UBO 窗口，3 号写贴图数组。
     */
    void                                                                  update_descriptor();

private:
    /** @brief VMA 分配器，每帧 UBO 与 SBT 缓冲由它分配。 */
    const vma::raii::Allocator&     allocator;
    /** @brief 物理设备，构造时读 minUniformBufferOffsetAlignment 并查询光追管线属性。 */
    const vk::raii::PhysicalDevice& physical_device;
    /** @brief 逻辑设备，创建缓冲、描述符、管线、SBT 与一次性命令缓冲。 */
    const vk::raii::Device&         device;
    /** @brief 管线缓存，透传给 create_from_shader。 */
    const vk::raii::PipelineCache&  pipeline_cache;
    /** @brief 延迟回收站，旧管线 / 旧 SBT 与 SBT 构建的临时资源按提交令牌销毁。 */
    vulkan_recycle_bin&             recycle_bin;
    /** @brief 图形队列，SBT 拷贝提交到它，命令池与 queue family 也取自它。 */
    const vulkan_queue&             graphic_queue;
    /** @brief 传输队列，upload_buffer 的 staging 拷贝走它。 */
    const vulkan_queue&             transfer_queue;

    /** @brief 模型管理器（只读引用），提供 TLAS 与模型 SSBO 地址。 */
    const scene_model_manager&    model_manager;
    /** @brief 材质管理器（只读引用），提供材质 SSBO 地址与材质数。 */
    const scene_material_manager& material_manager;
    /** @brief 灯光管理器（只读引用），提供灯光 SSBO 地址与灯光数。 */
    const scene_light_manager&    light_manager;
    /** @brief 贴图管理器（只读引用），提供 bindless 描述符数组。 */
    const scene_texture_manager&  texture_manager;
    /** @brief 外部持有的渲染输出图，以 storage image 形式绑定；本类不拥有它。 */
    vulkan_image&                 render_output;

    /** @brief 当前渲染宽度，resize 记录；0 表示尚未 resize。 */
    uint32_t width         = 0;
    /** @brief 当前渲染高度，resize 记录；0 表示尚未 resize。 */
    uint32_t height        = 0;
    /** @brief 累积帧号，每帧 render 后自增并写进 UBO；update / reset_accumulation 清零。 */
    uint32_t frame_index   = 0;

    /** @brief 每帧 UBO 槽跨度 = sizeof(vulkan_rt_scene_data) 按 minUniformBufferOffsetAlignment 向上取整。 */
    vk::DeviceSize ubo_offset = 0;
    /** @brief host 可见的每帧 UBO，容量 MAX_FRAMES_IN_FLIGHT 个槽。 */
    vulkan_buffer  ubo;

    /** @brief 光追管线，五个入口与五组着色器组固定。 */
    vulkan_pipeline                      pipeline;
    /** @brief 描述符池，容量为 MAX_FRAMES_IN_FLIGHT 个描述符集。 */
    vk::raii::DescriptorPool             descriptor_pool = nullptr;
    /** @brief 每帧一个描述符集，绑定 TLAS、输出图、UBO 窗口与贴图数组。 */
    std::vector<vk::raii::DescriptorSet> descriptor_sets;
    /** @brief 与 pipeline 配对的 SBT；四个 region 的设备地址随每次创建确定。 */
    vulkan_shader_binding_table          sbt;
};
