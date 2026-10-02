#pragma once

#ifndef __SLANG__
#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <vulkan/vulkan.hpp>
#endif  // !__SLANG__

#ifdef __SLANG__
#define GPU_MAT4 float4x4
#define GPU_VEC4 float4
#define GPU_VEC3 float3
#define GPU_VEC2 float2
#define GPU_ADDRRESS(T) T*
#define GPU_DEFAULT(...)
#else
#define GPU_MAT4 glm::mat4
#define GPU_VEC4 glm::vec4
#define GPU_VEC3 glm::vec3
#define GPU_VEC2 glm::vec2
#define GPU_ADDRRESS(T) uint64_t
#define GPU_DEFAULT(...) = __VA_ARGS__
#endif  // __SLANG__

enum class light_type : uint32_t
{
    directional = 0,
    point       = 1,
    spot        = 2
};

struct gpu_light_data
{
    GPU_VEC3 color         GPU_DEFAULT(GPU_VEC3(1.f));
    uint32_t active_type   GPU_DEFAULT(0);
    GPU_VEC3 direction     GPU_DEFAULT(GPU_VEC3(0.f, -1.f, 0.f));
    float intensity        GPU_DEFAULT(1.f);
    GPU_VEC3 position      GPU_DEFAULT(GPU_VEC3(0.f));
    float range            GPU_DEFAULT(10.f);
    float inner_cone_angle GPU_DEFAULT(glm::radians(15.f));
    float outer_cone_angle GPU_DEFAULT(glm::radians(30.f));
};

struct gpu_material_data
{
    GPU_VEC3 background_color GPU_DEFAULT(GPU_VEC3(1.f));
    uint32_t texture_index    GPU_DEFAULT(std::numeric_limits<uint32_t>::max());
    GPU_VEC3 foreground_color GPU_DEFAULT(GPU_VEC3(1.f));

    float roughness         GPU_DEFAULT(0.5f);
    GPU_VEC3 emission       GPU_DEFAULT(GPU_VEC3(0.f));
    float emission_strength GPU_DEFAULT(0.f);
    GPU_VEC3 absorption     GPU_DEFAULT(GPU_VEC3(0.f));
    float metallic          GPU_DEFAULT(0.f);
    float reflectance       GPU_DEFAULT(0.5f);
    float ior               GPU_DEFAULT(1.5f);
    float transmission      GPU_DEFAULT(0.f);
    float opacity           GPU_DEFAULT(1.f);
};


struct gpu_model_vertex
{
    GPU_VEC3 position;
    GPU_VEC3 normal;
    GPU_VEC2 uv;

#ifndef __SLANG__
    auto operator<=>(const gpu_model_vertex& _other) const noexcept = default;

    [[nodiscard]] static consteval auto get_binding_description() noexcept
    {
        return vk::VertexInputBindingDescription{0, sizeof(gpu_model_vertex), vk::VertexInputRate::eVertex};
    }

    [[nodiscard]] static consteval auto get_attribute_descriptions() noexcept
    {
        return std::array{
            vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, offsetof(gpu_model_vertex, position)),
            vk::VertexInputAttributeDescription(1, 0, vk::Format::eR32G32B32Sfloat, offsetof(gpu_model_vertex, normal)),
            vk::VertexInputAttributeDescription(2, 0, vk::Format::eR32G32Sfloat, offsetof(gpu_model_vertex, uv)),
        };
    }
#endif  // !__SLANG__
};

struct gpu_model_data
{
    GPU_MAT4 model_matrix   GPU_DEFAULT(GPU_MAT4(1.f));
    uint32_t material_index GPU_DEFAULT(std::numeric_limits<uint32_t>::max());
    GPU_ADDRRESS(gpu_model_vertex) vertex_address GPU_DEFAULT(0u);
    GPU_ADDRRESS(uint32_t) index_address GPU_DEFAULT(0u);
};

struct raster_scene_data
{
    GPU_MAT4 proj_matrix     GPU_DEFAULT(GPU_MAT4(1.f));
    GPU_MAT4 proj_inv_matrix GPU_DEFAULT(GPU_MAT4(1.f));
    GPU_MAT4 view_matrix     GPU_DEFAULT(GPU_MAT4(1.f));
    GPU_MAT4 view_inv_matrix GPU_DEFAULT(GPU_MAT4(1.f));
    GPU_ADDRRESS(gpu_light_data) lights GPU_DEFAULT(0);
    GPU_ADDRRESS(gpu_material_data) materials GPU_DEFAULT(0);
    GPU_ADDRRESS(gpu_model_data) models GPU_DEFAULT(0);
    uint32_t light_count    GPU_DEFAULT(0);
    uint32_t texture_count  GPU_DEFAULT(0);
    uint32_t material_count GPU_DEFAULT(0);
    uint32_t model_count    GPU_DEFAULT(0);
    uint32_t skybox_index   GPU_DEFAULT(std::numeric_limits<uint32_t>::max());
    GPU_VEC3 view_position  GPU_DEFAULT(GPU_VEC3(0.f));
    float exposure          GPU_DEFAULT(1.f);
};

