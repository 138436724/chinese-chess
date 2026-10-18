/**
 * @file string_helper.cpp
 * @brief string_helper 的独立编译单元，只 include 自己的头文件。
 * @details 所有实现都是头文件里的模板 / inline 函数，本文件不额外定义符号。它让
 *          string_helper.h 在 /W4 /WX 下独立参与编译，可发现部分声明与包含问题；
 *          未实例化的模板路径及传递包含掩盖的问题不一定会被检出。
 */

#include "string_helper.h"
