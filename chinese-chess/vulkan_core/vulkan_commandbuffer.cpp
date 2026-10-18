/**
 * @file vulkan_commandbuffer.cpp
 * @brief 命令缓冲的分配、录制、提交与完成等待的实现。
 * @details create 校验命令池与 owner queue 匹配后逐个移动出包装；begin_record 先等上一次
 *          提交的 token 再 reset。submit 把二进制 semaphore 等待项与 timeline 依赖一起交给
 *          vulkan_queue::submit，只有成功才发布 token，失败会把三类待提交信息清空再重抛。
 *          析构走 token.wait_for_cleanup，覆盖「已提交但后续流程抛异常」的清理路径。
 */

#include "vulkan_commandbuffer.h"

#include "vulkan_queue.h"
#include "vulkan_semaphore.h"

#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

/**
 * @brief 等自己的 token 完成后再把命令缓冲交还给 RAII 句柄。
 * @note 正常路径里回收站已经确认过完成，这里兜底的是「已提交、但后续分配或提交抛异常」。
 */
vulkan_commandbuffer::~vulkan_commandbuffer()
{
    // Usually already complete (the recycle bin checked it). Also protects a
    // successfully submitted local CB if a subsequent allocation/submit throws.
    token.wait_for_cleanup();
}

/**
 * @brief 逐成员 std::exchange：句柄、queue、token 与三张表都移交给新对象。
 */
vulkan_commandbuffer::vulkan_commandbuffer(vulkan_commandbuffer&& _other) noexcept
    : commandbuffer(std::exchange(_other.commandbuffer, nullptr))
    , queue(std::exchange(_other.queue, nullptr))
    , token(std::exchange(_other.token, {}))
    , waited_info(std::exchange(_other.waited_info, {}))
    , signal_info(std::exchange(_other.signal_info, {}))
    , dependencies(std::exchange(_other.dependencies, {}))
{
}

/**
 * @brief 自赋值提前返回，否则逐成员 std::ranges::swap，两边都可安全析构。
 */
vulkan_commandbuffer& vulkan_commandbuffer::operator=(vulkan_commandbuffer&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(commandbuffer, _other.commandbuffer);
        std::ranges::swap(queue, _other.queue);
        std::ranges::swap(token, _other.token);
        std::ranges::swap(waited_info, _other.waited_info);
        std::ranges::swap(signal_info, _other.signal_info);
        std::ranges::swap(dependencies, _other.dependencies);
    }
    return *this;
}

/**
 * @brief 校验 owner queue 与命令池一致后批量分配，并把每个句柄移进独立包装。
 * @note 命令池必须来自 _queue，否则提交与 reset 会跨 queue 使用池，这里直接抛。
 */
std::vector<vulkan_commandbuffer> vulkan_commandbuffer::create(const vk::raii::Device&              _device,
                                                               const vk::CommandBufferAllocateInfo& _allocate_info,
                                                               const vulkan_queue*                  _queue)
{
    if (_queue == nullptr)
    {
        throw std::runtime_error("Command buffer requires an owning queue.");
    }

    if (_allocate_info.commandPool != *(_queue->get_command_pool()))
    {
        throw std::runtime_error("Command pool in allocate info must match queue's command pool.");
    }

    std::vector<vk::raii::CommandBuffer> commandbuffers = vk::raii::CommandBuffers(_device, _allocate_info);

    return commandbuffers | std::views::transform([&](vk::raii::CommandBuffer& _commandbuffer) {
               vulkan_commandbuffer c;
               c.create(std::move(_commandbuffer), _queue);
               return c;
           })
           | std::ranges::to<std::vector>();
}

/**
 * @brief 先等上一次提交完成，再 reset 并 begin 一条新的录制。
 * @note 按命令缓冲自己的上次提交 token 等待；上层 frame slot 的等待不能替代这层局部保护。
 */
void vulkan_commandbuffer::begin_record(vk::CommandBufferUsageFlags _usage)
{
    // The command buffer must not be reset while its previous submission may
    // still be executing. This is the per-command-buffer completion edge that
    // replaces the old global CPU counter.
    if (token.is_valid() && !token.is_complete())
    {
        token.semaphore->wait(token.value);
    }

    commandbuffer.reset();

    const vk::CommandBufferInheritanceInfo info;
    commandbuffer.begin(vk::CommandBufferBeginInfo(_usage, &info));
}

