#pragma once

#ifndef __SLANG__
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <utility>
#endif  // !__SLANG__

#ifdef __SLANG__
#define VULKAN_MAT4 float4x4
#define VULKAN_VEC4 float4
#define VULKAN_VEC3 float3
#define VULKAN_VEC2 float2
#define VULKAN_ADDRESS(T) T*
#define VULKAN_DEFAULT(...)
#else
#define VULKAN_MAT4 glm::mat4
#define VULKAN_VEC4 glm::vec4
#define VULKAN_VEC3 glm::vec3
#define VULKAN_VEC2 glm::vec2
#define VULKAN_ADDRESS(T) uint64_t
#define VULKAN_DEFAULT(...) = __VA_ARGS__
#endif  // __SLANG__

enum class light_type : uint32_t
{
    directional = 0,
    point       = 1,
    spot        = 2
};

enum class texture_channel : uint32_t
{
    albedo,
    normal,
    metallic_roughness,
    occlusion,
    emissive,

    channel_count
};

#ifndef __SLANG__
inline constexpr uint32_t MAX_CHANNEL_COUNT = std::to_underlying(texture_channel::channel_count);
#endif  // !__SLANG__

struct vulkan_light_data
{
    VULKAN_VEC3 color      VULKAN_DEFAULT(VULKAN_VEC3(1.f));
    uint32_t active_type   VULKAN_DEFAULT(0);
    VULKAN_VEC3 direction  VULKAN_DEFAULT(VULKAN_VEC3(0.f, -1.f, 0.f));
    float intensity        VULKAN_DEFAULT(1.f);
    VULKAN_VEC3 position   VULKAN_DEFAULT(VULKAN_VEC3(0.f));
    float range            VULKAN_DEFAULT(10.f);
    float inner_cone_angle VULKAN_DEFAULT(glm::radians(15.f));
    float outer_cone_angle VULKAN_DEFAULT(glm::radians(30.f));
};

struct vulkan_material_data
{
    VULKAN_VEC3 albedo      VULKAN_DEFAULT(VULKAN_VEC3(1.f));
    uint32_t texture_index  VULKAN_DEFAULT(std::numeric_limits<uint32_t>::max());
    float roughness         VULKAN_DEFAULT(0.5f);
    VULKAN_VEC3 emission    VULKAN_DEFAULT(VULKAN_VEC3(0.f));
    float emission_strength VULKAN_DEFAULT(0.f);
    VULKAN_VEC3 absorption  VULKAN_DEFAULT(VULKAN_VEC3(0.f));
    float metallic          VULKAN_DEFAULT(0.f);
    float reflectance       VULKAN_DEFAULT(0.5f);
    float ior               VULKAN_DEFAULT(1.5f);
    float transmission      VULKAN_DEFAULT(0.f);
    float opacity           VULKAN_DEFAULT(1.f);
};


struct vulkan_model_vertex
{
    VULKAN_VEC3 position;
    VULKAN_VEC3 normal;
    VULKAN_VEC2 uv;

#ifndef __SLANG__
    auto operator<=>(const vulkan_model_vertex& _other) const noexcept = default;
#endif  // !__SLANG__
};

struct vulkan_model_data
{
    VULKAN_MAT4 model_matrix VULKAN_DEFAULT(VULKAN_MAT4(1.f));
    uint32_t material_index  VULKAN_DEFAULT(std::numeric_limits<uint32_t>::max());
    VULKAN_ADDRESS(vulkan_model_vertex) vertex_address VULKAN_DEFAULT(0u);
    VULKAN_ADDRESS(uint32_t) index_address VULKAN_DEFAULT(0u);
};

// 环境（天空盒）：背景、IBL 与环境 NEE 共用这一份数据
struct vulkan_environment_data
{
    uint32_t environment_index  VULKAN_DEFAULT(std::numeric_limits<uint32_t>::max());  // bindless 槽，越界 ⇒ 程序化天空
    float environment_intensity VULKAN_DEFAULT(1.f);
};

