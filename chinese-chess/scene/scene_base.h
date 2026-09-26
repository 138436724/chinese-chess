#pragma once

#include "scene_camera.h"

#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

PRO_DEF_MEM_DISPATCH(mem_base_update, update);
PRO_DEF_MEM_DISPATCH(mem_base_clear, clear);
// clang-format off
struct manager_base : pro::facade_builder
    ::add_convention<mem_base_update, bool(std::vector<vk::SemaphoreSubmitInfo>&)>
    ::add_convention<mem_base_clear, void()>
    ::build
{
};
// clang-format on


PRO_DEF_MEM_DISPATCH(mem_render_resize, resize);
PRO_DEF_MEM_DISPATCH(mem_render_update, update);
PRO_DEF_MEM_DISPATCH(mem_render_render, render);
PRO_DEF_MEM_DISPATCH(mem_render_recreate, recreate);
PRO_DEF_MEM_DISPATCH(mem_render_reset_accumulation, reset_accumulation);
PRO_DEF_MEM_DISPATCH(mem_render_set_debug_flags, set_debug_flags);
PRO_DEF_MEM_DISPATCH(mem_render_get_debug_flags, get_debug_flags);
PRO_DEF_MEM_DISPATCH(mem_render_set_debug_mat_override, set_debug_mat_override);
PRO_DEF_MEM_DISPATCH(mem_render_get_debug_force_metallic, get_debug_force_metallic);
PRO_DEF_MEM_DISPATCH(mem_render_get_debug_force_roughness, get_debug_force_roughness);
// clang-format off
struct manager_render : pro::facade_builder
    ::add_convention<mem_render_resize, void(uint32_t, uint32_t)>
    ::add_convention<mem_render_update, void()>
    ::add_convention<mem_render_render, void(const scene_camera&, const vk::raii::CommandBuffer&, uint32_t)>
    ::add_convention<mem_render_recreate, void()>
    ::add_convention<mem_render_reset_accumulation, void()>
    // 物理正确性调试通道:光栅化渲染器实现为空操作(它没有物理正确的着色)
    ::add_convention<mem_render_set_debug_flags, void(uint32_t)>
    ::add_convention<mem_render_get_debug_flags, uint32_t()>
    ::add_convention<mem_render_set_debug_mat_override, void(float, float)>
    ::add_convention<mem_render_get_debug_force_metallic, float()>
    ::add_convention<mem_render_get_debug_force_roughness, float()>
    ::add_skill<pro::skills::as_view>
    ::support_relocation<pro::constraint_level::nothrow>
    ::support_destruction<pro::constraint_level::nothrow>
    ::build
{
};
// clang-format on
