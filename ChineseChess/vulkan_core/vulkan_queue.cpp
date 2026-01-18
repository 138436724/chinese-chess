module vulkan_queue;

vulkan_queue::vulkan_queue(vulkan_queue&& _other) noexcept
	: index(std::move(_other.index)),
	queue(std::move(_other.queue)),
	command_pool(std::move(_other.command_pool))
{
}

vulkan_queue& vulkan_queue::operator=(vulkan_queue&& _other) noexcept
{
	if (this != &_other)
	{
		std::swap(index, _other.index);
		std::swap(queue, _other.queue);
		std::swap(command_pool, _other.command_pool);
	}
	return *this;
}

void vulkan_queue::create(const vk::raii::Device& _device)
{
	queue = vk::raii::Queue(_device, index, 0);
	vk::CommandPoolCreateInfo pool_info(vk::CommandPoolCreateFlagBits::eResetCommandBuffer, index, nullptr);
	command_pool = vk::raii::CommandPool(_device, pool_info);
}

void vulkan_queue::set_index(uint32_t _index)
{
	index = _index;
}

uint32_t vulkan_queue::get_index() const
{
	return index;
}

const vk::raii::Queue& vulkan_queue::get_queue() const
{
	return queue;
}

const vk::raii::CommandPool& vulkan_queue::get_command_pool() const
{
	return command_pool;
}