struct rt_scene_data
{
    GPU_MAT4 proj_inv_matrix GPU_DEFAULT(GPU_MAT4(1.f));
    GPU_MAT4 view_inv_matrix GPU_DEFAULT(GPU_MAT4(1.f));
    GPU_ADDRRESS(gpu_light_data) lights GPU_DEFAULT(0);
    GPU_ADDRRESS(gpu_material_data) materials GPU_DEFAULT(0);
    GPU_ADDRRESS(gpu_model_data) models GPU_DEFAULT(0);
    uint32_t light_count    GPU_DEFAULT(0);
    uint32_t texture_count  GPU_DEFAULT(0);
    uint32_t material_count GPU_DEFAULT(0);
    uint32_t model_count    GPU_DEFAULT(0);
    uint32_t skybox_index   GPU_DEFAULT(std::numeric_limits<uint32_t>::max());
    uint32_t frame_index    GPU_DEFAULT(0);
    float exposure          GPU_DEFAULT(1.f);
};

#ifdef __SLANG__
struct gpu_data_layout_check
{
    float model_vertex_must_be_32_bytes[sizeof(gpu_model_vertex) == 32 ? 1 : -1];
    float model_data_must_be_88_bytes[sizeof(gpu_model_data) == 88 ? 1 : -1];
    float material_data_must_be_80_bytes[sizeof(gpu_material_data) == 80 ? 1 : -1];
    float light_data_must_be_56_bytes[sizeof(gpu_light_data) == 56 ? 1 : -1];
    float rt_scene_data_must_be_184_bytes[sizeof(rt_scene_data) == 184 ? 1 : -1];
    float raster_scene_data_must_be_320_bytes[sizeof(raster_scene_data) == 320 ? 1 : -1];
};
#else
static_assert(sizeof(gpu_light_data) == 56);
static_assert(offsetof(gpu_light_data, color) == 0 && offsetof(gpu_light_data, active_type) == 12
              && offsetof(gpu_light_data, direction) == 16 && offsetof(gpu_light_data, intensity) == 28
              && offsetof(gpu_light_data, position) == 32 && offsetof(gpu_light_data, range) == 44
              && offsetof(gpu_light_data, inner_cone_angle) == 48 && offsetof(gpu_light_data, outer_cone_angle) == 52);

static_assert(sizeof(gpu_material_data) == 80);
static_assert(offsetof(gpu_material_data, background_color) == 0 && offsetof(gpu_material_data, texture_index) == 12
              && offsetof(gpu_material_data, foreground_color) == 16 && offsetof(gpu_material_data, roughness) == 28
              && offsetof(gpu_material_data, emission) == 32 && offsetof(gpu_material_data, emission_strength) == 44
              && offsetof(gpu_material_data, absorption) == 48 && offsetof(gpu_material_data, metallic) == 60
              && offsetof(gpu_material_data, reflectance) == 64 && offsetof(gpu_material_data, ior) == 68
              && offsetof(gpu_material_data, transmission) == 72 && offsetof(gpu_material_data, opacity) == 76);

static_assert(sizeof(gpu_model_vertex) == 32);
static_assert(offsetof(gpu_model_vertex, position) == 0 && offsetof(gpu_model_vertex, normal) == 12
              && offsetof(gpu_model_vertex, uv) == 24);

static_assert(sizeof(gpu_model_data) == 88);
static_assert(offsetof(gpu_model_data, model_matrix) == 0 && offsetof(gpu_model_data, material_index) == 64
              && offsetof(gpu_model_data, vertex_address) == 72 && offsetof(gpu_model_data, index_address) == 80);

static_assert(sizeof(raster_scene_data) == 320);
static_assert(offsetof(raster_scene_data, proj_matrix) == 0 && offsetof(raster_scene_data, proj_inv_matrix) == 64
              && offsetof(raster_scene_data, view_matrix) == 128 && offsetof(raster_scene_data, view_inv_matrix) == 192
              && offsetof(raster_scene_data, lights) == 256 && offsetof(raster_scene_data, materials) == 264
              && offsetof(raster_scene_data, models) == 272 && offsetof(raster_scene_data, light_count) == 280
              && offsetof(raster_scene_data, texture_count) == 284 && offsetof(raster_scene_data, material_count) == 288
              && offsetof(raster_scene_data, model_count) == 292 && offsetof(raster_scene_data, skybox_index) == 296
              && offsetof(raster_scene_data, view_position) == 300 && offsetof(raster_scene_data, exposure) == 312);

static_assert(sizeof(rt_scene_data) == 184);
static_assert(offsetof(rt_scene_data, proj_inv_matrix) == 0 && offsetof(rt_scene_data, view_inv_matrix) == 64
              && offsetof(rt_scene_data, lights) == 128 && offsetof(rt_scene_data, materials) == 136
              && offsetof(rt_scene_data, models) == 144 && offsetof(rt_scene_data, light_count) == 152
              && offsetof(rt_scene_data, texture_count) == 156 && offsetof(rt_scene_data, material_count) == 160
              && offsetof(rt_scene_data, model_count) == 164 && offsetof(rt_scene_data, skybox_index) == 168
              && offsetof(rt_scene_data, frame_index) == 172 && offsetof(rt_scene_data, exposure) == 176);
#endif  // __SLANG__
