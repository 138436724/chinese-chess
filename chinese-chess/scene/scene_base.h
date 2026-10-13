#pragma once

#include "scene_camera.h"
#include "vulkan_core/vulkan_data_type.h"

#include <cstdint>
#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>
#include <vulkan/vulkan_raii.hpp>

PRO_DEF_MEM_DISPATCH(mem_render_resize, resize);
PRO_DEF_MEM_DISPATCH(mem_render_update, update);
PRO_DEF_MEM_DISPATCH(mem_render_render, render);
PRO_DEF_MEM_DISPATCH(mem_render_recreate, recreate);
PRO_DEF_MEM_DISPATCH(mem_render_reset_accumulation, reset_accumulation);
// clang-format off
struct manager_render : pro::facade_builder
    ::add_convention<mem_render_resize, void(uint32_t, uint32_t)>
    ::add_convention<mem_render_update, void()>
    ::add_convention<mem_render_render, void(const scene_camera&, const vk::raii::CommandBuffer&, const vulkan_environment_data&)>
    ::add_convention<mem_render_recreate, void()>
    ::add_convention<mem_render_reset_accumulation, void()>
    ::add_skill<pro::skills::as_view>
    ::support_relocation<pro::constraint_level::nothrow>
    ::support_destruction<pro::constraint_level::nothrow>
    ::build
{
};
// clang-format on
