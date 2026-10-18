/**
 * @file vulkan_swapchain.h
 * @brief 交换链与呈现路径：按 image index 持有 view、render-finished semaphore 与 present fence。
 * @details 请求 MAX_FRAMES_IN_FLIGHT + 1 张 image 并 clamp 到 surface 上下限，graphics 与 present
 *          不同族时用 concurrent sharing。acquire/present 返回显式的 ready/skip/recreate/fatal，
 *          取消路径用 releaseSwapchainImagesKHR 归还 image 并替换已信号过的 semaphore。
 *          帧槽游标由内部 vulkan_frame_scheduler 独占，本类只把上下文转发给上层。
 */
#pragma once

#include "vulkan_frame_scheduler.h"
#include "vulkan_queue.h"

#include <cstdint>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

/**
 * @brief 交换链 RAII 包装：WSI 资源、帧调度器与呈现状态的唯一所有者。
 * @details create 建 surface 副本、选格式与呈现模式、接上 present 队列，再调 recreate 建第一版交换链；
 *          recreate 会先 drain，再用临时 vector 建好新 image 状态后整体替换。
 *          acquire_image → 记录并提交 → submitted(token) → present_image 是唯一合法帧序。
 * @note present fence 只证明呈现资源可回收，不证明像素已经显示，因此释放 image 资源仍要等 fence。
 */
class vulkan_swapchain
{
public:
    /** @brief 构造空对象；create 之前所有句柄为空、extent 为 0。 */
    vulkan_swapchain() = default;
    /** @brief 调 shutdown：取消未完成帧、排空帧调度器与 present fence，再按 view→semaphore→swapchain 销毁。 */
    ~vulkan_swapchain();
    /** @brief 禁止拷贝：交换链与信号量不可复制。 */
    vulkan_swapchain(const vulkan_swapchain&) = delete;
    /** @brief 禁止拷贝赋值。 */
    vulkan_swapchain& operator=(const vulkan_swapchain&) = delete;

    /**
     * @brief 建 surface 副本、选格式与呈现模式、建 present 队列与帧调度器，并创建第一版交换链。
     * @param instance      创建 surface 副本用的实例。
     * @param physical      物理设备，用于查询 surface 能力、格式与呈现模式。
     * @param device        逻辑设备，所有 WSI 句柄都由它创建。
     * @param surface       窗口系统给的 VkSurfaceKHR，所有权转交内部 raii 句柄。
     * @param graphics      图形队列，帧命令在它的族上提交。
     * @param transfer      传输队列，仅用于判断 present 队列是否可共享其提交状态。
     * @param present_index present 队列族下标。
     * @param width         初始宽度（像素）。
     * @param height        初始高度（像素）。
     * @throws std::runtime_error surface 不提供任何受支持格式时抛出。
     * @throws vk::SystemError surface 查询、队列或交换链创建失败时抛出。
     * @note present 队列与 graphics 或 transfer 同族时共享 submission_state（同一把提交锁与时间线）。
     */
    void create(const vk::raii::Instance& instance, const vk::raii::PhysicalDevice& physical,
                const vk::raii::Device& device, vk::SurfaceKHR surface,
                const vulkan_queue& graphics, const vulkan_queue& transfer,
                uint32_t present_index, uint32_t width, uint32_t height);
    /**
     * @brief 以新尺寸重建交换链及每个 image 的 view、render-finished semaphore 与 present fence。
     * @param width  新宽度（像素）。
     * @param height 新高度（像素）。
     * @return 尺寸为 0 或交换链创建返回 OutOfDate 时为 false，此时 images 被清空、等待下次重建。
     * @throws std::logic_error 仍有未完成的帧（frames.active()）时抛出。
     * @throws vk::SystemError surface 能力查询、drain 或交换链创建失败时抛出。
     * @note 先 drain 帧调度器与所有 present fence；image 数取 MAX_FRAMES_IN_FLIGHT + 1 并 clamp 到 surface 上下限。
     */
    bool recreate(uint32_t width, uint32_t height);
    /**
     * @brief 为当前 slot 准备好同步对象后 acquire 一张交换链 image。
     * @return ready 时 frame 携带 slot/image/generation；skip、recreate、fatal 时不应继续渲染。
     * @throws std::logic_error 上一帧尚未结束时抛出（帧调度器 prepare 拒绝复用活动 slot）。
     * @note acquire 失败或 skip 不推进 slot；成功返回 ready 后必须 present_image 或 abandon_frame。
     */
    [[nodiscard]] vulkan_frame_result acquire_image();
    /**
     * @brief 登记本帧成功提交的完成凭据，供该 slot 下次复用前等待。
     * @param token 提交返回的 token，必须有效。
     * @throws std::logic_error 当前不在 acquired 状态或 token 无效时抛出。
     */
    void submitted(vulkan_submission_token token);
    /**
     * @brief 用该 image 的 render-finished semaphore 与 present fence 呈现当前 image。
     * @return ready、recreate 或 fatal；recreate 时本帧仍算完成，下次 acquire 会触发重建。
     * @throws std::logic_error 尚未调用 submitted 时抛出。
     * @throws vk::SystemError present fence 重置或 present 调用失败时抛出。
     * @note 呈现入队成功（含 OutOfDate/SurfaceLost）才置 present_pending 并推进 slot；
     *       present fence 只表示 image 资源可回收。
     */
    [[nodiscard]] vulkan_frame_result present_image();
    /**
     * @brief 异常/取消路径：归还已 acquire 的 image、替换相关 semaphore 并复位帧调度器。
     * @throws vk::SystemError 等待 fence 或 releaseSwapchainImagesKHR 失败时抛出。
     */
    void abandon_frame();
    /** @brief 幂等关闭：cancel_acquisition(false) 加 drain；device lost 只打印，其他异常 terminate。 */
    void shutdown() noexcept;
    /**
     * @brief 开关 acquire/present/recreate 的 WSI 日志。
     * @param value true 时把每步的 generation/slot/image/result 打到 stdout 并 flush。
     */
    void set_trace(bool value) noexcept { trace = value; }

