#include "InspectorRegistry.h"

#include "World/Core/ComponentRegistry.h"
#include "World/Core/Log.h"
#include "World/Scene/Components.h"
#include "World/Scene/ScriptEngine.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

namespace World
{
	namespace
	{
		void DrawScriptStatus(ScriptInstanceState state, const std::string& error)
		{
			const char* label = "Pending";
			switch (state)
			{
				case ScriptInstanceState::Creating: label = "Creating"; break;
				case ScriptInstanceState::Running: label = "Running"; break;
				case ScriptInstanceState::Destroying: label = "Destroying"; break;
				case ScriptInstanceState::Stopped: label = "Stopped"; break;
				case ScriptInstanceState::Faulted: label = "Faulted"; break;
				default: break;
			}
			ImGui::Text("Status: %s", label);
			if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
		}

		bool DrawVec3Control(const std::string& label, glm::vec3& values, float resetValue, float columnWidth)
		{
			ImGuiIO& io = ImGui::GetIO();
			auto boldFont = io.Fonts->Fonts[1];
			ImGui::PushID(label.c_str());
			bool changed = false;

			if (ImGui::BeginTable("##Vec3ControlTable", 2, ImGuiTableFlags_Resizable))
			{
				ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, columnWidth);
				ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableNextRow();

				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				float columnWidthAvail = ImGui::GetContentRegionAvail().x;
				float textWidth = ImGui::CalcTextSize(label.c_str()).x;
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidthAvail - textWidth) * 0.5f);
				ImGui::Text(label.c_str());

