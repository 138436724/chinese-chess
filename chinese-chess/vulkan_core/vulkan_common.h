#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vulkan/vulkan_raii.hpp>

class vulkan_common
{
public:
	inline static vk::SampleCountFlagBits MASS_SAMPLE_COUNT = vk::SampleCountFlagBits::e1;
	inline static vk::Format DEPTH_FORMAT = vk::Format::eUndefined;
	inline static const uint32_t MAX_FRAMES_IN_FLIGHT = 2;
	inline static constexpr bool USE_OCIO = true;

	static uint32_t find_memory_type(const vk::raii::PhysicalDevice& _physical_device, uint32_t _type_filter, vk::MemoryPropertyFlags _properties);

	static vk::Format find_supported_format(const vk::raii::PhysicalDevice& _physical_device, const std::vector<vk::Format>& _candidates, vk::ImageTiling _tiling, vk::FormatFeatureFlags _features);

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