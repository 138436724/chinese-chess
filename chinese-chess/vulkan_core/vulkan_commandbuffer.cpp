#include "vulkan_commandbuffer.h"
#include <algorithm>
#include <ranges>

vulkan_commandbuffer::vulkan_commandbuffer(vulkan_commandbuffer&& _other) noexcept
	:commandbuffer_level(std::move(_other.commandbuffer_level)),
	commandbuffer(std::move(_other.commandbuffer)),
	fence(std::move(_other.fence)),
	device(std::move(_other.device)),
	queue(std::move(_other.queue))
{
}

vulkan_commandbuffer& vulkan_commandbuffer::operator=(vulkan_commandbuffer&& _other) noexcept
{
	if (this != &_other)
	{
		std::swap(commandbuffer_level, _other.commandbuffer_level);
		std::swap(commandbuffer, _other.commandbuffer);
		std::swap(fence, _other.fence);
		std::swap(device, _other.device);
		std::swap(queue, _other.queue);
	}
	return *this;
}

std::vector<vulkan_commandbuffer> vulkan_commandbuffer::create(const vk::CommandBufferAllocateInfo& _allocate_info, const vk::raii::Device* _device, const vk::raii::Queue* _queue)
{
	std::vector<vk::raii::CommandBuffer> commandbuffers = vk::raii::CommandBuffers(*_device, _allocate_info);

	auto cs = commandbuffers
		| std::views::transform([&](vk::raii::CommandBuffer& _commandbuffer)
			{
				vulkan_commandbuffer c;
				c.create(_allocate_info.level, std::move(_commandbuffer), _device, _queue);
				return c;
			})
		| std::ranges::to<std::vector>();

	return cs;
}

void vulkan_commandbuffer::create(vk::CommandBufferLevel _commandbuffer_level, vk::raii::CommandBuffer&& _commandbuffer, const vk::raii::Device* _device, const vk::raii::Queue* _queue)
{
	commandbuffer_level = _commandbuffer_level;
	commandbuffer = std::move(_commandbuffer);
	device = _device;
	queue = _queue;

	if (commandbuffer_level == vk::CommandBufferLevel::ePrimary)
	{
		fence = vk::raii::Fence(*device, vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled));
	}
}

void vulkan_commandbuffer::begin_record(vk::CommandBufferUsageFlags _usage) const
{
	if (commandbuffer_level == vk::CommandBufferLevel::ePrimary)
	{
		while (vk::Result::eTimeout == device->waitForFences(*fence, vk::True, std::numeric_limits<uint64_t>::max()))
		{
		};
		device->resetFences(*fence);
	}
	commandbuffer.reset();
	vk::CommandBufferInheritanceInfo info;
	commandbuffer.begin(vk::CommandBufferBeginInfo(_usage, &info));
}

void vulkan_commandbuffer::end_record() const
{
	commandbuffer.end();
}

void vulkan_commandbuffer::submit(const std::vector<vk::SemaphoreSubmitInfo>& _waited, const std::vector<vk::SemaphoreSubmitInfo>& _signal, bool _immediately) const
{
	vk::CommandBufferSubmitInfo commandbuffer_submit_info(*commandbuffer, 0);
	vk::SubmitInfo2 submit_info({}, _waited, commandbuffer_submit_info, _signal);

	queue->submit2(submit_info, *fence);

	if (_immediately && commandbuffer_level == vk::CommandBufferLevel::ePrimary)
	{
		while (vk::Result::eTimeout == device->waitForFences(*fence, vk::True, std::numeric_limits<uint64_t>::max()))
		{
		};
	}
}

const vk::raii::CommandBuffer& vulkan_commandbuffer::operator*() const noexcept
{
	return commandbuffer;
}