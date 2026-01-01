export module vulkan_commandbuffer;

import std;
import vulkan_hpp;

export class vulkan_commandbuffer
{
public:
	vulkan_commandbuffer() = default;
	~vulkan_commandbuffer() = default;
	vulkan_commandbuffer(vk::raii::CommandBuffer& _commandbuffer, vk::raii::Fence& _fence, const vk::raii::Device* _device, const vk::raii::Queue* _queue);
	vulkan_commandbuffer(vulkan_commandbuffer&) = delete;
	vulkan_commandbuffer(vulkan_commandbuffer&& _other) noexcept;
	vulkan_commandbuffer& operator=(vulkan_commandbuffer&) = delete;
	vulkan_commandbuffer& operator=(vulkan_commandbuffer&& _other) noexcept;

	void begin_record(vk::CommandBufferUsageFlags _usage);
	void end_record();
	void submit(const std::vector<vk::SemaphoreSubmitInfo>& _waited, const std::vector<vk::SemaphoreSubmitInfo>& _signal, bool _immediately);

	const vk::raii::CommandBuffer& operator*() const;

private:
	vk::raii::CommandBuffer commandbuffer = nullptr;
	vk::raii::Fence fence = nullptr;
	const vk::raii::Device* device = nullptr;
	const vk::raii::Queue* queue = nullptr;
};