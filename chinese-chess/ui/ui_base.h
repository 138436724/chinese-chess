#pragma once

#include "scene/scene_manager.h"
#include <proxy/v4/proxy.h>
#include <proxy/v4/proxy_macros.h>

PRO_DEF_MEM_DISPATCH(mem_create, create);
PRO_DEF_MEM_DISPATCH(mem_resize, resize);
PRO_DEF_MEM_DISPATCH(mem_update, update);

struct ui_base : pro::facade_builder
	::add_convention<mem_create, void(scene_manager*)>
	::add_convention<mem_resize, void(uint32_t, uint32_t) noexcept>
	::add_convention<mem_update, void() noexcept>
	::support_relocation<pro::constraint_level::nontrivial>
	::support_destruction<pro::constraint_level::nontrivial>
	::build
{
};