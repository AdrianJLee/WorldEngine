#pragma once

#include "World/Core/UUID.h"
#include "World/Core/Memory/Memory.h"
#include "World/Core/Memory/PoolAllocator.h"
#include "World/Scene/Entity.h"
#include "World/Scene/SceneCamera.h"
#include "World/Scene/ScriptableEntity.h"
#include "World/Renderer/Texture.h"
#include "World/Schema/Schema.h"
#include "World/Schema/BuiltinAssetOps.h"

#include <any>
#include <box2d/id.h>
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <sol/sol.hpp>

#include <string>
#include <unordered_map>

namespace World
{
	// 复制组件时只保留"编辑配置",剔除运行态。默认整体复制;有运行态的组件提供特化。
	template <typename T>
	T CloneComponentConfiguration(const T& source) { return source; }

	struct UUIDComponent
	{
		UUID ID;

		UUIDComponent() = default;
		UUIDComponent(const UUID& id) : ID(id) {}

		WE_SCHEMA_BODY(World, UUIDComponent, Component)
			WE_FIELD(ID, Object, Of(UUID));
		WE_SCHEMA_END
	};

	struct TagComponent
	{
		std::string Tag;

		TagComponent() = default;
		TagComponent(const std::string& tag) : Tag(tag) {}

		WE_SCHEMA_BODY(World, TagComponent, Component)
			WE_FIELD(Tag, String);
		WE_SCHEMA_END
	};

	struct TransformComponent
	{
		glm::vec3 Location { 0.0f, 0.0f, 0.0f };
		glm::vec3 Rotation { 0.0f, 0.0f, 0.0f };
		glm::quat RotationQuat { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale { 1.0f, 1.0f, 1.0f };
		glm::mat4 Transform { 1.0f };

		TransformComponent(const glm::mat4& transform) { SetTransform(transform); }
		TransformComponent(const glm::vec3& location = glm::vec3 { 0.0f, 0.0f, 0.0f }, const glm::vec3& rotation = glm::vec3 { 0.0f, 0.0f, 0.0f }, const glm::vec3& scale = glm::vec3 { 1.0f, 1.0f, 1.0f })
		{
			SetTransform(location, rotation, scale);
		}

		void SetLocation(const glm::vec3& location) { Location = location; RecalculateTransform(); }
		void SetRotation(const glm::vec3& rotation) { Rotation = rotation; RotationQuat = glm::quat(rotation); RecalculateTransform(); }
		void SetScale(const glm::vec3& scale) { Scale = scale; RecalculateTransform(); }
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
			Location = glm::vec3(transform[3]);
			Scale.x = glm::length(glm::vec3(transform[0]));
			Scale.y = glm::length(glm::vec3(transform[1]));
			Scale.z = glm::length(glm::vec3(transform[2]));
			if (glm::determinant(transform) < 0)
				Scale.x *= -1.0f;
			glm::mat4 rotationMatrix = transform;
			rotationMatrix[0] /= Scale.x;
			rotationMatrix[1] /= Scale.y;
			rotationMatrix[2] /= Scale.z;
			rotationMatrix[3] = glm::vec4(0, 0, 0, 1);
			RotationQuat = glm::quat_cast(rotationMatrix);
			Rotation = glm::eulerAngles(RotationQuat);
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

		operator const glm::mat4&() const { return Transform; }

		WE_SCHEMA_BODY(World, TransformComponent, Component)
			WE_FIELD(Location, Vec3, Group("Transform"));
			WE_FIELD(Rotation, Vec3, Group("Transform"));
			WE_FIELD(RotationQuat, Quat, Transient, Group("Transform"));
			WE_FIELD(Scale, Vec3, Group("Transform"));
			WE_FIELD(Transform, Mat4, Transient, Group("Transform"));
		WE_SCHEMA_END
	};

	struct SpriteComponent
	{
		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };
		Ref<Texture2D> Texture;
		float TilingFactor = 1.0f;

		SpriteComponent() = default;
		SpriteComponent(const glm::vec4& color) : Color(color) {}

		WE_SCHEMA_BODY(World, SpriteComponent, Component)
			WE_FIELD(Color, Vec4);
			WE_FIELD(Texture, Asset, Of("Texture2D"));
			WE_FIELD(TilingFactor, Float);
		WE_SCHEMA_END
	};

	struct CircleRendererComponent
	{
		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };
		float Thickness = 1.0f;
		float Fade = 0.005f;

