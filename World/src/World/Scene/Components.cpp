#include "wldpch.h"
#include "Components.h"
#include "World/Core/Memory/PoolAllocator.h"
#include "World/ImGui/ImGuiDrawLibrary.h"

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

namespace World
{
	void TagComponent::ComponentPropertiesUI(Entity entity)
	{
		if (entity.HasComponent<TagComponent>())
		{
			auto& tag = entity.GetComponent<TagComponent>().Tag;
			char buffer[256];
			memset(buffer, 0, sizeof(buffer));
			strcpy_s(buffer, sizeof(buffer), tag.c_str());
			if (ImGui::InputText("##Tag", buffer, sizeof(buffer)))
			{
				tag = std::string(buffer);
			}
		}
	}

	void UUIDComponent::ComponentPropertiesUI(Entity entity)
	{
		if (entity.HasComponent<UUIDComponent>())
		{
			auto& uuid = entity.GetComponent<UUIDComponent>().ID;
			ImGui::Text("UUID: %llu", (uint64_t)uuid);
		}
	}

	void TransformComponent::ComponentPropertiesUI(Entity entity)
	{
		ImGuiDrawLibrary::DrawComponent<TransformComponent>("TransformComponent", entity, [](TransformComponent& transform)
			{
				ImGuiDrawLibrary::DrawVec3Control("Location", transform.Location, 0.0f, 75.0f);

				glm::vec3 rotationDegrees = glm::degrees(transform.Rotation);
				ImGuiDrawLibrary::DrawVec3Control("Rotation", rotationDegrees, 0.0f, 75.0f);
				transform.Rotation = glm::radians(rotationDegrees);

				ImGuiDrawLibrary::DrawVec3Control("Scale", transform.Scale, 1.0f, 75.0f);

				transform.SetTransform(transform.Location, transform.Rotation, transform.Scale);
			});
	}

