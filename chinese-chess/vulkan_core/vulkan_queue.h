/**
 * @file vulkan_queue.h
 * @brief 单个 VkQueue 的 RAII 封装：队列、命令池，以及按实际 VkQueue 共享的提交状态。
 * @details 同一 VkQueue 的多个 vulkan_queue（别名队列）共享一份 submission_state：一条 timeline 信号量、
 *          一把提交互斥、一个 latest 前沿，所以 submit / present 的串行化是跨对象生效的。
 *          提交成功后才发布 vulkan_submission_token；失败会把共享状态置 stopped 并拒绝后续提交。
 *          present 直接走 dispatcher 返回原始 vk::Result，由 swapchain 分类成 ready / recreate / fatal。
 */

#pragma once

#include "vulkan_semaphore.h"
#include "vulkan_submission_token.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vulkan/vulkan_raii.hpp>

class vulkan_queue
{
public:
    // Shared by every vulkan_queue that maps to the same VkQueue. Aliased
    // queue objects therefore share one completion timeline, one submission
    // sequence and the same submission mutex.
    /**
     * @brief 同一实际 VkQueue 共享的提交状态。
     * @details semaphore 是完成时间线，latest 是已成功提交的最大 value；mutex 同时保护 submit 与 present
     *          对 VkQueue 的访问。stopped 在任一提交失败后置位，该状态上的提交与呈现随即被拒绝；
     *          queue_index 用于校验别名只能来自同一 queue family。
     */
    struct submission_state
    {
        /** @brief 该队列的完成时间线；value 由 submit 在锁内 reserve，只有提交成功才对外发布。 */
        vulkan_semaphore      semaphore;
        /** @brief 提交互斥：submit 与 present 共用，保证同一 VkQueue 上的调用串行。 */
        std::mutex            mutex;
        /** @brief 已成功提交的最大时间线 value，供 get_latest_token() 取「已提交前沿」。 */
        std::atomic<uint64_t> latest      = 0;
        /** @brief 单向失败标志：置位后该共享状态拒绝新的提交与呈现。 */
        std::atomic<bool>     stopped     = false;
        /** @brief 创建时记录的 queue family index，别名队列必须与它相同。 */
        uint32_t              queue_index = vk::QueueFamilyIgnored;
    };

    /** @brief 空对象：family index 为 Ignored、队列 / 命令池 / 共享状态均为空。 */
    vulkan_queue()                               = default;
    /** @brief 析构释放命令池及其包装；VkQueue 本身不单独销毁，共享状态按 shared_ptr 引用计数释放。 */
    ~vulkan_queue()                              = default;
    /** @brief 禁用拷贝：此包装独占命令池，VkQueue 可以有别名，但别名必须共享 submission_state。 */
    vulkan_queue(const vulkan_queue&)            = delete;
    /** @brief 禁用拷贝赋值：同上。 */
    vulkan_queue& operator=(const vulkan_queue&) = delete;
    /** @brief 移动构造：搬走句柄与共享状态；源对象句柄为空，但 index 被置为 0，不是 QueueFamilyIgnored。 */
    vulkan_queue(vulkan_queue&& _other) noexcept;
    /** @brief 移动赋值：逐成员 std::ranges::swap，旧队列与命令池被换给源对象并随其析构释放。 */
    vulkan_queue& operator=(vulkan_queue&& _other) noexcept;

    /**
     * @brief 建独立队列：自己造一份 submission_state（含 timeline 信号量）与命令池。
     * @param _device 逻辑设备。
     * @param _index queue family index；队列固定取该 family 的第 0 条。
     * @throws vk::SystemError 信号量、队列或命令池创建失败。
     * @note 命令池带 eResetCommandBuffer 标志，使命令缓冲可以单独 reset 后重录。
     * @note 本重载不查找已有包装；同一 device/family 的第 0 条队列已有包装时，必须用共享状态重载创建别名。
     */
    void create(const vk::raii::Device& _device, uint32_t _index);
    /**
     * @brief 建别名队列：共用已有的 submission_state（同一 timeline / 互斥 / latest）。
     * @param _device 逻辑设备。
     * @param _index queue family index，必须与 _state->queue_index 相同。
     * @param _state 被共享的提交状态，不能为空，且须来自同一逻辑设备的同一实际队列；实现只检查 family 下标。
     * @throws std::runtime_error _state 为空，或它的 queue_index 与 _index 不一致。
     * @note 空状态或 family 不匹配的校验失败不会改成员；校验之后的 Vulkan 创建失败不保证恢复旧对象。
     */
    void create(const vk::raii::Device& _device, uint32_t _index, std::shared_ptr<submission_state> _state);

