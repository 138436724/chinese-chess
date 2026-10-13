#pragma once

#include "scene/scene_handle.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

class scene_manager;

class ui_node
{
public:
    explicit ui_node(scene_manager& _manager) noexcept;
    ~ui_node() = default;

    void resize(uint32_t _width, uint32_t _height) noexcept;
    void update();
    void handle(int _key, int _scancode, int _action, int _mods) noexcept;

private:
    void update_material();
    void update_model();

    [[nodiscard]] std::optional<int> material_index_of(material_handle _material) const noexcept;

    scene_manager& manager;

    std::vector<material_handle> materials;
    std::vector<model_handle>    models;

    std::unordered_map<std::filesystem::path, texture_handle> textures;
};