		WE_SCHEMA_BODY(World, CircleRendererComponent, Component)
			WE_FIELD(Color, Vec4);
			WE_FIELD(Thickness, Float);
			WE_FIELD(Fade, Float);
		WE_SCHEMA_END
	};

	struct CameraComponent
	{
		CameraComponent() = default;
		CameraComponent(const SceneCamera& sceneCamera, bool primary = false)
			: Camera(sceneCamera), Primary(primary)
		{
		}

		SceneCamera Camera;
		bool Primary = true;
		bool FixedAspectRatio = false;

		WE_SCHEMA_BODY(World, CameraComponent, Component)
			WE_FIELD(Camera, Object, Of(SceneCamera));
			WE_FIELD(Primary, Bool);
			WE_FIELD(FixedAspectRatio, Bool);
		WE_SCHEMA_END
	};

	enum class ScriptInstanceState { Pending, Creating, Running, Destroying, Stopped, Faulted };

	struct NativeScriptComponent
	{
		ScriptableEntity* Instance = nullptr;
		ScriptInstanceState State = ScriptInstanceState::Pending;
		std::string LastError;
		uint64_t Generation = 0;
		bool CreateEntered = false;
		ScriptableEntity* (*InstantiateScript)() = nullptr;
		void (*DestroyScript)(ScriptableEntity*&) = nullptr;

		std::string ScriptName;
		std::unordered_map<std::string, Schema::Value> FieldValues;

		template <typename T>
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

		WE_SCHEMA_BODY(World, NativeScriptComponent, Component)
			WE_FIELD(ScriptName, String);
		WE_SCHEMA_END

		// 无 ImGui 的字段访问合同,供 Editor Inspector 与测试共用;运行期场景也用它应用字段。
		ScriptableEntity* GetOrCreateEditorInstance(bool allowCreate, bool& outOwned);
		void ReleaseEditorInstance(ScriptableEntity* preview);
		void ResetEditorFieldState();
		Schema::Value GetErasedFieldValue(const Schema::TypeSchema& typeSchema, const Schema::FieldSchema& field, ScriptableEntity* instance);
		void SetErasedFieldValue(const Schema::TypeSchema& typeSchema, const Schema::FieldSchema& field, ScriptableEntity* instance, const Schema::Value& value);

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
		std::string ScriptFilePath = ""; // 例如 "assets/scripts/Player.lua"

		// 每个实体独立的 Lua 环境,防止变量冲突。
		sol::environment LuaEnv;
		sol::table ScriptTable;
		sol::protected_function OnCreateFunc;
		sol::protected_function OnUpdateFunc;
		sol::protected_function OnDestroyFunc;

		std::unordered_map<std::string, LuaScriptField> CachedFields;
		std::filesystem::file_time_type LastModifiedTime;

		bool IsLoaded = false;
		ScriptInstanceState State = ScriptInstanceState::Pending;
		std::string LastError;
		uint64_t Generation = 0;
		bool CreateEntered = false;
		Entity RuntimeEntity;

		LuaScriptComponent() = default;
		LuaScriptComponent(const LuaScriptComponent&) = default;
		LuaScriptComponent(const std::string& path) : ScriptFilePath(path) {}

		WE_SCHEMA_BODY(World, LuaScriptComponent, Component)
			WE_FIELD(ScriptFilePath, String);
		WE_SCHEMA_END
	};

	struct RigidBody2DComponent
	{
		enum class BodyType
		{
			Static = 0, Dynamic, Kinematic
		};
		WE_ENUM_SCHEMA(World, BodyType, Int32)
			WE_ENUM_VALUE(Static);
			WE_ENUM_VALUE(Dynamic);
			WE_ENUM_VALUE(Kinematic);
		WE_ENUM_END

		BodyType Type = BodyType::Static;
		b2BodyId RuntimeBodyId = b2_nullBodyId;
		bool FixedRotation = false;

		WE_SCHEMA_BODY(World, RigidBody2DComponent, Component)
			WE_FIELD(Type, Enum, Of(BodyType));
			WE_FIELD(FixedRotation, Bool);
		WE_SCHEMA_END
	};

	struct BoxCollider2DComponent
	{
		glm::vec2 Offset { 0.0f, 0.0f };
		glm::vec2 Size { 0.5f, 0.5f };
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.2f;
		bool ShowCollider = true;

		WE_SCHEMA_BODY(World, BoxCollider2DComponent, Component)
			WE_FIELD(Offset, Vec2);
			WE_FIELD(Size, Vec2);
			WE_FIELD(Density, Float);
			WE_FIELD(Friction, Float);
			WE_FIELD(Restitution, Float);
			WE_FIELD(ShowCollider, Bool);
		WE_SCHEMA_END
	};

	struct CircleCollider2DComponent
	{
		glm::vec2 Offset { 0.0f, 0.0f };
		float Radius = 0.5f;
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.2f;
		bool ShowCollider = true;

		WE_SCHEMA_BODY(World, CircleCollider2DComponent, Component)
			WE_FIELD(Offset, Vec2);
			WE_FIELD(Radius, Float);
			WE_FIELD(Density, Float);
			WE_FIELD(Friction, Float);
			WE_FIELD(Restitution, Float);
			WE_FIELD(ShowCollider, Bool);
		WE_SCHEMA_END
	};

	// 组件配置克隆特化(剔除运行态)。
	NativeScriptComponent CloneComponentConfiguration(const NativeScriptComponent& source);
	LuaScriptComponent CloneComponentConfiguration(const LuaScriptComponent& source);
	RigidBody2DComponent CloneComponentConfiguration(const RigidBody2DComponent& source);
}
