/**
 * @file scene_rayquery_render.h
 * @brief compute + Ray Query 路径的渲染器：一次 dispatch 完成路径追踪，结果写进 render_output。
 * @details 与 RT 管线共用 pbr / lighting / path_tracing 的积分器和采样规则，求交后端不同。
 *          描述符固定五个 binding：0 TLAS、1 输出 storage image、2 每帧 UBO、3 bindless 贴图数组、4 线性累积图。
 *          render() 把 UBO 写进 _frame_slot 槽后同步输出与累积图到 eGeneral，dispatch ceil(w/8) x ceil(h/8)
 *          个 workgroup，末尾保留输出图的计算读写屏障；后续合成 / 回读仍须按各自阶段另做转换。frame_index 每帧递增，update() 与
 *          reset_accumulation() 都把它清零。
 */

#pragma once

#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_data_type.h"
#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_pipeline.h"

#include <cstdint>
#include <vector>
#include <vulkan-memory-allocator-hpp/vk_mem_alloc_raii.hpp>
#include <vulkan/vulkan_raii.hpp>

class scene_camera;
class vulkan_recycle_bin;
class vulkan_queue;
class scene_model_manager;
class scene_material_manager;
class scene_light_manager;
class scene_texture_manager;

/**
 * @brief Ray Query 计算渲染器：持有计算管线、每帧 UBO、描述符池 / 集，输出写外部 render_output。
 * @details 只建一条计算管线（入口 rayQueryComputeMain，numthreads 8x8），没有 SBT 与顶点输入。
 *          render_output 与 accumulation 都是引用；resize 只记宽高，两张图由 scene_manager 统一管理。
 *          本类只录制帧命令，正常路径不等待 GPU；旧管线与描述符按令牌回收，回收失败清理可能等待。
 */
class scene_rayquery_render
{
public:
    /**
     * @brief 建每帧 UBO 与计算管线，保存各管理器与输出图的引用。
     * @param _allocator VMA 分配器，UBO 由它分配。
     * @param _physical_device 物理设备，查 minUniformBufferOffsetAlignment 算 UBO 槽跨度。
     * @param _device 逻辑设备，创建缓冲、描述符与管线。
     * @param _pipeline_cache 管线缓存，交给 create_from_shader。
     * @param _recycle_bin 延迟回收站，F5 换下的管线与重建的描述符在这里按令牌销毁。
     * @param _graphic_queue 图形队列，UBO 的 queue family 取自它，dispatch 录入该队列的帧命令缓冲。
     * @param _transfer_queue 传输队列引用，当前实现仅保存，未用于上传或提交。
     * @param _model_manager 模型管理器，提供 TLAS 与模型 SSBO 地址。
     * @param _material_manager 材质管理器，提供材质 SSBO 地址与材质数。
     * @param _light_manager 灯光管理器，提供灯光 SSBO 地址与灯光数。
     * @param _texture_manager 贴图管理器，update_descriptor 从这里取 bindless 采样器数组。
     * @param _render_output 渲染输出图（外部持有），以 storage image 形式绑定。
     * @param _accumulation 独立的 RGBA32F 线性均值图（外部持有）。
     * @throws std::runtime_error rayQueryComputeMain 编译失败。
     * @throws vk::SystemError UBO 或管线创建失败。
     * @note 全部引用都必须比本对象活得久；构造后 width / height 为 0、描述符尚未分配，须先 resize() 和 update() 再 render()。
     */
    scene_rayquery_render(const vma::raii::Allocator&     _allocator,
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
                          vulkan_image&                   _render_output,
                          vulkan_image&                   _accumulation);
    /**
     * @brief 默认析构：UBO、管线与描述符按成员声明逆序直接归还，不等待 GPU。
     * @note 调用方必须保证没有在飞帧仍在使用这些资源。
     */
    ~scene_rayquery_render() = default;

