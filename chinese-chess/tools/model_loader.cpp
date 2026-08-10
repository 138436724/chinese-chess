#include "model_loader.h"

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

std::expected<model_loader::model_data, std::string> model_loader::load_model(const std::filesystem::path& _file_path) noexcept
{
    auto read_data = fastgltf::GltfDataBuffer::FromPath(_file_path);
    if (read_data.error() != fastgltf::Error::None)
    {
        return std::unexpected("Cannot read model file.");
    }

    fastgltf::GltfDataBuffer data = std::move(read_data.get());

    fastgltf::Parser parse;
    auto             asset = parse.loadGltf(data, _file_path.parent_path(), fastgltf::Options::LoadExternalBuffers);
    if (asset.error() != fastgltf::Error::None)
    {
        return std::unexpected("Invalid gltf model file.");
    }

    const fastgltf::Asset gltf = std::move(asset.get());

    model_data result;

    for (const auto& _mesh : gltf.meshes)
    {
        for (const auto& _primitives : _mesh.primitives)
        {
            const auto initial_vtx = static_cast<uint32_t>(result.vertices.size());

            if (!_primitives.indicesAccessor.has_value())
            {
                return std::unexpected("Error model index data.");
            }
            const fastgltf::Accessor& index_accessor = gltf.accessors.at(_primitives.indicesAccessor.value());
            result.indices.reserve(result.indices.size() + index_accessor.count);

            fastgltf::iterateAccessor<uint32_t>(gltf, index_accessor,
                                                [&](uint32_t index) { result.indices.push_back(index + initial_vtx); });

            const auto position_attribute = _primitives.findAttribute("POSITION");
            if (position_attribute == _primitives.attributes.end())
            {
                return std::unexpected("Error model vertex data.");
            }

            const fastgltf::Accessor& position_accessor = gltf.accessors.at(position_attribute->accessorIndex);
            result.vertices.resize(result.vertices.size() + position_accessor.count);

            fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, position_accessor, [&](glm::vec3 _position, size_t _index) {
                model_vertex vertex(_position, glm::vec3(1.f, 0.f, 0.f), glm::vec2(1.f, 1.f));
                result.vertices.at(initial_vtx + _index) = vertex;
            });

            const auto normals = _primitives.findAttribute("NORMAL");
            if (normals != _primitives.attributes.end())
            {
                fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors.at(normals->accessorIndex),
                                                              [&](glm::vec3 _normal, size_t _index) {
                                                                  result.vertices.at(initial_vtx + _index).normal = _normal;
                                                              });
            }

            const auto uvs = _primitives.findAttribute("TEXCOORD_0");
            if (uvs != _primitives.attributes.end())
            {
                fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors.at(uvs->accessorIndex), [&](glm::vec2 _uv, size_t _index) {
                    result.vertices.at(initial_vtx + _index).uv = _uv;  // glm::vec2(_uv.x, 1.f - _uv.y); // flip y
                });
            }
        }
    }

    return result;
}
