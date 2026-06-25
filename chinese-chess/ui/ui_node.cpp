#include "ui_node.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <ranges>
#ifdef _WIN32
#include <commdlg.h>
#endif // _WIN32

constexpr std::u8string_view MATERIAL_MANAGER = u8"材质管理";
constexpr std::u8string_view ADD_MATERIAL = u8"添加材质";
constexpr std::u8string_view MATERIAL = u8"材质";
constexpr std::u8string_view MATERIAL_INDEX = u8"材质ID";
constexpr std::u8string_view MATERIAL_BACKGROUND_COLOR = u8"背景色";
constexpr std::u8string_view MATERIAL_FOREGROUND_COLOR = u8"前景色";
constexpr std::u8string_view MATERIAL_ROUGHNESS = u8"粗糙度";
constexpr std::u8string_view MATERIAL_METALLIC = u8"金属度";
constexpr std::u8string_view DELETE_MATERIAL = u8"删除材质";
constexpr std::u8string_view NO_MATERIAL = u8"无材质";
constexpr std::u8string_view ADD_IMAGE = u8"添加贴图";

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

void ui_node::resize(uint32_t/* _width*/, uint32_t/* _height*/) noexcept
{
}

void ui_node::update() noexcept
{
	update_model();
	update_material();
}

void ui_node::update_material() noexcept
{
	ImGui::SeparatorText(reinterpret_cast<const char*>(MATERIAL_MANAGER.data()));

	if (ImGui::Button(reinterpret_cast<const char*>(ADD_MATERIAL.data())))
	{
		materials.push_back(manager->create<scene_material>());
	}

	std::optional<size_t> delete_index = std::nullopt;
	for (size_t i = 0; i < materials.size(); ++i)
	{
		ImGui::PushID(static_cast<int>(i));

		auto& material_ptr = materials.at(i);

		if (ImGui::CollapsingHeader(std::format("{} {}", reinterpret_cast<const char*>(MATERIAL.data()), i).c_str(), ImGuiTreeNodeFlags_None))
		{
			if (ImGui::ColorEdit3(reinterpret_cast<const char*>(MATERIAL_BACKGROUND_COLOR.data()), glm::value_ptr(material_ptr->background_color)))
			{
				manager->need_update();
			}

			if (ImGui::ColorEdit3(reinterpret_cast<const char*>(MATERIAL_FOREGROUND_COLOR.data()), glm::value_ptr(material_ptr->foreground_color)))
			{
				manager->need_update();
			}

			if (!textures.empty())
			{
				ImGui::Text(textures.at(material_ptr->alpha_map).generic_string().c_str());
				ImGui::SameLine();
			}

			if (ImGui::Button(reinterpret_cast<const char*>(ADD_IMAGE.data())))
			{
				std::filesystem::path file_path;

#ifdef _WIN32
				TCHAR szFile[MAX_PATH] = { 0 };

				OPENFILENAME ofn;
				ZeroMemory(&ofn, sizeof(ofn));
				ofn.lStructSize = sizeof(ofn);
				ofn.lpstrFile = szFile;
				ofn.nMaxFile = sizeof(szFile);
				ofn.lpstrFilter = L"image\0*.png\0All\0*.*\0";
				ofn.nFilterIndex = 1;
				ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

				if (GetOpenFileName(&ofn))
				{
					file_path = szFile;
				}
#endif // _WIN32

				if (!file_path.empty())
				{
					if (material_ptr->alpha_map)
					{
						std::erase_if(textures, [&](const auto& pair) { return pair.first == material_ptr->alpha_map; });
					}
					material_ptr->alpha_map = manager->create<scene_image>(file_path, false);
					textures.insert(std::make_pair(material_ptr->alpha_map, file_path));
					manager->need_update();
				}
			}

			if (ImGui::DragFloat(reinterpret_cast<const char*>(MATERIAL_ROUGHNESS.data()), &material_ptr->roughness, 0.01f, 0.f, 1.f))
			{
				manager->need_update();
			}

			if (ImGui::DragFloat(reinterpret_cast<const char*>(MATERIAL_METALLIC.data()), &material_ptr->metallic, 0.01f, 0.f, 1.f))
			{
				manager->need_update();
			}

			if (ImGui::Button(reinterpret_cast<const char*>(DELETE_MATERIAL.data())))
			{
				delete_index = i;
			}
		}

		ImGui::PopID();
	}

	if (delete_index.has_value())
	{
		std::erase_if(materials, [&](const auto& p) { return p == materials.at(delete_index.value()); });
		manager->need_update();
	}

	if (materials.empty())
	{
		ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), reinterpret_cast<const char*>(NO_MATERIAL.data()));
	}
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
			models.push_back(manager->create<scene_model>(file_path));
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

			glm::vec3 translate, scale, skew;
			glm::quat rotate;
			glm::vec4 perspective;
			if (glm::decompose(model_ptr->model_matrix, scale, rotate, translate, skew, perspective))
			{
				glm::vec3 euler_radians = glm::eulerAngles(rotate);
				glm::vec3 euler_degrees = glm::degrees(euler_radians);

				bool modified = false;
				modified |= ImGui::DragFloat3(reinterpret_cast<const char*>(MODEL_TRANSLATION.data()), glm::value_ptr(translate), 0.1f);
				modified |= ImGui::DragFloat3(reinterpret_cast<const char*>(MODEL_ROTATION.data()), glm::value_ptr(euler_degrees));
				modified |= ImGui::DragFloat3(reinterpret_cast<const char*>(MODEL_SCALING.data()), glm::value_ptr(scale), 0.1f);

				if (modified)
				{
					glm::vec3 new_euler_radians = glm::radians(euler_degrees);
					glm::quat new_rotate = glm::quat(new_euler_radians);

					glm::mat4 T = glm::translate(glm::mat4(1.0f), translate);
					glm::mat4 R = glm::mat4_cast(new_rotate);
					glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);

					model_ptr->model_matrix = T * R * S;;
					manager->need_update();
				}
			}

			if (!materials.empty())
			{
				const auto all_material_index = std::views::iota(0u, materials.size())
					| std::views::transform([this](const auto index) { return std::format("{} {}", reinterpret_cast<const char*>(MATERIAL.data()), index); })
					| std::ranges::to<std::vector>();

				const auto all_material_index_string = all_material_index
					| std::views::transform([](const auto& s) { return s.c_str(); })
					| std::ranges::to<std::vector>();

				int material_index = get_material_index(model_ptr->material).value_or(-1);
				if (ImGui::Combo(reinterpret_cast<const char*>(MATERIAL_INDEX.data()), &material_index, all_material_index_string.data(), static_cast<int>(all_material_index_string.size())))
				{
					model_ptr->material = materials.at(material_index);
					manager->need_update();
				}
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
		std::erase_if(models, [&](const auto& p) { return p == models.at(delete_index.value()); });
		manager->need_update();
	}

	if (models.empty())
	{
		ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), reinterpret_cast<const char*>(NO_MODEL.data()));
	}
}

std::optional<int> ui_node::get_material_index(const std::weak_ptr<scene_material>& _material) const noexcept
{
	auto materials_with_index = materials | std::views::enumerate;

	std::owner_less<void> cmp;
	auto iter = std::ranges::find_if(materials_with_index, [&](const auto& p)
		{
			return !cmp(std::get<1>(p), _material) && !cmp(_material, std::get<1>(p));
		});

	if (iter != materials_with_index.end())
	{
		return static_cast<int>(std::get<0>(*iter));
	}
	else
	{
		return std::nullopt;
	}
}
