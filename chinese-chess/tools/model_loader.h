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
 *          当前按单一几何体使用；结果不保留 primitive 材质、拓扑或节点实例边界。
 */
struct gltf_model_data
{
    /** @brief 合并后的顶点，位置 / 法线 / uv 的排布由 vulkan_model_vertex 决定。 */
    std::vector<vulkan_model_vertex> vertices;
    /** @brief 合并后的原始索引，已加前序 primitive 顶点偏移；未检查或转换 primitive 拓扑。 */
    std::vector<uint32_t>            indices;
};

/**
 * @brief 解析 .gltf / .glb 并把所有 primitive 合并成一份顶点与索引数据。
 * @param _file_path 模型文件路径；同目录的外部 .bin 缓冲会被一并读入。
 * @return 成功返回 gltf_model_data；显式检测到的读取、解析、缺 POSITION 或索引错误返回错误串，底层异常仍可能传播。
 * @note 缺 NORMAL 时按位置径向朝外兜底（位置长度平方不大于 1e-12 时退回 +Y）；缺 TEXCOORD_0 时 uv 保持默认的 (1,1)。
 * @note 检查 POSITION 的 accessor 下标，并要求已有 NORMAL / TEXCOORD_0 的计数与 POSITION 一致。
 * @note 只接受带索引的 primitive，但不验证拓扑或索引数是否为 3 的倍数；当前场景调用方须提供三角形列表。
 */
[[nodiscard]] std::expected<gltf_model_data, std::string> load_model(const std::filesystem::path& _file_path);

}  // namespace model_loader
