#include "wldpch.h"
#include "Scene.h"
#include "World/Scene/Components.h"
#include "World/Scene/TransformSystem.h"
#include "World/Scene/CameraSystem.h"
#include "World/Scene/MovementSystem.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Renderer/AnimationSystem.h"
#include "World/Renderer/FrameExtract.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Gameplay/SystemRegistry.h"
#include "World/Physics/Physics3D.h"
#include <box2d/box2d.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>

namespace World
{
	namespace
	{
		// ---- PLUG-T2c:活场景表 ----------------------------------------------------
		// Scene 构造/析构维护 (WorldContext, registry) 指针;插件卸载前的活实例查询用它
		// 枚举"进程内所有活场景"(可按 WorldContext 过滤)。表本身加锁;注册表内容的读写
		// 仍由场景 owner 线程负责 —— 查询走宿主主线程的卸载路径(与场景同线程)。
		struct LiveSceneEntry
		{
			Scene* Owner = nullptr;
			const WorldContext* Context = nullptr;
			const entt::registry* Registry = nullptr;
		};

		std::mutex g_LiveScenesMutex;
		std::vector<LiveSceneEntry> g_LiveScenes;

		void RegisterLiveScene(Scene* owner, const WorldContext* context, const entt::registry* registry)
		{
			std::lock_guard<std::mutex> lock(g_LiveScenesMutex);
			g_LiveScenes.push_back({ owner, context, registry });
		}

		void UnregisterLiveScene(const entt::registry* registry)
		{
			std::lock_guard<std::mutex> lock(g_LiveScenesMutex);
			g_LiveScenes.erase(std::remove_if(g_LiveScenes.begin(), g_LiveScenes.end(),
				[registry](const LiveSceneEntry& entry) { return entry.Registry == registry; }),
				g_LiveScenes.end());
		}

		template<typename T>
		std::vector<entt::entity> Snapshot(entt::registry& registry)
		{
			auto view = registry.view<T>();
			return { view.begin(), view.end() };
		}

		thread_local bool s_InsideFramePipeline = false;



		// 脚本属性按名字找 schema 字段(大小写敏感;不含嵌套/非叶类型)。
		const Schema::FieldSchema* FindSchemaField(const Schema::TypeSchema& type, const std::string& name)
		{
			for (const Schema::FieldSchema& field : type.Fields)
			{
				if (field.Name == name)
					return &field;
			}
			return nullptr;
		}

		void Report(const std::string& error)
		{
			if (Log::GetCoreLogger()) WLD_CORE_ERROR("{0}", error);
		}

		// W3f:刚体创建的唯一实现(世界启动与运行时 AddComponent 共用)。
		// 形状/质量折算与 OnPhysics2DStart 的既有口径一致:位置/旋转取 Transform,尺寸乘 Scale。
		bool BuildRuntimeBody(b2WorldId worldId, entt::registry& registry, entt::entity entity)
		{
			if (!b2World_IsValid(worldId)) return false;
			auto* rb = registry.try_get<RigidBody2DComponent>(entity);
			if (!rb || b2Body_IsValid(rb->RuntimeBodyId)) return false;
			auto toBox2DType = [](RigidBody2DComponent::BodyType type)
			{
				switch (type)
				{
					case RigidBody2DComponent::BodyType::Static: return b2_staticBody;
					case RigidBody2DComponent::BodyType::Dynamic: return b2_dynamicBody;
					case RigidBody2DComponent::BodyType::Kinematic: return b2_kinematicBody;
				}
				return b2_staticBody;
			};
			auto* transform = registry.try_get<TransformComponent>(entity);
			if (!transform)
			{
				Report("[Physics] Missing Transform for rigid body entity=" + std::to_string(static_cast<uint32_t>(entity)));
				return false;
			}
			b2BodyDef bodyDef = b2DefaultBodyDef();
			bodyDef.type = toBox2DType(rb->Type);
			bodyDef.position = { transform->Location.x, transform->Location.y };
			bodyDef.rotation = b2MakeRot(transform->Rotation.z);
			bodyDef.motionLocks.angularZ = rb->FixedRotation;
			bodyDef.isBullet = rb->Ccd;   // P7:CCD(高速小物体不隧穿)
			rb->RuntimeBodyId = b2CreateBody(worldId, &bodyDef);
			// P5:每个 shape 统一挂 (a) 碰撞过滤(Box2D b2Filter 口径,取自刚体的 Layer/Mask)、
			// (b) 传感器开关、(c) 事件开关 —— Box2D 的 enableContactEvents / enableSensorEvents
			// **默认是 false**,不显式打开就永远收不到任何事件;(d) 实体句柄(供事件反查)。
			const auto PrepareShape = [&](b2ShapeDef& shapeDef, bool isSensor)
			{
				shapeDef.filter.categoryBits = rb->Layer;
				shapeDef.filter.maskBits = rb->Mask;
				shapeDef.isSensor = isSensor;
				shapeDef.enableContactEvents = true;
				shapeDef.enableSensorEvents = true;
				shapeDef.userData = Physics::PackEntityUserData(entity);
			};
			if (const auto* box = registry.try_get<BoxCollider2DComponent>(entity))
			{
				b2ShapeDef shapeDef = b2DefaultShapeDef();
				shapeDef.density = box->Density;
				shapeDef.material.friction = box->Friction;
				shapeDef.material.restitution = box->Restitution;
				PrepareShape(shapeDef, box->IsSensor);
				b2Polygon polygon = b2MakeOffsetBox(transform->Scale.x * box->Size.x, transform->Scale.y * box->Size.y,
					{ box->Offset.x, box->Offset.y }, b2MakeRot(0.0f));
				b2CreatePolygonShape(rb->RuntimeBodyId, &shapeDef, &polygon);
			}
			if (const auto* circle = registry.try_get<CircleCollider2DComponent>(entity))
			{
				b2Circle shape;
				shape.center = { circle->Offset.x, circle->Offset.y };
				shape.radius = circle->Radius * transform->Scale.x;
				b2ShapeDef shapeDef = b2DefaultShapeDef();
				shapeDef.density = circle->Density;
				shapeDef.material.friction = circle->Friction;
				shapeDef.material.restitution = circle->Restitution;
				PrepareShape(shapeDef, circle->IsSensor);
				b2CreateCircleShape(rb->RuntimeBodyId, &shapeDef, &shape);
			}
			return true;
		}

		// ---- P7:Box2D 关节(Fixed=Weld / Distance / Hinge=Revolute) ----
		//
		// 局部锚点用 b2JointDef::localFrameA/B(b2Transform 的 p 即局部连接点,旋转留单位),
		// Box2D 自己按刚体当前位姿换算 —— 与 3D 侧"局部→世界"的算法语义一致。
		// 两个实体都必须已经有活刚体;任一缺失 ⇒ 返回 false 交给调用方报可读错误。
		bool BuildRuntimeJoint(b2WorldId worldId, entt::registry& registry, entt::entity owner)
		{
			if (!b2World_IsValid(worldId)) return false;
			auto* joint = registry.try_get<JointComponent>(owner);
			if (!joint || b2Joint_IsValid(joint->RuntimeJointId)) return false;

			const entt::entity other = joint->Connected;
			if (!registry.valid(other) || other == owner) return false;
			auto* ownerBody = registry.try_get<RigidBody2DComponent>(owner);
			auto* otherBody = registry.try_get<RigidBody2DComponent>(other);
			if (!ownerBody || !otherBody) return false;
			if (!b2Body_IsValid(ownerBody->RuntimeBodyId) || !b2Body_IsValid(otherBody->RuntimeBodyId)) return false;

			b2Transform localA = b2Transform { { joint->AnchorSelf.x, joint->AnchorSelf.y }, b2Rot_identity };
			b2Transform localB = b2Transform { { joint->AnchorOther.x, joint->AnchorOther.y }, b2Rot_identity };
			b2JointId created = b2_nullJointId;
			switch (joint->Type)
			{
				case JointComponent::JointKind::Fixed:
				{
					b2WeldJointDef def = b2DefaultWeldJointDef();
					def.base.bodyIdA = ownerBody->RuntimeBodyId;
					def.base.bodyIdB = otherBody->RuntimeBodyId;
					def.base.localFrameA = localA;
					def.base.localFrameB = localB;
					created = b2CreateWeldJoint(worldId, &def);
					break;
				}
				case JointComponent::JointKind::Distance:
				{
					b2DistanceJointDef def = b2DefaultDistanceJointDef();
					def.base.bodyIdA = ownerBody->RuntimeBodyId;
					def.base.bodyIdB = otherBody->RuntimeBodyId;
					def.base.localFrameA = localA;
					def.base.localFrameB = localB;
					if (joint->MinDistance >= 0.0f && joint->MaxDistance >= joint->MinDistance)
					{
						def.enableLimit = true;
						def.minLength = joint->MinDistance;
						def.maxLength = joint->MaxDistance;
					}
					// length 由 Box2D 按 localFrameA/B 的初始世界距离推导(默认 1.0 是占位);
					// 这里显式留 < 0 时交给 Box2D 的默认行为不可靠,所以按两刚体当前位姿算一次。
					const b2Vec2 worldA = b2Body_GetWorldPoint(ownerBody->RuntimeBodyId, { joint->AnchorSelf.x, joint->AnchorSelf.y });
					const b2Vec2 worldB = b2Body_GetWorldPoint(otherBody->RuntimeBodyId, { joint->AnchorOther.x, joint->AnchorOther.y });
					def.length = b2Distance(worldA, worldB);
					created = b2CreateDistanceJoint(worldId, &def);
					break;
				}
				case JointComponent::JointKind::Hinge:
				{
					b2RevoluteJointDef def = b2DefaultRevoluteJointDef();
					def.base.bodyIdA = ownerBody->RuntimeBodyId;
					def.base.bodyIdB = otherBody->RuntimeBodyId;
					def.base.localFrameA = localA;
					def.base.localFrameB = localB;
					created = b2CreateRevoluteJoint(worldId, &def);
					break;
				}
			}
			if (!b2Joint_IsValid(created)) return false;
			b2Joint_SetCollideConnected(created, joint->EnableCollision);
			joint->RuntimeJointId = created;
			return true;
		}

