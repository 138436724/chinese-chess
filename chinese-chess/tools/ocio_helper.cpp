/**
 * @file ocio_helper.cpp
 * @brief ocio_helper 的实现：GPU shader 描述生成、桩函数整行替换后编译，以及 uniform 缓冲打包。
 * @details 描述符固定用 HLSL SM 5.0 语言（与 Slang 语法接近）、函数名 ocio_conversion、资源前缀 ocio_。
 *          替换按行进行：只把含 "float4 ocio_conversion" 的那一行换成 OCIO 生成的整段函数文本，
 *          其余行原样保留，再用 '\n' 拼回去交给 shader_compiler 编译。
 *          uniform 打包按 std140 手工写：float3 只写 12 字节，数组元素之间留 16 字节步长。
 */

#include "ocio_helper.h"

#include "shader_compiler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief 建 config、取默认 display / view 的显示变换，再抽取 GPU shader 信息。
 * @note 先 getOptimizedGPUProcessor 再 extractGpuShaderInfo；语言 / 函数名 / 资源前缀在这里定死，
 *       下游 replace_and_compile 与 uniform 打包都依赖这几个名字。
 */
OCIO::GpuShaderDescRcPtr ocio_helper::generate_shader_info(const std::filesystem::path& _ocio_path)
{
    const auto ocio_function_name = std::string(OCIO_FUNCTION_NAME);

    const auto config = OCIO::Config::CreateFromFile(_ocio_path.generic_string().c_str());

    const char* const display = config->getDefaultDisplay();
    const char* const view    = config->getDefaultView(display);

    const auto transform = OCIO::DisplayViewTransform::Create();
    transform->setSrc(OCIO::ROLE_SCENE_LINEAR);
    transform->setDisplay(display);
    transform->setView(view);

    const auto processor     = config->getProcessor(transform);
    const auto gpu_processor = processor->getOptimizedGPUProcessor(OCIO::OPTIMIZATION_ALL);

    auto shader_desc = OCIO::GpuShaderDesc::CreateShaderDesc();
    shader_desc->setLanguage(OCIO::GPU_LANGUAGE_HLSL_SM_5_0);  // hlsl and slang are very similar
    shader_desc->setFunctionName(ocio_function_name.c_str());
    shader_desc->setResourcePrefix("ocio_");

    gpu_processor->extractGpuShaderInfo(shader_desc);

    return shader_desc;
}

/**
 * @brief 按行替换桩函数后编译：只有含 "float4 ocio_conversion" 的行会被整行换掉。
 * @note 其余行按 '\n' 分割后拼回；不主动追加换行，源文件原有尾换行可被保留。
 * @note 桩文件读失败抛 std::runtime_error；编译失败把 shader_compiler 的错误串原样返回。
 */
std::expected<std::vector<char>, std::string> ocio_helper::replace_and_compile(const OCIO::GpuShaderDescRcPtr& _shader_desc,
                                                                               const std::filesystem::path& _shader_path,
                                                                               const std::vector<std::string_view>& _entry_name)
{
    const auto ocio_function_name = ("float4 " + std::string(OCIO_FUNCTION_NAME));

    const std::string shader_string = _shader_desc->getShaderText();

    // 在调用方指定的着色器中整行替换 OCIO 桩；桩函数必须完整地位于同一行。
    std::ifstream shader_file(_shader_path);
    if (!shader_file)
    {
        throw std::runtime_error("Cannot open ocio slang file!");
    }

    const std::string original_string((std::istreambuf_iterator<char>(shader_file)), std::istreambuf_iterator<char>());
    shader_file.close();

    const std::string final_string = original_string | std::views::split('\n') | std::views::transform([&](const auto& _line) {
                                         const std::string s(_line.begin(), _line.end());
                                         if (s.contains(ocio_function_name)) [[unlikely]]
                                         {
                                             return shader_string;
                                         }
                                         return s;
                                     })
                                     | std::views::join_with('\n') | std::ranges::to<std::string>();

    return SHADER_COMPILER.compile_shader_to_spv(final_string, _entry_name);
}

/**
 * @brief 逐 uniform 把值写进按 getUniformBufferSize() 分配的字节块。
 * @note double 先收窄成 float、bool 写成 int，float3 只拷 12 字节；数组按 16 字节步长填。
 * @note 认不出的 uniform（既不是标量也不是数组）直接跳过，对应字节保持 0。
 */
std::vector<uint8_t> ocio_helper::get_uniform_buffer_data(const OCIO::GpuShaderDescRcPtr& _shader_desc)
{
    std::vector<uint8_t> buffer(_shader_desc->getUniformBufferSize());

    std::ranges::for_each(std::views::iota(0u, _shader_desc->getNumUniforms()), [&](const uint32_t i) {
        OCIO::GpuShaderDesc::UniformData uniform_data;
        _shader_desc->getUniform(i, uniform_data);

        uint8_t* dest = buffer.data() + uniform_data.m_bufferOffset;
        if (uniform_data.m_getDouble)
        {
            const float val = static_cast<float>(uniform_data.m_getDouble());
            std::memcpy(dest, &val, sizeof(float));
        }
        else if (uniform_data.m_getBool)
        {
            const int val = uniform_data.m_getBool() ? 1 : 0;
            std::memcpy(dest, &val, sizeof(int));
        }
        else if (uniform_data.m_getFloat3)
        {
            // 这里只写 3 个 float；后续字段位置由 OCIO 的 m_bufferOffset 决定。
            const auto vals = uniform_data.m_getFloat3();
            std::memcpy(dest, vals.data(), 3 * sizeof(float));
        }
        else if (uniform_data.m_vectorFloat.m_getSize && uniform_data.m_vectorFloat.m_getVector)
        {
            // In std140, each array element is padded to 16 bytes
            const float* const vals  = uniform_data.m_vectorFloat.m_getVector();
            const size_t       count = uniform_data.m_vectorFloat.m_getSize();
            for (size_t j = 0; j < count; ++j)
            {
                std::memcpy(dest + j * 16, &vals[j], sizeof(float));
            }
        }
        else if (uniform_data.m_vectorInt.m_getSize && uniform_data.m_vectorInt.m_getVector)
        {
            // In std140, each array element is padded to 16 bytes
            const int* const vals  = uniform_data.m_vectorInt.m_getVector();
            const size_t     count = uniform_data.m_vectorInt.m_getSize();
            for (size_t j = 0; j < count; ++j)
            {
                std::memcpy(dest + j * 16, &vals[j], sizeof(int));
            }
        }
    });

    return buffer;
}
