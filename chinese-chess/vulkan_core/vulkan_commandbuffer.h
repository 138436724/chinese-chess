#pragma once

#include "vulkan_queue.h"
#include "vulkan_semaphore.h"

#include <vulkan/vulkan_raii.hpp>

class vulkan_commandbuffer
{
public:
    vulkan_commandbuffer()                                 = default;
    ~vulkan_commandbuffer()                                = default;
    vulkan_commandbuffer(vulkan_commandbuffer&)            = delete;
    vulkan_commandbuffer& operator=(vulkan_commandbuffer&) = delete;
    vulkan_commandbuffer(vulkan_commandbuffer&& _other) noexcept;
    vulkan_commandbuffer& operator=(vulkan_commandbuffer&& _other) noexcept;

    static std::vector<vulkan_commandbuffer> create(const vk::raii::Device&              _device,
                                                    const vk::CommandBufferAllocateInfo& _allocate_info,
                                                    const vulkan_queue*                  _queue,
                                                    vulkan_semaphore*                    _semaphore);

    void begin_record(vk::CommandBufferUsageFlags _usage);
    void end_record() const;
    void submit(bool _immediately);
    void wait();

    void add_waited_info(std::vector<vk::SemaphoreSubmitInfo>&& _submit_infos) noexcept;
    void add_signal_info(std::vector<vk::SemaphoreSubmitInfo>&& _submit_infos) noexcept;

    vk::SemaphoreSubmitInfo        get_submit_info() const noexcept;
    const vk::raii::CommandBuffer& operator*() const noexcept;

private:
    void create(vk::CommandBufferLevel    _commandbuffer_level,
                vk::raii::CommandBuffer&& _commandbuffer,
                const vulkan_queue*       _queue,
                vulkan_semaphore*         _semaphore);

private:
    vk::CommandBufferLevel  commandbuffer_level = vk::CommandBufferLevel::ePrimary;
    vk::raii::CommandBuffer commandbuffer       = nullptr;

    const vulkan_queue* queue = nullptr;

    vulkan_semaphore* semaphore       = nullptr;
    uint64_t          semaphore_value = 0;

    std::vector<vk::SemaphoreSubmitInfo> waited_info;
    std::vector<vk::SemaphoreSubmitInfo> signal_info;
};
