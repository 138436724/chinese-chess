#include "image_helper.h"

std::expected<ktx2_texture_ptr, std::string> image_helper::read_ktx2(const std::filesystem::path& _ktx2_path) noexcept
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
    const auto format = ktxTexture2_IsHDR(tex.get()) ? KTX_TTF_BC6HU : KTX_TTF_BC7_RGBA;
    if (const auto error = ktxTexture2_TranscodeBasis(tex.get(), format, 0); error != KTX_SUCCESS)
    {
        return std::unexpected(std::format("ktxTexture2_TranscodeBasis {} failed: {}.", _ktx2_path.generic_string(),
                                           ktxErrorString(error)));
    }

    return tex;
}

std::expected<void, std::string> image_helper::write_ktx2(const std::filesystem::path& _ktx2_path, const ktx2_texture_ptr& _tex) noexcept
{
    if (const auto error = ktxTexture2_WriteToNamedFile(_tex.get(), _ktx2_path.generic_string().c_str()); error != KTX_SUCCESS)
    {
        return std::unexpected(std::format("ktxTexture2_WriteToNamedFile {} failed: {}.", _ktx2_path.generic_string(),
                                           ktxErrorString(error)));
    }
    return {};
}
