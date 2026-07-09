#pragma once

#include "scene/scene_manager.h"

#include <glm/glm.hpp>
#include <imgui.h>

class ui_camera
{
public:
    ui_camera(scene_manager& _manager);
    ~ui_camera() = default;

    void resize(uint32_t _width, uint32_t _height) noexcept;
    void update() noexcept;
    void handle(int _glfw_key) noexcept;

private:
    scene_manager& manager;

    int active_type = static_cast<int>(projection_type::orthographic);

    glm::vec4 orthographic_range    = glm::vec4(-10.f, 10.f, -10.f, 10.f);
    glm::vec2 orthographic_near_far = glm::vec2(0.1f, 100.f);

    float     fov_y                = 90.f;  // need glm::radians to function
    float     aspect               = 16.f / 9.f;
    glm::vec2 perspective_near_far = glm::vec2(0.1f, 100.f);

    glm::vec3 position  = glm::vec3(0.f);
    glm::vec3 direction = glm::vec3(0.f, 0.f, -1.f);
    glm::vec3 world_up  = glm::vec3(0.f, 1.f, 0.f);
};
