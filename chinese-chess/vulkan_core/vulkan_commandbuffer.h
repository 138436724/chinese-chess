#pragma once
#define VK_USE_PLATFORM_WIN32_KHR

#include <vulkan/vulkan_raii.hpp>

class vulkan_commandbuffer
{
public:
	vulkan_commandbuffer() = default;
	~vulkan_commandbuffer() = default;
	vulkan_commandbuffer(vulkan_commandbuffer&) = delete;
	vulkan_commandbuffer(vulkan_commandbuffer&& _other) noexcept;
	vulkan_commandbuffer& operator=(vulkan_commandbuffer&) = delete;
	vulkan_commandbuffer& operator=(vulkan_commandbuffer&& _other) noexcept;

	static std::vector<vulkan_commandbuffer> create(const vk::CommandBufferAllocateInfo& _allocate_info, const vk::raii::Device* _device, const vk::raii::Queue* _queue);
	void create(vk::CommandBufferLevel _commandbuffer_level, vk::raii::CommandBuffer&& _commandbuffer, const vk::raii::Device* _device, const vk::raii::Queue* _queue);

	void begin_record(vk::CommandBufferUsageFlags _usage) const;
	void end_record() const;
	void submit(const std::vector<vk::SemaphoreSubmitInfo>& _waited, const std::vector<vk::SemaphoreSubmitInfo>& _signal, bool _immediately) const;
	void wait() const;

	const vk::raii::CommandBuffer& operator*() const noexcept;

private:
	vk::CommandBufferLevel commandbuffer_level = vk::CommandBufferLevel::ePrimary;
	vk::raii::CommandBuffer commandbuffer = nullptr;
	vk::raii::Fence fence = nullptr;
	const vk::raii::Device* device = nullptr;
	const vk::raii::Queue* queue = nullptr;
};