		// ---- P5:把 Box2D 的事实翻译成 ECS 事件 ----
		//
		// Box2D 原生只给 begin/end(b2World_GetContactEvents / b2World_GetSensorEvents),
		// **没有 persist** —— 这里用"每步枚举当前接触、减去本步 begin"补齐,让 2D/3D 对外是
		// 同一套 Begin/Persist/End 语义。
		//
		// 只做翻译,不做策略:事件顺序由 Box2D 的数组顺序 + registry 遍历顺序决定,
		// 同一固定步输入下**确定**(可复现)。事件里的实体句柄来自 shape 的 userData,
		// shape 已销毁(End 事件里可能出现)则跳过 —— 不做悬垂反查。
		uint64_t PackEntityPair(entt::entity a, entt::entity b)
		{
			const uint32_t x = entt::to_integral(a);
			const uint32_t y = entt::to_integral(b);
			return (static_cast<uint64_t>(std::min(x, y)) << 32) | std::max(x, y);
		}

		entt::entity ResolveShapeEntity(b2ShapeId shape)
		{
			if (!b2Shape_IsValid(shape)) return entt::null;
			return Physics::UnpackEntityUserData(b2Shape_GetUserData(shape));
		}

		// 从 b2ContactData 的流形里取"世界系法线 / 首个接触点 / 最大穿透深度"。
		void FillContactGeometry(const b2Manifold& manifold, Physics::ContactEvent& event)
		{
			event.Normal = glm::vec3(manifold.normal.x, manifold.normal.y, 0.0f);
			float deepest = 0.0f;
			for (int point = 0; point < manifold.pointCount && point < 2; ++point)
			{
				const b2ManifoldPoint& manifoldPoint = manifold.points[point];
				if (point == 0)
					event.Point = glm::vec3(manifoldPoint.clipPoint.x, manifoldPoint.clipPoint.y, 0.0f);
				deepest = std::max(deepest, -manifoldPoint.separation);
			}
			event.PenetrationDepth = std::max(0.0f, deepest);
		}

		void HarvestPhysics2DEvents(b2WorldId world, entt::registry& registry,
			std::vector<Physics::ContactEvent>& outContacts, std::vector<Physics::TriggerEvent>& outTriggers,
			std::set<uint64_t>& sensorOverlaps)
		{
			if (!b2World_IsValid(world)) return;

			std::unordered_set<uint64_t> beganContacts;
			std::unordered_set<uint64_t> beganTriggers;

			const b2ContactEvents contactEvents = b2World_GetContactEvents(world);
			for (int index = 0; index < contactEvents.beginCount; ++index)
			{
				const b2ContactBeginTouchEvent& source = contactEvents.beginEvents[index];
				// 传感器接触由 sensor 事件通道负责,避免同一事实报两遍。
				if (b2Shape_IsSensor(source.shapeIdA) || b2Shape_IsSensor(source.shapeIdB)) continue;
				const entt::entity entityA = ResolveShapeEntity(source.shapeIdA);
				const entt::entity entityB = ResolveShapeEntity(source.shapeIdB);
				if (entityA == entt::null || entityB == entt::null) continue;
				Physics::ContactEvent event;
				event.EntityA = entityA;
				event.EntityB = entityB;
				event.Phase = Physics::ContactPhase::Begin;
				if (b2Contact_IsValid(source.contactId))
					FillContactGeometry(b2Contact_GetData(source.contactId).manifold, event);
				beganContacts.insert(PackEntityPair(entityA, entityB));
				outContacts.push_back(event);
			}
			for (int index = 0; index < contactEvents.endCount; ++index)
			{
				const b2ContactEndTouchEvent& source = contactEvents.endEvents[index];
				const entt::entity entityA = ResolveShapeEntity(source.shapeIdA);
				const entt::entity entityB = ResolveShapeEntity(source.shapeIdB);
				if (entityA == entt::null || entityB == entt::null) continue;
				Physics::ContactEvent event;
				event.EntityA = entityA;
				event.EntityB = entityB;
				event.Phase = Physics::ContactPhase::End;   // 流形此时已不可靠 ⇒ 几何量保持为零
				outContacts.push_back(event);
			}

			const b2SensorEvents sensorEvents = b2World_GetSensorEvents(world);
			for (int index = 0; index < sensorEvents.beginCount; ++index)
			{
				const b2SensorBeginTouchEvent& source = sensorEvents.beginEvents[index];
				const entt::entity sensor = ResolveShapeEntity(source.sensorShapeId);
				const entt::entity visitor = ResolveShapeEntity(source.visitorShapeId);
				if (sensor == entt::null || visitor == entt::null) continue;
				Physics::TriggerEvent event;
				event.SensorEntity = sensor;
				event.OtherEntity = visitor;
				event.Phase = Physics::ContactPhase::Begin;
				const uint64_t beginKey = PackEntityPair(sensor, visitor);
				beganTriggers.insert(beginKey);
				sensorOverlaps.insert(beginKey);   // Persist 的事实源:仍在重叠的配对
				outTriggers.push_back(event);
			}
			for (int index = 0; index < sensorEvents.endCount; ++index)
			{
				const b2SensorEndTouchEvent& source = sensorEvents.endEvents[index];
				const entt::entity sensor = ResolveShapeEntity(source.sensorShapeId);
				const entt::entity visitor = ResolveShapeEntity(source.visitorShapeId);
				if (sensor == entt::null || visitor == entt::null) continue;
				Physics::TriggerEvent event;
				event.SensorEntity = sensor;
				event.OtherEntity = visitor;
				event.Phase = Physics::ContactPhase::End;
				sensorOverlaps.erase(PackEntityPair(sensor, visitor));
				outTriggers.push_back(event);
			}

			// Persist(传感器):Box2D **不把传感器重叠算作接触** —— b2Body_GetContactData 枚举不到,
			// 所以这里用场景自己维护的"仍在重叠"集合补齐。顺序取有序集合 ⇒ 同一固定步输入下确定。
			for (const uint64_t key : sensorOverlaps)
			{
				if (beganTriggers.count(key)) continue;   // 本步刚 Begin 的,不重复报 Persist
				const entt::entity sensor = static_cast<entt::entity>(key >> 32);
				const entt::entity visitor = static_cast<entt::entity>(key & 0xFFFFFFFFu);
				Physics::TriggerEvent event;
				event.SensorEntity = sensor;
				event.OtherEntity = visitor;
				event.Phase = Physics::ContactPhase::Persist;
				outTriggers.push_back(event);
			}

			// Persist:本步仍在接触、但不是本步 Begin 的配对。两个刚体都会枚举到同一条接触 ⇒ 去重。
			std::unordered_set<uint64_t> persistedContacts;
			for (const entt::entity entity : registry.view<RigidBody2DComponent>())
			{
				const RigidBody2DComponent& rigidBody = registry.get<RigidBody2DComponent>(entity);
				if (!b2Body_IsValid(rigidBody.RuntimeBodyId)) continue;
				const int capacity = b2Body_GetContactCapacity(rigidBody.RuntimeBodyId);
				if (capacity <= 0) continue;
				std::vector<b2ContactData> contacts(static_cast<std::size_t>(capacity));
				const int count = b2Body_GetContactData(rigidBody.RuntimeBodyId, contacts.data(), capacity);
				for (int index = 0; index < count; ++index)
				{
					const b2ContactData& data = contacts[index];
					const entt::entity entityA = ResolveShapeEntity(data.shapeIdA);
					const entt::entity entityB = ResolveShapeEntity(data.shapeIdB);
					if (entityA == entt::null || entityB == entt::null) continue;
					const bool sensorA = b2Shape_IsValid(data.shapeIdA) && b2Shape_IsSensor(data.shapeIdA);
					const bool sensorB = b2Shape_IsValid(data.shapeIdB) && b2Shape_IsSensor(data.shapeIdB);
					if (sensorA || sensorB) continue;   // 传感器重叠不在这里(见上面 sensorOverlaps 通道)
					const uint64_t key = PackEntityPair(entityA, entityB);
					if (beganContacts.count(key) || !persistedContacts.insert(key).second) continue;
					Physics::ContactEvent event;
					event.EntityA = entityA;
					event.EntityB = entityB;
					event.Phase = Physics::ContactPhase::Persist;
					FillContactGeometry(data.manifold, event);
					outContacts.push_back(event);
				}
			}
		}

	}

	Scene::Scene(WorldContext& context) : m_Context(&context), m_OwnerThread(std::this_thread::get_id())
	{
		RegisterLiveScene(this, m_Context, &m_Registry);
	}

