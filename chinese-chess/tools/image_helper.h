#pragma once

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // disable error in Imath
#pragma warning(disable : 4267)  // disable warning in OpenImageIO
#pragma warning(disable : 4244)  // disable warning in OpenImageIO
#endif                           // _MSC_VER

#include <Imath/half.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/typedesc.h>
#include <filesystem>
#include <span>
#include <string_view>

#ifdef _MSC_VER
#pragma warning(pop)
#endif  // _MSC_VER

#define IMAGE_HELPER image_helper::get_image_help()
constexpr std::u8string_view CAPTURES_PATH = u8"resources\\captures\\";
constexpr std::u8string_view TEXTURES_PATH = u8"resources\\textures\\";

template <typename T>
    requires(std::is_arithmetic_v<T> || std::is_same_v<T, half>)
struct image_info
{
    uint32_t       width    = 0;
    uint32_t       height   = 0;
    uint32_t       channels = 0;
    std::vector<T> buffer;
};

class image_helper
{
public:
    template <typename T>
        requires(OIIO::TypeDescFromC<T>().value() != OIIO::TypeDesc::UNKNOWN)
    image_info<T> read_image(const std::filesystem::path& _image_path, std::optional<uint32_t> _channels = std::nullopt);

    template <typename T>
        requires(OIIO::TypeDescFromC<T>().value() != OIIO::TypeDesc::UNKNOWN)
    void write_image(const std::filesystem::path& _image_path,
                     uint32_t                     _width,
                     uint32_t                     _height,
                     const std::span<T>           _data,
                     std::optional<uint32_t>      _channels = std::nullopt);

    static image_helper& get_image_help() noexcept;

private:
    image_helper()                                = default;
    ~image_helper()                               = default;
    image_helper(const image_helper&)             = delete;
    image_helper& operator=(const image_helper&)  = delete;
    image_helper(const image_helper&&)            = delete;
    image_helper& operator=(const image_helper&&) = delete;

    static image_helper helper;
};

template <typename T>
    requires(OIIO::TypeDescFromC<T>().value() != OIIO::TypeDesc::UNKNOWN)
inline image_info<T> image_helper::read_image(const std::filesystem::path& _image_path, std::optional<uint32_t> _channels)
{
    auto image = OIIO::ImageInput::open(_image_path);
    if (!image)
    {
        throw std::runtime_error(std::format("Failed to open image {}.", _image_path.string()));
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
        throw std::runtime_error(std::format("Failed to read image {}.", _image_path.string()));
    }

    return info;
}

template <typename T>
    requires(OIIO::TypeDescFromC<T>().value() != OIIO::TypeDesc::UNKNOWN)
inline void image_helper::write_image(const std::filesystem::path& _image_path,
                                      uint32_t                     _width,
                                      uint32_t                     _height,
                                      const std::span<T>           _data,
                                      std::optional<uint32_t>      _channels)
{
    auto image = OIIO::ImageOutput::create(_image_path);

    uint32_t channels = _channels.value_or(static_cast<uint32_t>(_data.size() / (static_cast<size_t>(_width) * _height)));
    if (!image->open(_image_path, OIIO::ImageSpec(_width, _height, channels, OIIO::TypeDescFromC<T>().value())))
    {
        throw std::runtime_error(std::format("Can not open file, please check path: {}", _image_path.generic_string()));
    }
    if (!image->write_image(OIIO::TypeDescFromC<T>().value(), _data.data(), sizeof(T) * channels,
                            sizeof(T) * channels * _width, OIIO::AutoStride))
    {
        throw std::runtime_error("Can not write file!");
    }

    image->close();
}
