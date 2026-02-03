#pragma once

#include "scene_camera.h"
#include "vulkan_core/vulkan_application.h"
#include <proxy/v4/proxy.h>
#include <proxy/v4/proxy_macros.h>

PRO_DEF_MEM_DISPATCH(mem_create, create);
PRO_DEF_MEM_DISPATCH(mem_resize, resize);
PRO_DEF_MEM_DISPATCH(mem_update, update);
PRO_DEF_MEM_DISPATCH(mem_render, render);
PRO_DEF_MEM_DISPATCH(mem_destroy, destroy);

struct scene_node : pro::facade_builder
	::add_convention<mem_create, void(const vulkan_application*, vk::SampleCountFlagBits, vk::Format, vk::Format)>
	::add_convention<mem_resize, void(const vulkan_application*, uint32_t, uint32_t)>
	::add_convention<mem_update, void(const scene_camera*) noexcept>
	::add_convention<mem_render, void(const vk::raii::CommandBuffer&) noexcept>
	::add_convention<mem_destroy, void() noexcept>
	::support_relocation<pro::constraint_level::nothrow>
	::support_destruction<pro::constraint_level::nothrow>
	//::add_skill<pro::skills::rtti> todo
	::build
{
};