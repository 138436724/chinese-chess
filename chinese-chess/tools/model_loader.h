#pragma once

#include <array>
#include <filesystem>
#include <glm/glm.hpp>
#include <string_view>
#include <vulkan/vulkan.hpp>

constexpr std::string_view MODELS_PATH = "resources\\models\\";

enum class model_vertex_type : uint32_t
{
    position,
    normal,
    uv
};

struct model_vertex
{
    glm::vec3 position = {0.f, 0.f, 0.f};
    glm::vec3 normal   = {0.f, 0.f, 0.f};
    glm::vec2 uv       = {0.f, 0.f};

    auto operator<=>(const model_vertex& _other) const noexcept = default;

    [[nodiscard]] static consteval auto get_binding_description() noexcept
    {
        return vk::VertexInputBindingDescription{0, sizeof(model_vertex), vk::VertexInputRate::eVertex};
    }

    [[nodiscard]] static consteval auto get_attribute_description(model_vertex_type _type, uint32_t _location) noexcept
    {
        vk::VertexInputAttributeDescription description{};

        description.binding  = 0;
        description.location = _location;

        switch (_type)
        {
            case model_vertex_type::position:
                description.format = vk::Format::eR32G32B32Sfloat;
                description.offset = offsetof(model_vertex, position);
                break;
            case model_vertex_type::normal:
                description.format = vk::Format::eR32G32B32Sfloat;
                description.offset = offsetof(model_vertex, normal);
                break;
            case model_vertex_type::uv:
                description.format = vk::Format::eR32G32Sfloat;
                description.offset = offsetof(model_vertex, uv);
                break;
        }

        return description;
    }

    template <model_vertex_type... attributes>
    [[nodiscard]] static consteval auto get_attribute_descriptions() noexcept
    {
        auto create = []<std::size_t... indices>(std::index_sequence<indices...>) {
            return std::array{get_attribute_description(attributes, indices)...};
        };
        return create(std::make_index_sequence<sizeof...(attributes)>());

        //return[]<std::size_t...indices>(std::index_sequence<indices...>)
        //{
        //	return std::array{ make_attribute_description(attributes, indices)... };
        //}(std::make_index_sequence<sizeof...(attributes)>());

        //return std::array{
        //	vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, offsetof(model_vertex, position)),
        //	vk::VertexInputAttributeDescription(1, 0, vk::Format::eR32G32B32Sfloat, offsetof(model_vertex, normal)),
        //	vk::VertexInputAttributeDescription(2, 0, vk::Format::eR32G32Sfloat, offsetof(model_vertex, uv)),
        //};
    }
};

namespace model_loader {

[[nodiscard]] bool load_model(const std::filesystem::path& _file_path,
                              std::vector<model_vertex>&   _vertices,
                              std::vector<uint32_t>&       _indices) noexcept;

}  // namespace model_loader
