#pragma once

#include "scene_handle.h"
#include "vulkan_core/vulkan_data_type.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>

struct scene_light_data
{
    light_type active_type = light_type::directional;

    glm::vec3 color     = glm::vec3(1.f);
    float     intensity = 1.f;

    glm::vec3 direction = glm::vec3(0.f, -1.f, 0.f);

    glm::vec3 position = glm::vec3(0.f);
    float     range    = 10.f;

    float inner_cone_angle = glm::radians(15.f);
    float outer_cone_angle = glm::radians(30.f);
};

struct scene_environment_data
{
    texture_handle image;
    float          intensity = 1.f;
};

struct scene_texture_data
{
    std::filesystem::path source;
    texture_usage         usage = texture_usage::color;
};

struct scene_material_data
{
    std::array<texture_handle, MAX_CHANNEL_COUNT> textures{};

    glm::vec3 albedo            = glm::vec3(1.0f);
    glm::vec3 emission          = glm::vec3(0.0f);
    glm::vec3 absorption        = glm::vec3(0.0f);
    float     roughness         = 0.5f;
    float     metallic          = 0.0f;
    float     reflectance       = 0.5f;
    float     ior               = 1.5f;
    float     transmission      = 0.0f;
    float     opacity           = 1.0f;
    float     emission_strength = 0.0f;
};

struct scene_model_data
{
    mesh_handle     mesh;
    material_handle material;

    bool      is_show      = true;
    glm::mat4 model_matrix = glm::mat4(1.0f);
};
