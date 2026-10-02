#pragma once

#include "shared/gpu_shared_types.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

constexpr std::string_view MODELS_PATH = "resources\\models\\";


namespace model_loader {

struct gltf_model_data
{
    std::vector<gpu_model_vertex> vertices;
    std::vector<uint32_t>         indices;
};

[[nodiscard]] std::expected<gltf_model_data, std::string> load_model(const std::filesystem::path& _file_path) noexcept;

}  // namespace model_loader
