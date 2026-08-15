#pragma once
#include "tools/model_loader.h"
#include "vulkan_core/vulkan_acceleration_structure.h"

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <vector>

struct scene_material;

struct model_information
{
    size_t                        vertex_offset = 0;
    size_t                        index_offset  = 0;
    std::vector<model_vertex>     vertices;
    std::vector<uint32_t>         indices;
    vulkan_acceleration_structure blas_info;
};

struct scene_model
{
    [[nodiscard]] vk::AccelerationStructureInstanceKHR get_blas_instance() const noexcept;

    std::shared_ptr<model_information> model_info = nullptr;
    std::shared_ptr<scene_material>    material   = nullptr;

    bool     is_show      = true;
    uint32_t custom_index = 0;

    glm::mat4 model_matrix = glm::mat4(1.0f);
};