/** @brief 结束录制；提交前必须调用，否则提交的是未结束的命令缓冲。 */
void vulkan_commandbuffer::end_record() const
{
    commandbuffer.end();
}

/**
 * @brief 把当前录制的命令缓冲连同等待项、信号项与 timeline 依赖提交给 owner queue。
 * @note token 只在提交成功后发布；失败时先清空待提交信息再重抛，避免重试带上陈旧等待。
 * @note 成功提交后 waited_info / signal_info / dependencies 也会清空，下次提交要重新添加。
 */
vulkan_submission_token vulkan_commandbuffer::submit()
{
    if (static_cast<VkCommandBuffer>(*commandbuffer) == VK_NULL_HANDLE)
    {
        throw std::runtime_error("Cannot submit a command buffer that was never allocated or was moved from.");
    }

    if (queue == nullptr)
    {
        throw std::runtime_error("Command buffer has no owning queue.");
    }

    const vk::CommandBufferSubmitInfo commandbuffer_submit_info(*commandbuffer, 0);

    try
    {
        token = queue->submit(commandbuffer_submit_info, waited_info, signal_info, dependencies);
    }
    catch (...)
    {
        // A failed submit publishes no token; drop pending dependencies so a
        // discarded command buffer cannot carry stale waits into a retry.
        waited_info.clear();
        signal_info.clear();
        dependencies.clear();
        throw;
    }

    waited_info.clear();
    signal_info.clear();
    dependencies.clear();

    return token;
}

/** @brief 阻塞到最近一次成功提交完成；从未提交过时直接返回。 */
void vulkan_commandbuffer::wait() const
{
    if (token.is_valid())
    {
        token.semaphore->wait(token.value);
    }
}

/** @brief 追加一个二进制 semaphore 等待项，成功提交后与其它待提交信息一起清空。 */
void vulkan_commandbuffer::add_waited_info(const vk::SemaphoreSubmitInfo& _submit_info)
{
    waited_info.push_back(_submit_info);
}

/** @brief 批量追加等待项（append_range），语义与单条版本相同。 */
void vulkan_commandbuffer::add_waited_info(std::span<const vk::SemaphoreSubmitInfo> _submit_infos)
{
    waited_info.append_range(_submit_infos);
}

/** @brief 追加一个二进制 semaphore 信号项，提交时原样交给 vulkan_queue 拼进 VkSubmitInfo2。 */
void vulkan_commandbuffer::add_signal_info(const vk::SemaphoreSubmitInfo& _submit_info)
{
    signal_info.push_back(_submit_info);
}

/**
 * @brief 追加一个 timeline 依赖，submit 时按 semaphore + value 转成等待项。
 * @note 依赖必须来自已成功提交的 token，等待一个尚未提交的 value 会让提交永久阻塞。
 */
void vulkan_commandbuffer::add_dependency(vulkan_submission_token _dependency)
{
    dependencies.push_back(_dependency);
}

/** @brief 批量追加 timeline 依赖（append_range）。 */
void vulkan_commandbuffer::add_dependency(std::span<const vulkan_submission_token> _dependencies)
{
    dependencies.append_range(_dependencies);
}

/** @brief 返回最近一次成功提交的 token；从未提交过时是无效 token。 */
vulkan_submission_token vulkan_commandbuffer::get_submission_token() const noexcept
{
    return token;
}

/** @brief 返回底层命令缓冲句柄，供直接录制；调用方自己保证不在执行中。 */
const vk::raii::CommandBuffer& vulkan_commandbuffer::operator*() const noexcept
{
    return commandbuffer;
}

/** @brief 接管已分配的句柄并绑定 owner queue，不做任何校验。 */
void vulkan_commandbuffer::create(vk::raii::CommandBuffer&& _commandbuffer, const vulkan_queue* _queue) noexcept
{
    commandbuffer = std::move(_commandbuffer);
    queue         = _queue;
}
