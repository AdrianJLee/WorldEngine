#include "wldpch.h"
#include "SceneSerializer.h"

#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Core/UUID.h"

#include <filesystem>
#include <fstream>
#include <yaml-cpp/yaml.h>

namespace YAML
{
	template<>
	struct convert<glm::vec2>
	{
		static Node encode(const glm::vec2& vec)
		{
			Node node;
			node.push_back(vec.x);
			node.push_back(vec.y);
			return node;
		}
		static bool decode(const Node& node, glm::vec2& vec)
		{
			if (!node.IsSequence() || node.size() != 2)
				return false;
			vec.x = node[0].as<float>();
			vec.y = node[1].as<float>();
			return true;
		}
	};

	template<>
	struct convert<glm::vec3>
	{
		static Node encode(const glm::vec3& vec)
		{
			Node node;
			node.push_back(vec.x);
			node.push_back(vec.y);
			node.push_back(vec.z);
			return node;
		}
		static bool decode(const Node& node, glm::vec3& vec)
		{
			if (!node.IsSequence() || node.size() != 3)
				return false;
			vec.x = node[0].as<float>();
			vec.y = node[1].as<float>();
			vec.z = node[2].as<float>();
			return true;
		}

	};

	template<>
	struct convert<glm::vec4>
	{
		static Node encode(const glm::vec4& vec)
		{
			Node node;
			node.push_back(vec.x);
			node.push_back(vec.y);
			node.push_back(vec.z);
			node.push_back(vec.w);
			return node;
		}
		static bool decode(const Node& node, glm::vec4& vec)
		{
			if (!node.IsSequence() || node.size() != 4)
				return false;
			vec.x = node[0].as<float>();
			vec.y = node[1].as<float>();
			vec.z = node[2].as<float>();
			vec.w = node[3].as<float>();
			return true;
		}
	};
}
namespace World
{
	template<typename T>
	void AsValue(const YAML::Node& node, const char* key, T& value)
	{
		if (auto field = node[key])
		{
			try
			{
				value = field.as<T>();
			}
			catch (const std::exception& e)
			{
				WLD_CORE_WARN("Failed to parse field '{0}': {1}", key, e.what());
			}
		}
		else
		{
			WLD_CORE_WARN("Field '{0}' not found in YAML node!", key);
		}
	}

	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec2& vec)
	{
		out << YAML::Flow << YAML::BeginSeq << vec.x << vec.y << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec3& vec)
	{
		out << YAML::Flow << YAML::BeginSeq << vec.x << vec.y << vec.z << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::quat& quat)
	{
		out << YAML::Flow << YAML::BeginSeq << quat.x << quat.y << quat.z << quat.w << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec4& vec)
	{
		out << YAML::Flow << YAML::BeginSeq << vec.x << vec.y << vec.z << vec.w << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const std::filesystem::path& path)
	{
		out << path.string();
		return out;
	}



	SceneSerializer::SceneSerializer(const Ref<Scene>& scene)
		: m_Scene(scene)
	{

	}

	static void SerializeEntity(YAML::Emitter& out, Entity entity)
	{
		WLD_ASSERT(entity.HasComponent<UUIDComponent>(), "Entity does not have a UUIDComponent!");

		out << YAML::BeginMap; // Entity
		out << YAML::Key << "EntityID" << YAML::Value << (uint64_t)entity.GetComponent<UUIDComponent>().ID;

		{

			if (entity.HasComponent<TagComponent>())
			{
				auto& tag = entity.GetComponent<TagComponent>().Tag;
				out << YAML::Key << "TagComponent" << YAML::Value << tag;
			}

			if (entity.HasComponent<TransformComponent>())
			{
				auto& transform = entity.GetComponent<TransformComponent>();
				out << YAML::Key << "TransformComponent" << YAML::Value;
				out << YAML::BeginMap;
				out << YAML::Key << "Location" << YAML::Value << transform.Location;
				out << YAML::Key << "Rotation" << YAML::Value << transform.Rotation;
				out << YAML::Key << "Scale" << YAML::Value << transform.Scale;
				out << YAML::EndMap;
			}
			if (entity.HasComponent<CameraComponent>())
			{
				auto& cameraComponent = entity.GetComponent<CameraComponent>();
				auto& camera = cameraComponent.Camera;
				out << YAML::Key << "CameraComponent" << YAML::Value;
				out << YAML::BeginMap;
				out << YAML::Key << "Primary" << YAML::Value << cameraComponent.Primary;
				out << YAML::Key << "FixedAspectRatio" << YAML::Value << cameraComponent.FixedAspectRatio;
				out << YAML::Key << "ProjectionType" << YAML::Value << (int)camera.GetProjectionType();

				out << YAML::Key << "AspectRatio" << YAML::Value << camera.GetAspectRatio();

				out << YAML::Key << "PerspectiveFOV" << YAML::Value << camera.GetPerspectiveFOV();
				out << YAML::Key << "PerspectiveNearClip" << YAML::Value << camera.GetPerspectiveNearClip();
				out << YAML::Key << "PerspectiveFarClip" << YAML::Value << camera.GetPerspectiveFarClip();

				out << YAML::Key << "OrthographicZoom" << YAML::Value << camera.GetOrthographicZoom();
				out << YAML::Key << "OrthographicNearClip" << YAML::Value << camera.GetOrthographicNearClip();
				out << YAML::Key << "OrthographicFarClip" << YAML::Value << camera.GetOrthographicFarClip();

				out << YAML::EndMap;
			}

			if (entity.HasComponent<SpriteComponent>())
			{
				auto& sprite = entity.GetComponent<SpriteComponent>();
				out << YAML::Key << "SpriteComponent" << YAML::Value;
				out << YAML::BeginMap;
				out << YAML::Key << "Color" << YAML::Value << sprite.Color;

				if (sprite.Texture)
					out << YAML::Key << "TexturePath" << YAML::Value << sprite.Texture->GetPath();

				out << YAML::Key << "TilingFactor" << YAML::Value << sprite.TilingFactor;
				out << YAML::EndMap;
			}
			if (entity.HasComponent<CircleRendererComponent>())
			{
				auto& circleRenderer = entity.GetComponent<CircleRendererComponent>();
				out << YAML::Key << "CircleRendererComponent" << YAML::Value;
				out << YAML::BeginMap;
				out << YAML::Key << "Color" << YAML::Value << circleRenderer.Color;
				out << YAML::Key << "Thickness" << YAML::Value << circleRenderer.Thickness;
				out << YAML::Key << "Fade" << YAML::Value << circleRenderer.Fade;
				out << YAML::EndMap;
			}

			if (entity.HasComponent<BoxCollider2DComponent>())
			{
				auto& boxCollider = entity.GetComponent<BoxCollider2DComponent>();
				out << YAML::Key << "BoxCollider2DComponent" << YAML::Value;
				out << YAML::BeginMap;
				out << YAML::Key << "Size" << YAML::Value << boxCollider.Size;
				out << YAML::Key << "Offset" << YAML::Value << boxCollider.Offset;
				out << YAML::Key << "Density" << YAML::Value << boxCollider.Density;
				out << YAML::Key << "Friction" << YAML::Value << boxCollider.Friction;
				out << YAML::Key << "Restitution" << YAML::Value << boxCollider.Restitution;
				out << YAML::Key << "ShowCollider" << YAML::Value << boxCollider.ShowCollider;
				out << YAML::EndMap;
			}
			if (entity.HasComponent<CircleCollider2DComponent>())
			{
				auto& circleCollider = entity.GetComponent<CircleCollider2DComponent>();
				out << YAML::Key << "CircleCollider2DComponent" << YAML::Value;
				out << YAML::BeginMap;
				out << YAML::Key << "Radius" << YAML::Value << circleCollider.Radius;
				out << YAML::Key << "Offset" << YAML::Value << circleCollider.Offset;
				out << YAML::Key << "Density" << YAML::Value << circleCollider.Density;
				out << YAML::Key << "Friction" << YAML::Value << circleCollider.Friction;
				out << YAML::Key << "Restitution" << YAML::Value << circleCollider.Restitution;
				out << YAML::Key << "ShowCollider" << YAML::Value << circleCollider.ShowCollider;
				out << YAML::EndMap;
			}
			if (entity.HasComponent<RigidBody2DComponent>())
			{
				auto& rb = entity.GetComponent<RigidBody2DComponent>();
				out << YAML::Key << "RigidBody2DComponent" << YAML::Value;
				out << YAML::BeginMap;
				out << YAML::Key << "BodyType" << YAML::Value << (int)rb.Type;
				out << YAML::Key << "FixedRotation" << YAML::Value << rb.FixedRotation;
				out << YAML::EndMap;
			}
			if (entity.HasComponent<NativeScriptComponent>())
			{
				auto& script = entity.GetComponent<NativeScriptComponent>();
				out << YAML::Key << "NativeScriptComponent" << YAML::Value;
				out << YAML::BeginMap;
				out << YAML::Key << "ScriptName" << YAML::Value << script.ScriptName;
				out << YAML::EndMap;
			}
		}

		out << YAML::EndMap;
	}
	void SceneSerializer::Serialize(const std::string& filepath)
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		{
			out << YAML::Key << "Scene" << YAML::Value << "Untitled";

			out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;

			// 使用反向迭代器遍历实体，以修正保存加载后的顺序颠倒问题
			auto& entities = m_Scene->m_Registry.storage<entt::entity>();
			for (auto it = entities.rbegin(); it != entities.rend(); ++it)
			{
				Entity ent { m_Scene.get(), *it };
				if (!ent)
				{
					return;
				};

				SerializeEntity(out, ent);
			}
			out << YAML::EndSeq;
		}
		out << YAML::EndMap;


		{
			// 1. 将字符串路径转换为 path 对象
			std::filesystem::path path(filepath);

			// 2. 获取该文件所在的父目录路径
			// 例如：filepath 是 "assets/scenes/level.yaml"，parentPath 就是 "assets/scenes"
			std::filesystem::path parentPath = path.parent_path();

			// 3. 检查并创建路径
			// 如果 parentPath 为空（说明文件就在当前目录下），create_directories 会安全跳过
			if (!parentPath.empty() && !std::filesystem::exists(parentPath))
			{
				// create_directories 会创建多级目录（不存在的都会补齐）
				std::filesystem::create_directories(parentPath);
			}

			// 4. 现在可以放心地创建并写入文件了
			std::ofstream fout(filepath);
			if (fout.is_open())
			{
				fout << out.c_str();
				fout.close();
			}
		}
	}
	bool SceneSerializer::Deserialize(const std::string& filepath)
	{
		std::ifstream stream(filepath);
		if (!stream.is_open())
		{
			WLD_CORE_ERROR("Could not open file '{0}'", filepath);
			return false;
		}
		std::stringstream strstream;
		// 1. 将文件内容读入字符串流
		strstream << stream.rdbuf();

		YAML::Node data = YAML::Load(strstream.str());
		if (!data["Scene"])
		{
			WLD_CORE_ERROR("Could not find 'Scene' node in '{0}'", filepath);
			return false;
		}

		std::string sceneName = data["Scene"].as<std::string>();
		WLD_CORE_TRACE("Deserializing scene '{0}'", sceneName);

		{

			auto entities = data["Entities"];
			if (entities)
			{
				for (auto entity : entities)
				{
					uint64_t entityID = entity["EntityID"].as<uint64_t>();
					std::string name;
					auto tagComponent = entity["TagComponent"];
					if (tagComponent)
						name = tagComponent.as<std::string>();
					WLD_CORE_TRACE("Deserialized entity with ID: {0}, name: {1}", entityID, name);

					Entity deserializedEntity = Entity::CreateEntity(m_Scene.get(), name, World::UUID(entityID));

					{
						auto transformComponent = entity["TransformComponent"];
						if (transformComponent)
						{
							auto& transform = deserializedEntity.AddComponent<TransformComponent>();

							glm::vec3 Location;
							glm::vec3 Rotation;
							glm::vec3 Scale;
							AsValue<glm::vec3>(transformComponent, "Location", Location);
							AsValue<glm::vec3>(transformComponent, "Rotation", Rotation);
							AsValue<glm::vec3>(transformComponent, "Scale", Scale);
							transform.SetTransform(Location, Rotation, Scale);
						}
					}
					{
						auto cameraComponent = entity["CameraComponent"];
						if (cameraComponent)
						{
							auto& cameraComp = deserializedEntity.AddComponent<CameraComponent>();
							AsValue<bool>(cameraComponent, "Primary", cameraComp.Primary);
							AsValue<bool>(cameraComponent, "FixedAspectRatio", cameraComp.FixedAspectRatio);

							int projectionType;
							AsValue<int>(cameraComponent, "ProjectionType", projectionType);
							cameraComp.Camera.SetProjectionType((SceneCamera::ProjectionType)projectionType);

							float perspectiveFOV;
							AsValue<float>(cameraComponent, "PerspectiveFOV", perspectiveFOV);
							cameraComp.Camera.SetPerspectiveFOV(perspectiveFOV);

							float perspectiveNearClip;
							AsValue<float>(cameraComponent, "PerspectiveNearClip", perspectiveNearClip);
							cameraComp.Camera.SetPerspectiveNearClip(perspectiveNearClip);

							float perspectiveFarClip;
							AsValue<float>(cameraComponent, "PerspectiveFarClip", perspectiveFarClip);
							cameraComp.Camera.SetPerspectiveFarClip(perspectiveFarClip);

							float orthographicZoom;
							AsValue<float>(cameraComponent, "OrthographicZoom", orthographicZoom);
							cameraComp.Camera.SetOrthographicZoom(orthographicZoom);

							float orthographicNearClip;
							AsValue<float>(cameraComponent, "OrthographicNearClip", orthographicNearClip);
							cameraComp.Camera.SetOrthographicNearClip(orthographicNearClip);

							float orthographicFarClip;
							AsValue<float>(cameraComponent, "OrthographicFarClip", orthographicFarClip);
							cameraComp.Camera.SetOrthographicFarClip(orthographicFarClip);
						}
					}
					{
						auto spriteComponent = entity["SpriteComponent"];
						if (spriteComponent)
						{
							auto& sprite = deserializedEntity.AddComponent<SpriteComponent>();
							AsValue<glm::vec4>(spriteComponent, "Color", sprite.Color);

							// 加载纹理
							if (spriteComponent["TexturePath"])
							{
								std::string texturePath;
								AsValue<std::string>(spriteComponent, "TexturePath", texturePath);
								sprite.Texture = Texture2D::Create(texturePath);
							}

							// 加载平铺系数
							AsValue<float>(spriteComponent, "TilingFactor", sprite.TilingFactor);
						}
					}
					{
						auto circleRendererComponent = entity["CircleRendererComponent"];
						if (circleRendererComponent)
						{
							auto& circleRenderer = deserializedEntity.AddComponent<CircleRendererComponent>();
							AsValue<glm::vec4>(circleRendererComponent, "Color", circleRenderer.Color);
							AsValue<float>(circleRendererComponent, "Thickness", circleRenderer.Thickness);
							AsValue<float>(circleRendererComponent, "Fade", circleRenderer.Fade);

						}
					}
					{
						auto boxColliderComponent = entity["BoxCollider2DComponent"];
						if (boxColliderComponent)
						{
							auto& boxCollider = deserializedEntity.AddComponent<BoxCollider2DComponent>();
							AsValue<glm::vec2>(boxColliderComponent, "Size", boxCollider.Size);
							AsValue<glm::vec2>(boxColliderComponent, "Offset", boxCollider.Offset);
							AsValue<float>(boxColliderComponent, "Density", boxCollider.Density);
							AsValue<float>(boxColliderComponent, "Friction", boxCollider.Friction);
							AsValue<float>(boxColliderComponent, "Restitution", boxCollider.Restitution);
							AsValue<bool>(boxColliderComponent, "ShowCollider", boxCollider.ShowCollider);
						}

					}
					{
						auto circleColliderComponent = entity["CircleCollider2DComponent"];
						if (circleColliderComponent)
						{
							auto& circleCollider = deserializedEntity.AddComponent<CircleCollider2DComponent>();
							AsValue<float>(circleColliderComponent, "Radius", circleCollider.Radius);
							AsValue<glm::vec2>(circleColliderComponent, "Offset", circleCollider.Offset);
							AsValue<float>(circleColliderComponent, "Density", circleCollider.Density);
							AsValue<float>(circleColliderComponent, "Friction", circleCollider.Friction);
							AsValue<float>(circleColliderComponent, "Restitution", circleCollider.Restitution);
							AsValue<bool>(circleColliderComponent, "ShowCollider", circleCollider.ShowCollider);
						}
					}
					{
						auto rigidBodyComponent = entity["RigidBody2DComponent"];
						if (rigidBodyComponent)
						{
							auto& rb = deserializedEntity.AddComponent<RigidBody2DComponent>();
							AsValue<int>(rigidBodyComponent, "BodyType", (int&)rb.Type);
							AsValue<bool>(rigidBodyComponent, "FixedRotation", rb.FixedRotation);
						}
					}
					{
						auto nativeScriptComponent = entity["NativeScriptComponent"];
						if (nativeScriptComponent)
						{
							auto& script = deserializedEntity.AddComponent<NativeScriptComponent>();
							AsValue<std::string>(nativeScriptComponent, "ScriptName", script.ScriptName);
							if (!script.ScriptName.empty())
							{
								for (const auto& scriptName : TypeRegistry::Get().GetTypesByCategory(TypeCategory::Script))
								{
									if (TypeDescDataScript* scriptInfo = std::any_cast<TypeDescDataScript>(&TypeRegistry::Get().GetTypeDesc(scriptName)->UserData))
									{
										if (script.ScriptName == scriptName)
										{
											if (scriptInfo->BindFunc)
											{
												scriptInfo->BindFunc(script);
											}
											break;
										}
									}
								}
							}
						}
					}
				}
			}

		}

		return true;
	}
	void SceneSerializer::SerializeRuntime(const std::string& filepath)
	{

	}
	bool SceneSerializer::DeserializeRuntime(const std::string& filepath)
	{
		return false;
	}
}