struct vulkan_raster_scene_data
{
    VULKAN_MAT4 proj_matrix     VULKAN_DEFAULT(VULKAN_MAT4(1.f));
    VULKAN_MAT4 proj_inv_matrix VULKAN_DEFAULT(VULKAN_MAT4(1.f));
    VULKAN_MAT4 view_matrix     VULKAN_DEFAULT(VULKAN_MAT4(1.f));
    VULKAN_MAT4 view_inv_matrix VULKAN_DEFAULT(VULKAN_MAT4(1.f));
    VULKAN_ADDRESS(vulkan_light_data) lights VULKAN_DEFAULT(0);
    VULKAN_ADDRESS(vulkan_material_data) materials VULKAN_DEFAULT(0);
    VULKAN_ADDRESS(vulkan_model_data) models VULKAN_DEFAULT(0);
    uint32_t light_count        VULKAN_DEFAULT(0);
    uint32_t texture_count      VULKAN_DEFAULT(0);
    uint32_t material_count     VULKAN_DEFAULT(0);
    uint32_t model_count        VULKAN_DEFAULT(0);
    uint32_t environment_index  VULKAN_DEFAULT(std::numeric_limits<uint32_t>::max());
    VULKAN_VEC3 view_position   VULKAN_DEFAULT(VULKAN_VEC3(0.f));
    float exposure              VULKAN_DEFAULT(1.f);
    float environment_intensity VULKAN_DEFAULT(1.f);
};

struct vulkan_rt_scene_data
{
    VULKAN_MAT4 proj_inv_matrix VULKAN_DEFAULT(VULKAN_MAT4(1.f));
    VULKAN_MAT4 view_inv_matrix VULKAN_DEFAULT(VULKAN_MAT4(1.f));
    VULKAN_ADDRESS(vulkan_light_data) lights VULKAN_DEFAULT(0);
    VULKAN_ADDRESS(vulkan_material_data) materials VULKAN_DEFAULT(0);
    VULKAN_ADDRESS(vulkan_model_data) models VULKAN_DEFAULT(0);
    uint32_t light_count        VULKAN_DEFAULT(0);
    uint32_t texture_count      VULKAN_DEFAULT(0);
    uint32_t material_count     VULKAN_DEFAULT(0);
    uint32_t model_count        VULKAN_DEFAULT(0);
    uint32_t environment_index  VULKAN_DEFAULT(std::numeric_limits<uint32_t>::max());
    uint32_t frame_index        VULKAN_DEFAULT(0);
    float exposure              VULKAN_DEFAULT(1.f);
    float environment_intensity VULKAN_DEFAULT(1.f);
};

#ifdef __SLANG__
struct vulkan_data_layout_check
{
    float model_vertex_must_be_32_bytes[sizeof(vulkan_model_vertex) == 32 ? 1 : -1];
    float model_data_must_be_88_bytes[sizeof(vulkan_model_data) == 88 ? 1 : -1];
    float material_data_must_be_68_bytes[sizeof(vulkan_material_data) == 68 ? 1 : -1];
    float light_data_must_be_56_bytes[sizeof(vulkan_light_data) == 56 ? 1 : -1];
    float rt_scene_data_must_be_184_bytes[sizeof(vulkan_rt_scene_data) == 184 ? 1 : -1];
    float raster_scene_data_must_be_320_bytes[sizeof(vulkan_raster_scene_data) == 320 ? 1 : -1];
    float environment_data_must_be_8_bytes[sizeof(vulkan_environment_data) == 8 ? 1 : -1];
};
#else
static_assert(sizeof(vulkan_light_data) == 56);
static_assert(offsetof(vulkan_light_data, color) == 0 && offsetof(vulkan_light_data, active_type) == 12
              && offsetof(vulkan_light_data, direction) == 16 && offsetof(vulkan_light_data, intensity) == 28
              && offsetof(vulkan_light_data, position) == 32 && offsetof(vulkan_light_data, range) == 44
              && offsetof(vulkan_light_data, inner_cone_angle) == 48 && offsetof(vulkan_light_data, outer_cone_angle) == 52);

