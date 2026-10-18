#include "window.h"

#include <cstdlib>
#include <cstdio>
#include <exception>

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
