/**
 * @file image_helper.cpp
 * @brief image_helper 里两个非模板函数的实现：KTX2 读取（含转码）与 KTX2 落盘。
 * @details read / write / paste / compress 四个模板都在 image_helper.h 里；这里只留需要链接 KTX 库、
 *          又不必按元素类型实例化的部分。read_ktx2 打开文件后无条件转码：HDR 走 KTX_TTF_BC6HU_RGB，
 *          其余走 KTX_TTF_BC7_RGBA；write_ktx2 直接写当前内存中的纹理，不改格式。
 */

#include "image_helper.h"

#include <expected>
#include <filesystem>
#include <format>
#include <string>

/**
 * @brief 打开 .ktx2 并把 UASTC 数据转码成 GPU 压缩格式。
 * @note 无条件转码，不判断文件里是否已经是 BC 格式，全部按 HDR / LDR 选目标格式。
 * @note 转码后 tex->vkFormat 也变成 BC6H / BC7，调用方据此决定 Vulkan 侧的目标格式。
 */
std::expected<ktx2_texture_ptr, std::string> image_helper::read_ktx2(const std::filesystem::path& _ktx2_path)
{
    ktxTexture2* raw_tex = nullptr;
    if (const auto error = ktxTexture2_CreateFromNamedFile(_ktx2_path.generic_string().c_str(),
                                                           KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &raw_tex);
        error != KTX_SUCCESS)
    {
        return std::unexpected(std::format("ktxTexture2_CreateFromNamedFile {} failed: {}.",
                                           _ktx2_path.generic_string(), ktxErrorString(error)));
    }
    ktx2_texture_ptr tex(raw_tex, ktxTexture2_Destroy);

    // after transcode to BC7/BC6H, tex->vkFormat also to BC7/BC6H
    const auto format = ktxTexture2_IsHDR(tex.get()) ? KTX_TTF_BC6HU_RGB : KTX_TTF_BC7_RGBA;
    if (const auto error = ktxTexture2_TranscodeBasis(tex.get(), format, 0); error != KTX_SUCCESS)
    {
        return std::unexpected(std::format("ktxTexture2_TranscodeBasis {} failed: {}.", _ktx2_path.generic_string(),
                                           ktxErrorString(error)));
    }

    return tex;
}

/**
 * @brief 把 KTX2 纹理写成文件，路径由调用方给定。
 * @note 只写当前内存中的格式：若纹理已被 upload_ktx2 转码，写出来的就不是可移植的 UASTC 侧车了。
 */
std::expected<void, std::string> image_helper::write_ktx2(const std::filesystem::path& _ktx2_path, const ktx2_texture_ptr& _tex)
{
    if (const auto error = ktxTexture2_WriteToNamedFile(_tex.get(), _ktx2_path.generic_string().c_str()); error != KTX_SUCCESS)
    {
        return std::unexpected(std::format("ktxTexture2_WriteToNamedFile {} failed: {}.", _ktx2_path.generic_string(),
                                           ktxErrorString(error)));
    }
    return {};
}
