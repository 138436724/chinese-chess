#pragma once

#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <ranges>
#include <vulkan/vulkan_raii.hpp>

class vulkan_common
{
public:
	inline static vk::SampleCountFlagBits MSAA_SAMPLE_COUNT = vk::SampleCountFlagBits::e1;
	inline static vk::Format DEPTH_FORMAT = vk::Format::eUndefined;
	inline static const uint32_t MAX_FRAMES_IN_FLIGHT = 2;
	inline static constexpr bool USE_OCIO = true;

	inline static std::optional<uint32_t> find_memory_type(const vk::raii::PhysicalDevice& _physical_device, uint32_t _type_filter, vk::MemoryPropertyFlags _properties) noexcept
	{
		auto memory_properties = _physical_device.getMemoryProperties();
		auto properties_filter = memory_properties.memoryTypes
			| std::views::enumerate
			| std::views::filter([&](const auto& _tuple)
				{
					return (_type_filter & (1 << std::get<0>(_tuple))) && ((std::get<1>(_tuple).propertyFlags & _properties) == _properties);
				})
			| std::views::transform([](const auto& _tuple) { return std::get<0>(_tuple); });

		return properties_filter.empty() ? std::nullopt : std::optional<uint32_t>(static_cast<uint32_t>(properties_filter.front()));
	}

	inline static std::optional<vk::Format> find_supported_format(const vk::raii::PhysicalDevice& _physical_device, const std::vector<vk::Format>& _candidates, vk::ImageTiling _tiling, vk::FormatFeatureFlags _features) noexcept
	{
		auto format_iter = std::ranges::find_if(_candidates, [&](const auto& format)
			{
				vk::FormatProperties props = _physical_device.getFormatProperties(format);
				return (_tiling == vk::ImageTiling::eLinear && (props.linearTilingFeatures & _features) == _features)
					|| (_tiling == vk::ImageTiling::eOptimal && (props.optimalTilingFeatures & _features) == _features);
			});

		return format_iter == _candidates.end() ? std::nullopt : std::optional<vk::Format>(*format_iter);
	}

	inline static auto align_up(auto value, size_t alignment) noexcept
	{
		return ((value + alignment - 1) & ~(alignment - 1));
	};

	// VkTransformMatrixKHR is row-major 3x4, glm::mat4 is column-major; transpose before memcpy.
	inline static auto glm_matrix_to_vulkan(const glm::mat4& m) noexcept
	{
		vk::TransformMatrixKHR t;
		memcpy(&t, glm::value_ptr(glm::transpose(m)), sizeof(t));
		return t;
	};
};