	Scene::~Scene()
	{
		// sink 是跨帧装在场景上的(见 Renderer/RenderExtract.h):场景先死时必须通知宿主
		// 忘掉它,否则渲染器里那个 Scene* 会悬垂。
		if (m_RenderExtractSink)
		{
			m_RenderExtractSink->OnExtractSceneDestroyed(*this);
			m_RenderExtractSink = nullptr;
		}
		// Destroying a scene on another thread would also destroy Lua references there.
		if (m_OwnerThread != std::this_thread::get_id())
		{
			Report("Scene destruction must run on its owner thread");
			std::terminate();
		}
		StopScene();
		m_Lifetime.reset();
		UnregisterLiveScene(&m_Registry);
	}

	std::size_t Scene::CountLiveComponentInstances(entt::id_type componentId,
		const WorldContext* context)
	{
		std::lock_guard<std::mutex> lock(g_LiveScenesMutex);
		std::size_t count = 0;
		for (const LiveSceneEntry& entry : g_LiveScenes)
		{
			if (context && entry.Context != context)
				continue;
			const auto* storage = entry.Registry->storage(componentId);
			if (storage)
				count += storage->size();
		}
		return count;
	}

	std::vector<Scene*> Scene::LiveScenes(const WorldContext* context)
	{
		std::lock_guard<std::mutex> lock(g_LiveScenesMutex);
		std::vector<Scene*> scenes;
		scenes.reserve(g_LiveScenes.size());
		for (const LiveSceneEntry& entry : g_LiveScenes)
		{
			if (context && entry.Context != context)
				continue;
			if (entry.Owner)
				scenes.push_back(entry.Owner);
		}
		return scenes;
	}

	bool Scene::FindLiveEntity(const UUID& id, const WorldContext* context, const Scene* only,
		Scene** outScene, entt::entity* outEntity)
	{
		std::lock_guard<std::mutex> lock(g_LiveScenesMutex);
		for (const LiveSceneEntry& entry : g_LiveScenes)
		{
			if (!entry.Owner || !entry.Registry)
				continue;
			if (context && entry.Context != context)
				continue;
			if (only && entry.Owner != only)
				continue;
			for (const entt::entity handle : entry.Registry->view<UUIDComponent>())
			{
				const UUIDComponent& identity = entry.Registry->get<UUIDComponent>(handle);
				if (static_cast<uint64_t>(identity.ID) != static_cast<uint64_t>(id))
					continue;
				if (outScene)
					*outScene = entry.Owner;
				if (outEntity)
					*outEntity = handle;
				return true;
			}
		}
		return false;
	}

	void Scene::AssertOwnerThread() const
	{
		if (m_OwnerThread != std::this_thread::get_id())
			throw std::logic_error("Scene access must run on its owner thread");
	}

	void Scene::RegisterFrameSystem(FrameSystem system)
	{
		AssertOwnerThread();
		if (system.Name.empty() || !system.Update)
			throw std::invalid_argument("Scene frame system requires a name and an update function");
		for (const FrameSystem& existing : m_FrameSystemDefinitions)
			if (existing.Name == system.Name)
				throw std::invalid_argument("Scene frame system '" + system.Name + "' is already registered");

		if (!m_FrameSystems)
			m_FrameSystems = std::make_unique<Gameplay::SystemRegistry>();
		Gameplay::SystemDesc desc;
		desc.Name = system.Name;
		// PURE-ECS:阶段与顺序依赖来自注册方(默认 Update/无依赖),不再硬编码。
		desc.Phase = system.Phase;
		desc.ParallelSafe = system.ParallelSafe;
		desc.After = system.After;
		desc.Interval = system.Interval;
		desc.Condition = system.Condition;
		Gameplay::SystemRegistry::UpdateFn update = std::move(system.Update);
		if (!m_FrameSystems->Register(desc, std::move(update)))
			throw std::invalid_argument("Scene frame system '" + desc.Name + "' rejected: " + m_FrameSystems->GetLastError());
		m_FrameSystemDefinitions.push_back(std::move(system));
	}

	bool Scene::HasFrameSystem(const std::string& name) const
	{
		for (const FrameSystem& existing : m_FrameSystemDefinitions)
			if (existing.Name == name)
				return true;
		return false;
	}

	bool Scene::UnregisterFrameSystem(const std::string& name)
	{
		AssertOwnerThread();
		if (name.empty())
			return false;
		const auto found = std::find_if(m_FrameSystemDefinitions.begin(), m_FrameSystemDefinitions.end(),
			[&name](const FrameSystem& existing) { return existing.Name == name; });
		if (found == m_FrameSystemDefinitions.end())
			return false;
		m_FrameSystemDefinitions.erase(found);
		if (m_FrameSystems)
			m_FrameSystems->Unregister(name);
		return true;
	}

	bool Scene::UnregisterSystemByType(std::type_index type)
	{
		AssertOwnerThread();
		if (type == std::type_index(typeid(void)))
			return false;

		const auto isMatching = [&type](std::type_index itemType) {
			return itemType == type
				|| (itemType != std::type_index(typeid(void))
					&& std::string_view(itemType.name()) == std::string_view(type.name()));
		};

		// 检查 Startup
		const auto sFound = std::find_if(m_StartupSystems.begin(), m_StartupSystems.end(),
			[&isMatching](const LifecycleSystem& item) { return isMatching(item.TypeIndex); });
		if (sFound != m_StartupSystems.end())
		{
			m_StartupSystems.erase(sFound);
			return true;
		}

		// 检查 Teardown
		const auto tFound = std::find_if(m_TeardownSystems.begin(), m_TeardownSystems.end(),
			[&isMatching](const LifecycleSystem& item) { return isMatching(item.TypeIndex); });
		if (tFound != m_TeardownSystems.end())
		{
			m_TeardownSystems.erase(tFound);
			return true;
		}

		const auto found = std::find_if(m_FrameSystemDefinitions.begin(), m_FrameSystemDefinitions.end(),
			[&isMatching](const FrameSystem& existing) { return isMatching(existing.TypeIndex); });
		if (found == m_FrameSystemDefinitions.end())
			return false;
		const std::string name = found->Name;
		m_FrameSystemDefinitions.erase(found);
		if (m_FrameSystems)
			m_FrameSystems->Unregister(name);
		return true;
	}

	bool Scene::HasSystemByType(std::type_index type) const
	{
		if (type == std::type_index(typeid(void)))
			return false;

		const auto isMatching = [&type](std::type_index itemType) {
			return itemType == type
				|| (itemType != std::type_index(typeid(void))
					&& std::string_view(itemType.name()) == std::string_view(type.name()));
		};

		for (const auto& item : m_StartupSystems)
			if (isMatching(item.TypeIndex)) return true;
		for (const auto& item : m_TeardownSystems)
			if (isMatching(item.TypeIndex)) return true;

		for (const FrameSystem& existing : m_FrameSystemDefinitions)
		{
			if (isMatching(existing.TypeIndex))
				return true;
		}
		return false;
	}

	void Scene::RegisterStartupSystem(LifecycleSystem system)
	{
		AssertOwnerThread();
		m_StartupSystems.push_back(std::move(system));
	}

	void Scene::RegisterTeardownSystem(LifecycleSystem system)
	{
		AssertOwnerThread();
		m_TeardownSystems.push_back(std::move(system));
	}

	void Scene::RunStartupSystems()
	{
		AssertOwnerThread();
		const bool wasCommitting = m_Committing;
		m_Committing = true;
		for (const auto& sys : m_StartupSystems)
		{
			if (sys.Action)
				sys.Action(*this);
		}
		m_Committing = wasCommitting;
		FlushStructuralChanges();
	}

	void Scene::RunTeardownSystems()
	{
		AssertOwnerThread();
		const bool wasCommitting = m_Committing;
		m_Committing = true;
		for (const auto& sys : m_TeardownSystems)
		{
			if (sys.Action)
				sys.Action(*this);
		}
		m_Committing = wasCommitting;
	}

	void Scene::MarkComponentChanged(entt::id_type componentId)
	{
		m_ComponentVersions[componentId] = m_WorldTick;
	}

	uint64_t Scene::GetComponentChangeTick(entt::id_type componentId) const
	{
		const auto it = m_ComponentVersions.find(componentId);
		return it != m_ComponentVersions.end() ? it->second : 0;
	}

	uint64_t Scene::AddComponentChangeObserver(entt::id_type componentId, ComponentChangeObserverFn fn)
	{
		AssertOwnerThread();
		uint64_t handle = ++m_NextObserverId;
		m_ComponentChangeObservers.push_back({ handle, componentId, std::move(fn) });
		return handle;
	}

	bool Scene::RemoveComponentChangeObserver(uint64_t handle)
	{
		AssertOwnerThread();
		const auto it = std::find_if(m_ComponentChangeObservers.begin(), m_ComponentChangeObservers.end(),
			[handle](const ComponentChangeObserverEntry& entry) { return entry.Handle == handle; });
		if (it == m_ComponentChangeObservers.end())
			return false;
		m_ComponentChangeObservers.erase(it);
		return true;
	}

	void Scene::NotifyComponentChanged(Entity entity, entt::id_type componentId)
	{
		MarkComponentChanged(componentId);
		for (const auto& entry : m_ComponentChangeObservers)
		{
			if (entry.ComponentId == componentId && entry.Callback)
				entry.Callback(entity);
		}
	}

