#include "vulkan_commandbuffer.h"

#include <algorithm>
#include <ranges>

vulkan_commandbuffer::vulkan_commandbuffer(vulkan_commandbuffer&& _other) noexcept
    : commandbuffer_level(std::exchange(_other.commandbuffer_level, {}))
    , commandbuffer(std::exchange(_other.commandbuffer, nullptr))
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
        std::ranges::swap(commandbuffer_level, _other.commandbuffer_level);
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
        throw std::runtime_error("Commandpool in allocate info must match queue's commandpool.");
    }

    std::vector<vk::raii::CommandBuffer> commandbuffers = vk::raii::CommandBuffers(_device, _allocate_info);

    return commandbuffers | std::views::transform([&](vk::raii::CommandBuffer& _commandbuffer) {
               vulkan_commandbuffer c;
               c.create(_allocate_info.level, std::move(_commandbuffer), _queue, _semaphore);
               return c;
           })
           | std::ranges::to<std::vector>();
}

void vulkan_commandbuffer::begin_record(vk::CommandBufferUsageFlags _usage)
{
    if (commandbuffer_level == vk::CommandBufferLevel::ePrimary)
    {
        wait();
    }

    waited_info.clear();
    signal_info.clear();

    commandbuffer.reset();

    vk::CommandBufferInheritanceInfo info;
    commandbuffer.begin(vk::CommandBufferBeginInfo(_usage, &info));
}

void vulkan_commandbuffer::end_record() const
{
    commandbuffer.end();
}

void vulkan_commandbuffer::submit(bool _immediately)
{
    if (commandbuffer_level == vk::CommandBufferLevel::ePrimary)
    {
        vk::CommandBufferSubmitInfo commandbuffer_submit_info(*commandbuffer, 0);

        signal_info.push_back(semaphore->next(vk::PipelineStageFlagBits2::eAllCommands));
        semaphore_value = signal_info.back().value;

        const std::array submit_info = {vk::SubmitInfo2({}, waited_info, commandbuffer_submit_info, signal_info)};
        queue->submit(submit_info);

        if (_immediately)
        {
            wait();
        }
    }
#ifndef NDEBUG
    else
    {
        throw std::runtime_error("Secondary commandbuffer can not submit.");
    }
#endif  // !NDEBUG
}

void vulkan_commandbuffer::wait()
{
    if (commandbuffer_level == vk::CommandBufferLevel::ePrimary)
    {
        semaphore->wait(semaphore_value);
    }
#ifndef NDEBUG
    else
    {
        throw std::runtime_error("Secondary commandbuffer can not wait.");
    }
#endif  // !NDEBUG
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
    if (commandbuffer_level == vk::CommandBufferLevel::ePrimary)
    {
        return vk::SemaphoreSubmitInfo(semaphore->get_semaphore(), semaphore_value, vk::PipelineStageFlagBits2::eAllCommands);
    }
    else
    {
        std::unreachable();
        return {};
    }
}

const vk::raii::CommandBuffer& vulkan_commandbuffer::operator*() const noexcept
{
    return commandbuffer;
}

void vulkan_commandbuffer::create(vk::CommandBufferLevel    _commandbuffer_level,
                                  vk::raii::CommandBuffer&& _commandbuffer,
                                  const vulkan_queue*       _queue,
                                  vulkan_semaphore*         _semaphore)
{
    commandbuffer_level = _commandbuffer_level;
    commandbuffer       = std::move(_commandbuffer);
    queue               = _queue;
    semaphore           = _semaphore;

    if (commandbuffer_level == vk::CommandBufferLevel::ePrimary)
    {
        semaphore_value = _semaphore->get_cpu_value();
    }
#ifndef NDEBUG
    else
    {
        if (queue != nullptr || semaphore != nullptr)
        {
            throw std::runtime_error("Secondary commandbuffer no need queue and semaphore.");
        }
    }
#endif  // !NDEBUG
}