    /**
     * @brief 查询交换链颜色格式。
     * @return OCIO 开启时偏好 Unorm，否则偏好 sRGB；create 之前为 eUndefined。
     */
    [[nodiscard]] vk::Format get_format() const noexcept { return format; }
    /**
     * @brief 查询当前交换链像素尺寸。
     * @return 交换链 extent；未创建时为 {0, 0}。
     */
    [[nodiscard]] vk::Extent2D get_extent() const noexcept { return extent; }
    /**
     * @brief 查询当前交换链 image 数量。
     * @return images 的 size。
     */
    [[nodiscard]] uint32_t image_count() const noexcept { return static_cast<uint32_t>(images.size()); }
    /**
     * @brief 取内部交换链句柄。
     * @return vk::raii::SwapchainKHR 引用，供构造 PresentInfo 等只读用途。
     */
    [[nodiscard]] const vk::raii::SwapchainKHR& get_swapchain() const noexcept { return swapchain; }
    /**
     * @brief 取当前 acquisition 的 VkImage。
     * @return 当前 image 的 VkImage。
     * @throws std::logic_error 当前没有活动帧时抛出。
     */
    [[nodiscard]] vk::Image get_current_image() const;
    /**
     * @brief 取当前 acquisition 的 image view。
     * @return 当前 image 的 view 引用。
     * @throws std::logic_error 当前没有活动帧时抛出。
     */
    [[nodiscard]] const vk::raii::ImageView& get_current_imageview() const;
    /**
     * @brief 取 present 队列。
     * @return 内部 vulkan_queue 引用；可能与其他队列共享 submission_state。
     */
    [[nodiscard]] const vulkan_queue& get_present_queue() const noexcept { return present_queue; }
    /**
     * @brief 构造帧提交要等待的 acquire semaphore 信息。
     * @return 阶段为 color attachment output 的 SemaphoreSubmitInfo。
     * @throws std::out_of_range 帧调度器尚未 create 时抛出。
     */
    [[nodiscard]] vk::SemaphoreSubmitInfo get_waited_info() const;
    /**
     * @brief 构造帧提交要信号的 render-finished semaphore 信息。
     * @return 阶段为 ALL_COMMANDS 的 SemaphoreSubmitInfo，等待该 image 可再次呈现。
     * @throws std::logic_error 当前没有活动帧时抛出。
     */
    [[nodiscard]] vk::SemaphoreSubmitInfo get_signal_info() const;
    /**
     * @brief 取当前帧上下文。
     * @return 帧调度器记录的 slot/image/generation。
     * @throws std::logic_error 当前没有活动帧时抛出。
     */
    [[nodiscard]] vulkan_frame_context frame() const { return frames.context(); }
    /**
     * @brief 查询是否有已 acquire 但未 present 或取消的帧。
     * @return 帧调度器 active() 的结果。
     */
    [[nodiscard]] bool frame_active() const noexcept { return frames.active(); }
    /**
     * @brief 查询当前 frame slot 下标。
     * @return 当前 slot，索引命令缓冲与描述符集。
     */
    [[nodiscard]] uint32_t frame_slot() const noexcept { return frames.slot(); }

private:
    /**
     * @brief 单个交换链 image 的 WSI 资源与呈现状态。
     * @details 每个 image 独占一个 render-finished semaphore 与 present fence，因此不同 image 的呈现互不覆盖。
     */
    struct image_state
    {
        /** @brief 交换链分配的 VkImage，不拥有。 */
        vk::Image image;
        /** @brief 该 image 的 2D color view。 */
        vk::raii::ImageView view = nullptr;
        /** @brief 提交时信号、present 时等待的 render-finished semaphore。 */
        vk::raii::Semaphore render_finished = nullptr;
        /** @brief 随 present fence 传入，present 完成后由驱动信号。 */
        vk::raii::Fence present_done = nullptr;
        /** @brief 该 image 的 present 已入队但 fence 尚未确认。 */
        bool present_pending = false;
    };
    /** @brief 排空所有 slot 的提交与 acquire fence，再逐个等 present fence。 */
    void drain();
    /**
     * @brief 归还当前 acquisition：等提交与 acquire fence、release image，再按 reuse 重建信号量。
     * @param reuse true 时替换 acquire semaphore 与当前 image 的 render-finished semaphore。
     */
    void cancel_acquisition(bool reuse);
    /**
     * @brief trace 打开时打印一行 WSI 状态。
     * @param action 动作名（acquire/present/recreate/cancel 等）。
     * @param result 驱动返回的原始结果。
     * @param image  相关 image 下标；UINT32_MAX 表示改用当前帧上下文。
     */
    void log(const char* action, vk::Result result = vk::Result::eSuccess, uint32_t image = UINT32_MAX) const;

