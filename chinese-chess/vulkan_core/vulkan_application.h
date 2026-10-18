/**
 * @file vulkan_application.h
 * @brief Vulkan 应用编排器：持有实例、设备、分配器、交换链与屏幕合成管线。
 * @details 生命周期固定为 init（建实例）→ create（建设备、队列、命令缓冲、交换链、管线）→
 *          逐帧 begin_frame → render 或 cancel_frame → 析构；成员声明顺序决定析构逆序，
 *          交换链与队列先于 device 释放，应用对象必须晚于 scene_manager / ui_manager 析构。
 *          屏幕合成在 render 里采样 scene 与 UI 两张离屏图，用 3 顶点全屏三角形写入交换链 image。
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
 * @brief 应用级 Vulkan 编排器，集中持有实例、设备、分配器、队列、交换链与屏幕合成管线。
 * @details 只承担 WSI 与最终合成；场景与 UI 的渲染器分别由 scene_manager、ui_manager 持有。
 *          init 只建实例，create 才建设备与交换链，两者都只调用一次、不可重入。
 *          每帧先由 begin_frame 让回收站释放已完成资源再 acquire，随后 render 提交并呈现。
 * @note 成员声明顺序决定析构逆序：交换链与队列先于 allocator、device 释放，context 最后释放，
 *       因此应用对象必须晚于 scene_manager / ui_manager 析构。
 */
class vulkan_application
{
public:
    /** @brief 构造空对象，全部 Vulkan 句柄为 nullptr，随后必须依次调用 init 与 create。 */
    vulkan_application() = default;
    /** @brief 先 wait_idle_for_cleanup 再清空描述符声明，之后成员按声明逆序销毁。 */
    ~vulkan_application();

    /** @brief 禁止拷贝构造：实例与设备句柄唯一。 */
    vulkan_application(const vulkan_application&)            = delete;
    /** @brief 禁止拷贝赋值。 */
    vulkan_application& operator=(const vulkan_application&) = delete;
    /** @brief 禁止移动构造：scene_manager / ui_manager 长期持有本对象引用，地址必须稳定。 */
    vulkan_application(vulkan_application&&)                 = delete;
    /** @brief 禁止移动赋值。 */
    vulkan_application& operator=(vulkan_application&&)      = delete;

    // public to use
    /**
     * @brief 创建 VkInstance，并按需追加 WSI 扩展、Debug 校验层与调试信使。
     * @param _instance_layers     调用方要求的实例层名称。
     * @param _instance_extensions 调用方要求的实例扩展。
     * @param _flags               传给 VkInstanceCreateInfo 的创建标志。
     * @throws std::runtime_error Vulkan loader 低于 1.4，或要求的层、扩展不可用时抛出。
     * @throws vk::SystemError 实例或调试信使创建失败时抛出。
     * @note 固定追加 get_surface_capabilities2 与 surface_maintenance1 两项扩展；
     *       只在 create 之前调用一次，建好的 instance 由 create 复用。
     */
    void init(const std::vector<const char*>& _instance_layers,
              const std::vector<const char*>& _instance_extensions,
              vk::InstanceCreateFlags         _flags = {});
    /**
     * @brief 建成套初始化：物理/逻辑设备、VMA 分配器、队列、命令缓冲、交换链、管线缓存与合成管线。
     * @param _surface 窗口系统提供的 VkSurfaceKHR，所有权转交内部 raii 句柄。
     * @param _width   初始交换链宽度（像素）。
     * @param _height  初始交换链高度（像素）。
     * @throws std::runtime_error 没有同时满足 Vulkan 1.4、光追与 swapchain_maintenance1 的设备，
     *         或找不到可用深度格式时抛出。
     * @throws vk::SystemError 逻辑设备、分配器或交换链创建失败时抛出。
     * @note 命令缓冲数量固定为 MAX_FRAMES_IN_FLIGHT，与帧调度器的 slot 数一致。
     */
    void create(vk::SurfaceKHR _surface, uint32_t _width, uint32_t _height);
    /**
     * @brief 以新尺寸重建交换链。
     * @param _width  新宽度（像素）。
     * @param _height 新高度（像素）。
     * @return 重建成功为 true；尺寸为 0 或创建返回 OutOfDate 时为 false。
     * @throws std::logic_error 仍有未呈现或未取消的帧时抛出。
     * @throws vk::SystemError surface 能力查询或交换链创建失败时抛出。
     * @note 只重建 WSI 资源；scene/UI 的离屏图由各自管理器按同一尺寸 recreate。
     */
    bool resize(uint32_t _width, uint32_t _height);
    /**
     * @brief 开始一帧：先让回收站释放已完成条目，再向交换链 acquire 一张 image。
     * @return 帧结果；ready 时 frame 携带 slot/image/generation，其余状态应跳过本帧。
     * @throws std::logic_error 上一帧尚未呈现或取消、帧调度器仍处于活动状态时抛出。
     * @note acquire 失败或 skip 不推进 frame slot；返回 ready 后必须配对 render 或 cancel_frame。
     */
    [[nodiscard]] vulkan_frame_result begin_frame();
    /**
     * @brief 记录并提交屏幕合成命令：采样 scene/UI 图，画 3 顶点全屏三角形，再转布局并呈现。
     * @param frame         begin_frame 返回的帧上下文，必须与当前 acquisition 完全一致。
     * @param _ui_token     UI 渲染提交的完成凭据，作为本帧命令缓冲的依赖。
     * @param _scene_token  场景渲染提交的完成凭据，作为本帧命令缓冲的依赖。
     * @return 呈现结果；recreate 表示需要重建交换链，fatal 表示呈现失败。
     * @throws std::logic_error 帧上下文属于另一次 acquisition 时抛出。
     * @throws std::runtime_error 命令缓冲未分配、无 owner 队列或队列已停止时抛出。
     * @note 内部完成 submit 与 swapchain.submitted；acquire 成功后必须走 render 或 cancel_frame 之一。
     */
    [[nodiscard]] vulkan_frame_result render(const vulkan_frame_context& frame, vulkan_submission_token _ui_token, vulkan_submission_token _scene_token);
    /**
     * @brief 放弃当前帧：归还已 acquire 的 image 并复位帧调度器，不提交任何命令。
     * @throws vk::SystemError 等待 fence 或 releaseSwapchainImagesKHR 失败时抛出。
     */
    void cancel_frame();
    /**
     * @brief 设备级 waitIdle；设备尚未创建时静默返回。
     * @throws vk::SystemError 设备丢失或等待失败时抛出。
     */
    void wait_idle() const;
    /**
     * @brief 析构路径专用的 waitIdle：设备丢失只打印，其他异常打印后 terminate。
     * @note 标 noexcept；不能当运行期同步使用，失败时进程直接终止而不是抛异常。
     */
    void wait_idle_for_cleanup() const noexcept;

