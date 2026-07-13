#include "model_loader.h"

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

bool model_loader::load_model(const std::filesystem::path& _file_path,
                              std::vector<model_vertex>&   _vertices,
                              std::vector<uint32_t>&       _indices) noexcept
{
    auto read_data = fastgltf::GltfDataBuffer::FromPath(_file_path);
    if (read_data.error() != fastgltf::Error::None)
    {
        return false;
    }

    fastgltf::GltfDataBuffer data = std::move(read_data.get());

    //constexpr auto gltfOptions = fastgltf::Options::LoadGLBBuffers | fastgltf::Options::LoadExternalBuffers;fastgltf::Options::DontRequireValidAssetMember
    fastgltf::Parser parse;
    auto             asset = parse.loadGltf(data, _file_path.parent_path(), fastgltf::Options::LoadExternalBuffers);
    if (asset.error() != fastgltf::Error::None)
    {
        //std::println("{}", fastgltf::to_underlying(asset.error()));
        return false;
    }

    const fastgltf::Asset gltf = std::move(asset.get());

    _vertices.clear();
    _indices.clear();

    for (const auto& _mesh : gltf.meshes)
    {
        for (const auto& _primitives : _mesh.primitives)
        {
            const auto initial_vtx = static_cast<uint32_t>(_vertices.size());

            const fastgltf::Accessor& index_accessor = gltf.accessors.at(_primitives.indicesAccessor.value());
            _indices.reserve(_indices.size() + index_accessor.count);

            fastgltf::iterateAccessor<uint32_t>(gltf, index_accessor,
                                                [&](uint32_t index) { _indices.push_back(index + initial_vtx); });


            const fastgltf::Accessor& position_accessor = gltf.accessors.at(_primitives.findAttribute("POSITION")->accessorIndex);
            _vertices.resize(_vertices.size() + position_accessor.count);

            fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, position_accessor, [&](glm::vec3 _position, size_t _index) {
                model_vertex vertex(_position, glm::vec3(1.f, 0.f, 0.f), glm::vec2(1.f, 1.f));
                _vertices.at(initial_vtx + _index) = vertex;
            });


            const auto normals = _primitives.findAttribute("NORMAL");
            if (normals != _primitives.attributes.end())
            {
                fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors.at(normals->accessorIndex),
                                                              [&](glm::vec3 _normal, size_t _index) {
                                                                  _vertices.at(initial_vtx + _index).normal = _normal;
                                                              });
            }


            const auto uvs = _primitives.findAttribute("TEXCOORD_0");
            if (uvs != _primitives.attributes.end())
            {
                fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors.at(uvs->accessorIndex), [&](glm::vec2 _uv, size_t _index) {
                    _vertices.at(initial_vtx + _index).uv = _uv;  // glm::vec2(_uv.x, 1.f - _uv.y); // flip y
                });
            }
        }
    }

    return true;
}
