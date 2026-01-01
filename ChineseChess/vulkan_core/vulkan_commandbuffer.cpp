module vulkan_commandbuffer;

vulkan_commandbuffer::vulkan_commandbuffer(vk::raii::CommandBuffer& _commandbuffer, vk::raii::Fence& _fence, const vk::raii::Device* _device, const vk::raii::Queue* _queue)
	:commandbuffer(std::move(_commandbuffer)),
	fence(std::move(_fence)),
	device(_device),
	queue(_queue)
{
}

vulkan_commandbuffer::vulkan_commandbuffer(vulkan_commandbuffer&& _other) noexcept
	:commandbuffer(std::move(_other.commandbuffer)),
	fence(std::move(_other.fence)),
	device(std::move(_other.device)),
	queue(std::move(_other.queue))
{
}

vulkan_commandbuffer& vulkan_commandbuffer::operator=(vulkan_commandbuffer&& _other) noexcept
{
	if (this != &_other)
	{
		std::swap(commandbuffer, _other.commandbuffer);
		std::swap(fence, _other.fence);
		std::swap(device, _other.device);
		std::swap(queue, _other.queue);
	}
	return *this;
}

void vulkan_commandbuffer::begin_record(vk::CommandBufferUsageFlags _usage)
{
	while (vk::Result::eTimeout == device->waitForFences(*fence, vk::True, std::numeric_limits<uint64_t>::max()))
	{
	};
	device->resetFences(*fence);
	commandbuffer.reset();
	commandbuffer.begin(vk::CommandBufferBeginInfo(_usage));
}

void vulkan_commandbuffer::end_record()
{
	commandbuffer.end();
}

void vulkan_commandbuffer::submit(const std::vector<vk::SemaphoreSubmitInfo>& _waited, const std::vector<vk::SemaphoreSubmitInfo>& _signal, bool _immediately)
{
	vk::CommandBufferSubmitInfo commandbuffer_submit_info(*commandbuffer, 0);
	vk::SubmitInfo2 submit_info({}, _waited, commandbuffer_submit_info, _signal);

	queue->submit2(submit_info, *fence);

	if (_immediately)
	{
		while (vk::Result::eTimeout == device->waitForFences(*fence, vk::True, std::numeric_limits<uint64_t>::max()))
		{
		};
	}
}

const vk::raii::CommandBuffer& vulkan_commandbuffer::operator*() const
{
	return commandbuffer;
}