	void CameraComponent::ComponentPropertiesUI(Entity entity)
	{
		ImGuiDrawLibrary::DrawComponent<CameraComponent>("CameraComponent", entity, [](CameraComponent& cameraComponent)
			{
				auto& camera = cameraComponent.Camera;


				ImGui::Checkbox("Primary", &cameraComponent.Primary);
				ImGui::Checkbox("Fixed Aspect Ratio", &cameraComponent.FixedAspectRatio);
				const char* projectionTypeStrings[] = { "Perspective", "Orthographic" };
				// Get the current projection type as a string
				const char* currentProjectionTypeString = projectionTypeStrings[(int)camera.GetProjectionType()];
				if (ImGui::BeginCombo("Projection", currentProjectionTypeString))
				{
					for (int i = 0; i < 2; i++)
					{
						bool isSelected = (camera.GetProjectionType() == (SceneCamera::ProjectionType)i);
						if (ImGui::Selectable(projectionTypeStrings[i], isSelected))
						{
							camera.SetProjectionType((SceneCamera::ProjectionType)i);
						}
					}
					ImGui::EndCombo();
				}

				if (camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic)
				{
					float zoom = camera.GetOrthographicZoom();
					float nearClip = camera.GetOrthographicNearClip();
					float farClip = camera.GetOrthographicFarClip();
					if (ImGui::DragFloat("Zoom", &zoom, 0.1f))
					{
						camera.SetOrthographicZoom(zoom);
					}
					if (ImGui::DragFloat("Near Clip", &nearClip, 0.1f))
					{
						camera.SetOrthographicNearClip(nearClip);
					}
					if (ImGui::DragFloat("Far Clip", &farClip, 0.1f))
					{
						camera.SetOrthographicFarClip(farClip);
					}
				}
				else if (camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
				{
					float fov = camera.GetPerspectiveFOV();
					float nearClip = camera.GetPerspectiveNearClip();
					float farClip = camera.GetPerspectiveFarClip();
					if (ImGui::DragFloat("FOV", &fov, 0.1f, 0.0f, 180.0f))
					{
						camera.SetPerspectiveFOV(fov);
					}
					if (ImGui::DragFloat("Near Clip", &nearClip, 0.1f))
					{
						camera.SetPerspectiveNearClip(nearClip);
					}
					if (ImGui::DragFloat("Far Clip", &farClip, 0.1f))
					{
						camera.SetPerspectiveFarClip(farClip);
					}
				}
			});
	}

	void SpriteComponent::ComponentPropertiesUI(Entity entity)
	{

		ImGuiDrawLibrary::DrawComponent<SpriteComponent>("SpriteComponent", entity, [](SpriteComponent& sprite)
			{
				ImGui::ColorEdit4("Color", glm::value_ptr(sprite.Color));

				// 2. 纹理槽位 (Texture Slot)
				ImGui::Text("Texture");
				ImGui::NextColumn();

				uint32_t textureID = sprite.Texture ? sprite.Texture->GetRendererID() : 0;
				ImVec2 textureSize = { 64.0f, 64.0f };

				// 使用 ImageButton 增加可点击的视觉交互
				bool clicked = ImGui::ImageButton((ImTextureID)(uint64_t)textureID, textureSize, { 0, 1 }, { 1, 0 });
				if (clicked)
				{
					// 这里可以绑定点击打开本地文件选取窗口（如 FileDialog::OpenFile）
					// 或者打开游戏内的资产选择器面板
				}

				// --- 悬停提示 (Tooltip) ---
				if (ImGui::IsItemHovered())
				{
					ImGui::BeginTooltip();
					ImGui::Text("Drag and Drop from Content Browser");
					if (sprite.Texture)
					{
						ImGui::Spacing();
						// 悬停时显示更大尺寸的预览图
						ImGui::Image((ImTextureID)(uint64_t)textureID, { 256.0f, 256.0f }, { 0, 1 }, { 1, 0 });
					}
					ImGui::EndTooltip();
				}

				// --- 接受拖拽 (Accept Drag and Drop) ---
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const wchar_t* path = (const wchar_t*)payload->Data;

						std::filesystem::path texturePath = std::filesystem::path("assets") / path;
						sprite.Texture = Texture2D::Create(texturePath.string());
					}
					ImGui::EndDragDropTarget();
				}

				// 在纹理槽位右边显示文字状态和删除按钮
				ImGui::SameLine();
				ImGui::BeginGroup();
				if (sprite.Texture)
				{
					// 你也可以在这里调用 sprite.Texture->GetPath() 获取文件名并展示
					ImGui::Text(sprite.Texture->GetPath().c_str());
					if (ImGui::Button("Clear", ImVec2(50.0f, 0.0f)))
					{
						sprite.Texture = nullptr;
					}
				}
				else
				{
					ImGui::Text("None");
				}
				ImGui::EndGroup();


				// 平铺因子编辑
				ImGui::DragFloat("Tiling", &sprite.TilingFactor, 0.1f, 0.0f, 100.0f);

			});
	}

	void CircleRendererComponent::ComponentPropertiesUI(Entity entity)
	{
		ImGuiDrawLibrary::DrawComponent<CircleRendererComponent>("CircleRendererComponent", entity, [](CircleRendererComponent& circleRenderer)
			{
				ImGui::ColorEdit4("Color", glm::value_ptr(circleRenderer.Color));
				ImGui::DragFloat("Thickness", &circleRenderer.Thickness, 0.1f, 0.0f, 1.0f);
				ImGui::DragFloat("Fade", &circleRenderer.Fade, 0.001f, 0.0f, 1.0f);
			});
	}

	void NativeScriptComponent::ComponentPropertiesUI(Entity entity)
	{
		ImGuiDrawLibrary::DrawComponent<NativeScriptComponent>("NativeScriptComponent", entity, [](NativeScriptComponent& nativeScript) mutable
			{
				// 指针不为空则说明绑定了脚本
				bool isBound = (nativeScript.InstantiateScript != nullptr);

				// 使用刚刚我们在 Bind 中存下来的 ScriptName，如果是空的话显示 <None>
				const char* currentPreview = isBound ? (nativeScript.ScriptName.empty() ? "Unknown Script" : nativeScript.ScriptName.c_str()) : "<None>";
				// 如果脚本实例正在运行了，就不允许修改绑定了
				bool isRunning = (nativeScript.Instance != nullptr);

				if (isRunning)
				{
					ImGui::BeginDisabled(true); // 禁用以下控件
				}

				// 下拉列表
				if (ImGui::BeginCombo("Script Class", currentPreview))
				{
					// 提供置空选项
					if (ImGui::Selectable("<None>", !isBound))
					{
						nativeScript.InstantiateScript = nullptr;
						nativeScript.DestroyScript = nullptr;
						nativeScript.ScriptName = "";
						nativeScript.FieldValues.clear();
						nativeScript.isFirstDraw = true;
					}

					// 遍历注册表中所有脚本类型，展示在下拉列表中
					for (const auto& scriptName : TypeRegistry::Get().GetTypesByCategory(TypeCategory::Script))
					{
						if (TypeDescDataScript* scriptInfo = std::any_cast<TypeDescDataScript>(&TypeRegistry::Get().GetTypeDesc(scriptName)->UserData))
						{
							// 这个项是否已被选中？对比名字即可
							bool isSelected = (scriptName == nativeScript.ScriptName);
							if (ImGui::Selectable(scriptName.c_str(), isSelected))
							{
								// 通过工厂方法执行具体 T 的绑定 ( Bind<T>() )
								if (scriptInfo->BindFunc)
								{
									scriptInfo->BindFunc(nativeScript);

									nativeScript.ScriptName = scriptName; // 更新当前绑定的脚本名字
									nativeScript.FieldValues.clear();
									nativeScript.isFirstDraw = true;
								}
							}

							// 焦点选中定位
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
						// 清空函数指针和数据
						nativeScript.InstantiateScript = nullptr;
						nativeScript.DestroyScript = nullptr;
						nativeScript.ScriptName = "";
						nativeScript.FieldValues.clear();
						nativeScript.isFirstDraw = true;
					}
				}

				// 如果脚本正在运行，则禁用重新绑定和解绑按钮
				if (isRunning)
				{
					ImGui::EndDisabled();

					// 禁用提示
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					{
						ImGui::SetTooltip("Cannot rebind or unbind scripts while the game is running.");
					}
				}


				// 如果已经绑定了脚本，才显示下面的属性编辑界面
				if (isBound)
				{
					ImGui::Spacing();

					// ============================================
					// 自动生成脚本实例或缓存属性的 UI 界面
					// ============================================
					if (!nativeScript.ScriptName.empty())
					{
						const TypeDesc* typeDesc = TypeRegistry::Get().GetTypeDesc(nativeScript.ScriptName);
						if (typeDesc)
						{
							ImGui::Separator();
							ImGui::Text("Script Properties");
							ImGui::Spacing();

							ScriptableEntity* tempInstance = nullptr;

							if (nativeScript.Instance)
							{
								// 已经存在实例了，直接用它来反射获取当前值
								tempInstance = nativeScript.Instance;
							}
							else
							{
								if (nativeScript.isFirstDraw)
								{
									tempInstance = nativeScript.InstantiateScript();

								}
							}

							for (const auto& prop : typeDesc->Properties)
							{
								ImGui::PushID(prop.Name.c_str());

								// 1. 获取当前数据
								std::any currentVal;
								if (tempInstance)
								{
									currentVal = typeDesc->GetValueErased(dynamic_cast<void*>(tempInstance), prop);
									nativeScript.FieldValues[prop.Name] = currentVal;
								}
								else
								{
									auto it = nativeScript.FieldValues.find(prop.Name);
									if (it != nativeScript.FieldValues.end())
									{
										currentVal = it->second;
									}
									else
									{
										switch (prop.Type)
										{
											case DataType::Int32:
												currentVal = int32_t(0);
												break;
											case DataType::Float:
												currentVal = float(0.0f);
												break;
											case DataType::Bool:
												currentVal = bool(false);
												break;
											case DataType::Vec2:
												currentVal = glm::vec2(0.0f);
												break;
											case DataType::Vec3:
												currentVal = glm::vec3(0.0f);
												break;
											case DataType::String:
												currentVal = std::string("");
												break;
											case DataType::Enum:
												currentVal = 0;
												break;
											default: break;
										}
									}
								}

								bool valueChanged = false;

								// 2. 绘制 UI 控制器
								if (prop.Type == DataType::Vec3)
								{
									// Vec3 控制器自身已封装了带有名字在前的表格布局
									if (currentVal.has_value())
									{
										glm::vec3 val = std::any_cast<glm::vec3>(currentVal);
										glm::vec3 oldVal = val;
										ImGuiDrawLibrary::DrawVec3Control(prop.Name, val, 0.0f, 100.0f);
										if (val != oldVal) { currentVal = val; valueChanged = true; }
									}
								}
								else
								{
									// 对其它属性构建 2 列对齐表格
									if (ImGui::BeginTable("##PropertyTable", 2, ImGuiTableFlags_Resizable))
									{
										// 第一列宽固定 100 像素，维持跟 Vec3 控制器相同的对齐感
										ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 100.0f);
										ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
										ImGui::TableNextRow();

										ImGui::TableNextColumn();
										ImGui::AlignTextToFramePadding();
										ImGui::Text("%s", prop.Name.c_str()); // 名字在左边

										ImGui::TableNextColumn();
										ImGui::PushItemWidth(-1.0f); // 填满右边一列

										switch (prop.Type)
										{
											case DataType::Int32:
											{
												if (currentVal.has_value())
												{
													int32_t val = std::any_cast<int32_t>(currentVal);
													// 使用 "##XXX" 隐藏 ImGui 自身挂在后方的名字
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
													for (const auto& enumProperty : enumTypeDesc->Properties)
													{
														options.push_back(enumProperty.Name.c_str());
													}
													int enumVal = std::any_cast<int>(currentVal);


													if (ImGui::Combo("##Val", &enumVal, options.data(), static_cast<int>(options.size())))
													{
														currentVal = enumVal;
														valueChanged = true;
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

								// 3. 将新的值写回调
								if (valueChanged)
								{
									if (nativeScript.Instance)
										typeDesc->SetValueErased(tempInstance, prop, currentVal);
									else
										nativeScript.FieldValues[prop.Name] = currentVal;
								}
								ImGui::PopID();
							}


							// 第一次绘制时，如果是通过实例获取的值，绘制完后就销毁实例，避免内存泄漏；后续绘制则直接使用缓存的值
							if (nativeScript.isFirstDraw && tempInstance)
							{
								nativeScript.isFirstDraw = false;
								nativeScript.DestroyScript(tempInstance);
							}
						}
					}
				}



			});
	}
	void RigidBody2DComponent::ComponentPropertiesUI(Entity entity)
	{
		ImGuiDrawLibrary::DrawComponent<RigidBody2DComponent>("RigidBody2DComponent", entity, [](RigidBody2DComponent& rigidBody)
			{
				const char* bodyTypes[] = { "Static", "Dynamic", "Kinematic" };
				int currentType = static_cast<int>(rigidBody.Type);
				if (ImGui::Combo("Body Type", &currentType, bodyTypes, IM_ARRAYSIZE(bodyTypes)))
				{
					rigidBody.Type = static_cast<RigidBody2DComponent::BodyType>(currentType);
				}

				ImGui::Checkbox("Fixed Rotation", &rigidBody.FixedRotation);
			});
	}

	void BoxCollider2DComponent::ComponentPropertiesUI(Entity entity)
	{
		ImGuiDrawLibrary::DrawComponent<BoxCollider2DComponent>("BoxCollider2DComponent", entity, [](BoxCollider2DComponent& boxCollider)
			{
				ImGui::DragFloat2("Offset", glm::value_ptr(boxCollider.Offset), 0.1f);
				ImGui::DragFloat2("Size", glm::value_ptr(boxCollider.Size), 0.1f, 0.0f);
				ImGui::DragFloat("Density", &boxCollider.Density, 0.1f, 0.0f);
				ImGui::DragFloat("Friction", &boxCollider.Friction, 0.1f, 0.0f);
				ImGui::DragFloat("Restitution", &boxCollider.Restitution, 0.1f, 0.0f);
				ImGui::Checkbox("Show Collider", &boxCollider.ShowCollider);
			});
	}

	void CircleCollider2DComponent::ComponentPropertiesUI(Entity entity)
	{
		ImGuiDrawLibrary::DrawComponent<CircleCollider2DComponent>("CircleCollider2DComponent", entity, [](CircleCollider2DComponent& circleCollider)
			{
				ImGui::DragFloat2("Offset", glm::value_ptr(circleCollider.Offset), 0.1f);
				ImGui::DragFloat("Radius", &circleCollider.Radius, 0.1f, 0.0f);
				ImGui::DragFloat("Density", &circleCollider.Density, 0.1f, 0.0f);
				ImGui::DragFloat("Friction", &circleCollider.Friction, 0.1f, 0.0f);
				ImGui::DragFloat("Restitution", &circleCollider.Restitution, 0.1f, 0.0f);
				ImGui::Checkbox("Show Collider", &circleCollider.ShowCollider);
			});
	}
}