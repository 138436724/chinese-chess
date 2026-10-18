/**
 * @file scene_rasterization_render.h
 * @brief 光栅化前向 PBR 渲染器：MSAA 颜色 + 深度、全屏三角形天空盒与间接绘制几何。
 * @details render() 先把 render_output 转到颜色附件，再用一次动态渲染以 MSAA 颜色图为附件、
 *          resolve 到 render_output：先画 3 顶点全屏三角形（environment 管线，深度测试关闭），
 *          再以 eBack 剔除与 eLess 深度画 scene_model_manager 的 drawIndexedIndirect 命令。
 *          与两条光追路径共用 BRDF 与光源采样，但没有可见性项，因此没有阴影与 AO。
 *          描述符只在 update() 重建，render() 只写每帧 UBO 槽并录制命令。
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
 * @brief 光栅化渲染器的资源与命令录制者，结果写到外部持有的 render_output。
 * @details 自己持有 MSAA 颜色图、深度图、两条图形管线、每帧 UBO 与描述符池 / 集；
 *          render_output 只是引用（scene_manager 持有并在 resize 时换图）。构造不建附件，
 *          width / height 为 0，必须先 resize() 才能 render()。类不提交命令也不等待 GPU：
 *          管线、图像与描述符的旧副本都进回收站，按提交令牌销毁。
 */
class scene_rasterization_render
{
public:
    /**
     * @brief 建每帧 UBO 与两条图形管线，保存各管理器与输出图的引用。
     * @param _allocator VMA 分配器，UBO、MSAA 颜色图与深度图都由它分配。
     * @param _physical_device 物理设备，查最小 UBO 对齐并创建管线。
     * @param _device 逻辑设备，创建缓冲、图像、描述符与管线。
     * @param _pipeline_cache 管线缓存，交给 create_from_shader。
     * @param _recycle_bin 延迟回收站，resize 换下的图像与 F5 换下的管线在这里按令牌销毁。
     * @param _graphic_queue 图形队列，记录它的 queue family，并把一次性上传提交到它。
     * @param _transfer_queue 传输队列，upload_buffer 的 staging 拷贝与队列族转移走它。
     * @param _model_manager 模型管理器，提供顶点 / 索引 / 间接命令缓冲与模型数。
     * @param _material_manager 材质管理器，提供材质 SSBO 地址与材质数。
     * @param _light_manager 灯光管理器，提供灯光 SSBO 地址与灯光数。
     * @param _texture_manager 贴图管理器，update_descriptor 从这里取 bindless 采样器数组。
     * @param _render_output 渲染输出图（外部持有），render 以它为 resolve 目标。
     * @param _color_format 输出图格式，管线颜色附件使用；resize 建 MSAA 颜色图时实际取
     *        render_output.get_format()，当前两者一致。
     * @throws std::runtime_error 两支入口着色器编译失败。
     * @throws vk::SystemError UBO 或管线创建失败。
     * @note 全部引用都必须比本对象活得久；构造后 width / height 仍为 0，要等 resize() 建附件。
     */
    scene_rasterization_render(const vma::raii::Allocator&     _allocator,
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
                               vk::Format                      _color_format);
    /**
     * @brief 默认析构：图像、缓冲与管线按成员声明逆序直接归还，不等待 GPU。
     * @note 调用方必须保证没有在飞帧仍在使用这些资源。
     */
    ~scene_rasterization_render() = default;

