#pragma once

#include <memory>

class vulkan_application;

// Public ImGui platform callbacks own the WSI path; the upstream backend still
// owns its vertex/index buffers and performs all ImGui draw commands.
class ui_viewport_renderer
{
public:
    explicit ui_viewport_renderer(vulkan_application& app);
    ~ui_viewport_renderer();
    ui_viewport_renderer(const ui_viewport_renderer&) = delete;
    ui_viewport_renderer& operator=(const ui_viewport_renderer&) = delete;
    void check_errors();
private:
    struct implementation;
    std::unique_ptr<implementation> impl;
};
