/**
 * @file vulkan_submission_token.cpp
 * @brief 让 vulkan_submission_token.h 自成一个编译单元，验证头文件自足。
 * @details token 的方法全在头文件内实现，本 TU 不另定义接口。
 *          单独包含该头可发现依赖包含顺序的问题，但不能排除由第三方头间接补齐依赖的情况。
 */
#include "vulkan_submission_token.h"
