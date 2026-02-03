#pragma once
#define GLM_FORCE_RADIANS
#define GLM_ENABLE_EXPERIMENTAL
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define VK_USE_PLATFORM_WIN32_KHR

#include <array>
#include <filesystem>
#include <glm/glm.hpp>
#include <string_view>
#include <vulkan/vulkan.hpp>

#define MODEL_LOADER model_loader::get_model_loader()
constexpr std::u8string_view MODELS_PATH = u8"resources\\models\\";

struct model_vertex
{
	glm::vec3 position = { 0.f, 0.f, 0.f };
	glm::vec3 normal = { 0.f, 0.f, 0.f };
	glm::vec2 uv = { 0.f, 0.f };

	auto operator<=>(const model_vertex& _other) const = default;

	static const auto get_binding_description() noexcept
	{
		return vk::VertexInputBindingDescription{ 0, sizeof(model_vertex), vk::VertexInputRate::eVertex };
	}

	static const auto get_attribute_descriptions() noexcept
	{
		return std::array{
			vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, offsetof(model_vertex, position)),
			vk::VertexInputAttributeDescription(1, 0, vk::Format::eR32G32B32Sfloat, offsetof(model_vertex, normal)),
			vk::VertexInputAttributeDescription(2, 0, vk::Format::eR32G32Sfloat, offsetof(model_vertex, uv)),
		};
	}
};

class model_loader
{
public:
	bool load_model(const std::filesystem::path& _file_path, std::vector<model_vertex>& _vertices, std::vector<uint32_t>& _indices) noexcept;
	static model_loader& get_model_loader() noexcept;

private:
	model_loader() = default;
	~model_loader() = default;
	model_loader(const model_loader&) = delete;
	model_loader& operator=(const model_loader&) = delete;
	model_loader(const model_loader&&) = delete;
	model_loader& operator=(const model_loader&&) = delete;

	static model_loader loader;
};
