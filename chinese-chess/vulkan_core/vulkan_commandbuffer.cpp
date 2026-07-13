#include "vulkan_commandbuffer.h"

#include <algorithm>
#include <ranges>

vulkan_commandbuffer::vulkan_commandbuffer(vulkan_commandbuffer&& _other) noexcept
    : commandbuffer(std::exchange(_other.commandbuffer, nullptr))
    , queue(std::exchange(_other.queue, nullptr))
    , semaphore(std::exchange(_other.semaphore, nullptr))
    , semaphore_value(std::exchange(_other.semaphore_value, 0))
    , waited_info(std::exchange(_other.waited_info, {}))
    , signal_info(std::exchange(_other.signal_info, {}))
{
}

vulkan_commandbuffer& vulkan_commandbuffer::operator=(vulkan_commandbuffer&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(commandbuffer, _other.commandbuffer);
        std::ranges::swap(queue, _other.queue);
        std::ranges::swap(semaphore, _other.semaphore);
        std::ranges::swap(semaphore_value, _other.semaphore_value);
        std::ranges::swap(waited_info, _other.waited_info);
        std::ranges::swap(signal_info, _other.signal_info);
    }
    return *this;
}

std::vector<vulkan_commandbuffer> vulkan_commandbuffer::create(const vk::raii::Device&              _device,
                                                               const vk::CommandBufferAllocateInfo& _allocate_info,
                                                               const vulkan_queue*                  _queue,
                                                               vulkan_semaphore*                    _semaphore)
{
    if (_allocate_info.commandPool != *(_queue->get_command_pool()))
    {
        throw std::runtime_error("Command pool in allocate info must match queue's command pool.");
    }

    std::vector<vk::raii::CommandBuffer> commandbuffers = vk::raii::CommandBuffers(_device, _allocate_info);

    return commandbuffers | std::views::transform([&](vk::raii::CommandBuffer& _commandbuffer) {
               vulkan_commandbuffer c;
               c.create(std::move(_commandbuffer), _queue, _semaphore);
               return c;
           })
           | std::ranges::to<std::vector>();
}

void vulkan_commandbuffer::begin_record(vk::CommandBufferUsageFlags _usage)
{
    wait();

    commandbuffer.reset();

    const vk::CommandBufferInheritanceInfo info;
    commandbuffer.begin(vk::CommandBufferBeginInfo(_usage, &info));
}

void vulkan_commandbuffer::end_record() const
{
    commandbuffer.end();
}

void vulkan_commandbuffer::submit(bool _immediately)
{
    const vk::CommandBufferSubmitInfo commandbuffer_submit_info(*commandbuffer, 0);

    signal_info.push_back(semaphore->next(vk::PipelineStageFlagBits2::eAllCommands));
    semaphore_value = signal_info.back().value;

    const std::array submit_info = {vk::SubmitInfo2({}, waited_info, commandbuffer_submit_info, signal_info)};
    queue->submit(submit_info);

    waited_info.clear();
    signal_info.clear();

    if (_immediately)
    {
        wait();
    }
}

void vulkan_commandbuffer::wait() const
{
    semaphore->wait(semaphore_value);
}

void vulkan_commandbuffer::add_waited_info(std::vector<vk::SemaphoreSubmitInfo>&& _submit_infos) noexcept
{
    waited_info.append_range(_submit_infos | std::views::as_rvalue);
}

void vulkan_commandbuffer::add_signal_info(std::vector<vk::SemaphoreSubmitInfo>&& _submit_infos) noexcept
{
    signal_info.append_range(_submit_infos | std::views::as_rvalue);
}

vk::SemaphoreSubmitInfo vulkan_commandbuffer::get_submit_info() const noexcept
{
    return vk::SemaphoreSubmitInfo(semaphore->get_semaphore(), semaphore_value, vk::PipelineStageFlagBits2::eAllCommands);
}

const vk::raii::CommandBuffer& vulkan_commandbuffer::operator*() const noexcept
{
    return commandbuffer;
}

void vulkan_commandbuffer::create(vk::raii::CommandBuffer&& _commandbuffer, const vulkan_queue* _queue, vulkan_semaphore* _semaphore) noexcept
{
    commandbuffer   = std::move(_commandbuffer);
    queue           = _queue;
    semaphore       = _semaphore;
    semaphore_value = _semaphore->get_cpu_value();
}
