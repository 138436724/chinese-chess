#pragma once

#include "scene/scene_handle.h"

#include <cstdint>
#include <vector>

class scene_manager;

class ui_light
{
public:
    explicit ui_light(scene_manager& _manager);
    ~ui_light() = default;

    void resize(uint32_t _width, uint32_t _height) noexcept;
    void update();
    void handle(int _key, int _scancode, int _action, int _mods) noexcept;

private:
    scene_manager& manager;

    int                       add_light_type = 0;
    std::vector<light_handle> lights;
};
