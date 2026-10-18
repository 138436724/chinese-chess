/**
 * @file vulkan_queue.cpp
 * @brief vulkan_queue 的实现：共享提交状态、锁内提交、present 与已提交前沿查询。
 * @details submit 在 submission->mutex 内 reserve 时间线 value、合并依赖与二进制等待、调 vkQueueSubmit2，
 *          成功才写 latest 并返回 token；任何异常都先 stop_submissions() 再重抛。
 *          present 复用同一把锁并直接调 vkQueuePresentKHR，保留原始 vk::Result 供调用方分类。
 *          stopped 是单向的：一旦置位，该共享状态上的提交与呈现都抛 std::runtime_error。
 */

#include "vulkan_queue.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

/** @brief 移动构造：index、队列、命令池与共享状态一律 std::exchange 搬走，源对象被置空。 */
vulkan_queue::vulkan_queue(vulkan_queue&& _other) noexcept
    : index(std::exchange(_other.index, {}))
    , queue(std::exchange(_other.queue, nullptr))
    , command_pool(std::exchange(_other.command_pool, nullptr))
    , submission(std::exchange(_other.submission, nullptr))
{
}

/** @brief 移动赋值：逐成员 std::ranges::swap，旧队列与命令池被换给源对象并随其析构释放。 */
vulkan_queue& vulkan_queue::operator=(vulkan_queue&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(index, _other.index);
        std::ranges::swap(queue, _other.queue);
        std::ranges::swap(command_pool, _other.command_pool);
        std::ranges::swap(submission, _other.submission);
    }
    return *this;
}

/**
 * @brief 建独立队列：新建 submission_state、记下 family index、建 timeline 信号量，再转调共享版本。
 * @note 信号量先于队列与命令池创建；任一构造失败时 shared_ptr 随异常展开释放，不会泄漏已建对象。
 */
void vulkan_queue::create(const vk::raii::Device& _device, uint32_t _index)
{
    auto state         = std::make_shared<submission_state>();
    state->queue_index = _index;
    state->semaphore.create(_device);
    create(_device, _index, std::move(state));
}

/**
 * @brief 校验共享状态后建队列与命令池，并把 _state 存进成员。
 * @note 空状态与 family 不一致都抛 std::runtime_error，且校验在改造成员之前 ⇒ 失败时对象仍是原样。
 *       队列固定取该 family 的第 0 条，因此别名队列最终指向同一个 VkQueue。
 */
void vulkan_queue::create(const vk::raii::Device& _device, uint32_t _index, std::shared_ptr<submission_state> _state)
{
    if (!_state)
    {
        throw std::runtime_error("vulkan_queue cannot share a null submission state.");
    }

    if (_state->queue_index != _index)
    {
        throw std::runtime_error("vulkan_queue can only share submission state within the same queue family index.");
    }

    index        = _index;
    queue        = vk::raii::Queue(_device, index, 0);
    const vk::CommandPoolCreateInfo pool_info(vk::CommandPoolCreateFlagBits::eResetCommandBuffer, index, nullptr);
    command_pool = vk::raii::CommandPool(_device, pool_info);
    submission   = std::move(_state);
}

/**
 * @brief 在提交锁内分配时间线 value、合并依赖与二进制等待、提交一条 CB，成功后才写 latest 并返回 token。
 * @note 依赖按信号量合并：同一 timeline 取已记录的最大 value，重复的等待折叠成一条 eAllCommands 等待。
 *       提交失败走 catch(...)：先置 stopped 再重抛，保证该队列不会再接受半提交状态下的后续操作。
 *       latest 用 release 语义写、get_latest_token 用 acquire 读，与 token 的发布顺序一致。
 */
