#pragma once

#include <memory>
#include <vector>

struct scene_light;
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

    int                                       add_light_type = 0;
    std::vector<std::shared_ptr<scene_light>> lights;
};