    // Serialized per actual VkQueue. Allocates the signal value, merges
    // timeline dependencies and binary waits, submits, then publishes the
    // token. A failed submit publishes nothing.
    /**
     * @brief 提交一条命令缓冲并返回完成凭据。
     * @param _commandbuffer 待提交的唯一一条命令缓冲。
     * @param _binary_waits 二进制信号量等待（如 acquire 的 image-available）。
     * @param _binary_signals 二进制信号量信号（如 render-finished）。
     * @param _dependencies 时间线依赖；同一条 timeline 的多个依赖合并成一次等待（取最大 value）。
     * @return 本次提交的 token（时间线指针 + value），只有提交成功才返回。
     * @throws std::runtime_error 队列未初始化，或共享状态已被置 stopped（提交事务失败后）。
     * @throws vk::SystemError vkQueueSubmit2 失败；此时共享状态被置 stopped，不发布 token。
     * @note 全程持 submission->mutex，因此时间线 value 的分配与提交是原子的；整次提交以 eAllCommands
     *       阶段给出一条时间线信号。reserve 后失败会消耗该 value，但不发布 token，也不能据此推断失败提交的 GPU 状态。
     */
    [[nodiscard]] vulkan_submission_token submit(const vk::CommandBufferSubmitInfo&       _commandbuffer,
                                          std::span<const vk::SemaphoreSubmitInfo> _binary_waits,
                                          std::span<const vk::SemaphoreSubmitInfo> _binary_signals,
                                          std::span<const vulkan_submission_token>        _dependencies) const;

    /**
     * @brief 取 queue family index（即 create 传入的 _index）。
     * @return family index；默认构造时为 vk::QueueFamilyIgnored，移动构造后的源对象为 0，不可用本值单独判定是否初始化。
     */
    [[nodiscard]] uint32_t                                   get_index() const noexcept;
    /**
     * @brief 取底层队列句柄。
     * @return vk::raii::Queue 的引用；别名队列各自的包装都指向同一 family 的第 0 条队列。
     */
    [[nodiscard]] const vk::raii::Queue&                     get_queue() const noexcept;
    /**
     * @brief 取该队列专用的命令池（带 eResetCommandBuffer）。
     * @return 池句柄的引用；未初始化时是空句柄。
     */
    [[nodiscard]] const vk::raii::CommandPool&               get_command_pool() const noexcept;
    /**
     * @brief 取共享提交状态。
     * @return shared_ptr 副本；可用来核对两个队列对象是否指向同一 VkQueue，并延长状态寿命。
     */
    [[nodiscard]] std::shared_ptr<submission_state>          get_submission_state() const noexcept;
    /**
     * @brief 取该队列最近一次成功提交的 token。
     * @return token；未初始化时为空 token，从未提交时 value 为 0（等价于已立即完成）。
     */
    [[nodiscard]] vulkan_submission_token                           get_latest_token() const noexcept;
    /** @brief 置 stopped，禁止该共享状态上的后续提交与呈现；noexcept，供异常展开路径使用。 */
    void stop_submissions() const noexcept;
    /**
     * @brief 呈现：与 submit 共用同一把提交锁，并直接调 vkQueuePresentKHR 保留原始结果。
     * @param info 呈现信息（等待的 render-finished 信号量、swapchain、image index）。
     * @return 原始 vk::Result，由 swapchain 分成 eSuccess / eSuboptimalKHR / eErrorOutOfDateKHR / fatal。
     * @throws std::runtime_error 共享状态已被置 stopped。
     * @pre 已成功 create；本函数不像 submit 那样检查空 submission。
     * @note 不走 vk::raii::Queue 的成员函数，而是在持锁状态下直接调 vkQueuePresentKHR ⇒ 结果原样返回、不做分类；
     *       返回 eSuboptimalKHR 时呈现操作仍已入队，不能丢弃其 semaphore/fence 生命周期。
     */
    [[nodiscard]] vk::Result present(const vk::PresentInfoKHR& info) const;

private:
    /** @brief queue family index，同时作为 vk::raii::Queue 的 family 参数。 */
    uint32_t                           index        = vk::QueueFamilyIgnored;
    /** @brief 底层队列句柄；未初始化时为空。 */
    vk::raii::Queue                    queue        = nullptr;
    /** @brief 专用命令池；未初始化时为空。 */
    vk::raii::CommandPool              command_pool = nullptr;
    /** @brief 共享提交状态：别名队列指向同一对象，独立队列各有一份。 */
    std::shared_ptr<submission_state>  submission;
};