vulkan_submission_token vulkan_queue::submit(const vk::CommandBufferSubmitInfo&       _commandbuffer,
                                      std::span<const vk::SemaphoreSubmitInfo> _binary_waits,
                                      std::span<const vk::SemaphoreSubmitInfo> _binary_signals,
                                      std::span<const vulkan_submission_token>        _dependencies) const
{
    if (!submission)
    {
        throw std::runtime_error("vulkan_queue::submit called on an uninitialized queue.");
    }

    const std::lock_guard lock(submission->mutex);
    if (submission->stopped.load(std::memory_order_acquire))
    {
        throw std::runtime_error("Vulkan queue stopped after a failed submission transaction.");
    }

    try
    {
        const uint64_t value = submission->semaphore.reserve();

        std::vector<vk::SemaphoreSubmitInfo> waits(_binary_waits.begin(), _binary_waits.end());
        for (const auto& dependency : _dependencies)
        {
            if (!dependency.is_valid())
            {
                continue;
            }

            const vk::Semaphore semaphore = *dependency.semaphore->get_semaphore();
            const auto         iter       = std::ranges::find_if(waits, [semaphore](const auto& _wait) {
                return _wait.semaphore == semaphore;
            });
            if (iter != waits.end())
            {
                iter->value = std::max(iter->value, dependency.value);
                iter->stageMask |= vk::PipelineStageFlagBits2::eAllCommands;
            }
            else
            {
                waits.emplace_back(semaphore, dependency.value, vk::PipelineStageFlagBits2::eAllCommands);
            }
        }

        std::vector<vk::SemaphoreSubmitInfo> signals(_binary_signals.begin(), _binary_signals.end());
        signals.emplace_back(*submission->semaphore.get_semaphore(), value, vk::PipelineStageFlagBits2::eAllCommands);

        const std::array submit_infos{vk::SubmitInfo2({}, waits, _commandbuffer, signals)};
        queue.submit2(submit_infos, nullptr);

        submission->latest.store(value, std::memory_order_release);

        return vulkan_submission_token{.semaphore = &submission->semaphore, .value = value};
    }
    catch (...)
    {
        stop_submissions();
        throw;
    }
}

/** @brief 置 stopped（release 语义）；空对象直接忽略，noexcept 以便在异常展开路径里调用。 */
void vulkan_queue::stop_submissions() const noexcept
{
    if (submission) submission->stopped.store(true, std::memory_order_release);
}

/**
 * @brief 在提交锁内检查 stopped 后直调 vkQueuePresentKHR，返回原始 Result。
 * @note 与 submit 共用同一把锁 ⇒ 同一 VkQueue 上的提交与呈现不会并发；结果不做分类，交给调用方处理。
 */
vk::Result vulkan_queue::present(const vk::PresentInfoKHR& info) const
{
    const std::lock_guard lock(submission->mutex);
    if (submission->stopped.load(std::memory_order_acquire))
        throw std::runtime_error("Cannot present on a stopped Vulkan queue.");
    return static_cast<vk::Result>(queue.getDispatcher()->vkQueuePresentKHR(*queue, reinterpret_cast<const VkPresentInfoKHR*>(&info)));
}

/** @brief 返回创建时记录的 queue family index；未初始化时为 vk::QueueFamilyIgnored。 */
uint32_t vulkan_queue::get_index() const noexcept
{
    return index;
}

/** @brief 返回底层队列句柄；别名队列返回各自的 raii 包装，但底层是同一个 VkQueue。 */
const vk::raii::Queue& vulkan_queue::get_queue() const noexcept
{
    return queue;
}

/** @brief 返回该队列的命令池；分配命令缓冲时用它，未初始化时为空句柄。 */
const vk::raii::CommandPool& vulkan_queue::get_command_pool() const noexcept
{
    return command_pool;
}

/** @brief 按值返回 shared_ptr，让调用方（别名队列、回收站）延长共享状态的寿命。 */
std::shared_ptr<vulkan_queue::submission_state> vulkan_queue::get_submission_state() const noexcept
{
    return submission;
}

/** @brief 用 acquire 读 latest 并组 token；空对象返回空 token，从未提交时 value 为 0（视为已完成）。 */
vulkan_submission_token vulkan_queue::get_latest_token() const noexcept
{
    if (!submission)
    {
        return {};
    }
    return vulkan_submission_token{.semaphore = &submission->semaphore,
                            .value     = submission->latest.load(std::memory_order_acquire)};
}
