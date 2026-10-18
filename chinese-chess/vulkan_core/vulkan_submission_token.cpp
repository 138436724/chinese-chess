/**
 * @file vulkan_submission_token.cpp
 * @brief 让 vulkan_submission_token.h 自成一个编译单元，验证头文件自足。
 * @details token 的方法全是内联实现，本 TU 不产生任何符号。
 *          它存在的意义是在 /W4 /WX 下单独编译该头，漏 include 会立刻暴露。
 */
#include "vulkan_submission_token.h"
