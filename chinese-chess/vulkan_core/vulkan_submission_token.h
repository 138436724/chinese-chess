/**
 * @file vulkan_submission_token.h
 * @brief 指向某条队列完成时间线某一点的轻量 token。
 * @details token 只由 vulkan_queue::submit() 在提交成功后发布，因此等待它不会等一个从未被要求 signal 的值。
 *          默认构造的 token 无效，表示“无依赖”，is_complete() 对无效 token 恒为 true。
 *          不同队列的 token 携带不同信号量，除提交里记录的等待之外没有先后关系。
 */
#pragma once

#include "vulkan_semaphore.h"

#include <cstdint>

// A point on one queue's completion timeline. Tokens are only produced by
// vulkan_queue::submit() after the submit call succeeded, so waiting on (or
// recycling against) a token can never observe a value the GPU was not asked
// to signal. Tokens from different queues carry different semaphores and have
// no ordering relationship beyond the waits recorded in their submissions.
/**
 * @brief 一次提交的完成凭据：信号量指针加时间线值，可平凡拷贝的轻量聚合体。
 * @details 不拥有信号量；信号量属于 vulkan_queue 的 submission_state，必须比 token 活得久。
 *          回收站、命令缓冲与帧调度器都用它判定资源是否可复用。
 */
struct vulkan_submission_token
{
    /** @brief 指向队列共享的时间线信号量；nullptr 表示无效 token。 */
    const vulkan_semaphore* semaphore = nullptr;
    /** @brief 要等待的时间线值；semaphore 非空时从 1 开始。 */
    uint64_t                value     = 0;

    /**
     * @brief 判断 token 是否关联了信号量。
     * @return 关联了信号量返回 true；默认构造的 token 返回 false。
     */
    [[nodiscard]] bool is_valid() const noexcept { return semaphore != nullptr; }

    /**
     * @brief 判断 token 对应的时间线值是否已完成。
     * @return 无效 token 返回 true，否则返回 GPU 计数是否已达到 value。
     * @throws vk::SystemError 当计数查询失败。
     * @note 只读查询，不阻塞。
     */
    [[nodiscard]] bool is_complete() const
    {
        return semaphore == nullptr || semaphore->get_gpu_value() >= value;
    }

    /**
     * @brief 阻塞等待 token 完成。
     * @note 无效 token 是 no-op；等待失败时抛 vk::SystemError。
     */
    void wait() const { if (semaphore != nullptr) semaphore->wait(value); }
    /**
     * @brief 析构与异常路径专用的等待，设备丢失不抛异常。
     * @note 无效 token 是 no-op；实现转发到 vulkan_semaphore::wait_for_cleanup。
     */
    void wait_for_cleanup() const noexcept { if (semaphore != nullptr) semaphore->wait_for_cleanup(value); }
};
