#pragma once
#include "World/Core/ComponentRegistry.h"
#include "World/Core/UUID.h"
#include "World/Scene/SceneCamera.h"
#include "World/Scene/ScriptableEntity.h"
#include "World/Renderer/Texture.h"
#include "World/Reflection/Reflection.h"


#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "sol/sol.hpp"
namespace World
{
	struct UUIDComponent
	{
		REFLECT_BODY(UUIDComponent, TypeCategory::Component);

		PROPERTY(ID);
		World::UUID ID;

		UUIDComponent() = default;
		UUIDComponent(const World::UUID& id)
			: ID(id)
		{}

		COMPONENT_UI()
	};

	struct TagComponent
	{
		REFLECT_BODY(TagComponent, TypeCategory::Component);


		PROPERTY(Tag);
		std::string Tag;

		TagComponent() = default;
		TagComponent(const std::string& tag)
			: Tag(tag)
		{}

		COMPONENT_UI()
	};

	struct TransformComponent
	{
		REFLECT_BODY(TransformComponent, TypeCategory::Component);

		PROPERTY(Location);
		glm::vec3 Location { 0.0f, 0.0f, 0.0f };

		PROPERTY(Rotation);
		glm::vec3 Rotation { 0.0f, 0.0f, 0.0f };

		PROPERTY(RotationQuat);
		glm::quat RotationQuat { 1.0f, 0.0f, 0.0f, 0.0f };

		PROPERTY(Scale);
		glm::vec3 Scale { 1.0f, 1.0f, 1.0f };

		PROPERTY(Transform);
		glm::mat4 Transform { 1.0f };

		TransformComponent(const glm::mat4& transform)
		{
			SetTransform(transform);
		}
		TransformComponent(const glm::vec3& location = glm::vec3 { 0.0f, 0.0f, 0.0f }, const glm::vec3& rotation = glm::vec3 { 0.0f, 0.0f, 0.0f }, const glm::vec3& scale = glm::vec3 { 1.0f,1.0f,1.0f })
		{
			SetTransform(location, rotation, scale);
		}

		void SetLocation(const glm::vec3& location)
		{
			Location = location;
			RecalculateTransform();
		}
		void SetRotation(const glm::vec3& rotation)
		{
			Rotation = rotation;
			RotationQuat = glm::quat(rotation);
			RecalculateTransform();
		}
		void SetScale(const glm::vec3& scale)
		{
			Scale = scale;
			RecalculateTransform();
		}
		void SetTransform(const glm::vec3& location, const glm::vec3& rotation, const glm::vec3& scale)
		{
			Location = location;
			Rotation = rotation;
			Scale = scale;
			RotationQuat = glm::quat(rotation);
			RecalculateTransform();
		}
		void SetTransform(const glm::mat4& transform)
		{
			Transform = transform;
			// 1. 提取位置 (Translation)
			Location = glm::vec3(transform[3]);

			// 2. 提取缩放 (Scale) 并处理负缩放
			// 这种方法通过计算行列式的正负来尝试保留镜像信息
			Scale.x = glm::length(glm::vec3(transform[0]));
			Scale.y = glm::length(glm::vec3(transform[1]));
			Scale.z = glm::length(glm::vec3(transform[2]));

			// 如果行列式为负，说明存在镜像变换，通常需要修正一个轴
			if (glm::determinant(transform) < 0)
				Scale.x *= -1.0f;

			// 3. 提取旋转 (Rotation)
			// 必须先移除缩放的影响，否则 quat_cast 的结果是错误的
			glm::mat4 rotationMatrix = transform;
			rotationMatrix[0] /= Scale.x;
			rotationMatrix[1] /= Scale.y;
			rotationMatrix[2] /= Scale.z;
			rotationMatrix[3] = glm::vec4(0, 0, 0, 1); // 清除平移

			RotationQuat = glm::quat_cast(rotationMatrix);
			Rotation = glm::eulerAngles(RotationQuat); // 注意：这里返回的是弧度
		}
		void SetRotationQuat(const glm::quat& rotationQuat)
		{
			RotationQuat = rotationQuat;
			Rotation = glm::eulerAngles(rotationQuat);
			RecalculateTransform();
		}
		void RecalculateTransform()
		{
			glm::mat4 rotationMatrix = glm::mat4_cast(RotationQuat);
			glm::mat4 scaleMatrix = glm::scale(glm::mat4(1.0f), Scale);
			glm::mat4 translationMatrix = glm::translate(glm::mat4(1.0f), Location);
			Transform = translationMatrix * rotationMatrix * scaleMatrix;
		}

		operator const glm::mat4& () const
		{
			return Transform;
		}

		COMPONENT_UI()
	};

	struct SpriteComponent
	{
		REFLECT_BODY(SpriteComponent, TypeCategory::Component);

		SpriteComponent()
		{

		};

		SpriteComponent(const glm::vec4& color)
			: Color(color)
		{

		};

		PROPERTY(Color);
		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };

		PROPERTY(Texture);
		Ref<Texture2D> Texture;

		PROPERTY(TilingFactor);
		float TilingFactor = 1.0f;