    /**
     * @brief 按新尺寸重建 MSAA 颜色图与深度图，旧图进回收站。
     * @param _width 新宽度，像素。
     * @param _height 新高度，像素。
     * @note 宽高不变时直接返回；只建附件，不重建管线与描述符（描述符由 update() 负责）。
     */
    void resize(uint32_t _width, uint32_t _height);
    /**
     * @brief 重建描述符池与每帧描述符集。
     * @note 只写描述符，不重传场景数据；调用方须保证四个 manager 已经 update 完。
     */
    void update();
    /**
     * @brief 录制一帧光栅化渲染：写 UBO 槽、转布局、开动态渲染、画天空盒与几何。
     * @param _camera 当前相机，提供投影 / 视图矩阵、位置与曝光。
     * @param _commandbuffer 已 begin_record 的图形命令缓冲；本函数只录制，不 end 也不 submit。
     * @param _environment 环境数据（bindless 槽 + 强度），写进 UBO。
     * @param _frame_slot 帧槽下标，决定 UBO 的哪个槽；必须小于 MAX_FRAMES_IN_FLIGHT。
     * @note 依赖 update() 建好的 descriptor_sets，且至少有 _frame_slot + 1 个。
     * @note 只有模型数大于 0 时才录 drawIndexedIndirect；命令须已由 model_manager.update() 生成。
     * @note 结束后 render_output 停在 eColorAttachmentOptimal，后续合成会再转布局。
     */
    void render(const scene_camera& _camera, const vk::raii::CommandBuffer& _commandbuffer, const vulkan_environment_data& _environment, uint32_t _frame_slot);
    /**
     * @brief 重建两条图形管线（F5 热重载）。
     * @throws std::runtime_error 着色器编译失败；此时旧管线尚未退休，仍可继续使用。
     * @throws vk::SystemError 管线创建失败；旧管线同样保持不变。
     * @note 两条新管线均创建成功后才开始登记旧资源回收；回收登记阶段的分配失败不保证整组替换原子性。
     */
    void recreate();
    /**
     * @brief 空实现：光栅化没有累积缓冲，切换回本渲染器时无需清零。
     */
    void reset_accumulation() noexcept;

private:
    /**
     * @brief 建天空盒管线：全屏三角形、无顶点输入、关闭背面剔除与深度测试。
     * @param _color_format 动态渲染的颜色附件格式。
     * @return 新建的图形管线。
     * @throws std::runtime_error scene_rasterization_render.slang 编译失败。
     * @throws vk::SystemError 管线创建失败。
     */
    [[nodiscard]] vulkan_pipeline create_environment_pipeline(vk::Format _color_format);
    /**
     * @brief 建几何管线：顶点输入为 vulkan_model_vertex，eBack 剔除并开启深度测试与写入。
     * @param _color_format 动态渲染的颜色附件格式。
     * @return 新建的图形管线。
     * @throws std::runtime_error scene_rasterization_render.slang 编译失败。
     * @throws vk::SystemError 管线创建失败。
     */
    [[nodiscard]] vulkan_pipeline create_pipeline(vk::Format _color_format);
    /**
     * @brief 重建描述符池与 MAX_FRAMES_IN_FLIGHT 个描述符集。
     * @note binding 0 是每帧 UBO 窗口，binding 1 是 MAX_BINDLESS_TEXTURES 个采样贴图。
     * @note 旧池与旧集先搬进回收站，随后立即重新分配；中途抛异常时成员停在空状态。
     */
    void                          update_descriptor();

private:
    /** @brief VMA 分配器，UBO 与两张附件图都由它分配。 */
    const vma::raii::Allocator&     allocator;
    /** @brief 物理设备，构造时读 minUniformBufferOffsetAlignment 算 UBO 槽跨度。 */
    const vk::raii::PhysicalDevice& physical_device;
    /** @brief 逻辑设备，创建缓冲、图像、描述符与管线。 */
    const vk::raii::Device&         device;
    /** @brief 管线缓存，透传给 create_from_shader。 */
    const vk::raii::PipelineCache&  pipeline_cache;
    /** @brief 延迟回收站，旧图像、旧管线与旧描述符按提交令牌销毁。 */
    vulkan_recycle_bin&             recycle_bin;
    /** @brief 图形队列，附件图与缓冲的 queue family 取自它。 */
    const vulkan_queue&             graphic_queue;
    /** @brief 传输队列，upload_buffer 的 staging 拷贝走它。 */
    const vulkan_queue&             transfer_queue;

    /** @brief 模型管理器（只读引用），提供顶点 / 索引 / 间接命令缓冲与模型数。 */
    const scene_model_manager&    model_manager;
    /** @brief 材质管理器（只读引用），提供材质 SSBO 地址与材质数。 */
    const scene_material_manager& material_manager;
    /** @brief 灯光管理器（只读引用），提供灯光 SSBO 地址与灯光数。 */
    const scene_light_manager&    light_manager;
    /** @brief 贴图管理器（只读引用），提供 bindless 描述符数组。 */
    const scene_texture_manager&  texture_manager;
    /** @brief 外部持有的渲染输出图，作为 resolve 目标；本类不拥有它。 */
    vulkan_image&                 render_output;

    /** @brief 当前渲染宽度，resize 后与附件图一致；0 表示尚未 resize。 */
    uint32_t         width         = 0;
    /** @brief 当前渲染高度，resize 后与附件图一致；0 表示尚未 resize。 */
    uint32_t         height        = 0;
    /** @brief 构造时给定的输出格式，两条图形管线都按它建。 */
    const vk::Format color_format  = vk::Format::eUndefined;

    /** @brief 每帧 UBO 槽跨度 = sizeof(vulkan_raster_scene_data) 按 minUniformBufferOffsetAlignment 向上取整。 */
    vk::DeviceSize ubo_offset = 0;
    /** @brief host 可见的每帧 UBO，容量 MAX_FRAMES_IN_FLIGHT 个槽，render 里写入后 flush。 */
    vulkan_buffer  ubo;

    /** @brief MSAA 颜色附件图，resize 重建，采样数 MSAA_SAMPLE_COUNT。 */
    vulkan_image                         color_image;
    /** @brief 深度附件图，resize 重建，格式 DEPTH_FORMAT。 */
    vulkan_image                         depth_image;
    /** @brief 天空盒图形管线：全屏三角形，无顶点输入，深度测试关闭。 */
    vulkan_pipeline                      environment_pipeline;
    /** @brief 几何图形管线：vulkan_model_vertex 顶点输入，eBack 剔除并写深度。 */
    vulkan_pipeline                      pipeline;
    /** @brief 描述符池，容量为 MAX_FRAMES_IN_FLIGHT 个描述符集。 */
    vk::raii::DescriptorPool             descriptor_pool = nullptr;
    /** @brief 每帧一个描述符集，绑定 UBO 窗口与 bindless 贴图数组。 */
    std::vector<vk::raii::DescriptorSet> descriptor_sets;
};
