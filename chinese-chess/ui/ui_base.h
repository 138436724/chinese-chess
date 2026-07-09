#pragma once

#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>

PRO_DEF_MEM_DISPATCH(mem_resize, resize);
PRO_DEF_MEM_DISPATCH(mem_update, update);
PRO_DEF_MEM_DISPATCH(mem_handle, handle);

// clang-format off
struct ui_base : pro::facade_builder
    ::add_convention<mem_resize, void(uint32_t, uint32_t) noexcept>
    ::add_convention<mem_update, void() noexcept>
    ::add_convention<mem_handle, void(int) noexcept>
    ::support_relocation<pro::constraint_level::nontrivial>
    ::support_destruction<pro::constraint_level::nontrivial>
    ::build
{
};
// clang-format on
