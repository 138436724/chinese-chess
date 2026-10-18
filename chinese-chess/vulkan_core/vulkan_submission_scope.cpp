/**
 * @file vulkan_submission_scope.cpp
 * @brief 让 vulkan_submission_scope.h 自成一个编译单元，验证头文件自足。
 * @details 守卫的构造与析构都定义在头里，本 TU 不产生任何符号。
 *          它存在的意义是在 /W4 /WX 下单独编译该头，漏 include 会立刻暴露。
 */
#include "vulkan_submission_scope.h"
