/**
 * @file vulkan_submission_scope.cpp
 * @brief 让 vulkan_submission_scope.h 自成一个编译单元，验证头文件自足。
 * @details 守卫的构造与析构都定义在头里，本 TU 不另定义接口。
 *          单独包含该头可发现依赖包含顺序的问题，但不能排除由第三方头间接补齐依赖的情况。
 */
#include "vulkan_submission_scope.h"
