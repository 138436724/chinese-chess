/**
 * @file scene_base.cpp
 * @brief scene_base.h 的自包含编译单元。
 * @details 该头只声明 PRO_DEF_MEM_DISPATCH 与 manager_render facade，没有需要单独定义的成员函数；
 *          本文件只 include 自身，用于在 /W4 /WX 下暴露头文件遗漏的 include。
 */
#include "scene_base.h"