	// W5-3:帧系统调度统一走 Gameplay::SystemRegistry
	// (阶段/同阶段依赖/并行安全并行派发/逐系统耗时),Scene 只保留面向宿主的薄封装。
	void Scene::RunFrameSystems(Timestep ts)
	{
		AssertOwnerThread();
		AdvanceWorldTick();
		// 一帧的耗时表:谁先跑谁清(固定阶段先于可变阶段,见 OnFixedUpdate)。帧末复位。
		if (!m_FrameTimingsBegun)
		{
			m_FrameSystemTimings.clear();
			m_FrameTimingsBegun = true;
		}
		// P5:本帧一个固定步都没跑 ⇒ 本帧不可能有新物理事件,把上一帧的清掉(读到的永远是"本帧的")。
		if (!m_PhysicsEventsBegun)
			ClearPhysicsEvents();
		if (!m_FrameSystems)
		{
			m_FrameTimingsBegun = false;
			m_PhysicsEventsBegun = false;
			return;
		}

		s_InsideFramePipeline = true;
		struct PipelineScopeGuard
		{
			~PipelineScopeGuard() { s_InsideFramePipeline = false; }
		} guard;

		// PURE-ECS(工业口径):可变阶段 = Update → Late → PreRender,每帧一次。
		// **Fixed 阶段不在这里** —— 它在 RunFixedFrameSystems 里按固定 dt 跑 0..N 次。
		// 同阶段内的顺序依赖由 SystemRegistry 的 After 拓扑排序负责。
		for (int phase = static_cast<int>(Gameplay::SystemPhase::Update);
			phase <= static_cast<int>(Gameplay::SystemPhase::PreRender); ++phase)
		{
			m_FrameSystems->RunPhase(static_cast<Gameplay::SystemPhase>(phase), ts);
			for (const Gameplay::SystemTiming& timing : m_FrameSystems->GetLastTimings())
				m_FrameSystemTimings.push_back({ timing.Name, timing.ParallelSafe, timing.Milliseconds });
		}
		m_FrameTimingsBegun = false;   // 本帧结束:下一帧重新开始收集
		m_PhysicsEventsBegun = false;  // 本帧结束:下一帧的固定步重新清空物理事件
	}

	// ---- P6:固定步长 → 渲染插值(仅表现层) ----
	void Scene::SetFixedStepAlpha(float alpha)
	{
		m_FixedStepAlpha = std::clamp(alpha, 0.0f, 1.0f);
	}

	void Scene::RecordPhysicsInterpolationState()
	{
		if (!m_PhysicsInterpolationEnabled) return;

		// 只**更新**已有状态,不做结构写 —— 本函数在固定步阶段(帧管线内)被调用,
		// 而运行态的结构写是受保护的。状态的创建放在两处非帧内的点:
		//   * OnRuntimeStart(所有物理实体);
		//   * EnsurePhysicsBody(运行时 AddComponent 补建刚体的提交点,新实体当帧补上)。
		for (const entt::entity entity : m_Registry.view<PhysicsInterpolationState>())
		{
			auto& state = m_Registry.get<PhysicsInterpolationState>(entity);
			// 渲染矩阵的权威来源与抽取侧一致:有 WorldTransformComponent 用它(层级求解结果),
			// 否则回退到实体自己的 TransformComponent。
			if (const auto* world = m_Registry.try_get<WorldTransformComponent>(entity)) state.PreviousMatrix = world->Matrix;
			else if (const auto* transform = m_Registry.try_get<TransformComponent>(entity)) state.PreviousMatrix = transform->Transform;
			state.Valid = true;
		}
	}

	// ---- P5:物理事实 → ECS 事件队列 ----
	// 产出在固定步长阶段(物理系统),消费在可变阶段(任意系统)。见 Scene.h 的契约说明。
	void Scene::EnqueueContactEvent(const Physics::ContactEvent& event)
	{
		m_ContactEvents.push_back(event);
	}

	void Scene::EnqueueTriggerEvent(const Physics::TriggerEvent& event)
	{
		m_TriggerEvents.push_back(event);
	}

	void Scene::ClearPhysicsEvents()
	{
		m_ContactEvents.clear();
		m_TriggerEvents.clear();
	}

	// ---- 固定步长阶段(工业口径)----
	// 物理与移动在 `PreFixed` / `Fixed` 推进:它们必须跑固定 dt 才可复现 ——
	// 同一段真实时间、不管帧率是多少,结果一致(GameApp 的累加器负责"跑几步")。
	void Scene::RunFixedFrameSystems(Timestep fixedDt)
	{
		AssertOwnerThread();
		if (!m_FrameTimingsBegun)
		{
			m_FrameSystemTimings.clear();
			m_FrameTimingsBegun = true;
		}
		// P5:本帧的第一个固定步 ⇒ 物理事件从这里开始累积(一帧 0..N 步只清一次)。
		if (!m_PhysicsEventsBegun)
		{
			ClearPhysicsEvents();
			m_PhysicsEventsBegun = true;
		}
		if (!m_FrameSystems)
			return;

		s_InsideFramePipeline = true;
		struct PipelineScopeGuard
		{
			~PipelineScopeGuard() { s_InsideFramePipeline = false; }
		} guard;

		for (int phase = static_cast<int>(Gameplay::SystemPhase::PreFixed);
			phase <= static_cast<int>(Gameplay::SystemPhase::Fixed); ++phase)
		{
			m_FrameSystems->RunPhase(static_cast<Gameplay::SystemPhase>(phase), fixedDt);
			for (const Gameplay::SystemTiming& timing : m_FrameSystems->GetLastTimings())
				m_FrameSystemTimings.push_back({ timing.Name, timing.ParallelSafe, timing.Milliseconds });
		}
	}

	void Scene::OnFixedUpdate(Timestep fixedDt)
	{
		AssertOwnerThread();
		// 与 OnUpdateRuntime 对称:**不做运行态守卫** —— 宿主已经按"运行时才调"接线,
		// 而两个物理系统自身有空世界保护(OnUpdatePhysics2D/3D 各自早退),
		// 无守卫让 headless 测试能直接按固定步驱动(与 OnUpdateRuntime 同一用法)。
		EnsureDefaultFrameSystems();
		RunFixedFrameSystems(fixedDt);
	}
	const char* Scene::GetFrameSystemStatsDescription(const Scene& scene)
	{
		// 静态缓冲:仅供日志/调试打印,不做线程安全承诺。
		static std::string description;
		description.clear();
		for (const FrameSystemTiming& timing : scene.m_FrameSystemTimings)
		{
			if (!description.empty())
				description += ", ";
			description += timing.Name + (timing.ParallelSafe ? "[par]" : "[excl]") + "=" +
				std::to_string(timing.Milliseconds) + "ms";
		}
		return description.c_str();
	}

	void Scene::AssertStructuralWrite() const
	{
		AssertOwnerThread();
		if (m_CallbackDepth && m_ScriptWriteDepth == 0)
			throw std::logic_error("Synchronous structural writes are forbidden in lifecycle callbacks; use DeferStructuralChange");
		if (m_State == SceneState::Stopping || (IsActive() && !m_Committing && m_ScriptWriteDepth == 0))
			throw std::logic_error("An active scene accepts structural writes only while committing a command");
	}

	Scene::ScriptWriteScope::ScriptWriteScope(Scene& scene) : m_Scene(&scene)
	{
		scene.AssertOwnerThread();
		if (scene.m_State == SceneState::Stopping || scene.m_StopRequested)
			throw std::logic_error("Script structural writes are forbidden while the scene is stopping");
		// 只放行生命周期回调内的白名单写;回调外仍需走既有的结构提交点。
		if (scene.m_CallbackDepth == 0 && !scene.m_Committing)
			throw std::logic_error("Script structural writes are only allowed inside lifecycle callbacks or structural commits");
		++scene.m_ScriptWriteDepth;
	}

	Scene::ScriptWriteScope::~ScriptWriteScope()
	{
		if (m_Scene && m_Scene->m_ScriptWriteDepth)
			--m_Scene->m_ScriptWriteDepth;
	}

	// W4:事件/计时器回调的派发作用域。与生命周期回调的 InvokeCallback 同源地抬升
	// m_CallbackDepth 并设置"当前脚本来源",同时打开白名单结构写窗口(m_ScriptWriteDepth),
	// 使 CreateEntityShell / AddComponent / SetParent 在事件/计时器回调里当帧生效。
	// owner(句柄含场景令牌 + generation)不可用时 IsValid()==false,调用方不得执行回调体。
	Scene::ScriptCallbackScope::ScriptCallbackScope(Scene& scene, Entity owner, entt::id_type component,
		uint64_t generation)
		: m_Scene(&scene)
	{
		scene.AssertOwnerThread();
		if (!owner.IsValid() || owner.GetScene() != &scene)
			return;
		if (scene.m_State == SceneState::Stopping || scene.m_StopRequested)
			return;
		const ScriptSource source { static_cast<entt::entity>(owner), component, generation, false };
		if (!scene.IsSourceAlive(source))
			return;
		m_PreviousSource = scene.m_CallbackSource;
		scene.m_CallbackSource = source;
		++scene.m_CallbackDepth;
		++scene.m_ScriptWriteDepth;
		m_Valid = true;
	}

	Scene::ScriptCallbackScope::~ScriptCallbackScope()
	{
		if (!m_Valid || !m_Scene)
			return;
		m_Scene->m_CallbackSource = m_PreviousSource;
		if (m_Scene->m_CallbackDepth)
			--m_Scene->m_CallbackDepth;
		if (m_Scene->m_ScriptWriteDepth)
			--m_Scene->m_ScriptWriteDepth;
		m_Valid = false;
	}

