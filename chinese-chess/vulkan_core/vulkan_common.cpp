#include "vulkan_common.h"
#include <algorithm>
#include <ranges>

uint32_t vulkan_common::find_memory_type(const vk::raii::PhysicalDevice& _physical_device, uint32_t _type_filter, vk::MemoryPropertyFlags _properties)
{
	auto memory_properties = _physical_device.getMemoryProperties();
	auto properties_filter = memory_properties.memoryTypes
		| std::views::enumerate
		| std::views::filter([&](const auto& _tuple)
			{
				return (_type_filter & (1 << std::get<0>(_tuple))) && ((std::get<1>(_tuple).propertyFlags & _properties) == _properties);
			})
		| std::views::transform([](const auto& _tuple) { return std::get<0>(_tuple); });

	if (properties_filter.empty())
	{
		throw std::runtime_error("failed to find suitable memory type!");
	}

	return static_cast<uint32_t>(properties_filter.front());
}

vk::Format vulkan_common::find_supported_format(const vk::raii::PhysicalDevice& _physical_device, const std::vector<vk::Format>& _candidates, vk::ImageTiling _tiling, vk::FormatFeatureFlags _features)
{
	auto format_iter = std::ranges::find_if(_candidates, [&](const auto& format)
		{
			vk::FormatProperties props = _physical_device.getFormatProperties(format);
			return (_tiling == vk::ImageTiling::eLinear && (props.linearTilingFeatures & _features) == _features)
				|| (_tiling == vk::ImageTiling::eOptimal && (props.optimalTilingFeatures & _features) == _features);
		});

	if (format_iter == _candidates.end())
	{
		throw std::runtime_error("failed to find supported format!");
	}

	return *format_iter;
}