static_assert(sizeof(vulkan_material_data) == 68);
static_assert(offsetof(vulkan_material_data, albedo) == 0 && offsetof(vulkan_material_data, texture_index) == 12
              && offsetof(vulkan_material_data, roughness) == 16 && offsetof(vulkan_material_data, emission) == 20
              && offsetof(vulkan_material_data, emission_strength) == 32
              && offsetof(vulkan_material_data, absorption) == 36 && offsetof(vulkan_material_data, metallic) == 48
              && offsetof(vulkan_material_data, reflectance) == 52 && offsetof(vulkan_material_data, ior) == 56
              && offsetof(vulkan_material_data, transmission) == 60 && offsetof(vulkan_material_data, opacity) == 64);

static_assert(sizeof(vulkan_model_vertex) == 32);
static_assert(offsetof(vulkan_model_vertex, position) == 0 && offsetof(vulkan_model_vertex, normal) == 12
              && offsetof(vulkan_model_vertex, uv) == 24);

static_assert(sizeof(vulkan_model_data) == 88);
static_assert(offsetof(vulkan_model_data, model_matrix) == 0 && offsetof(vulkan_model_data, material_index) == 64
              && offsetof(vulkan_model_data, vertex_address) == 72 && offsetof(vulkan_model_data, index_address) == 80);

static_assert(sizeof(vulkan_environment_data) == 8);
static_assert(offsetof(vulkan_environment_data, environment_index) == 0
              && offsetof(vulkan_environment_data, environment_intensity) == 4);

static_assert(sizeof(vulkan_raster_scene_data) == 320);
static_assert(offsetof(vulkan_raster_scene_data, proj_matrix) == 0 && offsetof(vulkan_raster_scene_data, proj_inv_matrix) == 64
              && offsetof(vulkan_raster_scene_data, view_matrix) == 128 && offsetof(vulkan_raster_scene_data, view_inv_matrix) == 192
              && offsetof(vulkan_raster_scene_data, lights) == 256 && offsetof(vulkan_raster_scene_data, materials) == 264
              && offsetof(vulkan_raster_scene_data, models) == 272 && offsetof(vulkan_raster_scene_data, light_count) == 280
              && offsetof(vulkan_raster_scene_data, texture_count) == 284
              && offsetof(vulkan_raster_scene_data, material_count) == 288 && offsetof(vulkan_raster_scene_data, model_count) == 292
              && offsetof(vulkan_raster_scene_data, environment_index) == 296
              && offsetof(vulkan_raster_scene_data, view_position) == 300 && offsetof(vulkan_raster_scene_data, exposure) == 312
              && offsetof(vulkan_raster_scene_data, environment_intensity) == 316);

static_assert(sizeof(vulkan_rt_scene_data) == 184);
static_assert(offsetof(vulkan_rt_scene_data, proj_inv_matrix) == 0 && offsetof(vulkan_rt_scene_data, view_inv_matrix) == 64
              && offsetof(vulkan_rt_scene_data, lights) == 128 && offsetof(vulkan_rt_scene_data, materials) == 136
              && offsetof(vulkan_rt_scene_data, models) == 144 && offsetof(vulkan_rt_scene_data, light_count) == 152
              && offsetof(vulkan_rt_scene_data, texture_count) == 156 && offsetof(vulkan_rt_scene_data, material_count) == 160
              && offsetof(vulkan_rt_scene_data, model_count) == 164 && offsetof(vulkan_rt_scene_data, environment_index) == 168
              && offsetof(vulkan_rt_scene_data, frame_index) == 172 && offsetof(vulkan_rt_scene_data, exposure) == 176
              && offsetof(vulkan_rt_scene_data, environment_intensity) == 180);
#endif  // __SLANG__
