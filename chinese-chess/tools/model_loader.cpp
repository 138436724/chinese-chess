/**
 * @file model_loader.cpp
 * @brief model_loader 的实现：fastgltf 解析 + accessor 逐项展开成顶点与索引。
 * @details 两个匿名命名空间辅助函数负责「按下标取 accessor 不抛异常」和「缺法线时的径向兜底」。
 *          遍历顺序是 mesh -> primitive -> 属性：每个 primitive 先按 POSITION 的 count 扩容顶点数组，
 *          再用带下标的 iterateAccessor 回填 normal / uv，索引统一加上该 primitive 的起始顶点偏移。
 */

#include "model_loader.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <filesystem>
#include <format>
#include <glm/glm.hpp>
#include <string>
#include <utility>

namespace
{
// accessor 一律按下标自查（不用 .at()：malformed / 截断的文件必须走 expected，而不是 std::terminate）
/**
 * @brief 按下标取 accessor，越界返回 nullptr 而不是抛异常。
 * @note 坏 / 截断的 glTF 只能让加载返回 expected 错误，不能让 .at() 变成 std::terminate。
 */
[[nodiscard]] const fastgltf::Accessor* find_accessor(const fastgltf::Asset& _gltf, size_t _index) noexcept
{
    return _index < _gltf.accessors.size() ? &_gltf.accessors[_index] : nullptr;
}

// 缺法线时的兜底：径向朝外；顶点落在原点时退回 +Y
// （原实现写死的 (1,0,0) 既不是朝外也不是 normalize(position)，只是一个无意义的单位 X 轴）
/**
 * @brief 缺法线时的兜底：取位置方向的单位向量，位置落在原点则退回 +Y。
 * @note 写死的 (1,0,0) 既不是朝外也不是 normalize(position)，对球面类模型会整体走样。
 */
[[nodiscard]] glm::vec3 fallback_normal(const glm::vec3& _position) noexcept
{
    const float length_squared = glm::dot(_position, _position);
    return length_squared > 1e-12f ? _position / std::sqrt(length_squared) : glm::vec3(0.f, 1.f, 0.f);
}
}  // namespace

/**
 * @brief 读入 glTF、逐 primitive 合并顶点与索引，所有错误都走 expected 的 error 串。
 * @note 先用 GltfDataBuffer::FromPath 读整份文件，再以文件所在目录为 base path 加载外部 .bin。
 * @note 顶点先按 POSITION 的 count 一次性扩容，属性回填用迭代下标写入；索引越界会让整个加载失败，
 *       不把半成品交给上层。
 */
std::expected<model_loader::gltf_model_data, std::string> model_loader::load_model(const std::filesystem::path& _file_path)
{
    auto read_data = fastgltf::GltfDataBuffer::FromPath(_file_path);
    if (read_data.error() != fastgltf::Error::None)
    {
        return std::unexpected(std::format("Cannot read model file {}: {}", _file_path.generic_string(),
                                           fastgltf::getErrorMessage(read_data.error())));
    }

    fastgltf::GltfDataBuffer data = std::move(read_data.get());

    fastgltf::Parser parse;
    auto             asset = parse.loadGltf(data, _file_path.parent_path(), fastgltf::Options::LoadExternalBuffers);
    if (asset.error() != fastgltf::Error::None)
    {
        return std::unexpected(std::format("Invalid gltf model file {}: {}", _file_path.generic_string(),
                                           fastgltf::getErrorMessage(asset.error())));
    }

    const fastgltf::Asset gltf = std::move(asset.get());

    gltf_model_data result;

    for (const auto& _mesh : gltf.meshes)
    {
        for (const auto& _primitive : _mesh.primitives)
        {
            const auto position_attribute = _primitive.findAttribute("POSITION");
            if (position_attribute == _primitive.attributes.end())
            {
                return std::unexpected("Error model vertex data: POSITION attribute is missing.");
            }

            const fastgltf::Accessor* position_accessor = find_accessor(gltf, position_attribute->accessorIndex);
            if (position_accessor == nullptr)
            {
                return std::unexpected("Error model vertex data: POSITION accessor index out of range.");
            }

            const auto initial_vtx  = static_cast<uint32_t>(result.vertices.size());
            const auto vertex_count = position_accessor->count;
            result.vertices.resize(result.vertices.size() + vertex_count);

            fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, *position_accessor, [&](glm::vec3 _position, size_t _index) {
                result.vertices.at(initial_vtx + _index) =
                    vulkan_model_vertex(_position, fallback_normal(_position), glm::vec2(1.f, 1.f));
            });

            const auto normals = _primitive.findAttribute("NORMAL");
            if (normals != _primitive.attributes.end())
            {
                const fastgltf::Accessor* normal_accessor = find_accessor(gltf, normals->accessorIndex);
                if (normal_accessor == nullptr || normal_accessor->count != vertex_count)
                {
                    return std::unexpected("Error model vertex data: NORMAL accessor is out of range or its count mismatches POSITION.");
                }

                fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, *normal_accessor, [&](glm::vec3 _normal, size_t _index) {
                    result.vertices.at(initial_vtx + _index).normal = _normal;
                });
            }

            const auto uvs = _primitive.findAttribute("TEXCOORD_0");
            if (uvs != _primitive.attributes.end())
            {
                const fastgltf::Accessor* uv_accessor = find_accessor(gltf, uvs->accessorIndex);
                if (uv_accessor == nullptr || uv_accessor->count != vertex_count)
                {
                    return std::unexpected("Error model vertex data: TEXCOORD_0 accessor is out of range or its count mismatches POSITION.");
                }

                fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, *uv_accessor, [&](glm::vec2 _uv, size_t _index) {
                    result.vertices.at(initial_vtx + _index).uv = _uv;  // glm::vec2(_uv.x, 1.f - _uv.y); // flip y
                });
            }

            if (!_primitive.indicesAccessor.has_value())
            {
                return std::unexpected("Error model index data: primitive is not indexed.");
            }

            const fastgltf::Accessor* index_accessor = find_accessor(gltf, _primitive.indicesAccessor.value());
            if (index_accessor == nullptr)
            {
                return std::unexpected("Error model index data: accessor index out of range.");
            }

            bool index_out_of_range = false;
            result.indices.reserve(result.indices.size() + index_accessor->count);
            fastgltf::iterateAccessor<uint32_t>(gltf, *index_accessor, [&](uint32_t _index) {
                index_out_of_range = index_out_of_range || _index >= vertex_count;
                result.indices.push_back(_index + initial_vtx);
            });

            if (index_out_of_range)
            {
                return std::unexpected("Error model index data: index accessor references a vertex outside the primitive.");
            }
        }
    }

    return result;
}