		COMPONENT_UI()
	};

	struct CircleRendererComponent
	{
		REFLECT_BODY(CircleRendererComponent, TypeCategory::Component);

		PROPERTY(Color);
		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };
		PROPERTY(Thickness);
		float Thickness = 1.0f;
		PROPERTY(Fade);
		float Fade = 0.005f;

		COMPONENT_UI()
	};

	struct CameraComponent
	{
		REFLECT_BODY(CameraComponent, TypeCategory::Component);

		CameraComponent() = default;
		CameraComponent(const SceneCamera& sceneCamera, bool primary = false)
			:Camera(sceneCamera),
			Primary(primary)
		{

		}
		PROPERTY(Camera);
		SceneCamera Camera;

		PROPERTY(Primary);
		bool Primary = true;

		PROPERTY(FixedAspectRatio);
		bool FixedAspectRatio = false;

		COMPONENT_UI()
	};

	struct NativeScriptComponent
	{
		REFLECT_BODY(NativeScriptComponent, TypeCategory::Component);

		ScriptableEntity* Instance = nullptr;
		// 原始函数指针
		ScriptableEntity* (*InstantiateScript)() = nullptr;
		void (*DestroyScript)(ScriptableEntity*&) = nullptr;

		PROPERTY(ScriptName);
		std::string ScriptName;

		template<typename T>
		void Bind()
		{
			InstantiateScript = []()
				{
					return static_cast<ScriptableEntity*>(WLD_POOL_NEW(T));
				};
			DestroyScript = [](ScriptableEntity*& scriptableEntity)
				{

					WLD_POOL_DELETE(T, PoolTag::General, scriptableEntity);
					scriptableEntity = nullptr;
				};
		}

		COMPONENT_UI();

		std::unordered_map<std::string, std::any> FieldValues;
	private:
		bool isFirstDraw = true;
	};


	enum class LuaFieldType { None, Float, Int, Bool, String };
	struct LuaScriptField
	{
		LuaFieldType Type = LuaFieldType::None;
		std::any Value;

		static std::string GetLuaTypeName(LuaFieldType type)
		{
			switch (type)
			{
				case LuaFieldType::None: return "any";
				case LuaFieldType::Float: return "number";
				case LuaFieldType::Int: return "number";
				case LuaFieldType::Bool: return "boolean";
				case LuaFieldType::String: return "string";
				default: return "any";
			}
		}
	};

	struct LuaScriptComponent
	{
		REFLECT_BODY(LuaScriptComponent, TypeCategory::Component);

		PROPERTY(ScriptFilePath);
		std::string ScriptFilePath = ""; // Lua 文件的路径，比如 "assets/scripts/Player.lua"

		// 核心：为每个实体创建一个独立的 Lua 运行环境，防止变量冲突
		sol::environment LuaEnv;
		sol::table ScriptTable;
		// 缓存从 Lua 文件里读取出来的函数
		sol::protected_function OnCreateFunc;
		sol::protected_function OnUpdateFunc;
		sol::protected_function OnDestroyFunc;

		std::unordered_map<std::string, LuaScriptField> CachedFields;
		std::filesystem::file_time_type LastModifiedTime;

		// 标记是否已经加载过文件
		bool IsLoaded = false;

		LuaScriptComponent() = default;
		LuaScriptComponent(const LuaScriptComponent&) = default;
		LuaScriptComponent(const std::string& path) : ScriptFilePath(path) {}

		COMPONENT_UI()
	};

	struct RigidBody2DComponent
	{
		REFLECT_BODY(RigidBody2DComponent, TypeCategory::Component);


		enum class BodyType
		{
			Static = 0, Dynamic, Kinematic
		};
		REFLECT_ENUM(BodyType);
		PROPERTY_ENUM(BodyType, Static);
		PROPERTY_ENUM(BodyType, Dynamic);
		PROPERTY_ENUM(BodyType, Kinematic);

		PROPERTY(Type);
		BodyType Type = BodyType::Static;

		b2BodyId RuntimeBodyId = b2_nullBodyId;

		PROPERTY(FixedRotation);
		bool FixedRotation = false;

		COMPONENT_UI()
	};

	struct BoxCollider2DComponent
	{
		REFLECT_BODY(BoxCollider2DComponent, TypeCategory::Component);

		PROPERTY(Offset);
		glm::vec2 Offset { 0.0f, 0.0f };
		PROPERTY(Size);
		glm::vec2 Size { 0.5f, 0.5f };
		PROPERTY(Density);
		float Density = 1.0f;
		PROPERTY(Friction);
		float Friction = 0.5f;
		PROPERTY(Restitution);
		float Restitution = 0.2f;
		PROPERTY(ShowCollider);
		bool ShowCollider = true;

		COMPONENT_UI()
	};

	struct CircleCollider2DComponent
	{
		REFLECT_BODY(CircleCollider2DComponent, TypeCategory::Component);

		PROPERTY(Offset);
		glm::vec2 Offset { 0.0f, 0.0f };
		PROPERTY(Radius);
		float Radius = 0.5f;
		PROPERTY(Density);
		float Density = 1.0f;
		PROPERTY(Friction);
		float Friction = 0.5f;
		PROPERTY(Restitution);
		float Restitution = 0.2f;
		PROPERTY(ShowCollider);
		bool ShowCollider = true;

		COMPONENT_UI()
	};
}