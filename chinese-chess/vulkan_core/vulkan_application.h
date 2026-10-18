/**
 * @file vulkan_application.h
 * @brief vulkan 应用主体。持有实例、物理设备、逻辑设备、分配器、交换链与屏幕合成管线。
 */
#pragma once

#include "vulkan_buffer.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_descriptor.h"
#include "vulkan_device.h"
#include "vulkan_image.h"
#include "vulkan_physical_device.h"
#include "vulkan_pipeline.h"
#include "vulkan_pipeline_cache.h"
#include "vulkan_queue.h"
#include "vulkan_recycle_bin.h"
#include "vulkan_sampler.h"
#include "vulkan_semaphore.h"
#include "vulkan_swapchain.h"

#include <cstdint>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

/**
 * @brief vulkan 应用主体，集中持有实例、物理设备、逻辑设备、分配器、队列、交换链与屏幕合成管线等。
 * @details 只承担 WSI（Window System Integration） 与最终的UI与场景合成。
 *          init 初始化创建实例，create 创建设备与交换链，两者都只需调用一次、不可重入。
 *          每帧需要先调用 begin_frame 开始这一帧，随后调用 render 合成并呈现。
 * @note 析构函数按成员声明顺序逆序析构，因此该应用对象应晚于上层析构。
 */
class vulkan_application
{
public:
    vulkan_application() = default;  ///< @brief 默认构造函数，实际数据需要使用 init 和 create 创建。
    ~vulkan_application();           ///< @brief 先 wait_idle 再清空描述符声明，之后按成员变量顺序逆序析构。
    vulkan_application(const vulkan_application&)            = delete;  ///< @brief 禁止拷贝构造。
    vulkan_application& operator=(const vulkan_application&) = delete;  ///< @brief 禁止拷贝赋值。
    vulkan_application(vulkan_application&&)                 = delete;  ///< @brief 禁止移动构造。
    vulkan_application& operator=(vulkan_application&&)      = delete;  ///< @brief 禁止移动赋值。

    /**
     * @brief 创建 VkInstance，并按需追加 WSI 扩展、Debug 校验层与调试信使。
     * @param _instance_layers 要求使用的实例层名称。
     * @param _instance_extensions 要求使用的实例扩展名称。
     * @param _flags 传给 VkInstanceCreateInfo 的创建标志。
     * @throws std::runtime_error vulkan loader 低于 1.4，或要求的层、扩展不可用时抛出。
     * @throws vk::SystemError 实例或调试信使创建失败时由 vulkan 抛出。
     * @note 固定追加 vk::KHRGetSurfaceCapabilities2ExtensionName 和 vk::KHRSurfaceMaintenance1ExtensionName 两项扩展；
     *       只在 create 之前调用一次，建好的 instance 由 create 复用。
     */
    void init(const std::vector<const char*>& _instance_layers,
              const std::vector<const char*>& _instance_extensions,
              vk::InstanceCreateFlags         _flags = {});

    /**
     * @brief 创建和初始化物理设备、逻辑设备、VMA 分配器、队列、命令缓冲、交换链、管线缓存与合成管线。
     * @param _surface 窗口系统提供的 VkSurfaceKHR，所有权由内部 raii 句柄持有。
     * @param _width 初始交换链宽度（像素）。
     * @param _height 初始交换链高度（像素）。
     * @throws std::runtime_error 没有同时满足 Vulkan 1.4、光追与 swapchain_maintenance1 的设备或找不到可用深度格式时抛出。
     * @throws vk::SystemError 逻辑设备、分配器或交换链创建失败时由 vulkan 抛出。
     * @note 命令缓冲数量固定为 MAX_FRAMES_IN_FLIGHT，与帧调度器的 slot 数一致。
     */
    void create(vk::SurfaceKHR _surface, uint32_t _width, uint32_t _height);

    /**
     * @brief 以新尺寸重建交换链。
     * @param _width 新宽度（像素）。
     * @param _height 新高度（像素）。
     * @return 重建成功为 true；尺寸为 0 或创建返回 OutOfDate 时为 false。
     * @throws std::logic_error 仍有未呈现或未取消的帧时抛出。
     * @throws vk::SystemError surface 能力查询或交换链创建失败时由 vulkan 抛出。
     */
    bool resize(uint32_t _width, uint32_t _height);

