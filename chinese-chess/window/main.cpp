/**
 * @file main.cpp
 * @brief 进程入口：构造 glfw_window 并跑完整渲染循环。
 * @details 所有初始化与帧循环异常都在这里兜底：std::exception 打印 what()，其他异常打印固定文案，
 *          两者都返回 EXIT_FAILURE；窗口正常关闭时返回 EXIT_SUCCESS。
 */
#include "window.h"

#include <cstdlib>
#include <cstdio>
#include <exception>

/**
 * @brief 创建窗口对象、进入渲染循环，并把异常转换成进程退出码。
 * @return 正常退出返回 EXIT_SUCCESS；捕获到 std::exception 或未知异常返回 EXIT_FAILURE。
 */
int main()
{
    try
    {
        glfw_window w;
        w.render();
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Rendering stopped: %s\n", error.what());
    }
    catch (...)
    {
        std::fputs("Rendering stopped after an unknown error.\n", stderr);
    }
    return EXIT_FAILURE;
}
