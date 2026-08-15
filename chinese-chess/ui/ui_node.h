#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

struct scene_model;
struct scene_material;
struct scene_image;
class scene_manager;

class ui_node
{
public:
    explicit ui_node(scene_manager& _manager) noexcept;
    ~ui_node() = default;

    void resize(uint32_t _width, uint32_t _height) noexcept;
    void update();
    void handle(int _glfw_key) noexcept;

private:
    void update_material();
    void update_model();

    [[nodiscard]] std::optional<int> get_material_index(const std::weak_ptr<scene_material>& _material) const noexcept;

    scene_manager& manager;

    std::vector<std::shared_ptr<scene_material>>                            materials;
    std::vector<std::shared_ptr<scene_model>>                               models;
    std::unordered_map<std::shared_ptr<scene_image>, std::filesystem::path> textures;
};