    /**
     * @brief 开始新的一帧。回收站会在此时释放已完成条目，然后向交换链 acquire 一张 image。
     * @return 帧结果。ready 时 frame 携带 slot/image/generation，其余状态应跳过本帧。
     * @throws std::logic_error 上一帧尚未呈现或取消、帧调度器仍处于活动状态时抛出。
     * @note acquire 失败或 skip 不推进 frame slot；返回 ready 后必须配对使用 render 或 cancel_frame。
     */
    [[nodiscard]] vulkan_frame_result begin_frame();

    /**
     * @brief 记录并提交屏幕合成命令。采样 scene/UI 图，绘制 3 顶点的全屏三角形，转换布局并呈现。
     * @param _frame begin_frame 返回的帧上下文，必须与当前 acquisition 完全一致。
     * @param _ui_token UI 渲染提交的完成凭据，作为本帧命令缓冲的依赖。
     * @param _scene_token 场景渲染提交的完成凭据，作为本帧命令缓冲的依赖。
     * @return 呈现结果。recreate 表示需要重建交换链，fatal 表示呈现失败。
     * @throws std::logic_error 无活动帧，或上下文的 slot/image/generation 与当前帧不匹配时抛出。
     * @throws std::runtime_error 命令缓冲未分配、无 owner 队列或队列已停止时抛出。
     * @note 内部完成 submit 与 swapchain.submitted。acquire 成功后必须走 render 或 cancel_frame 之一。
     */
    [[nodiscard]] vulkan_frame_result render(const vulkan_frame_context& _frame,
                                             vulkan_submission_token     _ui_token,
                                             vulkan_submission_token     _scene_token);
    /**
     * @brief 放弃当前帧。归还已 acquire 的 image 并复位帧调度器，不提交任何命令。
     * @throws vk::SystemError 等待 fence 或 releaseSwapchainImagesKHR 失败时由 vulkan 抛出。
     */
    void cancel_frame();

    /**
     * @brief 设备级 waitIdle。设备尚未创建时静默返回。
     * @throws vk::SystemError 设备丢失或等待失败时由 vulkan 抛出。
     */
    void wait_idle() const;

    /**
     * @brief 绑定待合成的 scene 和 UI 图像，并据此重建屏幕合成描述符集。
     * @param _scene_image 场景渲染输出，与 UI 图混合后呈现。
     * @param _ui_image UI 渲染输出，与场景图混合后呈现。
     * @throws vk::SystemError 描述符池创建或描述符集分配失败后由 vulkan 抛出。
     * @pre 旧描述符集及其图像视图已不再被 GPU 使用；本函数直接销毁旧集，不执行等待。
     * @note 两个 image 仅保存指针，不获取所有权。
     */
    void bind_image(vulkan_image& _scene_image, vulkan_image& _ui_image);

    /**
     * @brief 返回实例句柄。
     * @return 内部 vk::raii::Instance 的常量引用。init 之前默认持有 nullptr。
     */
    [[nodiscard]] const vk::raii::Instance& get_instance() const noexcept;

    /**
     * @brief 返回 VMA 分配器。
     * @return 内部 vma::raii::Allocator 的常量引用。create 之前默认持有 nullptr。
     */
    [[nodiscard]] const vma::raii::Allocator& get_allocator() const noexcept;

    /**
     * @brief 返回物理设备封装，其中记录了 graphics/compute/transfer 等队列的队列族id。
     * @return 内部 vulkan_physical_device 的常量引用。create 之前默认持有 nullptr。
     */
    [[nodiscard]] const vulkan_physical_device& get_physical_device() const noexcept;

    /**
     * @brief 返回逻辑设备封装。
     * @return 内部 vulkan_device 的常量引用。create 之前默认持有 nullptr，operator bool 为 false。
     */
    [[nodiscard]] const vulkan_device& get_device() const noexcept;

    /**
     * @brief 返回管线缓存封装。
     * @return 内部 vulkan_pipeline_cache 的常量引用。重建管线时复用。
     */
    [[nodiscard]] const vulkan_pipeline_cache& get_pipeline_cache() const noexcept;

    /**
     * @brief 返回交换链封装。
     * @return 内部 vulkan_swapchain 的常量引用。用于查询交换链格式、尺寸与 present 队列。
     */
    [[nodiscard]] const vulkan_swapchain& get_swapchain() const noexcept;

    /**
     * @brief 返回图形队列。
     * @return 内部 vulkan_queue 的引用。
     */
    [[nodiscard]] vulkan_queue& get_graphic_queue() noexcept;

