#include "ui_node.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <ranges>

constexpr std::u8string_view MATERIAL_MANAGER = u8"材质管理";
constexpr std::u8string_view MATERIAL = u8"材质";
constexpr std::u8string_view MATERIAL_INDEX = u8"材质ID";

constexpr std::u8string_view MODEL_MANAGER = u8"物体管理";
constexpr std::u8string_view ADD_MODEL = u8"添加物体";
constexpr std::u8string_view MODEL = u8"物体";
constexpr std::u8string_view SHOW_MODEL = u8"可见性";
constexpr std::u8string_view MODEL_TRANSLATION = u8"平移";
constexpr std::u8string_view MODEL_ROTATION = u8"旋转";
constexpr std::u8string_view MODEL_SCALING = u8"缩放";
constexpr std::u8string_view DELETE_MODEL = u8"删除物体";
constexpr std::u8string_view NO_MODEL = u8"无物体";

void ui_node::create(scene_manager* _manager)
{
	manager = _manager;
}

void ui_node::update() noexcept
{
	update_model();
	update_material();
}

void ui_node::update_material() noexcept
{
	ImGui::SeparatorText(reinterpret_cast<const char*>(MATERIAL_MANAGER.data()));

	// need load image from file
}

void ui_node::update_model() noexcept
{
	ImGui::SeparatorText(reinterpret_cast<const char*>(MODEL_MANAGER.data()));

	if (ImGui::Button(reinterpret_cast<const char*>(ADD_MODEL.data())))
	{
		std::filesystem::path file_path;

#ifdef _WIN32
		TCHAR szFile[MAX_PATH] = { 0 };

		OPENFILENAME ofn;
		ZeroMemory(&ofn, sizeof(ofn));
		ofn.lStructSize = sizeof(ofn);
		ofn.lpstrFile = szFile;
		ofn.nMaxFile = sizeof(szFile);
		ofn.lpstrFilter = L"gltf model\0*.glb\0All\0*.*\0";
		ofn.nFilterIndex = 1;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

		if (GetOpenFileName(&ofn))
		{
			file_path = szFile;
		}
#endif // _WIN32

		if (!file_path.empty())
		{
			models.push_back(manager->create_model(file_path));
		}
	}

	std::optional<size_t> delete_index = std::nullopt;
	for (size_t i = 0; i < models.size(); ++i)
	{
		ImGui::PushID(static_cast<int>(i));

		auto& model_ptr = models.at(i);

		if (ImGui::CollapsingHeader(std::format("{} {}", reinterpret_cast<const char*>(MODEL.data()), i).c_str(), ImGuiTreeNodeFlags_None))
		{
			if (ImGui::Checkbox(reinterpret_cast<const char*>(SHOW_MODEL.data()), &model_ptr->is_show))
			{
				manager->need_update();
			}

			glm::vec3 pos, scale, skew;
			glm::quat rot;
			glm::vec4 persp;
			if (glm::decompose(model_ptr->model_matrix, scale, rot, pos, skew, persp))
			{
				bool modified = false;
				modified |= ImGui::DragFloat3(reinterpret_cast<const char*>(MODEL_TRANSLATION.data()), glm::value_ptr(pos), 0.1f);
				modified |= ImGui::DragFloat4(reinterpret_cast<const char*>(MODEL_ROTATION.data()), glm::value_ptr(rot));
				modified |= ImGui::DragFloat3(reinterpret_cast<const char*>(MODEL_SCALING.data()), glm::value_ptr(scale), 0.1f);

				if (modified)
				{
					glm::mat4 T = glm::translate(glm::mat4(1.0f), pos);
					glm::mat4 R = glm::mat4_cast(rot);
					glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);
					model_ptr->model_matrix = T * R * S;;
					manager->need_update();
				}
			}

			const auto all_material_index = std::views::iota(0u, materials.size() - 1)
				| std::views::transform([this](const auto index) { return std::format("{} {}", reinterpret_cast<const char*>(MATERIAL.data()), index); })
				| std::ranges::to<std::vector>();

			if (ImGui::Combo(reinterpret_cast<const char*>(MATERIAL_INDEX.data()), &add_light_type, all_material_index.data(), static_cast<int>(all_material_index.size())))
			{
				model_ptr->material = materials.at(add_light_type);
				manager->need_update();
			}

			if (ImGui::Button(reinterpret_cast<const char*>(DELETE_MODEL.data())))
			{
				delete_index = i;
			}
		}

		ImGui::PopID();
	}

	if (delete_index.has_value())
	{
		manager->remove_model(models.at(delete_index.value()));
		std::erase_if(models, [&](const auto& p) { return p == models.at(delete_index.value()); });
	}

	if (models.empty())
	{
		ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), reinterpret_cast<const char*>(NO_MODEL.data()));
	}
}