	Entity Scene::CreateEntityShell(const std::string& name)
	{
		AssertOwnerThread();
		if (m_State == SceneState::Stopping || m_StopRequested)
			throw std::logic_error("Cannot create an entity shell while the scene is stopping");
		// 回调深度是本契约唯一放行的检查;活动场景在回调外仍只接受提交点写入。
		if (m_CallbackDepth == 0 && IsActive() && !m_Committing)
			throw std::logic_error("An active scene accepts structural writes only while committing a command or inside a script callback");
		const entt::entity handle = m_Registry.create();
		m_Registry.emplace<TagComponent>(handle, name);
		m_Registry.emplace<UUIDComponent>(handle, UUID());
		Entity entity(this, handle);
		NotifyComponentAdded(entity, entt::type_id<TagComponent>().hash());
		NotifyComponentAdded(entity, entt::type_id<UUIDComponent>().hash());
		return entity;
	}

	bool Scene::IsInsideScriptCallback() const
	{
		AssertOwnerThread();
		return m_CallbackDepth != 0;
	}

	bool Scene::IsVisibleToCurrentScriptUpdate(entt::entity) const
	{
		AssertOwnerThread();
		return true;
	}

	entt::registry& Scene::GetRegistry() { AssertStructuralWrite(); return m_Registry; }
	const entt::registry& Scene::GetRegistry() const { AssertOwnerThread(); return m_Registry; }

	// P4-U13b:Prefab 实例注册表的访问器。它们只碰 std::vector 成员,不写 entt 注册表结构,
	// 所以只做线程归属检查(与 GetWorldSettings 同类);结构写保护仍由 GetRegistry 承担。
	std::vector<Gameplay::PrefabInstanceRecord>& Scene::PrefabInstances()
	{
		AssertOwnerThread();
		return m_PrefabInstances;
	}

	const std::vector<Gameplay::PrefabInstanceRecord>& Scene::PrefabInstances() const
	{
		AssertOwnerThread();
		return m_PrefabInstances;
	}

	Gameplay::PrefabInstanceRecord* Scene::FindPrefabInstance(entt::entity root)
	{
		AssertOwnerThread();
		for (Gameplay::PrefabInstanceRecord& record : m_PrefabInstances)
			if (record.Root == root)
				return &record;
		return nullptr;
	}

	const Gameplay::PrefabInstanceRecord* Scene::FindPrefabInstance(entt::entity root) const
	{
		AssertOwnerThread();
		for (const Gameplay::PrefabInstanceRecord& record : m_PrefabInstances)
			if (record.Root == root)
				return &record;
		return nullptr;
	}

	Gameplay::PrefabInstanceRecord& Scene::AddPrefabInstance(const std::string& prefabPath, entt::entity root)
	{
		AssertOwnerThread();
		if (Gameplay::PrefabInstanceRecord* existing = FindPrefabInstance(root))
		{
			existing->PrefabPath = prefabPath;
			return *existing;
		}
		Gameplay::PrefabInstanceRecord record;
		record.PrefabPath = prefabPath;
		record.Root = root;
		m_PrefabInstances.push_back(std::move(record));
		return m_PrefabInstances.back();
	}

	bool Scene::RemovePrefabInstance(entt::entity root)
	{
		AssertOwnerThread();
		for (auto it = m_PrefabInstances.begin(); it != m_PrefabInstances.end(); ++it)
		{
			if (it->Root != root)
				continue;
			m_PrefabInstances.erase(it);
			return true;
		}
		return false;
	}

	bool Scene::IsPendingDestroy(entt::entity entity) const
	{
		AssertOwnerThread();
		return m_PendingDestroy.count(entity) != 0;
	}

	bool Scene::IsPendingRemoval(entt::entity entity, entt::id_type component) const
	{
		auto it = m_PendingRemove.find(entity);
		return it != m_PendingRemove.end() && it->second.count(component) != 0;
	}

	bool Scene::IsSourceAlive(const ScriptSource& source) const
	{
		if (source.EntityHandle == entt::null) return true;
		if (!m_Registry.valid(source.EntityHandle) || IsPendingDestroy(source.EntityHandle) ||
			IsPendingRemoval(source.EntityHandle, source.Component)) return false;
		return true;
	}

	bool Scene::DeferStructuralChange(std::function<void(Scene&)> command)
	{
		AssertOwnerThread();
		if (!command || m_State == SceneState::Stopping || m_StopRequested || m_CallbackSource.Destroying) return false;
		if (!IsActive() && !m_CallbackDepth && !m_Committing)
		{
			try { command(*this); return true; }
			catch (const std::exception& error) { Report(std::string("[Scene] StructuralChange: ") + error.what()); }
			catch (...) { Report("[Scene] StructuralChange: unknown exception"); }
			return false;
		}
		m_Changes.push_back({ ChangeKind::General, std::move(command), m_CallbackSource });
		return true;
	}

	void Scene::RequestDestroy(entt::entity entity)
	{
		AssertOwnerThread();
		if (!m_Registry.valid(entity) || !m_PendingDestroy.insert(entity).second) return;
		m_Changes.push_back({ ChangeKind::DestroyEntity, {}, {}, entity });
		if (!IsActive() && !m_CallbackDepth && !m_Committing) FlushStructuralChanges();
	}

	void Scene::RequestRemove(entt::entity entity, entt::id_type component)
	{
		AssertOwnerThread();
		if (!m_Registry.valid(entity) || IsPendingDestroy(entity) || IsPendingRemoval(entity, component)) return;
		Entity target(this, entity);
		if (!target.HasComponent(component)) return;
		std::string reason;
		if (!target.CanRemoveComponent(component, &reason)) throw std::logic_error(reason);
		m_PendingRemove[entity].insert(component);
		m_Changes.push_back({ ChangeKind::RemoveComponent, {}, {}, entity, component });
		if (!IsActive() && !m_CallbackDepth && !m_Committing) FlushStructuralChanges();
	}

	bool Scene::CanApplyScriptReload() const
	{
		// 与 FlushStructuralChanges 的守卫同源:回调内/结构提交点内禁止改脚本实例,
		// 停止流程中也不允许(即将销毁全部实例)。
		return m_CallbackDepth == 0 && !m_Committing && m_ScriptWriteDepth == 0 && !m_StopRequested
			&& m_State != SceneState::Stopping;
	}

	void Scene::FlushStructuralChanges()
	{
		AssertOwnerThread();
		if (m_CallbackDepth || m_Committing || m_ScriptWriteDepth)
			throw std::logic_error("Structural changes cannot be recursively committed");
		if (m_StopRequested) { StopScene(); return; }
		std::vector<StructuralChange> batch;
		batch.swap(m_Changes);
		m_Committing = true;
		for (auto& change : batch)
		{
			const auto previousSource = m_CallbackSource;
			// Commands submitted by a command retain its original script owner too.
			m_CallbackSource = change.Source;
			try
			{
				switch (change.Kind)
				{
					case ChangeKind::General:
						if (m_State != SceneState::Stopping && IsSourceAlive(change.Source)) change.Command(*this);
						break;
					case ChangeKind::DestroyEntity: DestroyEntityNow(change.Target); break;
					case ChangeKind::RemoveComponent: RemoveComponentNow(change.Target, change.Component); break;
				}
			}
			catch (const std::exception& error) { FaultSource(change.Source, error.what()); }
			catch (...) { FaultSource(change.Source, "Unknown exception in structural command"); }
			m_CallbackSource = previousSource;
			if (m_StopRequested) break;
		}
		m_Committing = false;
		if (m_StopRequested) StopScene();
	}

	void Scene::InvokeCallback(const ScriptSource& source, const std::function<void()>& callback)
	{
		const auto previous = m_CallbackSource;
		m_CallbackSource = source;
		++m_CallbackDepth;
		try { callback(); }
		catch (...) { --m_CallbackDepth; m_CallbackSource = previous; throw; }
		--m_CallbackDepth;
		m_CallbackSource = previous;
	}

	void Scene::FaultSource(const ScriptSource& source, const std::string& error)
	{
		Report("[Scene] entity=" + std::to_string(static_cast<uint32_t>(source.EntityHandle)) +
			" phase=StructuralChange: " + error);
	}



	void Scene::DestroyPhysicsJoint(entt::entity entity)
	{
		auto* joint = m_Registry.try_get<JointComponent>(entity);
		if (!joint) return;
		const b2JointId jointId = joint->RuntimeJointId;
		joint->RuntimeJointId = b2_nullJointId;   // 先断引用再销毁(重入/失败都不留悬垂)
		if (b2Joint_IsValid(jointId)) b2DestroyJoint(jointId, true);
	}

	// 反向清理:某实体被删时,其它实体指向它的关节也要拆掉(否则 Box2D 里会留下悬垂 pair)。
	void Scene::DestroyJointsReferencing(entt::entity entity)
	{
		for (const entt::entity owner : m_Registry.view<JointComponent>())
			if (m_Registry.get<JointComponent>(owner).Connected == entity)
				DestroyPhysicsJoint(owner);
	}

