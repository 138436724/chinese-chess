/**
 * @file scene_base.cpp
 * @brief scene_base.h 的自包含编译单元。
 * @details 该头只声明 PRO_DEF_MEM_DISPATCH 与 manager_render facade，没有需要单独定义的成员函数；
 *          本文件只 include 对应头，用于在 /W4 /WX 下检查头文件可独立包含；不能排除依赖被第三方头间接补齐。
 */
#include "scene_base.h"