    /**
     * @brief 返回传输队列。
     * @return 内部 vulkan_queue 的引用。
     */
    [[nodiscard]] vulkan_queue& get_transfer_queue() noexcept;

private:
    /** @brief 从 color/depth 采样数交集里挑最高一档，写入 vulkan_common::MSAA_SAMPLE_COUNT。都不支持则回退为 1x。*/
    static void pick_msaa_sample_count(const vulkan_physical_device& _physical_device) noexcept;

    /** @brief 依次探测 D32/D32S8/D24S8，写入 vulkan_common::DEPTH_FORMAT。
     * @throws std::runtime_error 所有深度格式均不可用时抛出。
     */
    static void pick_depth_format(const vulkan_physical_device& _physical_device);

    // use in `init` and `create`
    /** @brief 创建实例的内部实现。校验 loader、层与扩展，Debug 下建立调试信使。 */
    void create_instance(const std::vector<const char*>& _instance_layers,
                         const std::vector<const char*>& _instance_extensions,
                         vk::InstanceCreateFlags         _flags = {});

    /** @brief 按扩展与特性谓词选取物理设备并创建逻辑设备，返回 present 队列族下标。 */
    [[nodiscard]] uint32_t create_physical_device_and_logical_device(vk::SurfaceKHR _surface);

    /** @brief 编译混合阶段的着色器（含 OCIO 注入）、上传 LUT 并建立屏幕合成管线。 */
    void create_pipeline();

private:
    std::vector<const char*> required_instance_layers;  ///< @brief 生效的实例层。Debug 下尝试追加校验层。
    std::vector<const char*> required_instance_extensions;  ///< @brief 生效的实例扩展。create_instance 会追加 WSI 两项扩展和调试扩展。

    vk::raii::Context                context;                    ///< @brief vk::raii 的 dispatch context。
    vk::raii::Instance               instance        = nullptr;  ///< @brief VkInstance 的 raii 对象，init 之后有效。
    vk::raii::DebugUtilsMessengerEXT debug_messenger = nullptr;  ///< @brief 仅 Debug 下构建创建的校验消息信使。

    vulkan_physical_device physical_device;      ///< @brief 选中的物理设备与队列族下标。
    vulkan_device          device;               ///< @brief 逻辑设备。
    vma::raii::Allocator   allocator = nullptr;  ///< @brief VMA 分配器，默认启用 bufferDeviceAddress，API 版本 1.4。
    vulkan_pipeline_cache  pipeline_cache;       ///< @brief 磁盘持久化的管线缓存，只在窗口析构时写盘。

    vulkan_queue graphic_queue;   ///< @brief 图形队列及其命令池。
    vulkan_queue transfer_queue;  ///< @brief 传输队列。与图形队列同族时共享 submission_state。

    vulkan_swapchain swapchain;  ///< @brief 交换链与内部帧调度器，负责 acquire、present 与 present fence。

    vulkan_pipeline   pipeline;    ///< @brief 屏幕合成图形管线，仅绘制一个全屏三角形。
    vulkan_descriptor descriptor;  ///< @brief 屏幕合成描述符声明与每帧一个的描述符集。

    vulkan_recycle_bin                recycle_bin;     ///< @brief 延迟回收站，begin_frame 时释放已完成的缓冲与图像。
    std::vector<vulkan_commandbuffer> commandbuffers;  ///< @brief 每个 frame slot 分配一条屏幕合成主命令缓冲。

    vulkan_buffer             ocio_ubo;  ///< @brief OCIO 的 uniform 缓冲，仅在 USE_OCIO 且 OCIO 声明了 uniform 时创建。
    std::vector<vulkan_image> ocio_images;      ///< @brief OCIO LUT 图（1D/2D/3D），与 ocio_samplers 一一对应。
    std::vector<vulkan_sampler> ocio_samplers;  ///< @brief OCIO LUT 采样器，按 LUT 插值方式选 nearest 或 linear。

    vulkan_image*  bind_scene_image = nullptr;  ///< @brief 当前绑定的场景输出图，仅借用不拥有。
    vulkan_image*  bind_ui_image    = nullptr;  ///< @brief 当前绑定的 UI 输出图，仅借用不拥有。
    vulkan_sampler image_sampler;               ///< @brief 屏幕合成采样器。选择 clamp to edge 加 linear。
};
