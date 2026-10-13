#pragma once

#include <compare>
#include <cstdint>

enum class texture_usage : uint32_t
{
    color,
    environment,
    glyph
};

template <typename T>
struct scene_handle
{
    static constexpr uint32_t npos = 0xFFFFFFFFu;

    uint32_t index      = npos;
    uint32_t generation = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return index != npos; }

    auto operator<=>(const scene_handle&) const noexcept = default;
};

struct light_tag;
struct texture_tag;
struct material_tag;
struct mesh_tag;
struct model_tag;

using light_handle    = scene_handle<light_tag>;
using texture_handle  = scene_handle<texture_tag>;
using material_handle = scene_handle<material_tag>;
using mesh_handle     = scene_handle<mesh_tag>;
using model_handle    = scene_handle<model_tag>;
