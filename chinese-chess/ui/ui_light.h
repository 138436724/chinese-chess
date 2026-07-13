#pragma once

#include "scene/scene_manager.h"

#include <imgui.h>

class ui_light
{
public:
    explicit ui_light(scene_manager& _manager);
    ~ui_light() = default;

    void resize(uint32_t _width, uint32_t _height) noexcept;
    void update() noexcept;
    void handle(int _glfw_key) noexcept;

private:
    scene_manager& manager;

    int                                       add_light_type = 0;
    std::vector<std::shared_ptr<scene_light>> lights;
};
