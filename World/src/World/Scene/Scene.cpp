#include "wldpch.h"
#include "Scene.h"
#include "World/Scene/Components.h"
#include "World/Renderer/Renderer2D.h"
#include "World/Renderer/CommandBuffer.h"

#include <physics_world.h>
#include <box2d/box2d.h>
#include <unordered_map>
#include <glm/glm.hpp>

namespace World
{
	Scene::Scene()
	{}
	Scene::~Scene()
	{}
	void Scene::OnUpdateEditor(Timestep ts, const EditorCamera& camera)
	{}
	void Scene::OnUpdateRuntime(Timestep ts)
	{
		// Update scripts
		{
			m_Registry.view<NativeScriptComponent>().each([=](auto entity, NativeScriptComponent& scriptComponent)
				{
					if (!scriptComponent.Instance)
					{
						scriptComponent.Instance = scriptComponent.InstantiateScript();
						scriptComponent.Instance->m_Entity = Entity { this,entity };
						scriptComponent.Instance->OnCreate();

					}
					scriptComponent.Instance->OnUpdate(ts);
				});
		}

		// Update physics
		OnUpdatePhysics2D(ts);
	}
	void Scene::OnUpdateSimulation(Timestep ts, const EditorCamera& camera)
	{
		OnUpdatePhysics2D(ts);

	}
	void Scene::OnViewportResize(uint32_t width, uint32_t height)
	{
		m_ViewportWidth = width;
		m_ViewportHeight = height;
		auto view = m_Registry.view<CameraComponent>();

		for (auto entity : view)
		{
			auto& cameraComponent = view.get<CameraComponent>(entity);
			if (!cameraComponent.FixedAspectRatio)
			{
				cameraComponent.Camera.SetViewportSize(width, height);
			}

		}
	}
	void Scene::OnRuntimeStart()
	{
		OnPhysics2DStart();
	}
	void Scene::OnRuntimeStop()
	{
		OnPhysics2DStop();
	}
	void Scene::OnSimulationStart()
	{
		OnPhysics2DStart();
	}
	void Scene::OnSimulationStop()
	{
		OnPhysics2DStop();
	}
	Entity Scene::GetPrimaryCameraEntity()
	{
		auto view = m_Registry.view<CameraComponent>();
		for (auto entity : view)
		{
			auto& cameraComponent = view.get<CameraComponent>(entity);
			if (cameraComponent.Primary)
			{
				return Entity(this, entity);
			}
		}
		return Entity();
	}

	void Scene::DuplicateEntity(Entity entity)
	{

		auto view = m_Registry.view<TagComponent>();

		auto name = entity.GetComponent<TagComponent>().Tag;

		Entity newEntity = Entity::CreateEntity(this, name);

		for (const auto& componentInfo : World::ComponentRegistry::GetList())
		{
			bool hasComponent = entity.HasComponent(componentInfo.Id);

			if (hasComponent)
			{
				if (componentInfo.CopyFunc)
				{
					componentInfo.CopyFunc(newEntity, entity);
				}
			}
		}
	}

	void Scene::CopyScene(Ref<Scene>& other, Ref<Scene>& newScene)
	{
		newScene->m_ViewportHeight = other->m_ViewportHeight;
		newScene->m_ViewportWidth = other->m_ViewportWidth;

		auto& srcRegistry = other->m_Registry;
		auto& destRegistry = newScene->m_Registry;
		std::unordered_map<UUID, entt::entity> entityMap;

		auto view = srcRegistry.view<UUIDComponent>();
		for (auto entity : view)
		{
			UUID entityId = srcRegistry.get<UUIDComponent>(entity).ID;
			auto name = srcRegistry.get<TagComponent>(entity).Tag;

			Entity newEntity = Entity::CreateEntity(newScene.get(), name, entityId);
			entityMap[entityId] = newEntity;
		}

		for (const auto& componentInfo : World::ComponentRegistry::GetList())
		{
			if (componentInfo.CopyComponentFunc)
			{
				componentInfo.CopyComponentFunc(destRegistry, srcRegistry, entityMap);
			}
		}

	}