    // frame
    /**
     * @brief 绑定待合成的 scene/UI 图像，并据此重建屏幕合成描述符集。
     * @param _scene_image 场景渲染输出，合成时被采样。
     * @param _ui_image    UI 渲染输出，与场景图在同一 pass 里混合。
     * @throws vk::SystemError 描述符池/描述符集分配或写入失败时抛出。
     * @note 只保存指针，不取得所有权；两张图必须活到下一次 bind_image 或本对象析构。
     */
    void bind_image(vulkan_image& _scene_image, vulkan_image& _ui_image);

    // getters
    /**
     * @brief 返回实例句柄。
     * @return 内部 vk::raii::Instance；init 之前为空句柄。
     */
    [[nodiscard]] const vk::raii::Instance&     get_instance() const noexcept;
    /**
     * @brief 返回 VMA 分配器。
     * @return 内部 vma::raii::Allocator；create 之前为空。
     */
    [[nodiscard]] const vma::raii::Allocator&   get_allocator() const noexcept;
    /**
     * @brief 返回物理设备包装，其中缓存了 graphics/compute/transfer 队列族下标。
     * @return 内部 vulkan_physical_device 引用。
     */
    [[nodiscard]] const vulkan_physical_device& get_physical_device() const noexcept;
    /**
     * @brief 返回逻辑设备包装。
     * @return 内部 vulkan_device 引用；create 之前其 operator bool 为 false。
     */
    [[nodiscard]] const vulkan_device&          get_device() const noexcept;
    /**
     * @brief 返回管线缓存。
     * @return 内部 vulkan_pipeline_cache 引用，场景层建管线时复用。
     */
    [[nodiscard]] const vulkan_pipeline_cache&  get_pipeline_cache() const noexcept;
    /**
     * @brief 返回只读交换链。
     * @return 内部 vulkan_swapchain 引用，用于查询格式、尺寸与 present 队列。
     */
    [[nodiscard]] const vulkan_swapchain&       get_swapchain() const noexcept;
    /**
     * @brief 返回可写交换链引用。
     * @return 内部 vulkan_swapchain 引用，供需要驱动 WSI 重建的上层使用。
     */
    [[nodiscard]] vulkan_swapchain&             get_swapchain() noexcept { return swapchain; }
    /**
     * @brief 返回图形队列。
     * @return 内部 vulkan_queue 引用；上传与场景提交复用它。
     */
    [[nodiscard]] vulkan_queue&                 get_graphic_queue() noexcept;
    /**
     * @brief 返回传输队列。
     * @return 内部 vulkan_queue 引用；与图形队列同族时共享提交状态。
     */
    [[nodiscard]] vulkan_queue&                 get_transfer_queue() noexcept;

private:
    /** @brief 从 color/depth 采样数交集里挑最高一档，写入 vulkan_common::MSAA_SAMPLE_COUNT。 */
    static void pick_msaa_sample_count(const vulkan_physical_device& _physical_device) noexcept;
    /** @brief 依次探测 D32/D32S8/D24S8，写入 vulkan_common::DEPTH_FORMAT；都不可用时抛 runtime_error。 */
    static void pick_depth_format(const vulkan_physical_device& _physical_device);

