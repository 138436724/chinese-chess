/**
 * @file vulkan_frame_scheduler.h
 * @brief 帧状态机：唯一的 frame slot 游标、每槽 acquire semaphore/fence 与上次提交 token。
 * @details idle → acquired → submitted → finish 推进 slot；acquire 失败与 cancel 不推进游标。
 *          prepare 复用 slot 前先等该槽上次提交的 token，再等 acquire fence，避免覆盖仍在使用的信号量。
 *          swapchain 之外的场景/UI 只接收 frame_context，不维护各自的私有游标。
 */
#pragma once

#include "vulkan_submission_token.h"

#include <cstdint>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

/**
 * @brief 一帧 WSI 操作的结果分类。
 * @details ready 继续渲染；skip 本帧不渲染；recreate 需要重建交换链；fatal 视为不可恢复。
 */
enum class vulkan_frame_status
{
    ready,    ///< acquire 后可继续渲染；present 后表示无需重建，可开始后续帧。
    skip,     ///< 本帧跳过（TIMEOUT/NOT_READY），不推进 slot。
    recreate, ///< 需要重建交换链，包括 OUT_OF_DATE 及已处理本帧后的 SUBOPTIMAL。
    fatal     ///< surface/device lost 等不可恢复错误。
};

/**
 * @brief 一帧的身份：frame slot、交换链 image 下标与交换链 generation。
 * @details render 校验 slot/image/generation 是否与当前帧一致；generation 在每次成功 recreate 后自增。
 *          此三元组不是逐帧递增的序号，不应跨 acquisition 保存后再次使用。
 */
struct vulkan_frame_context
{
    /** @brief frame slot 下标，索引命令缓冲与描述符集。 */
    uint32_t slot = 0;
    /** @brief 本次 acquire 到的交换链 image 下标。 */
    uint32_t image = 0;
    /** @brief 交换链 generation，用于拒绝重建前取得的上下文。 */
    uint64_t generation = 0;
};

/**
 * @brief acquire 或 present 的返回物：状态分类、帧上下文与驱动原始结果。
 * @details status 决定上层动作，reason 保留原始 vk::Result 便于日志与错误分类。
 */
struct vulkan_frame_result
{
    /** @brief 本步结果分类，默认 skip。 */
    vulkan_frame_status status = vulkan_frame_status::skip;
    /** @brief acquire 成功时为活动帧；present 结果保留本次呈现帧的身份，其状态可能已结束。 */
    vulkan_frame_context frame;
    /** @brief 驱动返回的原始结果，成功时为 eSuccess。 */
    vk::Result reason = vk::Result::eSuccess;
};

// The only frame-slot cursor. Scene/UI/renderers receive its context and never
// advance private cursors. Acquisition failure and cancellation do not advance.
/**
 * @brief 唯一 frame slot 游标与逐槽同步对象的持有者。
 * @details 每个 slot 持有一个 acquire semaphore（供 vkAcquireNextImageKHR）、一个 acquire fence
 *          （证明信号操作已完成）以及该 slot 上次成功提交的 token。prepare 在复用 slot 前先等
 *          token 再等 fence，因此上次 acquire 的 semaphore 不会被提前覆盖。
 * @note acquire 失败与 cancel 不推进游标；只有 present 入队后 finish 才前进到下一个 slot。
 */
