/**
 * @file vulkan_data_type.cpp
 * @brief 让 vulkan_data_type.h 自成一个编译单元，验证该头文件自足。
 * @details 该头没有需要链接的实现，本 TU 只包含它本身。
 *          单独包含可发现依赖包含顺序的问题，但第三方头的间接包含仍可能掩盖缺少直接 include 的情况。
 */
#include "vulkan_data_type.h"
