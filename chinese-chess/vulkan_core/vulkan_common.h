#pragma once

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
	inline static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
	inline static constexpr bool USE_OCIO = true;

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