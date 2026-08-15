#pragma once

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // disable error in Imath
#pragma warning(disable : 4267)  // disable warning in OpenImageIO
#pragma warning(disable : 4244)  // disable warning in OpenImageIO
#endif                           // _MSC_VER

#include <Imath/half.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/typedesc.h>
#include <expected>
#include <filesystem>
#include <format>
#include <ktx.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <vulkan/vulkan.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif  // _MSC_VER

constexpr std::string_view CAPTURES_PATH = "resources\\captures\\";
constexpr std::string_view TEXTURES_PATH = "resources\\textures\\";

template <typename T>
    requires(OIIO::TypeDescFromC<T>().value() != OIIO::TypeDesc::UNKNOWN)
struct image_info
{
    uint32_t       width    = 0;
    uint32_t       height   = 0;
    uint32_t       channels = 0;
    std::vector<T> buffer;
};

using ktx2_texture_ptr = std::unique_ptr<ktxTexture2, decltype(&ktxTexture2_Destroy)>;

namespace image_helper {

template <typename T>
    requires(OIIO::TypeDescFromC<T>().value() != OIIO::TypeDesc::UNKNOWN)
[[nodiscard]] inline std::expected<image_info<T>, std::string> read_image(const std::filesystem::path& _image_path,
                                                                          std::optional<uint32_t> _channels = std::nullopt)
{
    auto image = OIIO::ImageInput::open(_image_path);
    if (!image)
    {
        return std::unexpected(std::format("Failed to open image {}.", _image_path.generic_string()));
    }

    const OIIO::ImageSpec& spec = image->spec();
    image_info<T>          info{
        .width    = static_cast<uint32_t>(spec.width),
        .height   = static_cast<uint32_t>(spec.height),
        .channels = _channels.value_or(static_cast<uint32_t>(spec.nchannels)),
    };

    info.buffer.resize(static_cast<size_t>(info.width) * info.height * info.channels, static_cast<T>(0));
    if (!image->read_image(0, 0, 0, info.channels, OIIO::TypeDescFromC<T>().value(), info.buffer.data(),
                           sizeof(T) * info.channels, sizeof(T) * info.channels * info.width, OIIO::AutoStride))
    {
        return std::unexpected(std::format("Failed to read image {}.", _image_path.generic_string()));
    }

    return info;
}

template <typename T>
    requires(OIIO::TypeDescFromC<T>().value() != OIIO::TypeDesc::UNKNOWN)
[[nodiscard]] inline std::expected<void, std::string> write_image(const std::filesystem::path& _image_path,
                                                                  uint32_t                     _width,
                                                                  uint32_t                     _height,
                                                                  const std::span<const T>     _data,
                                                                  std::optional<uint32_t>      _channels = std::nullopt)
{
    auto image = OIIO::ImageOutput::create(_image_path);
    if (!image)
    {
        return std::unexpected(std::format("Failed to open image {}.", _image_path.generic_string()));
    }

    const uint32_t channels = _channels.value_or(static_cast<uint32_t>(_data.size() / (static_cast<size_t>(_width) * _height)));
    if (!image->open(_image_path, OIIO::ImageSpec(_width, _height, channels, OIIO::TypeDescFromC<T>().value())))
    {
        return std::unexpected(std::format("Cannot open file, please check path: {}", _image_path.generic_string()));
    }
    if (!image->write_image(OIIO::TypeDescFromC<T>().value(), _data.data(), sizeof(T) * channels,
                            sizeof(T) * channels * _width, OIIO::AutoStride))
    {
        return std::unexpected("Cannot write file!");
    }

    image->close();
    return {};
}

template <typename T>
    requires(OIIO::TypeDescFromC<T>().value() != OIIO::TypeDesc::UNKNOWN)
[[nodiscard]] inline std::expected<void, std::string> paste_image(const image_info<T>& _src, image_info<T>& _dst, int _x, int _y)
{
    if (_x < 0 || _y < 0 || static_cast<uint32_t>(_x) + _src.width > _dst.width || static_cast<uint32_t>(_y) + _src.height > _dst.height)
    {
        return std::unexpected("paste_image: source image does not fit in destination canvas.");
    }
    if (_src.channels != _dst.channels && _src.channels != 1)
    {
        return std::unexpected("paste_image: unsupported channel mapping (only 1 to N replication supported).");
    }

    OIIO::ImageBuf dst(OIIO::ImageSpec(_dst.width, _dst.height, _dst.channels, OIIO::TypeDescFromC<T>().value()),
                       OIIO::make_span(_dst.buffer));
    OIIO::ImageBuf src(OIIO::ImageSpec(_src.width, _src.height, _src.channels, OIIO::TypeDescFromC<T>().value()),
                       OIIO::make_cspan(_src.buffer));

    if (_src.channels == _dst.channels)
    {
        if (!OIIO::ImageBufAlgo::paste(dst, _x, _y, 0, 0, src))
        {
            return std::unexpected("paste_image: OIIO ImageBufAlgo::paste failed.");
        }
    }
    else
    {
        for (uint32_t ch = 0; ch < _dst.channels; ++ch)
        {
            if (!OIIO::ImageBufAlgo::paste(dst, _x, _y, 0, static_cast<int>(ch), src))
            {
                return std::unexpected("paste_image: OIIO ImageBufAlgo::paste failed.");
            }
        }
    }
    return {};
}

template <typename T>
    requires(OIIO::TypeDescFromC<T>().value() != OIIO::TypeDesc::UNKNOWN)
[[nodiscard]] inline std::expected<image_info<T>, std::string> convert_channels(const image_info<T>& _src, uint32_t _channels)
{
    if (_channels == _src.channels)
    {
        return _src;
    }
    if (_src.channels != 1 && _channels != 1)
    {
        return std::unexpected("convert_channels: only 1 to N or N to 1 conversions supported.");
    }

    std::vector<int> channelorder(static_cast<size_t>(_channels), 0);  // 全取源通道 0

    image_info<T> result{.width    = _src.width,
                         .height   = _src.height,
                         .channels = _channels,
                         .buffer   = std::vector<T>(_src.buffer.size() / _src.channels * _channels)};

    OIIO::ImageBuf dst(OIIO::ImageSpec(_src.width, _src.height, _channels, OIIO::TypeDescFromC<T>().value()),
                       OIIO::make_span(result.buffer));
    OIIO::ImageBuf src(OIIO::ImageSpec(_src.width, _src.height, _src.channels, OIIO::TypeDescFromC<T>().value()),
                       OIIO::make_cspan(_src.buffer));

    if (!OIIO::ImageBufAlgo::channels(dst, src, static_cast<int>(_channels), OIIO::make_cspan(channelorder)))
    {
        return std::unexpected("convert_channels: OIIO ImageBufAlgo::channels failed.");
    }
    return result;
}

template <typename T>
    requires(OIIO::TypeDescFromC<T>().value() != OIIO::TypeDesc::UNKNOWN)
[[nodiscard]] inline std::expected<ktx2_texture_ptr, std::string> compress_to_ktx2(const image_info<T>& _image, bool _is_hdr) noexcept
{
    if ((_image.width % 4) != 0 || (_image.height % 4) != 0)
    {
        return std::unexpected(std::format("Image size {} or {} is not a multiple of 4.", _image.width, _image.height));
    }

    const auto* pixels      = reinterpret_cast<const uint8_t*>(_image.buffer.data());
    const auto  pixel_bytes = _image.buffer.size() * sizeof(T);

    const ktxTextureCreateInfo create_info{
        .vkFormat        = _is_hdr ? static_cast<ktx_uint32_t>(VK_FORMAT_R16G16B16A16_SFLOAT) :
                                     static_cast<ktx_uint32_t>(VK_FORMAT_R8G8B8A8_UNORM),
        .baseWidth       = _image.width,
        .baseHeight      = _image.height,
        .baseDepth       = 1,
        .numDimensions   = 2,
        .numLevels       = 1,
        .numLayers       = 1,
        .numFaces        = 1,
        .isArray         = KTX_FALSE,
        .generateMipmaps = KTX_FALSE,
    };

    ktxTexture2* raw_tex = nullptr;
    if (const auto error = ktxTexture2_Create(&create_info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &raw_tex); error != KTX_SUCCESS)
    {
        return std::unexpected(std::format("ktxTexture2_Create failed: {}.", ktxErrorString(error)));
    }
    ktx2_texture_ptr tex(raw_tex, ktxTexture2_Destroy);

    if (const auto error = ktxTexture_SetImageFromMemory(reinterpret_cast<ktxTexture*>(tex.get()), 0, 0, 0, pixels, pixel_bytes);
        error != KTX_SUCCESS)
    {
        return std::unexpected(std::format("ktxTexture_SetImageFromMemory failed: {}.", ktxErrorString(error)));
    }

    ktxBasisParams params{
        .structSize  = sizeof(params),
        .codec       = _is_hdr ? static_cast<ktx_basis_codec>(KTX_BASIS_CODEC_UASTC_HDR_4x4) :
                                 static_cast<ktx_basis_codec>(KTX_BASIS_CODEC_UASTC_LDR_4x4),
        .threadCount = std::thread::hardware_concurrency(),
    };

    if (const auto error = ktxTexture2_CompressBasisEx(tex.get(), &params); error != KTX_SUCCESS)
    {
        return std::unexpected(std::format("ktxTexture2_CompressBasisEx failed: {}.", ktxErrorString(error)));
    }

    return tex;
}

[[nodiscard]] std::expected<ktx2_texture_ptr, std::string> read_ktx2(const std::filesystem::path& _ktx2_path) noexcept;

[[nodiscard]] std::expected<void, std::string> write_ktx2(const std::filesystem::path& _ktx2_path, const ktx2_texture_ptr& _tex) noexcept;

}  // namespace image_helper
