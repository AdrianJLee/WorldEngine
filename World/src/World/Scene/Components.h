#pragma once
#include "World/Core/ComponentRegistry.h"
#include "World/Core/UUID.h"
#include "World/Scene/SceneCamera.h"
#include "World/Scene/ScriptableEntity.h"
#include "World/Renderer/Texture.h"
#include "World/Reflection/Reflection.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp> 

namespace World
{
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

	struct UUIDComponent
	{
		REFLECT_BODY(UUIDComponent, TypeCategory::Component);

		World::UUID ID;
		UUIDComponent() = default;
		UUIDComponent(const World::UUID& id)
			: ID(id)
		{}

		COMPONENT_UI()
	};

	struct TransformComponent
	{
		REFLECT_BODY(TransformComponent, TypeCategory::Component);

		glm::vec3 Location { 0.0f, 0.0f, 0.0f };
		// rad
		glm::vec3 Rotation { 0.0f, 0.0f, 0.0f };

		glm::quat RotationQuat { 1.0f, 0.0f, 0.0f, 0.0f };

		glm::vec3 Scale { 1.0f, 1.0f, 1.0f };

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
		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };
		Ref<Texture2D> Texture;
		float TilingFactor = 1.0f;

		COMPONENT_UI()
	};

	struct CircleRendererComponent
	{
		REFLECT_BODY(CircleRendererComponent, TypeCategory::Component);

		glm::vec4 Color { 1.0f, 1.0f, 1.0f, 1.0f };
		float Thickness = 1.0f;
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
		SceneCamera Camera;
		bool Primary = true;
		bool FixedAspectRatio = false;

		COMPONENT_UI()
	};

	struct NativeScriptComponent
	{
		REFLECT_BODY(NativeScriptComponent, TypeCategory::Component);

		ScriptableEntity* Instance = nullptr;

		// 原始函数指针
		ScriptableEntity* (*InstantiateScript)() = nullptr;
		void (*DestroyScript)(NativeScriptComponent*) = nullptr;
		std::string ScriptName;
		template<typename T>
		void Bind()
		{
			InstantiateScript = []()
				{
					return static_cast<ScriptableEntity*>(new T());
				};
			DestroyScript = [](NativeScriptComponent* component)
				{
					delete static_cast<T*>(component->Instance); component->Instance = nullptr;
				};
		}

		COMPONENT_UI()
	};

	struct RigidBody2DComponent
	{
		REFLECT_BODY(RigidBody2DComponent, TypeCategory::Component);

		enum class BodyType
		{
			Static = 0, Dynamic, Kinematic
		};
		BodyType Type = BodyType::Static;
		b2BodyId RuntimeBodyId = b2_nullBodyId;
		bool FixedRotation = false;

		COMPONENT_UI()
	};

	struct BoxCollider2DComponent
	{
		REFLECT_BODY(BoxCollider2DComponent, TypeCategory::Component);

		glm::vec2 Offset { 0.0f, 0.0f };
		glm::vec2 Size { 0.5f, 0.5f };
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.2f;
		bool ShowCollider = true;

		COMPONENT_UI()
	};

	struct CircleCollider2DComponent
	{
		REFLECT_BODY(CircleCollider2DComponent, TypeCategory::Component);

		glm::vec2 Offset { 0.0f, 0.0f };
		float Radius = 0.5f;
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.2f;
		bool ShowCollider = true;

		COMPONENT_UI()
	};
}