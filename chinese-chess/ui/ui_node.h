#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

struct cpu_model_data;
struct cpu_material_data;
struct cpu_image_data;
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

    [[nodiscard]] std::optional<int> get_material_index(const std::weak_ptr<cpu_material_data>& _material) const noexcept;

    scene_manager& manager;

    std::vector<std::shared_ptr<cpu_material_data>>                            materials;
    std::vector<std::shared_ptr<cpu_model_data>>                               models;
    std::unordered_map<std::shared_ptr<cpu_image_data>, std::filesystem::path> textures;
};
