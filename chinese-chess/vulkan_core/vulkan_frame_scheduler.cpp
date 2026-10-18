/**
 * @file vulkan_frame_scheduler.cpp
 * @brief vulkan_frame_scheduler 的实现：slot 同步对象的创建与 idle/acquired/submitted 状态迁移。
 * @details prepare 复用 slot 前先等上次提交 token，再等 acquire fence 并重置它；finish 只在
 *          submitted 下推进游标，cancel 可在任意状态就地回到 idle。drain 与
 *          replace_acquire_semaphore 都会等 acquire fence；替换前的 GPU 消费完成由调用方另外保证。
 */
#include "vulkan_frame_scheduler.h"

#include <limits>
#include <stdexcept>

namespace {
/**
 * @brief 以无限超时等待 fence，只在返回 eTimeout 时重试。
 */
void wait_fence(const vk::raii::Device& device, vk::Fence fence)
{
    while (device.waitForFences(fence, true, std::numeric_limits<uint64_t>::max()) == vk::Result::eTimeout) {}
}
}

/**
 * @brief 为 count 个 slot 建 acquire semaphore 与 acquire fence，fence 初值保持未信号。
 * @note 只允许调用一次；count 为 0 或 slots 非空时直接抛 logic_error，避免槽数与上层帧数脱节。
 */
void vulkan_frame_scheduler::create(const vk::raii::Device& device, uint32_t count)
{
    if (!count || !slots.empty()) throw std::logic_error("Invalid frame scheduler initialization.");
    slots.resize(count);
    for (auto& slot : slots)
    {
        slot.acquired = vk::raii::Semaphore(device, vk::SemaphoreCreateInfo());
        slot.acquire_done = vk::raii::Fence(device, vk::FenceCreateInfo());
    }
}

/**
 * @brief 复用当前 slot 前等待上次提交 token，再等 acquire fence 并重置它。
 * @note token 与 fence 分别覆盖提交和 acquire，不能拿其中一个充当另一个的完成证明。
 */
void vulkan_frame_scheduler::prepare(const vk::raii::Device& device)
{
    if (active()) throw std::logic_error("Previous frame has not finished.");
    auto& slot = slots.at(current);
    slot.completion.wait();
    if (slot.acquire_pending) wait_fence(device, *slot.acquire_done);
    slot.acquire_pending = false;
    device.resetFences(*slot.acquire_done);
}

/**
 * @brief 进入 acquired 状态并记录 slot/image/generation，同时把该槽标记为 acquire 待确认。
 */
vulkan_frame_context vulkan_frame_scheduler::acquired(uint32_t image, uint64_t generation)
{
    if (active()) throw std::logic_error("Frame acquired twice.");
    slots.at(current).acquire_pending = true;
    phase = state::acquired;
    frame = {current, image, generation};
    return frame;
}

/**
 * @brief 登记提交 token 并进入 submitted 状态；token 无效或状态不对直接抛 logic_error。
 */
void vulkan_frame_scheduler::submitted(vulkan_submission_token completion)
{
    if (phase != state::acquired || !completion.is_valid()) throw std::logic_error("Invalid frame submission.");
    slots.at(current).completion = completion;
    phase = state::submitted;
}

/**
 * @brief 游标前进到下一个 slot 并回到 idle；只有 present 入队后调用才合法。
 */
void vulkan_frame_scheduler::finish()
{
    if (phase != state::submitted) throw std::logic_error("Cannot advance an unsubmitted frame.");
    current = (current + 1) % static_cast<uint32_t>(slots.size());
    phase = state::idle;
}

/**
 * @brief 就地回到 idle，不推进游标也不清 token；下次 prepare 仍会等它。
 */
void vulkan_frame_scheduler::cancel() noexcept { phase = state::idle; }

/**
 * @brief 等所有 slot 的提交 token 与 pending acquire fence，不改状态、不推进游标。
 * @note 供 recreate 与 shutdown 使用，可重复调用。
 */
void vulkan_frame_scheduler::drain(const vk::raii::Device& device) const
{
    for (const auto& slot : slots)
    {
        slot.completion.wait();
        if (slot.acquire_pending) wait_fence(device, *slot.acquire_done);
    }
}

/**
 * @brief 等当前 slot 的 acquire fence 后换一个新的 acquire semaphore。
 * @note 旧信号量可能已被 acquire 信号却无提交消费，必须等 fence 后才能安全销毁。
 */
void vulkan_frame_scheduler::replace_acquire_semaphore(const vk::raii::Device& device)
{
    auto& slot = slots.at(current);
    if (slot.acquire_pending) wait_fence(device, *slot.acquire_done);
    slot.acquired = vk::raii::Semaphore(device, vk::SemaphoreCreateInfo());
}

/**
 * @brief 返回当前帧身份；idle 时抛 logic_error，不会返回过期上下文。
 */
vulkan_frame_context vulkan_frame_scheduler::context() const
{
    if (!active()) throw std::logic_error("No frame is active.");
    return frame;
}
