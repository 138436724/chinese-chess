#pragma once

#include "gpu_shared_types.h"
#include "vulkan_core/vulkan_acceleration_structure.h"
#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_sampler.h"

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <vector>

struct cpu_light_data
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

struct cpu_image_data
{
    vulkan_image image;
    sampler_type type = sampler_type::diffuse;
};

struct cpu_material_data
{
    // future replace by base_color image
    glm::vec3                       background_color = glm::vec3(0.0f, 0.0f, 0.0f);
    glm::vec3                       foreground_color = glm::vec3(0.0f, 0.0f, 0.0f);
    std::shared_ptr<cpu_image_data> alpha_map        = nullptr;  // only R channel

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

struct cpu_model_info
{
    size_t                        vertex_offset = 0;
    size_t                        index_offset  = 0;
    std::vector<gpu_model_vertex> vertices;
    std::vector<uint32_t>         indices;
    vulkan_acceleration_structure blas_info;
};

struct cpu_model_data
{
    std::shared_ptr<cpu_model_info>    model_info = nullptr;
    std::shared_ptr<cpu_material_data> material   = nullptr;

    bool     is_show      = true;
    uint32_t custom_index = 0;

    glm::mat4 model_matrix = glm::mat4(1.0f);

    [[nodiscard]] vk::AccelerationStructureInstanceKHR get_blas_instance() const noexcept;
};
