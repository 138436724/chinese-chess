#pragma once

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // disable error in Imath
#endif                           // _MSC_VER

#include <Imath/half.h>
#include <OpenColorIO/OpenColorIO.h>
#include <OpenColorIO/OpenColorTypes.h>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#ifdef _MSC_VER
#pragma warning(pop)
#endif  // _MSC_VER

constexpr std::string_view OCIOS_PATH         = "resources\\ocios\\";
constexpr std::string_view OCIO_FUNCTION_NAME = "ocio_conversion";

namespace OCIO = OCIO_NAMESPACE;

namespace ocio_helper {

[[nodiscard]] OCIO::GpuShaderDescRcPtr generate_shader_info(const std::filesystem::path& _ocio_path);
[[nodiscard]] std::expected<std::vector<char>, std::string> replace_and_compile(const OCIO::GpuShaderDescRcPtr& _shader_desc,
                                                                                const std::filesystem::path& _shader_path,
                                                                                const std::vector<std::string_view>& _entry_name);
[[nodiscard]] std::vector<uint8_t> get_uniform_buffer_data(const OCIO::GpuShaderDescRcPtr& _shader_desc);

template <typename T>
    requires(std::same_as<T, uint8_t> || std::same_as<T, uint16_t> || std::same_as<T, half> || std::same_as<T, float>)
inline void apply_on_image(const std::filesystem::path& _ocio_path, uint32_t _width, uint32_t _height, std::span<T> _data)
{
    const auto config = OCIO::Config::CreateFromFile(_ocio_path.string().c_str());

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
