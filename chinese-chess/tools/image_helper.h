/**
 * @file image_helper.h
 * @brief 图像读写与 KTX2（UASTC）压缩：OIIO 负责 EXR / PNG 等常规格式，KTX 负责 GPU 纹理容器。
 * @details read_image / write_image / paste_image 是 OIIO 的薄封装，元素类型 T 由 OIIO::TypeDescFromC
 *          限定（half / float / uint8_t 等），缓冲区一律行紧密排列。compress_to_ktx2 在 CPU 侧把 4 通道
 *          图压成 UASTC 的 KTX2（HDR 走 UASTC HDR 4x4），只在贴图与字形图集合成之后使用；真正的
 *          BC6H / BC7 转码发生在读取或上传时。两个路径常量相对运行目录 chinese-chess/。
 */
#pragma once

#include <Imath/half.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/typedesc.h>
#include <cstddef>
#include <cstdint>
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

constexpr std::string_view CAPTURES_PATH = "resources\\captures\\";
constexpr std::string_view TEXTURES_PATH = "resources\\textures\\";

/**
 * @brief 一张解包后的图像：尺寸、通道数与行紧密排列的像素。
 * @details 调用方应保持 buffer.size() == width * height * channels，结构本身不强制该约束。元素类型为 T，行间无
 *          padding，可以直接交给 OIIO 的 ImageBuf 或 KTX2 的 ktxTexture_SetImageFromMemory。
 */
template <typename T>
    requires(OIIO::TypeDescFromC<T>().value() != OIIO::TypeDesc::UNKNOWN)
struct image_info
{
    uint32_t       width    = 0;  ///< @brief 图像宽度（像素）
    uint32_t       height   = 0;  ///< @brief 图像高度（像素）
    uint32_t       channels = 0;  ///< @brief 每像素的通道数，buffer 的长度由它和宽高共同决定
    std::vector<T> buffer;  ///< @brief 行紧密排列的像素数据，长度为 width * height * channels
};

/** @brief 带 KTX 销毁器的 ktxTexture2 智能指针，所有失败路径都不会漏释放。 */
using ktx2_texture_ptr = std::unique_ptr<ktxTexture2, decltype(&ktxTexture2_Destroy)>;

namespace image_helper {

/**
 * @brief 用 OIIO 打开图像并按元素类型 T 解包成行紧密的像素数组。
 * @param _image_path 图像路径，格式由 OIIO 按扩展名或文件头判断。
 * @param _channels 请求读取的通道数；缺省取源图全部通道。大于源通道数时不自动补 alpha 为 1。
 * @return 成功返回 image_info<T>；打开失败或 read_image 失败都返回错误串。
 * @note 缓冲区先按 width * height * channels 填 0，行跨度按 channels 计算。
 * @note 固定读取 subimage 0、mip 0，按二维图分配；调用方应提供非深度、非体积图像。
 * @note 像素类型转换交给 OIIO；expected 只承载显式检查的失败，内存分配等异常仍可能传播。
 */
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

/**
 * @brief 把行紧密的像素写成图像文件，格式由扩展名决定。
 * @param _image_path 输出路径（.exr / .png 等）。
 * @param _width 图像宽度（像素）。
 * @param _height 图像高度（像素）。
 * @param _data 像素数据，长度应为 _width * _height * 通道数。
 * @param _channels 通道数；缺省由 _data.size() / (_width * _height) 整除推出。
 * @return 成功返回空 expected；创建输出、宽高为 0、open 失败或 write_image 失败都返回错误串。
 * @note 只校验宽高非零；给定 _channels 时不核对 _data 的长度是否够，数据不足会让 OIIO 越界读。
 * @note write_image 成功后显式 close()，但不检查 close 的结果；返回成功不保证最终关闭写入也成功。
 */
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

