/**
 * @file vulkan_data_type.cpp
 * @brief 让 vulkan_data_type.h 自成一个编译单元，验证该头文件自足。
 * @details 该头没有需要链接的实现，本 TU 只包含它本身。
 *          /W4 /WX 下若头里漏 include，在这里就会直接编不过，不必等到某个使用者才暴露。
 */
#include "vulkan_data_type.h"