class vulkan_frame_scheduler
{
public:
    /**
     * @brief 为 count 个 frame slot 各建一个 acquire semaphore 与 acquire fence。
     * @param device 逻辑设备，用于创建信号量与 fence。
     * @param count  slot 数，通常取 MAX_FRAMES_IN_FLIGHT。
     * @throws std::logic_error count 为 0 或已经 create 过时抛出。
     * @note 只在初始化阶段调用一次；fence 初值为未信号状态。
     */
    void create(const vk::raii::Device& device, uint32_t count);
    /**
     * @brief 复用当前 slot 前等待它上次提交的 token，再等 acquire fence 并重置该 fence。
     * @param device 逻辑设备，用于 waitForFences 与 resetFences。
     * @throws std::logic_error 当前已有活动帧时抛出。
     * @note token 与 acquire fence 分别覆盖 GPU 提交与 WSI 信号操作，复用前必须确认两者完成。
     */
    void prepare(const vk::raii::Device& device);
    /**
     * @brief 记录本次 acquisition，进入 acquired 状态并返回帧身份。
     * @param image      acquire 到的交换链 image 下标。
     * @param generation 当前交换链 generation。
     * @return 记录下来的帧上下文。
     * @throws std::logic_error 当前已有活动帧时抛出。
     */
    vulkan_frame_context acquired(uint32_t image, uint64_t generation);
    /**
     * @brief 记录本帧提交的完成凭据并进入 submitted 状态。
     * @param completion 提交返回的 token，必须有效。
     * @throws std::logic_error 当前不在 acquired 状态或 token 无效时抛出。
     */
    void submitted(vulkan_submission_token completion);
    /**
     * @brief 推进游标到下一个 slot 并回到 idle。
     * @throws std::logic_error 当前不是 submitted 状态时抛出。
     * @note 只在 present 入队后调用；提前调用会让未呈现的 image 状态悬空。
     */
    void finish();
    /**
     * @brief 就地回到 idle，不推进 slot、不清 token。
     * @pre 调用方已等待在途工作、归还已取得的 image，并按需处理未消费的二进制信号量；本函数只改 CPU 状态。
     */
    void cancel() noexcept;
    /**
     * @brief 等所有 slot 的提交 token 与 pending acquire fence，不改变状态、不推进游标。
     * @param device 逻辑设备，用于等待 acquire fence。
     */
    void drain(const vk::raii::Device& device) const;
    /**
     * @brief 等当前 slot 的 acquire fence 后换一个新的 acquire semaphore。
     * @param device 逻辑设备，用于创建新信号量。
     * @pre 调用方已等待所有可能消费旧 semaphore 的 GPU 提交；本函数只额外等待 acquire 的信号操作。
     * @note 允许替换 acquire 已信号、但从未被提交消费的 semaphore。
     */
    void replace_acquire_semaphore(const vk::raii::Device& device);
    /**
     * @brief 查询是否有已 acquire 但尚未 finish 或 cancel 的帧。
     * @return 当前状态不是 idle 时返回 true。
     */
    [[nodiscard]] bool active() const noexcept { return phase != state::idle; }
    /**
     * @brief 查询是否已登记提交 token 但尚未 present。
     * @return 当前状态为 submitted 时返回 true。
     */
    [[nodiscard]] bool has_submission() const noexcept { return phase == state::submitted; }
    /**
     * @brief 查询当前 frame slot 下标。
     * @return 当前 slot。
     */
    [[nodiscard]] uint32_t slot() const noexcept { return current; }
    /**
     * @brief 取当前 slot 的 acquire semaphore。
     * @return 该 slot 的 VkSemaphore，传给 vkAcquireNextImageKHR 信号。
     * @throws std::out_of_range create 之前调用时由 slots.at 抛出。
     */
    [[nodiscard]] vk::Semaphore acquire_semaphore() const { return *slots.at(current).acquired; }
    /**
     * @brief 取当前 slot 的 acquire fence。
     * @return 该 slot 的 VkFence，证明 acquire 信号操作已完成。
     * @throws std::out_of_range create 之前调用时由 slots.at 抛出。
     */
    [[nodiscard]] vk::Fence acquire_fence() const { return *slots.at(current).acquire_done; }
    /**
     * @brief 返回当前帧身份。
     * @return acquired 时记录的 slot/image/generation。
     * @throws std::logic_error 当前没有活动帧时抛出。
     */
    [[nodiscard]] vulkan_frame_context context() const;

private:
    /**
     * @brief 帧状态机：idle 无活动帧、acquired 已 acquire 未提交、submitted 已提交未呈现。
     * @details finish 只在 submitted 下允许，cancel 可从任意状态回到 idle。
     */
    enum class state { idle, acquired, submitted };
    /**
     * @brief 一个 frame slot 的同步对象与上次提交凭据。
     * @details acquire_pending 表示成功 acquire 关联的 fence 需要等待；drain 等待后不清标志，prepare 才清除它。
     */
    struct frame_slot
    {
        /** @brief 供 vkAcquireNextImageKHR 信号、帧提交等待的 acquire semaphore。 */
        vk::raii::Semaphore acquired = nullptr;
        /** @brief 证明 acquire 信号操作已完成的 fence。 */
        vk::raii::Fence acquire_done = nullptr;
        /** @brief 该 slot 上次成功提交的 token，prepare 与 drain 等待它。 */
        vulkan_submission_token completion;
        /** @brief 最近一次成功 acquire 的 fence 需要被等待；不表示它此刻仍未信号。 */
        bool acquire_pending = false;
    };
    /** @brief 全部 frame slot，create 时一次性建满。 */
    std::vector<frame_slot> slots;
    /** @brief 当前 slot 下标。 */
    uint32_t current = 0;
    /** @brief 当前帧状态。 */
    state phase = state::idle;
    /** @brief acquired 时记录的帧身份，供 context() 返回。 */
    vulkan_frame_context frame;
};