				ImGui::TableNextColumn();
				float totalWidth = ImGui::GetContentRegionAvail().x;
				float lineHeight = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
				ImVec2 buttonSize = { lineHeight + 3.0f, lineHeight };
				float totalDragFloatWidth = totalWidth - (buttonSize.x * 3.0f);
				ImGui::PushMultiItemsWidths(3, totalDragFloatWidth);
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2 { 0, 0 });

				{
					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.20f, 0.25f, 1.0f));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.30f, 0.35f, 1.0f));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.55f, 0.15f, 0.20f, 1.0f));
					ImGui::PushFont(boldFont);
					if (ImGui::Button("X", buttonSize)) { values.x = resetValue; changed = true; }
					ImGui::PopFont();
					ImGui::PopStyleColor(3);
					ImGui::SameLine();
					ImGui::DragFloat("##X", &values.x, 0.1f, 0.0f, 0.0f, "%.3f");
					if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
					ImGui::PopItemWidth();
					ImGui::SameLine();
				}

				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.60f, 0.30f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.75f, 0.40f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.45f, 0.25f, 1.0f));
				ImGui::PushFont(boldFont);
				if (ImGui::Button("Y", buttonSize)) { values.y = resetValue; changed = true; }
				ImGui::PopFont();
				ImGui::PopStyleColor(3);
				ImGui::SameLine();
				ImGui::DragFloat("##Y", &values.y, 0.1f, 0.0f, 0.0f, "%.3f");
				if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
				ImGui::PopItemWidth();
				ImGui::SameLine();

				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.75f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.60f, 0.90f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.35f, 0.60f, 1.0f));
				ImGui::PushFont(boldFont);
				if (ImGui::Button("Z", buttonSize)) { values.z = resetValue; changed = true; }
				ImGui::PopFont();
				ImGui::PopStyleColor(3);
				ImGui::SameLine();
				ImGui::DragFloat("##Z", &values.z, 0.1f, 0.0f, 0.0f, "%.3f");
				if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
				ImGui::PopItemWidth();

				ImGui::PopStyleVar();
				ImGui::EndTable();
			}

			ImGui::PopID();
			return changed;
		}

		template<typename T, typename UIFunction>
		bool DrawComponentFold(const char* name, Entity entity, UIFunction uiFunction, bool removable = true)
		{
			if (entity && !entity.GetScene()->IsPendingDestroy(entity) && entity.HasComponent<T>())
			{
				ImGui::Separator();
				ImGuiTreeNodeFlags flags =
					ImGuiTreeNodeFlags_DefaultOpen
					| ImGuiTreeNodeFlags_Framed
					| ImGuiTreeNodeFlags_SpanAvailWidth
					| ImGuiTreeNodeFlags_AllowItemOverlap;
				auto& component = entity.GetComponent<T>();
				std::string removalReason;
				const bool canRemove = removable && entity.CanRemoveComponent(entt::type_id<T>().hash(), &removalReason);
				bool changed = false;
				if (canRemove)
				{
					bool closable_group = true;
					if (ImGui::CollapsingHeader(name, &closable_group, flags))
						changed = uiFunction(component) || changed;
					if (!closable_group)
					{
						try { entity.RemoveComponent<T>(); changed = true; }
						catch (const std::exception& error) { ImGui::TextWrapped("Cannot remove: %s", error.what()); }
					}
				}
				else
				{
					if (ImGui::CollapsingHeader(name, flags))
					{
						if (removable && !removalReason.empty())
							ImGui::TextWrapped("Cannot remove: %s", removalReason.c_str());
						changed = uiFunction(component) || changed;
					}
				}
				return changed;
			}
			return false;
		}

		// 通用属性控件：把编辑结果写回 current，返回是否改动。
		bool DrawPropertyControl(const PropertyDesc& prop, const std::string& label, std::any& current)
		{
			if (!current.has_value())
			{
				ImGui::TextWrapped("%s: [no value]", label.c_str());
				return false;
			}

			switch (prop.Type)
			{
				case DataType::Bool:
				{
					bool v = std::any_cast<bool>(current);
					if (ImGui::Checkbox(label.c_str(), &v)) { current = v; return true; }
					break;
				}
				case DataType::Int8:
				case DataType::Int16:
				case DataType::Int32:
				case DataType::Int64:
				{
					int v = prop.Type == DataType::Int64
						? static_cast<int>(std::any_cast<int64_t>(current))
						: static_cast<int>(std::any_cast<int32_t>(current));
					bool hasRange = prop.Edit.Min.has_value() && prop.Edit.Max.has_value();
					bool edited = hasRange
						? ImGui::DragInt(label.c_str(), &v, 1, static_cast<int>(*prop.Edit.Min), static_cast<int>(*prop.Edit.Max))
						: ImGui::DragInt(label.c_str(), &v, 1);
					if (edited)
					{
						switch (prop.Type)
						{
							case DataType::Int8: current = static_cast<int8_t>(v); break;
							case DataType::Int16: current = static_cast<int16_t>(v); break;
							case DataType::Int64: current = static_cast<int64_t>(v); break;
							default: current = static_cast<int32_t>(v); break;
						}
						return true;
					}
					break;
				}
				case DataType::UInt8:
				case DataType::UInt16:
				case DataType::UInt32:
				case DataType::UInt64:
				{
					int v = prop.Type == DataType::UInt64
						? static_cast<int>(std::any_cast<uint64_t>(current))
						: static_cast<int>(std::any_cast<uint32_t>(current));
					if (ImGui::DragInt(label.c_str(), &v, 1, 0, 0))
					{
						switch (prop.Type)
						{
							case DataType::UInt8: current = static_cast<uint8_t>(v); break;
							case DataType::UInt16: current = static_cast<uint16_t>(v); break;
							case DataType::UInt64: current = static_cast<uint64_t>(v); break;
							default: current = static_cast<uint32_t>(v); break;
						}
						return true;
					}
					break;
				}
				case DataType::Float:
				{
					float v = std::any_cast<float>(current);
					bool hasRange = prop.Edit.Min.has_value() && prop.Edit.Max.has_value();
					bool edited = hasRange
						? ImGui::DragFloat(label.c_str(), &v, 0.1f, *prop.Edit.Min, *prop.Edit.Max, "%.3f")
						: ImGui::DragFloat(label.c_str(), &v, 0.1f, 0.0f, 0.0f, "%.3f");
					if (edited) { current = v; return true; }
					break;
				}
				case DataType::Double:
				{
					double v = std::any_cast<double>(current);
					if (ImGui::InputDouble(label.c_str(), &v)) { current = v; return true; }
					break;
				}
				case DataType::Vec2:
				{
					glm::vec2 v = std::any_cast<glm::vec2>(current);
					if (ImGui::DragFloat2(label.c_str(), glm::value_ptr(v), 0.1f)) { current = v; return true; }
					break;
				}
				case DataType::Vec3:
				{
					glm::vec3 v = std::any_cast<glm::vec3>(current);
					if (ImGui::DragFloat3(label.c_str(), glm::value_ptr(v), 0.1f)) { current = v; return true; }
					break;
				}
				case DataType::Vec4:
				{
					glm::vec4 v = std::any_cast<glm::vec4>(current);
					if (ImGui::DragFloat4(label.c_str(), glm::value_ptr(v), 0.1f)) { current = v; return true; }
					break;
				}
				case DataType::String:
				{
					std::string v = std::any_cast<std::string>(current);
					std::vector<char> buffer(std::max<size_t>(256, v.size() + 64), '\0');
					std::copy(v.begin(), v.end(), buffer.begin());
					if (ImGui::InputText(label.c_str(), buffer.data(), buffer.size()))
					{
						current = std::string(buffer.data());
						return true;
					}
					break;
				}
				case DataType::Enum:
				{
					const EnumDesc& enumDesc = std::any_cast<EnumDesc>(prop.UserData);
					const TypeDesc* enumTypeDesc = TypeRegistry::Get().GetTypeDesc(enumDesc.Name);
					if (enumTypeDesc)
					{
						std::vector<const char*> options;
						options.reserve(enumTypeDesc->Properties.size());
						for (const auto& enumProperty : enumTypeDesc->Properties)
							options.push_back(enumProperty.Name.c_str());
						int enumVal = std::any_cast<int32_t>(current);
						if (ImGui::Combo(label.c_str(), &enumVal, options.data(), static_cast<int>(options.size())))
						{
							current = enumVal;
							return true;
						}
					}
					else
					{
						ImGui::TextWrapped("%s: [unknown enum]", label.c_str());
					}
					break;
				}
				default:
					ImGui::TextWrapped("%s: [read-only type %d]", label.c_str(), static_cast<int>(prop.Type));
					break;
			}
			return false;
		}
	}

	// ---------------- 自定义检查器 ----------------

	bool DrawTagInspector(Entity entity)
	{
		return DrawComponentFold<TagComponent>("TagComponent", entity, [](TagComponent& tagComponent) -> bool
			{
				char buffer[256];
				memset(buffer, 0, sizeof(buffer));
				strcpy_s(buffer, sizeof(buffer), tagComponent.Tag.c_str());
				if (ImGui::InputText("##Tag", buffer, sizeof(buffer)))
				{
					tagComponent.Tag = std::string(buffer);
					return true;
				}
				return false;
			});
	}

	bool DrawUuidInspector(Entity entity)
	{
		if (entity.HasComponent<UUIDComponent>())
		{
			ImGui::Separator();
			ImGui::Text("UUID: %llu", static_cast<uint64_t>(entity.GetComponent<UUIDComponent>().ID));
		}
		return false;
	}

	bool DrawTransformInspector(Entity entity)
	{
		return DrawComponentFold<TransformComponent>("TransformComponent", entity, [](TransformComponent& transform) -> bool
			{
				bool changed = false;
				changed |= DrawVec3Control("Location", transform.Location, 0.0f, 75.0f);

				glm::vec3 rotationDegrees = glm::degrees(transform.Rotation);
				changed |= DrawVec3Control("Rotation", rotationDegrees, 0.0f, 75.0f);
				transform.Rotation = glm::radians(rotationDegrees);

				changed |= DrawVec3Control("Scale", transform.Scale, 1.0f, 75.0f);

				transform.SetTransform(transform.Location, transform.Rotation, transform.Scale);
				return changed;
			});
	}

	bool DrawCameraInspector(Entity entity)
	{
		return DrawComponentFold<CameraComponent>("CameraComponent", entity, [](CameraComponent& cameraComponent) -> bool
			{
				bool changed = false;
				auto& camera = cameraComponent.Camera;

				changed |= ImGui::Checkbox("Primary", &cameraComponent.Primary);
				changed |= ImGui::Checkbox("Fixed Aspect Ratio", &cameraComponent.FixedAspectRatio);
				const char* projectionTypeStrings[] = { "Perspective", "Orthographic" };
				const char* currentProjectionTypeString = projectionTypeStrings[static_cast<int>(camera.GetProjectionType())];
				if (ImGui::BeginCombo("Projection", currentProjectionTypeString))
				{
					for (int i = 0; i < 2; i++)
					{
						bool isSelected = (camera.GetProjectionType() == static_cast<SceneCamera::ProjectionType>(i));
						if (ImGui::Selectable(projectionTypeStrings[i], isSelected))
						{
							camera.SetProjectionType(static_cast<SceneCamera::ProjectionType>(i));
							changed = true;
						}
					}
					ImGui::EndCombo();
				}

				if (camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic)
				{
					float zoom = camera.GetOrthographicZoom();
					float nearClip = camera.GetOrthographicNearClip();
					float farClip = camera.GetOrthographicFarClip();
					if (ImGui::DragFloat("Zoom", &zoom, 0.1f)) { camera.SetOrthographicZoom(zoom); changed = true; }
					if (ImGui::DragFloat("Near Clip", &nearClip, 0.1f)) { camera.SetOrthographicNearClip(nearClip); changed = true; }
					if (ImGui::DragFloat("Far Clip", &farClip, 0.1f)) { camera.SetOrthographicFarClip(farClip); changed = true; }
				}
				else
				{
					float fov = camera.GetPerspectiveFOV();
					float nearClip = camera.GetPerspectiveNearClip();
					float farClip = camera.GetPerspectiveFarClip();
					if (ImGui::DragFloat("FOV", &fov, 0.1f, 0.0f, 180.0f)) { camera.SetPerspectiveFOV(fov); changed = true; }
					if (ImGui::DragFloat("Near Clip", &nearClip, 0.1f)) { camera.SetPerspectiveNearClip(nearClip); changed = true; }
					if (ImGui::DragFloat("Far Clip", &farClip, 0.1f)) { camera.SetPerspectiveFarClip(farClip); changed = true; }
				}
				return changed;
			});
	}

	bool DrawSpriteInspector(Entity entity)
	{
		return DrawComponentFold<SpriteComponent>("SpriteComponent", entity, [](SpriteComponent& sprite) -> bool
			{
				bool changed = false;
				changed |= ImGui::ColorEdit4("Color", glm::value_ptr(sprite.Color));

				ImGui::Text("Texture");
				ImGui::NextColumn();
				uint32_t textureID = sprite.Texture ? sprite.Texture->GetRendererID() : 0;
				ImVec2 textureSize = { 64.0f, 64.0f };
				ImGui::ImageButton((ImTextureID)(uint64_t)textureID, textureSize, { 0, 1 }, { 1, 0 });

				if (ImGui::IsItemHovered())
				{
					ImGui::BeginTooltip();
					ImGui::Text("Drag and Drop from Content Browser");
					if (sprite.Texture)
					{
						ImGui::Spacing();
						ImGui::Image((ImTextureID)(uint64_t)textureID, { 256.0f, 256.0f }, { 0, 1 }, { 1, 0 });
					}
					ImGui::EndTooltip();
				}

				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const wchar_t* path = static_cast<const wchar_t*>(payload->Data);
						std::filesystem::path texturePath = std::filesystem::path("assets") / path;
						sprite.Texture = Texture2D::Create(texturePath.string());
						changed = true;
					}
					ImGui::EndDragDropTarget();
				}

				ImGui::SameLine();
				ImGui::BeginGroup();
				if (sprite.Texture)
				{
					ImGui::Text("%s", sprite.Texture->GetPath().c_str());
					if (ImGui::Button("Clear", ImVec2(50.0f, 0.0f)))
					{
						sprite.Texture = nullptr;
						changed = true;
					}
				}
				else
				{
					ImGui::Text("None");
				}
				ImGui::EndGroup();

				changed |= ImGui::DragFloat("Tiling", &sprite.TilingFactor, 0.1f, 0.0f, 100.0f);
				return changed;
			});
	}

	bool DrawNativeScriptInspector(Entity entity)
	{
		return DrawComponentFold<NativeScriptComponent>("NativeScriptComponent", entity, [entity](NativeScriptComponent& nativeScript) -> bool
			{
				bool changed = false;
				bool isBound = (nativeScript.InstantiateScript != nullptr);
				const char* currentPreview = isBound ? (nativeScript.ScriptName.empty() ? "Unknown Script" : nativeScript.ScriptName.c_str()) : "<None>";
				bool isRunning = entity.GetScene()->IsActive();
				DrawScriptStatus(nativeScript.State, nativeScript.LastError);

				if (isRunning)
					ImGui::BeginDisabled(true);

				if (ImGui::BeginCombo("Script Class", currentPreview))
				{
					if (ImGui::Selectable("<None>", !isBound))
					{
						nativeScript.InstantiateScript = nullptr;
						nativeScript.DestroyScript = nullptr;
						nativeScript.ScriptName = "";
						nativeScript.ResetEditorFieldState();
						changed = true;
					}

					for (const auto& scriptName : TypeRegistry::Get().GetTypesByCategory(TypeCategory::Script))
					{
						TypeDesc* scriptType = TypeRegistry::Get().GetTypeDesc(scriptName);
						if (!scriptType)
							continue;
						if (TypeDescDataScript* scriptInfo = std::any_cast<TypeDescDataScript>(&scriptType->UserData))
						{
							const std::string displayName = scriptType->Name;
							bool isSelected = (displayName == nativeScript.ScriptName);
							if (ImGui::Selectable(displayName.c_str(), isSelected))
							{
								if (scriptInfo->BindFunc)
								{
									scriptInfo->BindFunc(nativeScript);
									nativeScript.ScriptName = displayName;
									nativeScript.ResetEditorFieldState();
									changed = true;
								}
							}
							if (isSelected)
								ImGui::SetItemDefaultFocus();
						}
					}
					ImGui::EndCombo();
				}
				ImGui::Spacing();

				if (isBound)
				{
					if (ImGui::Button("Unbind Script", ImVec2(-1.0f, 25.0f)))
					{
						nativeScript.InstantiateScript = nullptr;
						nativeScript.DestroyScript = nullptr;
						nativeScript.ScriptName = "";
						nativeScript.ResetEditorFieldState();
						changed = true;
					}
				}

				if (isRunning)
				{
					ImGui::EndDisabled();
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						ImGui::SetTooltip("Cannot rebind or unbind scripts while the game is running.");
				}

				isBound = nativeScript.InstantiateScript != nullptr;
				if (isBound && !nativeScript.ScriptName.empty())
				{
					const TypeDesc* typeDesc = TypeRegistry::Get().GetTypeDesc(nativeScript.ScriptName);
					if (typeDesc)
					{
						ImGui::Separator();
						ImGui::Text("Script Properties");
						ImGui::Spacing();

						bool owned = false;
						ScriptableEntity* preview = nullptr;
						try
						{
							preview = nativeScript.GetOrCreateEditorInstance(!isRunning, owned);
						}
						catch (const std::exception& error)
						{
							nativeScript.LastError = error.what();
							ImGui::TextWrapped("Preview failed: %s", error.what());
						}

						for (const auto& prop : typeDesc->Properties)
						{
							ImGui::PushID(prop.Name.c_str());
							std::any currentVal = nativeScript.GetErasedFieldValue(*typeDesc, prop, preview);
							bool valueChanged = false;

							if (prop.Type == DataType::Vec3)
							{
								if (currentVal.has_value())
								{
									glm::vec3 val = std::any_cast<glm::vec3>(currentVal);
									glm::vec3 oldVal = val;
									valueChanged = DrawVec3Control(prop.Name, val, 0.0f, 100.0f);
									if (valueChanged) currentVal = val;
									(void)oldVal;
								}
							}
							else
							{
								if (ImGui::BeginTable("##PropertyTable", 2, ImGuiTableFlags_Resizable))
								{
									ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 100.0f);
									ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
									ImGui::TableNextRow();
									ImGui::TableNextColumn();
									ImGui::AlignTextToFramePadding();
									ImGui::Text("%s", prop.Name.c_str());
									ImGui::TableNextColumn();
									ImGui::PushItemWidth(-1.0f);

									switch (prop.Type)
									{
										case DataType::Int32:
										{
											if (currentVal.has_value())
											{
												int32_t val = std::any_cast<int32_t>(currentVal);
												if (ImGui::DragInt("##Val", &val)) { currentVal = val; valueChanged = true; }
											}
											break;
										}
										case DataType::Float:
										{
											if (currentVal.has_value())
											{
												float val = std::any_cast<float>(currentVal);
												if (ImGui::DragFloat("##Val", &val, 0.1f)) { currentVal = val; valueChanged = true; }
											}
											break;
										}
										case DataType::Bool:
										{
											if (currentVal.has_value())
											{
												bool val = std::any_cast<bool>(currentVal);
												if (ImGui::Checkbox("##Val", &val)) { currentVal = val; valueChanged = true; }
											}
											break;
										}
										case DataType::String:
										{
											if (currentVal.has_value())
											{
												std::string val = std::any_cast<std::string>(currentVal);
												char buffer[256];
												memset(buffer, 0, sizeof(buffer));
												strncpy_s(buffer, val.c_str(), sizeof(buffer) - 1);
												if (ImGui::InputText("##Val", buffer, sizeof(buffer)))
												{
													currentVal = std::string(buffer);
													valueChanged = true;
												}
											}
											break;
										}
										case DataType::Enum:
										{
											if (currentVal.has_value())
											{
												std::vector<const char*> options;
												const EnumDesc& enumDesc = std::any_cast<EnumDesc>(prop.UserData);
												const TypeDesc* enumTypeDesc = TypeRegistry::Get().GetTypeDesc(enumDesc.Name);
												if (enumTypeDesc)
												{
													for (const auto& enumProperty : enumTypeDesc->Properties)
														options.push_back(enumProperty.Name.c_str());
													int enumVal = std::any_cast<int32_t>(currentVal);
													if (ImGui::Combo("##Val", &enumVal, options.data(), static_cast<int>(options.size())))
													{
														currentVal = enumVal;
														valueChanged = true;
													}
												}
											}
											break;
										}
										default:
											ImGui::Text("[Unsupported]");
											break;
									}
									ImGui::PopItemWidth();
									ImGui::EndTable();
								}
							}

							if (valueChanged)
							{
								nativeScript.SetErasedFieldValue(*typeDesc, prop, preview, currentVal);
								changed = true;
							}
							ImGui::PopID();
						}

						try
						{
							nativeScript.ReleaseEditorInstance(preview);
						}
						catch (const std::exception& error)
						{
							nativeScript.LastError = error.what();
						}
					}
				}
				return changed;
			});
	}

	bool DrawLuaScriptInspector(Entity entity)
	{
		return DrawComponentFold<LuaScriptComponent>("LuaScriptComponent", entity, [entity](LuaScriptComponent& component) -> bool
			{
				bool changed = false;
				const bool active = entity.GetScene()->IsActive();
				ImGui::BeginDisabled(active);
				bool isBound = !component.ScriptFilePath.empty();

				ImGui::Text("Script File");
				ImGui::SameLine();
				std::string pathString = isBound ? component.ScriptFilePath : "<Drop Lua File Here>";
				ImGui::Button(pathString.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0));

				if (!active && ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const wchar_t* path = static_cast<const wchar_t*>(payload->Data);
						std::filesystem::path scriptPath(path);
						if (scriptPath.extension() == ".lua")
						{
							component.ScriptFilePath = scriptPath.string();
							component.IsLoaded = false;
							ScriptEngine::InitScriptForEditor(component);
							changed = true;
						}
						else
						{
							WLD_CORE_WARN("Failed to bind script: '{0}' is not a .lua file!", scriptPath.filename().string());
						}
					}
					ImGui::EndDragDropTarget();
				}
				ImGui::Spacing();

				if (isBound)
				{
					if (ImGui::Button("Unbind Lua Script##Lua", ImVec2(-1.0f, 25.0f)))
					{
						component.ScriptFilePath.clear();
						component.IsLoaded = false;
						component.LuaEnv = sol::lua_nil;
						component.OnCreateFunc = sol::lua_nil;
						component.OnUpdateFunc = sol::lua_nil;
						component.OnDestroyFunc = sol::lua_nil;
						component.ScriptTable = sol::lua_nil;
						component.CachedFields.clear();
						component.State = ScriptInstanceState::Stopped;
						component.LastError.clear();
						changed = true;
					}
					if (!active && !component.ScriptFilePath.empty())
					try
					{
						std::filesystem::path filepath = WLD_ASSETPATH + std::string("/") + component.ScriptFilePath;
						if (std::filesystem::exists(filepath))
						{
							auto currentModifiedTime = std::filesystem::last_write_time(filepath);
							if (component.LastModifiedTime != currentModifiedTime)
							{
								ScriptEngine::InitScriptForEditor(component);
								component.LastModifiedTime = currentModifiedTime;
								component.IsLoaded = false;
							}
						}
					}
					catch (const std::filesystem::filesystem_error& error)
					{
						component.LastError = error.what();
					}
				}
				ImGui::EndDisabled();
				if (active) ImGui::TextWrapped("Stop the scene to rebind or reload scripts.");

				ImGui::Spacing();
				DrawScriptStatus(component.State, component.LastError);

				if (!component.CachedFields.empty())
				{
					ImGui::Separator();
					ImGui::Text("Script Properties");
					ImGui::Spacing();
					ImGui::BeginDisabled(active);
					if (active) ImGui::TextWrapped("Cached starting values; edit after stopping the scene.");

					if (ImGui::BeginTable("##LuaPropertiesTable", 2, ImGuiTableFlags_Resizable))
					{
						ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 100.0f);
						ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

						for (auto& [name, field] : component.CachedFields)
						{
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::AlignTextToFramePadding();
							ImGui::Text("%s", name.c_str());
							ImGui::TableNextColumn();
							ImGui::PushItemWidth(-1.0f);
							ImGui::PushID(name.c_str());

							switch (field.Type)
							{
								case LuaFieldType::Float:
								{
									float val = std::any_cast<float>(field.Value);
									if (ImGui::DragFloat("##val", &val, 0.1f)) changed = true;
									field.Value = val;
									break;
								}
								case LuaFieldType::Int:
								{
									int val = std::any_cast<int>(field.Value);
									if (ImGui::DragInt("##val", &val)) changed = true;
									field.Value = val;
									break;
								}
								case LuaFieldType::Bool:
								{
									bool val = std::any_cast<bool>(field.Value);
									if (ImGui::Checkbox("##val", &val)) changed = true;
									field.Value = val;
									break;
								}
								case LuaFieldType::String:
								{
									std::string val = std::any_cast<std::string>(field.Value);
									std::vector<char> buffer(std::max<size_t>(1024, val.size() + 256), '\0');
									std::copy(val.begin(), val.end(), buffer.begin());
									if (ImGui::InputText("##val", buffer.data(), buffer.size()))
									{
										field.Value = std::string(buffer.data());
										changed = true;
									}
									break;
								}
							}
							ImGui::PopID();
							ImGui::PopItemWidth();
						}
						ImGui::EndTable();
					}
					ImGui::EndDisabled();
				}
				return changed;
			});
	}

	// ---------------- 反射自动检查器 ----------------

	bool DrawAutoInspector(const TypeDesc& componentDesc, entt::id_type componentId, Entity entity)
	{
		void* instance = entity.GetComponent(componentId);
		if (!instance)
			return false;

		bool changed = false;
		for (const auto& prop : componentDesc.Properties)
		{
			const std::string label = prop.Edit.DisplayName.empty() ? prop.Name : prop.Edit.DisplayName;
			ImGui::PushID(prop.Name.c_str());
			if (prop.Edit.ReadOnly)
				ImGui::BeginDisabled();

			std::any current = componentDesc.GetValueErased(instance, prop);
			if (DrawPropertyControl(prop, label, current))
			{
				componentDesc.SetValueErased(instance, prop, current);
				changed = true;
			}

			if (prop.Edit.ReadOnly)
				ImGui::EndDisabled();
			ImGui::PopID();
		}
		return changed;
	}

	// ---------------- 静态注册 ----------------

	namespace
	{
		struct InspectorRegistration
		{
			InspectorRegistration()
			{
				InspectorRegistry::Register(entt::type_id<TagComponent>().hash(), DrawTagInspector);
				InspectorRegistry::Register(entt::type_id<UUIDComponent>().hash(), DrawUuidInspector);
				InspectorRegistry::Register(entt::type_id<TransformComponent>().hash(), DrawTransformInspector);
				InspectorRegistry::Register(entt::type_id<CameraComponent>().hash(), DrawCameraInspector);
				InspectorRegistry::Register(entt::type_id<SpriteComponent>().hash(), DrawSpriteInspector);
				InspectorRegistry::Register(entt::type_id<NativeScriptComponent>().hash(), DrawNativeScriptInspector);
				InspectorRegistry::Register(entt::type_id<LuaScriptComponent>().hash(), DrawLuaScriptInspector);
			}
		} s_InspectorRegistration;
	}
}
