#pragma once

#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_sampler.h"

#include <glm/glm.hpp>
#include <memory>

struct scene_image
{
    vulkan_image image;
    sampler_type type = sampler_type::diffuse;
};

struct scene_material
{
    glm::vec3                    background_color = glm::vec3(0.0f, 0.0f, 0.0f);
    glm::vec3                    foreground_color = glm::vec3(0.0f, 0.0f, 0.0f);
    std::shared_ptr<scene_image> alpha_map        = nullptr;
    float                        roughness        = 0.5f;
    float                        metallic         = 0.0f;
    float                        opacity          = 1.0f;
    float                        ior              = 1.5f;
    float                        transmission     = 0.0f;
};
