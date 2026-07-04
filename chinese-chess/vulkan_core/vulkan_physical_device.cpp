#include "vulkan_physical_device.h"

vulkan_physical_device::vulkan_physical_device(vulkan_physical_device&& _other) noexcept
	:physical_device(std::exchange(_other.physical_device, nullptr)),
	graphic_index(std::exchange(_other.graphic_index, vk::QueueFamilyIgnored)),
	compute_index(std::exchange(_other.compute_index, vk::QueueFamilyIgnored)),
	transfer_index(std::exchange(_other.transfer_index, vk::QueueFamilyIgnored))
{
}

vulkan_physical_device& vulkan_physical_device::operator=(vulkan_physical_device&& _other) noexcept
{
	if (this != &_other)
	{
		std::ranges::swap(physical_device, _other.physical_device);
		std::ranges::swap(graphic_index, _other.graphic_index);
		std::ranges::swap(compute_index, _other.compute_index);
		std::ranges::swap(transfer_index, _other.transfer_index);
	}
	return *this;
}

uint32_t vulkan_physical_device::get_queue_index(vk::QueueFlagBits _queue_type) const noexcept
{
	switch (_queue_type)
	{
	case vk::QueueFlagBits::eOpticalFlowNV:
		break;
	case vk::QueueFlagBits::eVideoEncodeKHR:
		break;
	case vk::QueueFlagBits::eVideoDecodeKHR:
		break;
	case vk::QueueFlagBits::eProtected:
		break;
	case vk::QueueFlagBits::eSparseBinding:
		break;
	case vk::QueueFlagBits::eTransfer:
		return transfer_index;
	case vk::QueueFlagBits::eCompute:
		return compute_index;
	case vk::QueueFlagBits::eGraphics:
		return graphic_index;
	default:
		break;
	}
	return vk::QueueFamilyIgnored;
}

const vk::raii::PhysicalDevice& vulkan_physical_device::operator*() const noexcept
{
	return physical_device;
}
