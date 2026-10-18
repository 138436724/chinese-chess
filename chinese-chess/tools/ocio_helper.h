/**
 * @file ocio_helper.h
 * @brief OpenColorIO 集成：取 GPU shader 描述、替换着色器里的 OCIO 桩并编译，以及 CPU 侧的图像转换。
 * @details GPU 侧与 CPU 侧共用同一份 config 与同一套「默认 display / view」：GPU 走
 *          generate_shader_info + replace_and_compile，CPU 走 apply_on_image，两条路径都从
 *          ROLE_SCENE_LINEAR 转到 config 的默认显示变换，截图与实时预览才对得上。
 *          OCIO 的 C++ 命名空间被别名成 OCIO，下面所有接口都用这个别名。
 */

#pragma once

#include <Imath/half.h>
#include <OpenColorIO/OpenColorIO.h>
#include <OpenColorIO/OpenColorTypes.h>
#include <concepts>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

constexpr std::string_view OCIOS_PATH         = "resources\\ocios\\";
constexpr std::string_view OCIO_FUNCTION_NAME = "ocio_conversion";

namespace OCIO = OCIO_NAMESPACE;

namespace ocio_helper {

/**
 * @brief 从 OCIO config 生成 GPU shader 描述，供替换着色器桩与打包 uniform 使用。
 * @param _ocio_path OCIO config 文件路径。
 * @return 已提取 GPU shader 信息的描述符；语言固定 HLSL SM 5.0，函数名 ocio_conversion，资源前缀 ocio_。
 * @throws OCIO::Exception config 打不开、默认 display / view 不存在或处理器创建失败时由 OCIO 抛出。
 * @note 变换固定为「ROLE_SCENE_LINEAR -> 默认 display 的默认 view」，与 apply_on_image 完全一致。
 */
[[nodiscard]] OCIO::GpuShaderDescRcPtr generate_shader_info(const std::filesystem::path& _ocio_path);
/**
 * @brief 把 OCIO 生成的函数体替换进着色器桩文件，再整段编译成 SPIR-V。
 * @param _shader_desc OCIO shader 描述，提供函数文本。
 * @param _shader_path 含 ocio_conversion 桩的着色器文件（vulkan_blend_image.slang）。
 * @param _entry_name 入口点名字列表，原样交给 shader_compiler。
 * @return 成功返回 SPIR-V 字节；编译失败返回错误串。
 * @throws std::runtime_error 桩着色器文件打不开。
 * @note 替换是整行级的：只把含 "float4 ocio_conversion" 的那一行换成生成的整段函数，其余行原样保留。
 * @note 走的是编译字符串的那条入口，不参与磁盘着色器缓存，每次启动都会重新编译。
 */
[[nodiscard]] std::expected<std::vector<char>, std::string> replace_and_compile(const OCIO::GpuShaderDescRcPtr& _shader_desc,
                                                                                const std::filesystem::path& _shader_path,
                                                                                const std::vector<std::string_view>& _entry_name);
/**
 * @brief 把 OCIO 的 uniform 列表打包成与 GPU 一致的字节块。
 * @param _shader_desc 生成时拿到的 shader 描述。
 * @return 长度等于 getUniformBufferSize() 的字节向量，没有 uniform 写入的位置保持 0。
 * @note 标量写 4 字节、float3 只写 12 字节，数组元素之间留 16 字节步长（std140 对齐）。
 * @note 写入偏移直接取 OCIO 给的 m_bufferOffset，本函数不自己算布局。
 */
[[nodiscard]] std::vector<uint8_t> get_uniform_buffer_data(const OCIO::GpuShaderDescRcPtr& _shader_desc);

/**
 * @brief 用 OCIO 在 CPU 上把 scene-linear 像素转成显示空间，原地修改。
 * @param _ocio_path OCIO config 文件路径。
 * @param _width 图像宽度（像素），必须非零。
 * @param _height 图像高度（像素），必须非零。
 * @param _data 像素数据，元素类型 T 决定 OCIO 位深，通道数由 _data.size() / (width * height) 推出。
 * @throws OCIO::Exception config 打不开或处理器创建失败时由 OCIO 抛出。
 * @note 只接受 uint8_t / uint16_t / half / float 四种元素类型，其余被 requires 拒绝。
 * @note 输入输出的元素类型与位深保持一致；整数位深的颜色转换仍受量化精度与数值范围限制。
 * @note 每次调用都重建 config 与 processor，并用 OPTIMIZATION_ALL 取优化后的 CPU 处理器。
 */
template <typename T>
    requires(std::same_as<T, uint8_t> || std::same_as<T, uint16_t> || std::same_as<T, half> || std::same_as<T, float>)
inline void apply_on_image(const std::filesystem::path& _ocio_path, uint32_t _width, uint32_t _height, std::span<T> _data)
{
    const auto config = OCIO::Config::CreateFromFile(_ocio_path.generic_string().c_str());

    const char* const display = config->getDefaultDisplay();
    const char* const view    = config->getDefaultView(display);

    const auto transform = OCIO::DisplayViewTransform::Create();
    transform->setSrc(OCIO::ROLE_SCENE_LINEAR);
    transform->setDisplay(display);
    transform->setView(view);

    OCIO::BitDepth depth = OCIO::BIT_DEPTH_UNKNOWN;
    if constexpr (std::is_same_v<T, uint8_t>)
    {
        depth = OCIO::BIT_DEPTH_UINT8;
    }
    else if constexpr (std::is_same_v<T, uint16_t>)
    {
        depth = OCIO::BIT_DEPTH_UINT16;
    }
    else if constexpr (std::is_same_v<T, half>)
    {
        depth = OCIO::BIT_DEPTH_F16;
    }
    else if constexpr (std::is_same_v<T, float>)
    {
        depth = OCIO::BIT_DEPTH_F32;
    }

    const auto processor     = config->getProcessor(transform);
    const auto cpu_processor = processor->getOptimizedCPUProcessor(depth, depth, OCIO::OPTIMIZATION_ALL);

    OCIO::PackedImageDesc image(_data.data(), static_cast<long>(_width), static_cast<long>(_height),
                                static_cast<long>(_data.size() / (_width * _height)), depth, OCIO::AutoStride,
                                OCIO::AutoStride, OCIO::AutoStride);
    cpu_processor->apply(image);
}

}  // namespace ocio_helper
