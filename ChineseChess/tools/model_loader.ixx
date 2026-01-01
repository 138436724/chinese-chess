export module model_loader;

import <cstddef>;
import <cstdint>;
import std;
import glm;
import vulkan_hpp;

export constexpr std::string_view MODELS_PATH = "resources\\models\\";

export struct model_vertex
{
	glm::vec3 position = { 0.f, 0.f, 0.f };
	glm::vec3 normal = { 0.f, 0.f, 0.f };
	glm::vec2 uv = { 0.f, 0.f };

	auto operator<=>(const model_vertex& _other) const = default;

	static const auto get_binding_description()
	{
		return vk::VertexInputBindingDescription{ 0, sizeof(model_vertex), vk::VertexInputRate::eVertex };
	}

	static const auto get_attribute_descriptions()
	{
		return std::array{
			vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, offsetof(model_vertex, position)),
			vk::VertexInputAttributeDescription(1, 0, vk::Format::eR32G32B32Sfloat, offsetof(model_vertex, normal)),
			vk::VertexInputAttributeDescription(2, 0, vk::Format::eR32G32Sfloat, offsetof(model_vertex, uv)),
		};
	}
};

export class model_loader
{
public:
	bool load_model_file(const std::filesystem::path& _file_path, std::vector<model_vertex>& _vertices, std::vector<uint32_t>& _indices);
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