    /** @brief 逻辑设备，create 时记录，所有 WSI 对象都由它创建。 */
    const vk::raii::Device* device = nullptr;
    /** @brief 物理设备，仅用于 surface 能力、格式与呈现模式查询。 */
    const vk::raii::PhysicalDevice* physical = nullptr;
    /** @brief 图形队列指针，仅用于取队列族下标。 */
    const vulkan_queue* graphics = nullptr;
    /** @brief 选中的颜色格式。 */
    vk::Format format = vk::Format::eUndefined;
    /** @brief 选中的颜色空间。 */
    vk::ColorSpaceKHR color_space = vk::ColorSpaceKHR::eSrgbNonlinear;
    /** @brief 当前交换链尺寸。 */
    vk::Extent2D extent{0, 0};
    /** @brief 选中的呈现模式；surface 支持时用 eMailbox，否则 eFifo。 */
    vk::PresentModeKHR present_mode = vk::PresentModeKHR::eFifo;
    /** @brief surface 的 raii 副本，持有到对象销毁。 */
    vk::raii::SurfaceKHR surface = nullptr;
    /** @brief 当前交换链；重建失败后会短暂为空。 */
    vk::raii::SwapchainKHR swapchain = nullptr;
    /** @brief present 队列包装，可能与其他队列共享提交状态。 */
    vulkan_queue present_queue;
    /** @brief 唯一的帧槽游标与逐槽同步对象。 */
    vulkan_frame_scheduler frames;
    /** @brief 每个交换链 image 对应的 view 与呈现资源。 */
    std::vector<image_state> images;
    /** @brief 交换链 generation，每次 recreate 成功自增，用于拒绝过期帧上下文。 */
    uint64_t generation = 0;
    /** @brief 当前是否有已 acquire 尚未归还的 image。 */
    bool image_acquired = false;
    /** @brief 本次 acquire 返回 SUBOPTIMAL，呈现后需要重建。 */
    bool recreate_after_present = false;
    /** @brief 是否打印 WSI 日志。 */
    bool trace = false;
};
