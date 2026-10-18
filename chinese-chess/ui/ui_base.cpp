/**
 * @file ui_base.cpp
 * @brief ui_base.h 的自包含编译单元。
 * @details facade 只有类型声明与三个 PRO_DEF_MEM_DISPATCH 宏展开，没有可写的实现；
 *          本文件只 include 自己的头，用于在 /W4 /WX 下独立校验头文件自包含。
 */
#include "ui_base.h"
