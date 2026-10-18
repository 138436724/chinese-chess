/**
 * @file ui_base.h
 * @brief UI 面板的统一 proxy facade：把 resize / update / handle 三项约定收敛成一个接口。
 * @details 摄像机、灯光、物体、棋局、环境五个面板互不继承，ui_manager 用
 *          std::vector<pro::proxy<ui_base>> 按固定顺序持有并转发调用。facade 只声明成员函数
 *          约定，不约束实现类型，也不要求拷贝；relocation 与 destruction 都要求 nothrow，
 *          以保证 vector 扩容与析构路径不抛异常。
 */
#pragma once

#include <cstdint>
#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>

/**
 * @brief 声明 resize 约定：窗口或交换链尺寸变化时，把新的像素宽高转发给面板。
 * @details 由 PRO_DEF_MEM_DISPATCH 生成 dispatch tag，供 ui_base 的 add_convention 引用。
 */
PRO_DEF_MEM_DISPATCH(mem_resize, resize);
/**
 * @brief 声明 update 约定：每帧构建该面板的全部 ImGui 控件。
 * @details 生成 dispatch tag；proxy 调用的就是实现类型上的无参 update()。
 */
PRO_DEF_MEM_DISPATCH(mem_update, update);
/**
 * @brief 声明 handle 约定：参数与 GLFW 的 key callback 一致，面板自行决定是否消费。
 * @details 生成 dispatch tag；四个 int 依次是 key / scancode / action / mods。
 */
PRO_DEF_MEM_DISPATCH(mem_handle, handle);

/**
 * @brief 所有 UI 面板的 facade 类型。
 * @details 实现类型必须提供 resize(uint32_t, uint32_t)、update() 与
 *          handle(int, int, int, int) 三个成员函数；面板只经 scene_manager& 读写场景，
 *          彼此之间不直接通信。构造出的 proxy 可随 vector 搬迁，销毁时要求不抛异常。
 * @note facade 的约定签名不带 noexcept：实现可以自行标 noexcept，也可以抛异常；
 *       未捕获的异常会穿过 proxy 落到窗口帧循环的 catch。
 */
// clang-format off
struct ui_base : pro::facade_builder
    ::add_convention<mem_resize, void(uint32_t, uint32_t)>
    ::add_convention<mem_update, void()>
    ::add_convention<mem_handle, void(int, int, int, int)>
    ::support_relocation<pro::constraint_level::nothrow>
    ::support_destruction<pro::constraint_level::nothrow>
    ::build
{
};
// clang-format on