    if (_width == 0 || _height == 0)
    {
        return std::unexpected(std::format("Failed to open image with {}x{}.", _width, _height));
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

/**
 * @brief 把一张小图按左上角坐标贴到目标画布上，原地修改目标。
 * @param _src 源图像，通道数要么与目标相同，要么为 1。
 * @param _dst 目标画布，尺寸与通道数不变，只覆盖对应区域。
 * @param _x 粘贴区域左上角在目标上的列坐标，从 0 开始。
 * @param _y 粘贴区域左上角在目标上的行坐标，从 0 开始。
 * @return 成功返回空 expected；越界、通道组合不支持或 OIIO paste 失败都返回错误串。
 * @note 通道数相同时一次 paste；_src 只有 1 个通道时逐个目标通道复制（按通道广播）。
 * @note 两个 ImageBuf 包住调用方 vector，不复制像素；调用方须保证缓冲长度与尺寸一致，坐标加法不溢出。
 * @note 失败不回滚已写像素；逐通道复制时，前面通道可能已经修改。
 */
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

/**
 * @brief 把 4 通道图像压成 UASTC 的 KTX2 纹理。
 * @param _image 源图像；宽高需非零且为 4 的倍数，通道数为 4；本函数显式检查倍数和通道数，零尺寸交给 KTX。
 * @param _is_hdr true 时按 R16G16B16A16_SFLOAT 建纹理并用 UASTC HDR 4x4 压缩，false 时按 R8G8B8A8_UNORM + UASTC LDR 4x4。
 * @return 成功返回持有 ktxTexture2 的指针；建纹理、填像素或压缩失败都返回错误串。
 * @note 只建 1 个 mip 层、不生成 mipmap；压缩线程数取 std::thread::hardware_concurrency()。
 * @note 返回的纹理仍是 UASTC 数据，转成 BC6H / BC7 发生在 upload_ktx2（上传）或 read_ktx2（读侧车）里。
 * @note 会核对 buffer.size() 是否等于 width * height * channels；不会转换像素类型。
 * @pre HDR 输入须使用 half（FP16）存储，LDR 输入须使用 uint8_t；requires 未限制 T 与 _is_hdr 的组合。
 */
template <typename T>
    requires(OIIO::TypeDescFromC<T>().value() != OIIO::TypeDesc::UNKNOWN)
[[nodiscard]] inline std::expected<ktx2_texture_ptr, std::string> compress_to_ktx2(const image_info<T>& _image, bool _is_hdr)
{
    if ((_image.width % 4) != 0 || (_image.height % 4) != 0 || (_image.channels != 4))
    {
        return std::unexpected(std::format("Image size {} or {} or channel {} is not a multiple of 4.", _image.width,
                                           _image.height, _image.channels));
    }

    if (_image.buffer.size() != static_cast<size_t>(_image.width) * _image.height * _image.channels)
    {
        return std::unexpected("Image need pixel not equal to provdes.");
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

/**
 * @brief 从 .ktx2 文件读入纹理并转码成 BC6H（HDR）或 BC7（LDR）。
 * @param _ktx2_path 侧车文件路径。
 * @return 成功返回已转码的 ktxTexture2；打开或转码失败返回错误串。
 * @note 用 KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT 连像素数据一起读进内存；转码后 vkFormat 也随之变成 BC6H / BC7。
 */
[[nodiscard]] std::expected<ktx2_texture_ptr, std::string> read_ktx2(const std::filesystem::path& _ktx2_path);

/**
 * @brief 把内存里的 KTX2 纹理写成 .ktx2 文件（即写侧车）。
 * @param _ktx2_path 目标文件路径。
 * @param _tex 非空源纹理，只借用不转移所有权；本函数不判空。
 * @return 成功返回空 expected；写文件失败返回错误串。
 * @note 必须在转码成 BC6H / BC7 之前调用，否则侧车就不再是可移植的 UASTC 中间格式。
 */
[[nodiscard]] std::expected<void, std::string> write_ktx2(const std::filesystem::path& _ktx2_path, const ktx2_texture_ptr& _tex);

}  // namespace image_helper
