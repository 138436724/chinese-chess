#include "vulkan_semaphore.h"

void vulkan_semaphore::create(const vk::raii::Device& _device)
{
    vk::StructureChain<vk::SemaphoreCreateInfo, vk::SemaphoreTypeCreateInfo> create_info(
        vk::SemaphoreCreateInfo({}), vk::SemaphoreTypeCreateInfo(vk::SemaphoreType::eTimeline, 0));
    semaphore = vk::raii::Semaphore(_device, create_info.get());
}

vk::SemaphoreSubmitInfo vulkan_semaphore::next(vk::PipelineStageFlagBits2 _stage) noexcept
{
    value.fetch_add(1, std::memory_order_relaxed);
    return vk::SemaphoreSubmitInfo(*semaphore, value, _stage);
}

void vulkan_semaphore::wait(uint64_t _value) const
{
    const vk::SemaphoreWaitInfo wait_info(vk::SemaphoreWaitFlagBits::eAny, *semaphore, _value);
    while (vk::Result::eTimeout == semaphore.getDevice().waitSemaphores(wait_info, std::numeric_limits<uint64_t>::max()))
    {
    }
}

uint64_t vulkan_semaphore::get_gpu_value() const noexcept
{
    return semaphore.getCounterValue();
}

uint64_t vulkan_semaphore::get_cpu_value() const noexcept
{
    return value.load(std::memory_order_relaxed);
}

const vk::raii::Semaphore& vulkan_semaphore::get_semaphore() const noexcept
{
    return semaphore;
}