	void Scene::EnsurePhysicsJoint(entt::entity entity)
	{
		if (!m_Registry.valid(entity)) return;
		auto* joint = m_Registry.try_get<JointComponent>(entity);
		if (!joint) return;

		const bool has3D = m_Registry.all_of<RigidBody3DComponent>(entity);
		if (m_Physics3D && has3D)
		{
			// 3D:交给 Jolt 约束。两端都必须已有活刚体,否则 Physics3DWorld::CreateJoint 会抛
			// 可读错误 —— 这里先自己判一次,把"还没就绪"和"配置错误"分开(前者静默跳过)。
			const entt::entity other = joint->Connected;
			if (!m_Registry.valid(other) || other == entity) return;
			if (!m_Registry.all_of<RigidBody3DComponent>(other)) return;
			m_Physics3D->CreateJoint(entity, other, static_cast<int>(joint->Type),
				joint->AnchorSelf, joint->AnchorOther, joint->Axis,
				joint->MinDistance, joint->MaxDistance, joint->EnableCollision);
			return;
		}

		if (!b2World_IsValid(m_PhysicsWorldId)) { joint->RuntimeJointId = b2_nullJointId; return; }
		if (BuildRuntimeJoint(m_PhysicsWorldId, m_Registry, entity)) return;
		// 目标还没就绪(对方没有活刚体/组件还没挂全):留空句柄,下一次 Start 或补挂时再建。
		if (!b2Joint_IsValid(joint->RuntimeJointId))
			joint->RuntimeJointId = b2_nullJointId;
	}

	void Scene::DestroyPhysicsBody(entt::entity entity)
	{
		auto* body = m_Registry.try_get<RigidBody2DComponent>(entity);
		if (!body) return;
		b2BodyId bodyId = body->RuntimeBodyId;
		// 先把组件切到"无刚体"状态,再销毁 Box2D 刚体(失败/重入都不会留下悬垂句柄)。
		body->RuntimeBodyId = b2_nullBodyId;
		if (b2Body_IsValid(bodyId)) b2DestroyBody(bodyId);
	}

	// ---- W3f:2D 物理运行时 API ----
	bool Scene::IsPhysics2DRunning() const
	{
		return b2World_IsValid(m_PhysicsWorldId);
	}

	// 组件 id 走 schema 的 StorageBinding(entt 类型 hash 已隐式注册,取不到不代表"没有该组件")。
	bool Scene::TryGetPhysicsBody(entt::entity entity, b2BodyId* bodyId, std::string* error)
	{
		auto Fail = [&](const std::string& message)
		{
			if (error) *error = message;
			return false;
		};
		if (bodyId) *bodyId = b2_nullBodyId;
		if (error) error->clear();
		if (!m_Registry.valid(entity))
			return Fail("requires a live entity");
		const Schema::TypeSchema* schema = m_Context->Schemas().Find("World::RigidBody2DComponent");
		if (!schema || !schema->Storage)
			return Fail("requires the RigidBody2DComponent schema to be registered");
		if (!m_Registry.all_of<RigidBody2DComponent>(entity))
			return Fail("requires a 2D rigid body; this entity has no RigidBody2DComponent");
		if (!b2World_IsValid(m_PhysicsWorldId))
			return Fail("requires a running 2D physics world; start the runtime (Scene:OnRuntimeStart) first");
		const RigidBody2DComponent& rigidBody = m_Registry.get<RigidBody2DComponent>(entity);
		if (!b2Body_IsValid(rigidBody.RuntimeBodyId))
			return Fail("has a RigidBody2DComponent but no live Box2D body; the component was not added to the running world");
		if (bodyId) *bodyId = rigidBody.RuntimeBodyId;
		return true;
	}

	void Scene::SyncPhysicsBodyFromTransform(entt::entity entity)
	{
		b2BodyId bodyId = b2_nullBodyId;
		std::string error;
		if (!TryGetPhysicsBody(entity, &bodyId, &error))
			throw std::logic_error("Entity:SyncPhysicsBody " + error);
		auto* transform = m_Registry.try_get<TransformComponent>(entity);
		if (!transform)
			throw std::logic_error("Entity:SyncPhysicsBody requires a TransformComponent");
		b2Body_SetTransform(bodyId, { transform->Location.x, transform->Location.y }, b2MakeRot(transform->Rotation.z));
		b2Body_SetAwake(bodyId, true);
	}

	void Scene::EnsurePhysicsBody(entt::entity entity)
	{
		if (!m_Registry.valid(entity)) return;
		auto* rigidBody = m_Registry.try_get<RigidBody2DComponent>(entity);
		if (!rigidBody) return;
		// P6:运行时补建刚体 ⇒ 同步补插值状态(本方法是结构提交点,允许结构写)。
		if (m_PhysicsInterpolationEnabled && !m_Registry.all_of<PhysicsInterpolationState>(entity))
			m_Registry.emplace<PhysicsInterpolationState>(entity);
		if (!b2World_IsValid(m_PhysicsWorldId))
		{
			// 世界未启动:AddComponent 只落组件配置;下一次 OnRuntimeStart 会照配置建刚体。
			rigidBody->RuntimeBodyId = b2_nullBodyId;
			return;
		}
		if (BuildRuntimeBody(m_PhysicsWorldId, m_Registry, entity)) return;
		// 已存在的刚体:组件是唯一事实源,类型在脚本侧改过后同步给 Box2D
		// (含 shape 的质量/惯量重算)。Transform 保持 Box2D 的权威状态,不在这里回推。
		if (!b2Body_IsValid(rigidBody->RuntimeBodyId)) return;
		b2BodyType box2dType = b2_staticBody;
		switch (rigidBody->Type)
		{
			case RigidBody2DComponent::BodyType::Static: box2dType = b2_staticBody; break;
			case RigidBody2DComponent::BodyType::Dynamic: box2dType = b2_dynamicBody; break;
			case RigidBody2DComponent::BodyType::Kinematic: box2dType = b2_kinematicBody; break;
		}
		if (b2Body_GetType(rigidBody->RuntimeBodyId) != box2dType)
			b2Body_SetType(rigidBody->RuntimeBodyId, box2dType);
	}

	void Scene::DestroyEntityNow(entt::entity entity)
	{
		if (m_Registry.valid(entity))
		{
			DestroyJointsReferencing(entity);
			DestroyPhysicsJoint(entity);
			DestroyPhysicsBody(entity);
			if (m_Physics3D) { m_Physics3D->DestroyJoints(entity); m_Physics3D->DestroyBody(entity); }
			m_Registry.destroy(entity);
		}
		m_PendingDestroy.erase(entity);
		m_PendingRemove.erase(entity);
	}

	void Scene::RemoveComponentNow(entt::entity entity, entt::id_type component)
	{
		if (m_Registry.valid(entity) && !IsPendingDestroy(entity))
		{
			Entity target(this, entity);
			std::string reason;
			if (target.HasComponent(component) && target.CanRemoveComponent(component, &reason))
			{
				if (component == entt::type_id<RigidBody2DComponent>().hash()) DestroyPhysicsBody(entity);
				else if (component == entt::type_id<RigidBody3DComponent>().hash()) { if (m_Physics3D) m_Physics3D->DestroyBody(entity); }
				else if (component == entt::type_id<JointComponent>().hash())
				{
					DestroyPhysicsJoint(entity);
					if (m_Physics3D) m_Physics3D->DestroyJoints(entity);
				}
				if (auto* storage = m_Registry.storage(component))
				{
					storage->remove(entity);
					NotifyComponentRemoved(target, component);
				}
			}
			else if (!reason.empty()) Report("[Scene] RemoveComponent: " + reason);
		}
		auto it = m_PendingRemove.find(entity);
		if (it != m_PendingRemove.end())
		{
			it->second.erase(component);
			if (it->second.empty()) m_PendingRemove.erase(it);
		}
	}

	// ---- PURE-ECS:帧内一次的三个步骤(见 Scene.h 的说明)----
	void Scene::BeginFrame()
	{
		AssertOwnerThread();
		m_WorldTransformsDone = false;
		m_AnimationDone = false;
		m_RenderExtractDone = false;
	}

	void Scene::EnsureWorldTransforms()
	{
		AssertOwnerThread();
		if (m_WorldTransformsDone)
			return;
		m_WorldTransformsDone = true;
		TransformSystem::UpdateWorldTransforms(m_Registry);
	}

	void Scene::EnsureAnimationAdvanced(Timestep dt)
	{
		AssertOwnerThread();
		if (m_AnimationDone)
			return;
		m_AnimationDone = true;
		AnimationSystem::Update(*this, dt.GetSeconds());
	}

	void Scene::SetRenderExtractSink(IRenderExtractSink* sink)
	{
		AssertOwnerThread();
		m_RenderExtractSink = sink;
	}

	FrameExtract& Scene::RenderExtract()
	{
		if (!m_RenderExtract)
			m_RenderExtract = std::make_unique<FrameExtract>();
		return *m_RenderExtract;
	}

	const FrameExtract& Scene::RenderExtract() const
	{
		// const 重载不惰性创建(不可能返回可写引用);没有缓冲时返回一个空壳。
		static const FrameExtract kEmpty;
		return m_RenderExtract ? *m_RenderExtract : kEmpty;
	}

	void Scene::EnsureRenderExtract(float deltaSeconds)
	{
		AssertOwnerThread();
		if (m_RenderExtractDone)
			return;
		m_RenderExtractDone = true;
		if (!m_RenderExtractSink)
			return;   // 无渲染宿主(纯逻辑测试/工具):抽取不是必需的
		RenderExtract().Clear();
		m_RenderExtractSink->ExtractScene(*this, deltaSeconds);
	}

