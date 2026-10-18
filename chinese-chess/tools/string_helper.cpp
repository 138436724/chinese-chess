/**
 * @file string_helper.cpp
 * @brief string_helper 的独立编译单元，只 include 自己的头文件。
 * @details 所有实现都是头文件里的模板 / inline 函数，本文件不含任何定义。它存在的意义是让
 *          string_helper.h 在 /W4 /WX 下自成 TU 编译：头里漏 include 或模板签名写错时当场暴露，
 *          不必等到某个真正使用它的 TU 才报错。
 */

#include "string_helper.h"
