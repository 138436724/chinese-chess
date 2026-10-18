/**
 * @file scene_base.h
 * @brief 场景渲染器的类型擦除接口 manager_render，用同一组约定驱动三个渲染后端。
 * @details 用 pro::facade_builder 声明 resize / update / render / recreate / reset_accumulation 五个约定：
 *          三个渲染器各自实现同名成员函数，scene_manager 只持有 pro::proxy<manager_render>。
 *          约定不要求 noexcept；具体后端允许有更强的异常规范，[[nodiscard]] 不参与函数类型匹配。
 * @note facade 显式要求 nothrow 的 relocation 与 destruction：proxy 在 unordered_map 中移动或销毁时不得抛异常。
 */
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
/**
 * @brief 场景渲染器的 proxy facade，规定三个后端必须提供的五个成员函数。
 * @details resize 接收新的输出尺寸；update 重建描述符并重置光追采样序号；render 接收相机、命令缓冲、
 *          环境数据与帧槽下标；recreate 重建管线（RT 还重建 SBT）；reset_accumulation 只重置采样序号，
 *          光追下一帧用首样本覆盖累积图，光栅后端为空实现。
 *          add_skill<pro::skills::as_view> 让 scene_manager 用 proxy_view 指向 map 中的实例而不转移所有权。
 *          support_relocation / support_destruction 都是 nothrow：proxy 被移动或销毁时不得抛异常。
 */
struct manager_render : pro::facade_builder
    ::add_convention<mem_render_resize, void(uint32_t, uint32_t)>
    ::add_convention<mem_render_update, void()>
    ::add_convention<mem_render_render, void(const scene_camera&, const vk::raii::CommandBuffer&, const vulkan_environment_data&, uint32_t)>
    ::add_convention<mem_render_recreate, void()>
    ::add_convention<mem_render_reset_accumulation, void()>
    ::add_skill<pro::skills::as_view>
    ::support_relocation<pro::constraint_level::nothrow>
    ::support_destruction<pro::constraint_level::nothrow>
    ::build
{
};
// clang-format on