	void Scene::OnUpdateEditor(Timestep, const EditorCamera&)
	{
		AssertOwnerThread();
		// 编辑态不跑帧系统,但"本帧一次"的步骤仍要按帧推进:
		// 渲染前由 EnsureWorldTransforms / EnsureAnimationAdvanced 兜底执行(见 SceneRenderer)。
		BeginFrame();
	}
	// 内置帧系统:运行/模拟更新保持"脚本 + 物理"的原有顺序与语义,标记为独占(主线程)。
	// 宿主可再注册 parallel-safe 系统(不得触碰注册表结构),它们会在内置阶段之前并行执行。
	void Scene::EnsureDefaultFrameSystems()
	{
		if (!m_FrameSystemDefinitions.empty())
			return;
		// PURE-ECS(工业口径):**物理与移动跑固定步长**(`Fixed` 阶段)。
		// 此前它们注册在默认的 `Update` 阶段、拿的是可变帧时间 ⇒ 同一段真实时间在不同帧率下
		// 结果不同(物理不可复现)。引擎的累加器(GameApp 的 m_Accumulator / MaxFixedStepsPerFrame)
		// 已经负责"一帧跑几步",这里只是把系统挪到它该在的阶段。
		RegisterFrameSystem({ "physics-2d", false, [this](Timestep ts) { OnUpdatePhysics2D(ts); },
			Gameplay::SystemPhase::Fixed, {} });
		RegisterFrameSystem({ "physics-3d", false, [this](Timestep ts) { OnUpdatePhysics3D(ts); },
			Gameplay::SystemPhase::Fixed, {} });
		RegisterFrameSystem({ "movement-system", false, [this](Timestep ts) { MovementSystem().Update(*this, ts); },
			Gameplay::SystemPhase::Fixed, { "physics-2d", "physics-3d" } });
		// 可变阶段(每帧一次):世界矩阵传播/相机/动画/抽取 —— 表现层,看到的是本帧
		// **最后一次**物理步之后的位姿(GameApp 的顺序是 Fixed(0..N) → Update → Late → PreRender)。
		RegisterFrameSystem({ "transform-system", false, [this](Timestep ts) { (void)ts; TransformSystem::UpdateWorldTransforms(m_Registry); } });
		RegisterFrameSystem({ "camera-system", false, [this](Timestep ts) { (void)ts; CameraSystem::UpdateAllCameras(m_Registry, m_ViewportWidth, m_ViewportHeight); } });
		// PURE-ECS:骨骼动画采样进 `PreRender` 阶段 —— 它必须排在 transform-system 之后、
		// 渲染之前;进管线后它出现在 Systems Pipeline 里,用户系统也能用 `after` 排序它。
		// 幂等:编辑态不跑帧系统,由 SceneRenderer 兜底跑同一个 EnsureAnimationAdvanced。
		RegisterFrameSystem({ "animation-system", false,
			[this](Timestep ts) { EnsureAnimationAdvanced(ts); },
			Gameplay::SystemPhase::PreRender, {} });
		// PURE-ECS:渲染抽取也进管线(PreRender,**排在 animation-system 之后** ——
		// 蒙皮绘制要拿本帧的调色板)。相机无关,所以这里做;剔除/阴影矩阵/pass 留在提交侧。
		// 编辑态不跑帧系统 ⇒ SceneRenderer::SubmitScene 里兜底跑同一个 EnsureRenderExtract。
		RegisterFrameSystem({ "render-extract", false,
			[this](Timestep ts) { EnsureRenderExtract(ts.GetSeconds()); },
			Gameplay::SystemPhase::PreRender, { "animation-system" } });
	}

	void Scene::OnUpdateRuntime(Timestep ts)
	{
		BeginFrame();
		EnsureDefaultFrameSystems();
		// Pure ECS:系统脚本热重载 —— 每帧轮询 scripts/systems/,变化过的文件整份重跑。
		if (ScriptEngine::IsInitialized())
			ScriptEngine::PollSystemScriptReload(*this, ts.GetSeconds());
		RunFrameSystems(ts);
		FlushStructuralChanges();
	}

	void Scene::OnUpdateSimulation(Timestep ts, const EditorCamera&)
	{
		BeginFrame();
		EnsureDefaultFrameSystems();
		RunFrameSystems(ts);
	}

	void Scene::OnRuntimeStart()
	{
		AssertOwnerThread();
		if (IsActive()) return;
		// P1b D6:2D/3D 物理同实体互斥 —— 在创建任何物理世界之前校验;失败时场景保持 Stopped
		// 并抛出可读错误(编辑器 Play 捕获后回 Edit,日志里能看到原因)。
		std::string physicsError;
		if (!Physics3DWorld::ValidateScene(*this, &physicsError))
		{
			Report("[Physics3D] " + physicsError);
			throw std::logic_error(physicsError);
		}
		OnPhysics2DStart();
		OnPhysics3DStart();
		// P7:两个物理世界都就绪后再建关节 —— 关节要求两端都已有活刚体,而 3D 分支依赖
		// m_Physics3D 已存在(放在 OnPhysics2DStart 里会让 3D 关节永远建不出来)。
		// 顺序按 registry 遍历 ⇒ 同一存档输入下确定。
		for (const auto entity : m_Registry.view<JointComponent>())
		{
			m_Registry.get<JointComponent>(entity).RuntimeJointId = b2_nullJointId;
			EnsurePhysicsJoint(entity);
		}
		// P6:为物理实体预建插值状态(结构写只允许在非运行态 / 提交点做;这里 m_State 还是 Stopped)。
		if (m_PhysicsInterpolationEnabled)
		{
			for (const auto entity : m_Registry.view<RigidBody2DComponent>())
				if (!m_Registry.all_of<PhysicsInterpolationState>(entity))
					m_Registry.emplace<PhysicsInterpolationState>(entity);
			for (const auto entity : m_Registry.view<RigidBody3DComponent>())
				if (!m_Registry.all_of<PhysicsInterpolationState>(entity))
					m_Registry.emplace<PhysicsInterpolationState>(entity);
		}
		EnsureDefaultFrameSystems();
		if (ScriptEngine::IsInitialized())
		{
			ScriptEngine::LoadSystemScripts(*this);
		}
		// PURE-ECS:模块登记的场景系统(项目层 C++ 系统)在引擎内建帧系统之后挂上;
		// 与 Lua 系统脚本同一时机、同一个 SystemRegistry。契约见 WorldContext.h。
		m_Context->RunSceneAttachHooks(*this);
		m_State = SceneState::Running;
		RunStartupSystems();
	}
	void Scene::OnSimulationStart() { OnRuntimeStart(); }
	void Scene::OnRuntimeStop() { StopScene(); }
	void Scene::OnSimulationStop() { StopScene(); }



	void Scene::StopScene()
	{
		AssertOwnerThread();
		if (m_State == SceneState::Stopping) return;
		// PURE-ECS:模块登记的场景系统先撤销(与 RunSceneAttachHooks 对称);只在真的从
		// 运行态停下来时跑,重复 StopScene 不重复 Detach。
		if (m_State == SceneState::Running)
		{
			RunTeardownSystems();
			m_Context->RunSceneDetachHooks(*this);
		}
		m_State = SceneState::Stopping;
		m_StopRequested = false;
		m_Changes.clear();
		// Pure ECS:场景停止时撤销系统脚本注册的帧系统,下一次启动重新装载。
		if (ScriptEngine::IsInitialized())
			ScriptEngine::UnloadSystemScripts(*this);
		OnPhysics2DStop();
		OnPhysics3DStop();
		// Destruction callbacks may request further idempotent deletes/removals, but no general work.
		while (!m_PendingDestroy.empty() || !m_PendingRemove.empty())
		{
			const auto destroys = m_PendingDestroy;
			for (const auto entity : destroys) DestroyEntityNow(entity);
			const auto removals = m_PendingRemove;
			for (const auto& [entity, components] : removals)
				for (const auto component : components) RemoveComponentNow(entity, component);
		}
		m_Changes.clear();
		m_StopRequested = false;
		m_State = SceneState::Stopped;
	}

	void Scene::OnViewportResize(uint32_t width, uint32_t height)
	{
		AssertOwnerThread();
		m_ViewportWidth = width;
		m_ViewportHeight = height;
		CameraSystem::UpdateAllCameras(m_Registry, width, height);
	}

	Entity Scene::GetPrimaryCameraEntity()
	{
		AssertOwnerThread();
		for (const auto entity : m_Registry.view<CameraComponent, TransformComponent>())
			if (!IsPendingDestroy(entity) && !IsPendingRemoval(entity, entt::type_id<CameraComponent>().hash()) &&
				m_Registry.get<CameraComponent>(entity).Primary) return Entity(this, entity);
		return {};
	}

	void Scene::DuplicateEntity(Entity entity)
	{
		AssertStructuralWrite();
		if (!entity.IsValid() || entity.GetScene() != this || IsPendingDestroy(entity)) return;
		if (IsActive() && (entity.HasComponent<RigidBody2DComponent>() || entity.HasComponent<BoxCollider2DComponent>() || entity.HasComponent<CircleCollider2DComponent>()))
			throw std::logic_error("Duplicating physics components requires a stopped scene");
		const std::string name = entity.HasComponent<TagComponent>() ? entity.GetComponent<TagComponent>().Tag : "Empty Entity";
		Entity copy = Entity::CreateEntity(this, name);
		if (entity.HasComponent<TransformComponent>()) copy.AddComponent<TransformComponent>(entity.GetComponent<TransformComponent>());
		for (const Schema::TypeSchema* schema : m_Context->Schemas().List(Schema::TypeCategory::Component))
		{
			if (!schema || !schema->Storage || !schema->Storage->Copy) continue;
			if (entity.HasComponent(schema->Storage->ComponentId))
				schema->Storage->Copy(static_cast<void*>(&copy), static_cast<void*>(&entity));
		}
	}

