#pragma once

#include <atomic>
#include <vulkan/vulkan_raii.hpp>

class vulkan_semaphore
{
public:
    vulkan_semaphore()                                     = default;
    ~vulkan_semaphore()                                    = default;
    vulkan_semaphore(const vulkan_semaphore&)              = delete;
    vulkan_semaphore& operator=(const vulkan_semaphore&)   = delete;
    vulkan_semaphore(vulkan_semaphore&& _other)            = delete;
    vulkan_semaphore& operator=(vulkan_semaphore&& _other) = delete;

    void create(const vk::raii::Device& _device);
    [[nodiscard("semaphore submit info need submit!")]]
    vk::SemaphoreSubmitInfo next(vk::PipelineStageFlagBits2 _stage = vk::PipelineStageFlagBits2::eAllCommands) noexcept;
    void                    wait(uint64_t _value) const;

    [[nodiscard]] uint64_t                   get_gpu_value() const noexcept;
    [[nodiscard]] uint64_t                   get_cpu_value() const noexcept;
    [[nodiscard]] const vk::raii::Semaphore& get_semaphore() const noexcept;

private:
    vk::raii::Semaphore   semaphore = nullptr;
    std::atomic<uint64_t> value     = 0;
};
