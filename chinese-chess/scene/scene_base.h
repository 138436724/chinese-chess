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
    ::support_relocation<pro::constraint_level::nontrivial>
    ::support_destruction<pro::constraint_level::nontrivial>
    ::build
{
};
// clang-format on


PRO_DEF_MEM_DISPATCH(mem_render_resize, resize);
PRO_DEF_MEM_DISPATCH(mem_render_update, update);
PRO_DEF_MEM_DISPATCH(mem_render_render, render);
PRO_DEF_MEM_DISPATCH(mem_render_recreate, recreate);
PRO_DEF_MEM_DISPATCH(mem_render_reset_accumulation, reset_accumulation);
// clang-format off
struct manager_render : pro::facade_builder
    ::add_convention<mem_render_resize, void(uint32_t, uint32_t)>
    ::add_convention<mem_render_update, void()>
    ::add_convention<mem_render_render, void(const scene_camera&, const vk::raii::CommandBuffer&, uint32_t)>
    ::add_convention<mem_render_recreate, void()>
    ::add_convention<mem_render_reset_accumulation, void()>
    ::support_relocation<pro::constraint_level::nontrivial>
    ::support_destruction<pro::constraint_level::nontrivial>
    ::build
{
};
// clang-format on
