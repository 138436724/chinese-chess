#pragma once

#include "scene/scene_light.h"

#include <imgui.h>
#include <memory>
#include <vector>

class scene_manager;

class ui_light
{
public:
    explicit ui_light(scene_manager& _manager);
    ~ui_light() = default;

    void resize(uint32_t _width, uint32_t _height) noexcept;
    void update();
    void handle(int _glfw_key) noexcept;

private:
    scene_manager& manager;

    int                                       add_light_type = 0;
    std::vector<std::shared_ptr<scene_light>> lights;
};
