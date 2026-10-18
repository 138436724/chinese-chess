/**
 * @file model_loader.h
 * @brief glTF / GLB 模型加载：把 fastgltf 的 mesh 数据合并成 vulkan_model_vertex 与索引数组。
 * @details 只取 POSITION / NORMAL / TEXCOORD_0 三个顶点属性与 primitive 的索引，材质、贴图与节点变换
 *          一律忽略（当前资产只需几何本身，材质由场景层另行指定）。MODELS_PATH 是运行时模型目录，
 *          相对运行目录 chinese-chess/。
 */

#pragma once

#include "vulkan_core/vulkan_data_type.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

constexpr std::string_view MODELS_PATH = "resources\\models\\";


namespace model_loader {

/**
 * @brief 一次加载的合并结果：所有 mesh / primitive 的顶点与索引。
 * @details 索引已经加上前面 primitive 的顶点偏移，所以整块可以直接建一个顶点缓冲 + 一个索引缓冲，
 *          不需要按 primitive 拆 submesh。顶点顺序与 glTF 里的 primitive 顺序一致。
 */
struct gltf_model_data
{
    /** @brief 合并后的顶点，位置 / 法线 / uv 的排布由 vulkan_model_vertex 决定。 */
    std::vector<vulkan_model_vertex> vertices;
    /** @brief 合并后的三角形索引，已经带上前序 primitive 的顶点偏移。 */
    std::vector<uint32_t>            indices;
};

/**
 * @brief 解析 .gltf / .glb 并把所有 primitive 合并成一份顶点与索引数据。
 * @param _file_path 模型文件路径；同目录的外部 .bin 缓冲会被一并读入。
 * @return 成功返回 gltf_model_data；读文件、解析、缺属性、索引越界等失败都返回错误串。
 * @note 缺 NORMAL 时按位置径向朝外兜底（位置在原点则退回 +Y）；缺 TEXCOORD_0 时 uv 保持默认的 (1,1)。
 * @note 只接受带索引的 primitive；POSITION / NORMAL / TEXCOORD_0 的 accessor 下标与计数都会自查。
 */
[[nodiscard]] std::expected<gltf_model_data, std::string> load_model(const std::filesystem::path& _file_path);

}  // namespace model_loader
