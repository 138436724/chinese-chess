#pragma once

#include <cstdint>
#include <filesystem>

class scene_manager;

class ui_environment
{
public:
    explicit ui_environment(scene_manager& _manager);
    ~ui_environment() = default;

    void resize(uint32_t _width, uint32_t _height) noexcept;
    void update();
    void handle(int _key, int _scancode, int _action, int _mods) noexcept;

private:
    scene_manager& manager;
};