	void Scene::OnPhysics2DStart()
	{
		b2WorldDef worldDef = b2DefaultWorldDef();
		worldDef.gravity = { 0.0f, -9.8f };
		worldDef.restitutionThreshold = 0.5f;
		m_PhysicsWorldId = b2CreateWorld(&worldDef);

		auto view = m_Registry.view<RigidBody2DComponent>();
		for (auto entity : view)
		{
			Entity Entity = { this,entity };
			auto& transform = Entity.GetComponent<TransformComponent>();
			auto& rb = Entity.GetComponent<RigidBody2DComponent>();

			b2BodyDef bodyDef = b2DefaultBodyDef();
			switch (rb.Type)
			{
				case RigidBody2DComponent::BodyType::Static:
					bodyDef.type = b2_staticBody;
					break;
				case RigidBody2DComponent::BodyType::Dynamic:
					bodyDef.type = b2_dynamicBody;
					break;
				case RigidBody2DComponent::BodyType::Kinematic:
					bodyDef.type = b2_kinematicBody;
					break;
			}
			bodyDef.position = { transform.Location.x, transform.Location.y };
			bodyDef.rotation = b2MakeRot(transform.Rotation.z);
			bodyDef.motionLocks.angularZ = rb.FixedRotation;

			rb.RuntimeBodyId = b2CreateBody(m_PhysicsWorldId, &bodyDef);

			if (Entity.HasComponent<BoxCollider2DComponent>())
			{
				auto& bc2d = Entity.GetComponent<BoxCollider2DComponent>();

				// 1. 创建形状定义 (取代了 b2FixtureDef)
				b2ShapeDef shapeDef = b2DefaultShapeDef();
				shapeDef.density = bc2d.Density;   // 建议从组件读取，而不是硬编码

				shapeDef.material.friction = bc2d.Friction;
				shapeDef.material.restitution = bc2d.Restitution; // 新版可以直接在这里设弹性

				// 2. 创建盒模型几何数据 (取代了 boxShape.SetAsBox)
				// 注意：v3.0 的 b2MakeBox 依然接受“半宽”和“半高”
				float hx = transform.Scale.x * bc2d.Size.x;
				float hy = transform.Scale.y * bc2d.Size.y;
				b2Polygon boxPolygon = b2MakeOffsetBox(hx, hy, { bc2d.Offset.x, bc2d.Offset.y }, b2MakeRot(0.0f));


				// 3. 将形状绑定到 Body 上 (取代了 body->CreateFixture)
				// 注意：这里返回的是 b2ShapeId，如果不需要后续操作可以不保存
				b2CreatePolygonShape(rb.RuntimeBodyId, &shapeDef, &boxPolygon);
			}
			if (Entity.HasComponent<CircleCollider2DComponent>())
			{
				auto& circle = Entity.GetComponent<CircleCollider2DComponent>();

				b2Circle circleShape;
				circleShape.center = { circle.Offset.x, circle.Offset.y }; // 碰撞盒偏移
				circleShape.radius = circle.Radius * transform.Scale.x;  // 实际半径（需考虑缩放）

				b2ShapeDef shapeDef = b2DefaultShapeDef();
				shapeDef.density = circle.Density; // 可以根据需要从组件读取
				shapeDef.material.friction = circle.Friction; // 可以根据需要从组件读取
				shapeDef.material.restitution = circle.Restitution; // 可以根据需要从组件读取

				b2CreateCircleShape(rb.RuntimeBodyId, &shapeDef, &circleShape);
			}
		}
	}
	void Scene::OnUpdatePhysics2D(Timestep ts)
	{
		const int subStepCount = 4;
		b2World_Step(m_PhysicsWorldId, ts.GetSeconds(), subStepCount);

		auto view = m_Registry.view<RigidBody2DComponent>();
		for (auto entity : view)
		{
			Entity Entity = { this,entity };
			auto& transform = Entity.GetComponent<TransformComponent>();
			auto& rb = Entity.GetComponent<RigidBody2DComponent>();

			b2Transform& bodyTransform = b2Body_GetTransform(rb.RuntimeBodyId);

			const b2Vec2& position = bodyTransform.p;

			float rotation = b2Rot_GetAngle(bodyTransform.q);
			transform.SetTransform({ position.x, position.y, transform.Location.z },
				{ transform.Rotation.x, transform.Rotation.y, rotation }, transform.Scale);
		}
	}
	void Scene::OnPhysics2DStop()
	{
		b2DestroyWorld(m_PhysicsWorldId);
		m_PhysicsWorldId = b2_nullWorldId;
	}
}