    // use in `init` and `create`
    /** @brief 建实例的内部实现：校验 loader、层与扩展，Debug 下建调试信使。 */
    void                   create_instance(const std::vector<const char*>& _instance_layers,
                                           const std::vector<const char*>& _instance_extensions,
                                           vk::InstanceCreateFlags         _flags = {});
    /** @brief 按扩展与特性谓词选物理设备并建逻辑设备，返回 present 队列族下标。 */
    [[nodiscard]] uint32_t create_physical_device_and_logical_device(vk::SurfaceKHR _surface);
    /** @brief 编译合成着色器（含 OCIO 注入）、上传 LUT 并建屏幕合成管线。 */
    void                   create_pipeline();

    /** @brief 生效的实例层；Debug 下可能被追加校验层。 */
    std::vector<const char*> required_instance_layers;
    /** @brief 生效的实例扩展；create_instance 会追加 WSI 两项与调试扩展。 */
    std::vector<const char*> required_instance_extensions;

    /** @brief vk::raii 的 dispatch context，必须是第一个构造的成员。 */
    vk::raii::Context                context;
    /** @brief VkInstance，init 之后有效。 */
    vk::raii::Instance               instance        = nullptr;
    /** @brief 仅 Debug 构建创建的校验消息信使。 */
    vk::raii::DebugUtilsMessengerEXT debug_messenger = nullptr;

    /** @brief 选中的物理设备与队列族下标。 */
    vulkan_physical_device physical_device;
    /** @brief 逻辑设备；在分配器、管线缓存、队列与交换链之后销毁，但先于 instance/context。 */
    vulkan_device          device;
    /** @brief VMA 分配器，启用 bufferDeviceAddress，API 版本 1.4。 */
    vma::raii::Allocator   allocator = nullptr;
    /** @brief 磁盘持久化的管线缓存，只在窗口析构时写盘。 */
    vulkan_pipeline_cache  pipeline_cache;

    /** @brief 图形/呈现队列，命令缓冲池建在它上面。 */
    vulkan_queue graphic_queue;
    /** @brief 传输队列；与图形队列同族时共享 submission_state。 */
    vulkan_queue transfer_queue;

    /** @brief 交换链与内部帧调度器，负责 acquire、present 与 present fence。 */
    vulkan_swapchain swapchain;

    /** @brief 屏幕合成图形管线（3 顶点全屏三角形，无顶点输入）。 */
    vulkan_pipeline   pipeline;
    /** @brief 屏幕合成描述符声明与每帧一个的描述符集。 */
    vulkan_descriptor descriptor;

    /** @brief 延迟回收站，begin_frame 时释放已完成的缓冲与图像。 */
    vulkan_recycle_bin                recycle_bin;
    /** @brief 每个 frame slot 一条屏幕合成主命令缓冲。 */
    std::vector<vulkan_commandbuffer> commandbuffers;

    /** @brief OCIO 的 uniform 缓冲，仅在 USE_OCIO 且 OCIO 声明了 uniform 时创建。 */
    vulkan_buffer               ocio_ubo;
    /** @brief OCIO LUT 图（1D/2D/3D），与 ocio_samplers 一一对应。 */
    std::vector<vulkan_image>   ocio_images;
    /** @brief OCIO LUT 采样器，按 LUT 插值方式选 nearest 或 linear。 */
    std::vector<vulkan_sampler> ocio_samplers;

    /** @brief 当前绑定的场景输出图，仅借用不拥有。 */
    vulkan_image*  bind_scene_image = nullptr;
    /** @brief 当前绑定的 UI 输出图，仅借用不拥有。 */
    vulkan_image*  bind_ui_image    = nullptr;
    /** @brief 屏幕合成采样器：clamp-to-edge 加 linear。 */
    vulkan_sampler image_sampler;
};