    /**
     * @brief 记录新的渲染尺寸；不创建任何图像（输出图由 scene_manager 持有）。
     * @param _width 新宽度，像素。
     * @param _height 新高度，像素。
     * @note render 总会录入 dispatch，组数为 ceil(width/8) x ceil(height/8)；任一组数为 0 时不启动着色器调用。
     */
    void resize(uint32_t _width, uint32_t _height);
    /**
     * @brief 清零累积帧号并重建描述符集。
     * @note 新描述符集意味着新的 TLAS / 输出图绑定，累积必须从第 0 帧重新开始。
     */
    void update();
    /**
     * @brief 写 UBO 槽、把输出与累积图转到 eGeneral、dispatch 计算并在末尾补一条 barrier。
     * @param _camera 当前相机，提供逆投影 / 逆视图矩阵与曝光。
     * @param _commandbuffer 已 begin_record、支持计算的图形队列命令缓冲；本函数只录制，不 end 也不 submit。
     * @param _environment 环境数据（bindless 槽 + 强度），写进 UBO。
     * @param _frame_slot 帧槽下标，决定 UBO 的哪个槽；必须小于 MAX_FRAMES_IN_FLIGHT。
     * @note 依赖 update() 建好的 descriptor_sets，且至少有 _frame_slot + 1 个。
     * @note 输出图作为 storage image 使用 eGeneral；前后屏障还负责访问同步，不一定发生布局改变。
     */
    void render(const scene_camera& _camera, const vk::raii::CommandBuffer& _commandbuffer, const vulkan_environment_data& _environment, uint32_t _frame_slot);
    /**
     * @brief 重建计算管线（F5 热重载）。
     * @throws std::runtime_error 着色器编译失败；旧管线尚未退休，仍可继续使用。
     * @throws vk::SystemError 管线创建失败；旧管线同样保持不变。
     * @note 先建新管线、成功后才把旧管线搬进回收站。
     */
    void recreate();
    /**
     * @brief 把样本序号清零；本函数不清图像，下一帧着色器用首样本覆盖累积图。
     */
    void reset_accumulation() noexcept;

private:
    /**
     * @brief 建计算管线：五个 binding（TLAS / 输出 / UBO / bindless 贴图 / 累积图）。
     * @return 新建的计算管线。
     * @throws std::runtime_error scene_rayquery_render.slang 编译失败。
     * @throws vk::SystemError 管线创建失败。
     */
    [[nodiscard]] vulkan_pipeline create_pipeline();
    /**
     * @brief 重建描述符池与 MAX_FRAMES_IN_FLIGHT 个描述符集。
     * @note 0 号写 TLAS，1 号写输出，2 号写 UBO，3 号写贴图数组，4 号写线性累积图。
     */
    void                          update_descriptor();

private:
    /** @brief VMA 分配器，每帧 UBO 由它分配。 */
    const vma::raii::Allocator&     allocator;
    /** @brief 物理设备，构造时读 minUniformBufferOffsetAlignment 算 UBO 槽跨度。 */
    const vk::raii::PhysicalDevice& physical_device;
    /** @brief 逻辑设备，创建缓冲、描述符与管线。 */
    const vk::raii::Device&         device;
    /** @brief 管线缓存，透传给 create_from_shader。 */
    const vk::raii::PipelineCache&  pipeline_cache;
    /** @brief 延迟回收站，旧管线与旧描述符按提交令牌销毁。 */
    vulkan_recycle_bin&             recycle_bin;
    /** @brief 图形队列，UBO 的 queue family 取自它；计算 dispatch 也录在图形队列的命令缓冲上。 */
    const vulkan_queue&             graphic_queue;
    /** @brief 传输队列引用，当前实现只保存，未用于提交。 */
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
    /** @brief 外部持有的 RGBA32F 线性累积图，不包含曝光。 */
    vulkan_image&                 accumulation;

    /** @brief 当前渲染宽度，resize 记录；0 表示尚未 resize。 */
    uint32_t width         = 0;
    /** @brief 当前渲染高度，resize 记录；0 表示尚未 resize。 */
    uint32_t height        = 0;
    /** @brief 64 位样本序号，render 先将当前值写入 UBO，再在录制末尾自增；update / reset_accumulation 清零。 */
    uint64_t frame_index   = 0;

    /** @brief 每帧 UBO 槽跨度 = sizeof(vulkan_rt_scene_data) 按 minUniformBufferOffsetAlignment 向上取整。 */
    vk::DeviceSize ubo_offset = 0;
    /** @brief host 可见的每帧 UBO，容量 MAX_FRAMES_IN_FLIGHT 个槽。 */
    vulkan_buffer  ubo;

    /** @brief Ray Query 计算管线，入口 rayQueryComputeMain。 */
    vulkan_pipeline                      pipeline;
    /** @brief 描述符池，容量为 MAX_FRAMES_IN_FLIGHT 个描述符集。 */
    vk::raii::DescriptorPool             descriptor_pool = nullptr;
    /** @brief 每帧一个描述符集，绑定 TLAS、输出图、UBO 窗口、贴图数组与独立累积图。 */
    std::vector<vk::raii::DescriptorSet> descriptor_sets;
};