	void Scene::CopyScene(Ref<Scene>& other, Ref<Scene>& newScene)
	{
		if (!other || !newScene || other == newScene) throw std::logic_error("Scene clone requires distinct source and destination scenes");
		other->AssertOwnerThread();
		newScene->AssertStructuralWrite();
		if (newScene->IsActive()) throw std::logic_error("Scene clone destination must be stopped");
		newScene->m_ViewportHeight = other->m_ViewportHeight;
		newScene->m_ViewportWidth = other->m_ViewportWidth;
		std::unordered_map<UUID, entt::entity> entityMap;
		for (const auto entity : other->m_Registry.view<UUIDComponent>())
		{
			const auto id = other->m_Registry.get<UUIDComponent>(entity).ID;
			const auto* tag = other->m_Registry.try_get<TagComponent>(entity);
			entityMap[id] = Entity::CreateEntity(newScene.get(), tag ? tag->Tag : "Empty Entity", id);
		}
		for (const Schema::TypeSchema* schema : other->m_Context->Schemas().List(Schema::TypeCategory::Component))
		{
			if (!schema || !schema->Storage || !schema->Storage->CopyAll) continue;
			schema->Storage->CopyAll(static_cast<void*>(&newScene->m_Registry), static_cast<void*>(&other->m_Registry), static_cast<const void*>(&entityMap));
		}
		// P4-U13b:prefab 实例注册表跟着场景一起克隆(编辑器 Play 的活动场景就是这份副本),
		// 句柄按 UUID 映射重定向——不复制的话 Play 期间层级/属性面板会看不到实例与覆盖。
		newScene->m_PrefabInstances.clear();
		for (const Gameplay::PrefabInstanceRecord& source : other->m_PrefabInstances)
		{
			const auto* sourceIdentity = other->m_Registry.try_get<UUIDComponent>(source.Root);
			if (!sourceIdentity)
				continue;
			const auto rootIt = entityMap.find(sourceIdentity->ID);
			if (rootIt == entityMap.end())
				continue;
			Gameplay::PrefabInstanceRecord& record =
				newScene->AddPrefabInstance(source.PrefabPath, rootIt->second);
			for (const auto& [handle, fields] : source.Overrides)
			{
				const auto* identity = other->m_Registry.try_get<UUIDComponent>(static_cast<entt::entity>(handle));
				if (!identity)
					continue;
				const auto entityIt = entityMap.find(identity->ID);
				if (entityIt == entityMap.end())
					continue;
				record.Overrides[static_cast<uint32_t>(entityIt->second)] = fields;
			}
		}
	}

	void Scene::OnPhysics2DStart()
	{
		if (b2World_IsValid(m_PhysicsWorldId)) return;
		b2WorldDef worldDef = b2DefaultWorldDef();
		worldDef.gravity = { 0.0f, -9.8f };
		worldDef.restitutionThreshold = 0.5f;
		m_PhysicsWorldId = b2CreateWorld(&worldDef);
		for (const auto entity : m_Registry.view<RigidBody2DComponent>())
		{
			auto& rb = m_Registry.get<RigidBody2DComponent>(entity);
			rb.RuntimeBodyId = b2_nullBodyId;
			// W3f:创建逻辑抽到 BuildRuntimeBody,与运行时 AddComponent 补建共用同一套形状/质量口径。
			BuildRuntimeBody(m_PhysicsWorldId, m_Registry, entity);
		}
	}

	void Scene::OnUpdatePhysics2D(Timestep ts)
	{
		if (!b2World_IsValid(m_PhysicsWorldId)) return;
		// P6:步进前先记下"上一固定步"的渲染矩阵(插值起点)。
		RecordPhysicsInterpolationState();
		b2World_Step(m_PhysicsWorldId, ts.GetSeconds(), 4);
		// P5:步进结束后立刻把 Box2D 的 begin/end/persist 翻译进场景事件队列
		// (队列在"本帧第一个固定步"清空,由 RunFixedFrameSystems 负责)。
		HarvestPhysics2DEvents(m_PhysicsWorldId, m_Registry, m_ContactEvents, m_TriggerEvents, m_SensorOverlaps2D);
		for (const auto entity : m_Registry.view<RigidBody2DComponent>())
		{
			if (IsPendingDestroy(entity) || IsPendingRemoval(entity, entt::type_id<RigidBody2DComponent>().hash())) continue;
			auto& rb = m_Registry.get<RigidBody2DComponent>(entity);
			auto* transform = m_Registry.try_get<TransformComponent>(entity);
			if (!transform || !b2Body_IsValid(rb.RuntimeBodyId))
			{
				Report("[Physics] Skipping invalid body or missing Transform entity=" + std::to_string(static_cast<uint32_t>(entity)));
				continue;
			}
			const b2Transform body = b2Body_GetTransform(rb.RuntimeBodyId);
			transform->SetTransform({ body.p.x, body.p.y, transform->Location.z },
				{ transform->Rotation.x, transform->Rotation.y, b2Rot_GetAngle(body.q) }, transform->Scale);
		}
	}

	void Scene::OnPhysics2DStop()
	{
		for (const auto entity : m_Registry.view<JointComponent>())
			m_Registry.get<JointComponent>(entity).RuntimeJointId = b2_nullJointId;
		if (b2World_IsValid(m_PhysicsWorldId)) b2DestroyWorld(m_PhysicsWorldId);
		m_PhysicsWorldId = b2_nullWorldId;
		m_SensorOverlaps2D.clear();   // 世界没了,重叠状态不能跨运行存活
		for (const auto entity : m_Registry.view<RigidBody2DComponent>())
			m_Registry.get<RigidBody2DComponent>(entity).RuntimeBodyId = b2_nullBodyId;
	}

	// ---- P1b D6:3D 物理(Jolt) ----
	bool Scene::SyncPhysics3DTransform(entt::entity entity, const glm::vec3& location, const glm::quat& rotation)
	{
		AssertOwnerThread();
		if (!m_Registry.valid(entity)) return false;
		if (IsPendingDestroy(entity) || IsPendingRemoval(entity, entt::type_id<RigidBody3DComponent>().hash())) return false;
		auto* transform = m_Registry.try_get<TransformComponent>(entity);
		if (!transform) return false;
		// 位姿变化才写:位置用绝对容差、旋转用四元数点积(避免浮点噪声导致每帧重算 Transform 矩阵、
		// 打断层级/标脏)。
		const bool locationChanged = glm::distance(transform->Location, location) > 1e-4f;
		const bool rotationChanged = std::fabs(glm::dot(transform->RotationQuat, rotation)) < 1.0f - 1e-4f;
		if (!locationChanged && !rotationChanged) return false;
		transform->Location = location;
		transform->RotationQuat = rotation;
		transform->Rotation = glm::eulerAngles(rotation);
		transform->RecalculateTransform();
		return true;
	}

	void Scene::OnPhysics3DStart()
	{
		if (m_Physics3D) return;
		auto world = std::make_unique<Physics3DWorld>();
		// P5:物理步进里的回调只做"翻译进事件队列"这一件事 —— 事件在固定步长阶段产出、
		// 由可变阶段的系统读取。队列操作本身不回调用户代码,所以不会把异常带进物理步进。
		world->SetContactCallback([this](const Physics::ContactEvent& event) { EnqueueContactEvent(event); });
		world->SetTriggerCallback([this](const Physics::TriggerEvent& event) { EnqueueTriggerEvent(event); });
		world->Start(*this);
		m_Physics3D = std::move(world);
	}

	void Scene::OnUpdatePhysics3D(Timestep ts)
	{
		if (!m_Physics3D) return;
		// P6:步进前先记下"上一固定步"的渲染矩阵(插值起点)。
		RecordPhysicsInterpolationState();
		m_Physics3D->Step(ts.GetSeconds());
		m_Physics3D->SyncTransforms();
	}

	void Scene::OnPhysics3DStop()
	{
		if (!m_Physics3D) return;
		m_Physics3D->Stop();
		m_Physics3D.reset();
	}

	uint64_t Scene::AddComponentObserver(entt::id_type componentId, std::function<void(Entity)> onAdd, std::function<void(Entity)> onRemove)
	{
		AssertOwnerThread();
		const uint64_t id = ++m_NextObserverId;
		m_ComponentObservers.push_back({ id, componentId, std::move(onAdd), std::move(onRemove) });
		return id;
	}

	void Scene::RemoveComponentObserver(uint64_t observerId)
	{
		AssertOwnerThread();
		auto it = std::remove_if(m_ComponentObservers.begin(), m_ComponentObservers.end(),
			[observerId](const ComponentObserverEntry& entry) {
				return entry.Id == observerId;
			});
		m_ComponentObservers.erase(it, m_ComponentObservers.end());
	}

	void Scene::NotifyComponentAdded(Entity entity, entt::id_type componentId)
	{
		AssertOwnerThread();
		std::vector<std::function<void(Entity)>> callbacks;
		callbacks.reserve(m_ComponentObservers.size());
		for (const auto& observer : m_ComponentObservers)
		{
			if (observer.ComponentId == componentId && observer.OnAdd)
				callbacks.push_back(observer.OnAdd);
		}
		for (const auto& cb : callbacks)
		{
			cb(entity);
		}
	}

	void Scene::NotifyComponentRemoved(Entity entity, entt::id_type componentId)
	{
		AssertOwnerThread();
		std::vector<std::function<void(Entity)>> callbacks;
		callbacks.reserve(m_ComponentObservers.size());
		for (const auto& observer : m_ComponentObservers)
		{
			if (observer.ComponentId == componentId && observer.OnRemove)
				callbacks.push_back(observer.OnRemove);
		}
		for (const auto& cb : callbacks)
		{
			cb(entity);
		}
	}
}
