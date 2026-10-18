/**
 * @file vulkan_semaphore.cpp
 * @brief 时间线信号量的创建、值分配、等待与计数查询。
 * @details wait() 用 vkWaitSemaphores 处理 eTimeout（无限超时下只有设备异常才会失败）。
 *          wait_for_cleanup() 把设备丢失降级为 stderr 提示，其他错误退回 vkDeviceWaitIdle，再失败则 terminate。
 *          reserve() 只做原子自增，是否真正提交由 vulkan_queue 保证。
 */
#include "vulkan_semaphore.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>

/**
 * @brief 创建初值为 0 的时间线信号量。
 * @note 用 StructureChain 把 SemaphoreTypeCreateInfo 接到 SemaphoreCreateInfo 上。
 */
void vulkan_semaphore::create(const vk::raii::Device& _device)
{
    vk::StructureChain<vk::SemaphoreCreateInfo, vk::SemaphoreTypeCreateInfo> create_info(
        vk::SemaphoreCreateInfo({}), vk::SemaphoreTypeCreateInfo(vk::SemaphoreType::eTimeline, 0));
    semaphore = vk::raii::Semaphore(_device, create_info.get());
}

/**
 * @brief 原子取下一个时间线值（从 1 开始）。
 * @note 只分配不提交；调用方负责在提交里 signal 这个值。
 */
uint64_t vulkan_semaphore::reserve() noexcept
{
    return value.fetch_add(1, std::memory_order_acq_rel) + 1;
}

/**
 * @brief 用 vkWaitSemaphores 阻塞到计数达到 _value。
 * @note 超时设为 UINT64_MAX，循环只是兜底规范允许的 eTimeout 返回。
 */
void vulkan_semaphore::wait(uint64_t _value) const
{
    const vk::SemaphoreWaitInfo wait_info({}, *semaphore, _value);
    while (vk::Result::eTimeout == semaphore.getDevice().waitSemaphores(
        wait_info, std::numeric_limits<uint64_t>::max(), *semaphore.getDispatcher()))
    {
    }
}

/**
 * @brief 容错等待：设备丢失打印后继续，其他错误退到 vkDeviceWaitIdle。
 * @note noexcept；设备级等待再失败时 terminate，避免 GPU 未完成就释放资源。
 */
void vulkan_semaphore::wait_for_cleanup(uint64_t _value) const noexcept
{
    try
    {
        wait(_value);
    }
    catch (const vk::DeviceLostError&)
    {
        std::fputs("Vulkan device lost while draining a submission; continuing teardown.\n", stderr);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Cannot safely drain Vulkan submission: %s\n", error.what());
        // A failed query/wait is not proof of completion. Do not free resources
        // still in use if even a device-wide emergency drain cannot succeed.
        try { semaphore.getDevice().waitIdle(*semaphore.getDispatcher()); }
        catch (const vk::DeviceLostError&) {}
        catch (...) { std::terminate(); }
    }
}

uint64_t vulkan_semaphore::get_gpu_value() const
{
    return semaphore.getCounterValue();
}

const vk::raii::Semaphore& vulkan_semaphore::get_semaphore() const noexcept
{
    return semaphore;
}
