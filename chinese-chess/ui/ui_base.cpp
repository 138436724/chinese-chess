/**
 * @file ui_base.cpp
 * @brief ui_base.h 的自包含编译单元。
 * @details facade 只有类型声明与三个 PRO_DEF_MEM_DISPATCH 宏展开，没有可写的实现；
 *          本文件只 include 自己的头，在 /W4 /WX 下检查可见的声明；不能穷尽模板实例化或传递包含问题。
 */
#include "ui_base.h"
