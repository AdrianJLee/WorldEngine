#include "wldpch.h"
#include "World/Physics/Physics3D.h"

#include "World/Core/Asset/WModelIO.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"

// D6 硬约束:Jolt 头文件只允许出现在本 TU(Physics3D.h / Scene.h 都不 include Jolt)。
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Geometry/IndexedTriangle.h>
#include <Jolt/Math/Float3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace World
{
	namespace
	{
		// 单层过滤:所有 3D 物体在同一 object layer / broadphase layer,全部互相碰撞
		// (分层碰撞按需再引入,不在 D6 范围)。
		constexpr JPH::ObjectLayer kObjectLayer = 0;
		constexpr JPH::uint kMaxBodies = 4096;
		constexpr JPH::uint kMaxBodyPairs = 2048;
		constexpr JPH::uint kMaxContactConstraints = 2048;
		constexpr JPH::uint kTempAllocatorBytes = 8u * 1024u * 1024u;
		constexpr JPH::uint kMaxJobs = 1024;

		constexpr float kLocationEpsilon = 1e-4f;
		constexpr float kRotationEpsilon = 1e-4f;
		constexpr int kDebugCircleSegments = 24;

		std::string EntityLabel(entt::entity entity)
		{
			return std::to_string(static_cast<uint32_t>(entity));
		}

		// Jolt 全局运行时(默认分配器 + Factory + 类型注册)按引用计数共享:
		// 多场景同时 Play / 多个 Physics3DWorld 并存时不会互相把 Factory 卸掉。
		// Start/Stop 都必须在场景 owner 线程调用,所以这里的计数不需要额外同步。
		int s_JoltRuntimeUsers = 0;

		void AcquireJoltRuntime()
		{
			if (s_JoltRuntimeUsers++ != 0) return;
			JPH::RegisterDefaultAllocator();
			JPH::Factory::sInstance = new JPH::Factory();
			JPH::RegisterTypes();
		}

		void ReleaseJoltRuntime()
		{
			if (s_JoltRuntimeUsers == 0) return;
			if (--s_JoltRuntimeUsers != 0) return;
			JPH::UnregisterTypes();
			delete JPH::Factory::sInstance;
			JPH::Factory::sInstance = nullptr;
		}

		JPH::RVec3 ToJoltPosition(const glm::vec3& value)
		{
			return JPH::RVec3(value.x, value.y, value.z);
		}

		JPH::Vec3 ToJoltVec3(const glm::vec3& value)
		{
			return JPH::Vec3(value.x, value.y, value.z);
		}

		JPH::Quat ToJoltQuat(const glm::quat& value)
		{
			return JPH::Quat(value.x, value.y, value.z, value.w);
		}

		// 单精度构建下 RVec3 就是 Vec3;模板统一处理 Vec3 / RVec3(Float3 也用同一读取口)。
		template <typename TVector>
		glm::vec3 ToGlm(const TVector& value)
		{
			return glm::vec3(static_cast<float>(value.GetX()), static_cast<float>(value.GetY()), static_cast<float>(value.GetZ()));
		}

		glm::quat ToGlmQuat(const JPH::Quat& value)
		{
			return glm::quat(value.GetW(), value.GetX(), value.GetY(), value.GetZ());
		}

		JPH::EMotionType ToJoltMotionType(RigidBody3DComponent::MotionType type)
		{
			switch (type)
			{
				case RigidBody3DComponent::MotionType::Static: return JPH::EMotionType::Static;
				case RigidBody3DComponent::MotionType::Kinematic: return JPH::EMotionType::Kinematic;
				case RigidBody3DComponent::MotionType::Dynamic: return JPH::EMotionType::Dynamic;
			}
			return JPH::EMotionType::Static;
		}

		bool ComponentPoseDiffers(const TransformComponent& transform, const JPH::RVec3& position, const JPH::Quat& rotation)
		{
			if (glm::distance(transform.Location, ToGlm(position)) > kLocationEpsilon) return true;
			return std::fabs(glm::dot(transform.RotationQuat, ToGlmQuat(rotation))) < 1.0f - kRotationEpsilon;
		}

		float MaxComponent(const glm::vec3& value)
		{
			return std::max(value.x, std::max(value.y, value.z));
		}

		void ValidatePositiveScale(entt::entity entity, const TransformComponent& transform)
		{
			const glm::vec3 scale = glm::abs(transform.Scale);
			if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z) ||
				scale.x <= 0.0f || scale.y <= 0.0f || scale.z <= 0.0f)
				throw std::logic_error("[Physics3D] entity " + EntityLabel(entity) +
					" has a non-positive or non-finite TransformComponent scale; 3D physics requires a positive scale");
		}

		void ValidateBoxCollider(const entt::registry& registry, entt::entity entity)
		{
			const auto* box = registry.try_get<BoxCollider3DComponent>(entity);
			if (!box) return;
			if (!std::isfinite(box->HalfExtents.x) || !std::isfinite(box->HalfExtents.y) || !std::isfinite(box->HalfExtents.z) ||
				box->HalfExtents.x <= 0.0f || box->HalfExtents.y <= 0.0f || box->HalfExtents.z <= 0.0f)
				throw std::logic_error("[Physics3D] entity " + EntityLabel(entity) +
					" BoxCollider3D HalfExtents must be positive and finite");
		}

		void ValidateSphereCollider(const entt::registry& registry, entt::entity entity)
		{
			const auto* sphere = registry.try_get<SphereCollider3DComponent>(entity);
			if (!sphere) return;
			if (!std::isfinite(sphere->Radius) || sphere->Radius <= 0.0f)
				throw std::logic_error("[Physics3D] entity " + EntityLabel(entity) +
					" SphereCollider3D Radius must be positive and finite");
		}

		void ValidateCapsuleCollider(const entt::registry& registry, entt::entity entity)
		{
			const auto* capsule = registry.try_get<CapsuleCollider3DComponent>(entity);
			if (!capsule) return;
			if (!std::isfinite(capsule->Radius) || capsule->Radius <= 0.0f)
				throw std::logic_error("[Physics3D] entity " + EntityLabel(entity) +
					" CapsuleCollider3D Radius must be positive and finite");
			if (!std::isfinite(capsule->HalfHeight) || capsule->HalfHeight < 0.0f)
				throw std::logic_error("[Physics3D] entity " + EntityLabel(entity) +
					" CapsuleCollider3D HalfHeight must be non-negative and finite");
		}

		std::string ResolveMeshColliderPath(const entt::registry& registry, entt::entity entity, const MeshCollider3DComponent& collider)
		{
			if (!collider.MeshPath.empty()) return collider.MeshPath;
			if (const auto* renderer = registry.try_get<MeshRendererComponent>(entity))
				if (!renderer->MeshPath.empty()) return renderer->MeshPath;
			return {};
		}

		// 收集实体上的全部 3D 碰撞体形状(带局部偏移;偏移不随 Scale 缩放,与 2D 的 Offset 口径一致)。
		void AppendColliderShapes(const entt::registry& registry, entt::entity entity, const TransformComponent& transform,
			std::vector<std::pair<glm::vec3, JPH::RefConst<JPH::ShapeSettings>>>& outShapes)
		{
			const glm::vec3 scale = glm::abs(transform.Scale);

			if (const auto* box = registry.try_get<BoxCollider3DComponent>(entity))
			{
				const glm::vec3 halfExtents = glm::abs(box->HalfExtents) * scale;
				outShapes.emplace_back(box->Offset,
					JPH::RefConst<JPH::ShapeSettings>(new JPH::BoxShapeSettings(ToJoltVec3(halfExtents))));
			}
			if (const auto* sphere = registry.try_get<SphereCollider3DComponent>(entity))
			{
				outShapes.emplace_back(sphere->Offset,
					JPH::RefConst<JPH::ShapeSettings>(new JPH::SphereShapeSettings(sphere->Radius * MaxComponent(scale))));
			}
			if (const auto* capsule = registry.try_get<CapsuleCollider3DComponent>(entity))
			{
				const float uniformScale = MaxComponent(scale);
				outShapes.emplace_back(capsule->Offset,
					JPH::RefConst<JPH::ShapeSettings>(new JPH::CapsuleShapeSettings(capsule->HalfHeight * uniformScale, capsule->Radius * uniformScale)));
			}
			if (const auto* mesh = registry.try_get<MeshCollider3DComponent>(entity))
			{
				const std::string path = ResolveMeshColliderPath(registry, entity, *mesh);
				if (path.empty())
					throw std::logic_error("[Physics3D] entity " + EntityLabel(entity) +
						" MeshCollider3D has no mesh: set MeshPath or add a MeshRendererComponent with a .wmodel path");

				Asset::WModelData model;
				std::string error;
				if (!Asset::WModelIO::ReadFile(path, model, &error))
					throw std::logic_error("[Physics3D] entity " + EntityLabel(entity) +
						" MeshCollider3D failed to read '" + path + "': " + error);

				if (mesh->Mode == MeshCollider3DComponent::ColliderMode::ConvexHull)
				{
					if (model.Vertices.size() < 4)
						throw std::logic_error("[Physics3D] entity " + EntityLabel(entity) +
							" MeshCollider3D convex hull needs at least 4 vertices ('" + path + "')");
					std::vector<JPH::Vec3> points;
					// ConvexHullShapeSettings 的凸包点数上限 256(cMaxPointsInHull);超出按等距抽样并告警。
					const size_t maxPoints = static_cast<size_t>(JPH::ConvexHullShape::cMaxPointsInHull);
					const size_t stride = std::max<size_t>(1u, (model.Vertices.size() + maxPoints - 1) / maxPoints);
					points.reserve(model.Vertices.size() / stride + 1);
					for (size_t index = 0; index < model.Vertices.size(); index += stride)
						points.push_back(ToJoltVec3(model.Vertices[index].Position * scale));
					if (stride > 1)
						WLD_CORE_WARN("[Physics3D] entity {0} convex hull '{1}' has {2} vertices; sampled every {3} to fit the 256 point limit",
							EntityLabel(entity), path, model.Vertices.size(), stride);
					outShapes.emplace_back(glm::vec3(0.0f),
						JPH::RefConst<JPH::ShapeSettings>(new JPH::ConvexHullShapeSettings(points.data(), static_cast<int>(points.size()))));
				}
				else
				{
					JPH::VertexList vertices;
					vertices.reserve(model.Vertices.size());
					for (const Asset::WModelVertex& vertex : model.Vertices)
					{
						const glm::vec3 position = vertex.Position * scale;
						vertices.push_back(JPH::Float3(position.x, position.y, position.z));
					}
					JPH::IndexedTriangleList triangles;
					triangles.reserve(model.Indices.size() / 3);
					for (size_t index = 0; index + 2 < model.Indices.size(); index += 3)
						triangles.push_back(JPH::IndexedTriangle(model.Indices[index], model.Indices[index + 1], model.Indices[index + 2], 0));
					if (triangles.empty())
						throw std::logic_error("[Physics3D] entity " + EntityLabel(entity) +
							" MeshCollider3D StaticTriangles has no triangles ('" + path + "')");
					outShapes.emplace_back(glm::vec3(0.0f), JPH::RefConst<JPH::ShapeSettings>(new JPH::MeshShapeSettings(vertices, triangles)));
				}
			}
		}

		JPH::ShapeSettings::ShapeResult CreateBodyShape(const entt::registry& registry, entt::entity entity,
			const TransformComponent& transform)
		{
			std::vector<std::pair<glm::vec3, JPH::RefConst<JPH::ShapeSettings>>> shapes;
			AppendColliderShapes(registry, entity, transform, shapes);
			if (shapes.size() == 1 && glm::dot(shapes.front().first, shapes.front().first) <= 1e-12f)
				return shapes.front().second->Create();
			if (shapes.empty())
			{
				WLD_CORE_WARN("[Physics3D] entity {0} has a RigidBody3DComponent but no 3D collider; using a default 0.5 half-extent box",
					EntityLabel(entity));
				return JPH::BoxShapeSettings(ToJoltVec3(glm::vec3(0.5f))).Create();
			}

			JPH::Ref<JPH::StaticCompoundShapeSettings> compound = new JPH::StaticCompoundShapeSettings();
			for (const auto& [offset, settings] : shapes)
				compound->AddShape(ToJoltVec3(offset), JPH::Quat::sIdentity(), settings.GetPtr());
			return compound->Create();
		}

		void AppendBoxLines(std::vector<DebugLine>& outLines, const glm::vec3& center, const glm::quat& rotation,
			const glm::vec3& halfExtents, const glm::vec3& offset)
		{
			const glm::vec3 origin = center + rotation * offset;
			glm::vec3 corners[8];
			int corner = 0;
			for (int signX = -1; signX <= 1; signX += 2)
				for (int signY = -1; signY <= 1; signY += 2)
					for (int signZ = -1; signZ <= 1; signZ += 2)
						corners[corner++] = origin + rotation * glm::vec3(static_cast<float>(signX) * halfExtents.x,
							static_cast<float>(signY) * halfExtents.y, static_cast<float>(signZ) * halfExtents.z);

			// 角点编号的 bit0/1/2 = Z/Y/X 的符号位,棱 = 只差一个 bit 的角点对。
			static constexpr int kEdges[12][2] = {
				{ 0, 1 }, { 0, 2 }, { 0, 4 }, { 1, 3 }, { 1, 5 }, { 2, 3 },
				{ 2, 6 }, { 3, 7 }, { 4, 5 }, { 4, 6 }, { 5, 7 }, { 6, 7 },
			};
			for (const auto& edge : kEdges)
				outLines.push_back({ corners[edge[0]], corners[edge[1]] });
		}

		// 画一个圆:法线轴 = 局部 axis(0=X / 1=Y / 2=Z),圆心在 center,平面由 rotation 定向。
		void AppendCircleLines(std::vector<DebugLine>& outLines, const glm::vec3& center, const glm::quat& rotation,
			float radius, int axis)
		{
			glm::vec3 previous { 0.0f };
			for (int segment = 0; segment <= kDebugCircleSegments; ++segment)
			{
				const float angle = glm::two_pi<float>() * static_cast<float>(segment) / static_cast<float>(kDebugCircleSegments);
				glm::vec3 local { 0.0f };
				local[(axis + 1) % 3] = std::cos(angle) * radius;
				local[(axis + 2) % 3] = std::sin(angle) * radius;
				const glm::vec3 point = center + rotation * local;
				if (segment > 0) outLines.push_back({ previous, point });
				previous = point;
			}
		}

		void AppendCapsuleLines(std::vector<DebugLine>& outLines, const glm::vec3& center, const glm::quat& rotation,
			float radius, float halfHeight, const glm::vec3& offset)
		{
			const glm::vec3 origin = center + rotation * offset;
			const glm::vec3 axis = rotation * glm::vec3(0.0f, 1.0f, 0.0f);
			const glm::vec3 top = origin + axis * halfHeight;
			const glm::vec3 bottom = origin - axis * halfHeight;
			AppendCircleLines(outLines, top, rotation, radius, /*axis=*/1);
			AppendCircleLines(outLines, bottom, rotation, radius, /*axis=*/1);
			for (int index = 0; index < 4; ++index)
			{
				const float angle = glm::half_pi<float>() * static_cast<float>(index);
				const glm::vec3 side = rotation * glm::vec3(std::cos(angle) * radius, 0.0f, std::sin(angle) * radius);
				outLines.push_back({ top + side, bottom + side });
			}
		}
	}

	struct Physics3DWorld::Impl
	{
		// 单层 broadphase / object layer 过滤(全部互相碰撞)。
		struct BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
		{
			JPH::uint GetNumBroadPhaseLayers() const override { return 1; }
			JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override { return JPH::BroadPhaseLayer(0); }
		#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
			const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "Default"; }
		#endif
		};

		struct ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
		{
		};

		struct ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
		{
		};

		// 接触监听:新增接触直接用 Body::GetUserData()(实体句柄);移除接触时 body 已被销毁/锁住,
		// 按文档不能读 body,所以查本模块自己的 BodyID → 实体映射。
		class ContactListenerImpl final : public JPH::ContactListener
		{
		public:
			explicit ContactListenerImpl(Impl& impl) : m_Impl(impl) {}

			void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold&, JPH::ContactSettings&) override
			{
				m_Impl.EmitContact(true,
					static_cast<entt::entity>(static_cast<uint32_t>(inBody1.GetUserData())),
					static_cast<entt::entity>(static_cast<uint32_t>(inBody2.GetUserData())));
			}

			void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override
			{
				const auto find = [this](const JPH::BodyID& bodyId)
				{
					const auto it = m_Impl.m_BodyEntities.find(bodyId.GetIndexAndSequenceNumber());
					return it == m_Impl.m_BodyEntities.end() ? entt::null : it->second;
				};
				const entt::entity entityA = find(inSubShapePair.GetBody1ID());
				const entt::entity entityB = find(inSubShapePair.GetBody2ID());
				if (entityA == entt::null || entityB == entt::null) return;
				m_Impl.EmitContact(false, entityA, entityB);
			}

		private:
			Impl& m_Impl;
		};

		explicit Impl(Physics3DWorld& owner) : m_Owner(owner)
		{
			m_System.Init(kMaxBodies, /*inNumBodyMutexes=*/0, kMaxBodyPairs, kMaxContactConstraints,
				m_BroadPhaseLayers, m_ObjectVsBroadPhaseFilter, m_ObjectLayerPairFilter);
			m_System.SetGravity(JPH::Vec3(0.0f, Physics3DWorld::kGravity, 0.0f));
			m_System.SetContactListener(&m_ContactListener);
		}

		void Start(Scene& scene)
		{
			m_Scene = &scene;
			const entt::registry& registry = static_cast<const Scene&>(scene).GetRegistry();
			JPH::BodyInterface& bodyInterface = m_System.GetBodyInterface();
			const auto rigidBodies = registry.view<RigidBody3DComponent>();
			for (const entt::entity entity : rigidBodies)
			{
				const RigidBody3DComponent& rigidBody = rigidBodies.get<RigidBody3DComponent>(entity);
				const TransformComponent* transform = registry.try_get<TransformComponent>(entity);
				if (!transform)
				{
					WLD_CORE_WARN("[Physics3D] entity {0} has a RigidBody3DComponent but no TransformComponent; body skipped",
						EntityLabel(entity));
					continue;
				}

				const JPH::ShapeSettings::ShapeResult shapeResult = CreateBodyShape(registry, entity, *transform);
				if (!shapeResult.IsValid())
					throw std::logic_error("[Physics3D] entity " + EntityLabel(entity) + " shape creation failed: " +
						std::string(shapeResult.GetError().c_str()));

				JPH::BodyCreationSettings settings(shapeResult.Get(), ToJoltPosition(transform->Location),
					ToJoltQuat(transform->RotationQuat), ToJoltMotionType(rigidBody.Type), kObjectLayer);
				settings.mUserData = static_cast<JPH::uint64>(static_cast<uint32_t>(entity));
				settings.mFriction = std::max(0.0f, rigidBody.Friction);
				settings.mRestitution = std::max(0.0f, rigidBody.Restitution);
				settings.mLinearDamping = std::max(0.0f, rigidBody.LinearDamping);
				settings.mAngularDamping = std::max(0.0f, rigidBody.AngularDamping);
				settings.mGravityFactor = rigidBody.UseGravity ? 1.0f : 0.0f;
				if (rigidBody.Type == RigidBody3DComponent::MotionType::Dynamic)
				{
					settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
					settings.mMassPropertiesOverride.mMass = std::max(0.0001f, rigidBody.Mass);
				}

				const JPH::BodyID bodyId = bodyInterface.CreateAndAddBody(settings, JPH::EActivation::Activate);
				if (bodyId.IsInvalid())
					throw std::logic_error("[Physics3D] Jolt rejected the body of entity " + EntityLabel(entity) +
						" (out of bodies or invalid settings)");
				m_Bodies.emplace(entity, bodyId);
				m_BodyEntities.emplace(bodyId.GetIndexAndSequenceNumber(), entity);
			}
		}

		void Stop()
		{
			JPH::BodyInterface& bodyInterface = m_System.GetBodyInterface();
			for (const auto& [entity, bodyId] : m_Bodies)
			{
				if (bodyId.IsInvalid()) continue;
				if (bodyInterface.IsAdded(bodyId)) bodyInterface.RemoveBody(bodyId);
				bodyInterface.DestroyBody(bodyId);
			}
			m_Bodies.clear();
			m_BodyEntities.clear();
			m_Scene = nullptr;
		}

		void Step(float deltaSeconds)
		{
			// 只读遍历必须走 const 重载:活动场景的非 const GetRegistry 会触发结构写断言。
			const entt::registry& registry = static_cast<const Scene&>(*m_Scene).GetRegistry();
			PushKinematicTransforms(registry);
			m_System.Update(deltaSeconds, /*inCollisionSteps=*/1, &m_TempAllocator, &m_JobSystem);
		}

		void SyncTransforms(Scene& scene)
		{
			JPH::BodyInterface& bodyInterface = m_System.GetBodyInterface();
			for (const auto& [entity, bodyId] : m_Bodies)
			{
				if (bodyInterface.GetMotionType(bodyId) == JPH::EMotionType::Static) continue;
				JPH::RVec3 position;
				JPH::Quat rotation;
				bodyInterface.GetPositionAndRotation(bodyId, position, rotation);
				scene.SyncPhysics3DTransform(entity, ToGlm(position), ToGlmQuat(rotation));
			}
		}

		void DestroyBody(entt::entity entity)
		{
			const auto it = m_Bodies.find(entity);
			if (it == m_Bodies.end()) return;
			JPH::BodyInterface& bodyInterface = m_System.GetBodyInterface();
			if (bodyInterface.IsAdded(it->second)) bodyInterface.RemoveBody(it->second);
			m_BodyEntities.erase(it->second.GetIndexAndSequenceNumber());
			bodyInterface.DestroyBody(it->second);
			m_Bodies.erase(it);
		}

		void CollectDebugLines(std::vector<DebugLine>& outLines) const
		{
			outLines.clear();
			const entt::registry& registry = static_cast<const Scene&>(*m_Scene).GetRegistry();
			const JPH::BodyInterface& bodyInterface = m_System.GetBodyInterface();
			for (const auto& [entity, bodyId] : m_Bodies)
			{
				JPH::RVec3 position;
				JPH::Quat rotation;
				bodyInterface.GetPositionAndRotation(bodyId, position, rotation);
				const glm::vec3 center = ToGlm(position);
				const glm::quat orientation = ToGlmQuat(rotation);

				const TransformComponent* transform = registry.try_get<TransformComponent>(entity);
				const glm::vec3 scale = transform ? glm::abs(transform->Scale) : glm::vec3(1.0f);
				if (const auto* box = registry.try_get<BoxCollider3DComponent>(entity))
					AppendBoxLines(outLines, center, orientation, glm::abs(box->HalfExtents) * scale, box->Offset);
				if (const auto* sphere = registry.try_get<SphereCollider3DComponent>(entity))
				{
					const float radius = sphere->Radius * MaxComponent(scale);
					const glm::vec3 origin = center + orientation * sphere->Offset;
					AppendCircleLines(outLines, origin, orientation, radius, 0);
					AppendCircleLines(outLines, origin, orientation, radius, 1);
					AppendCircleLines(outLines, origin, orientation, radius, 2);
				}
				if (const auto* capsule = registry.try_get<CapsuleCollider3DComponent>(entity))
				{
					const float uniformScale = MaxComponent(scale);
					AppendCapsuleLines(outLines, center, orientation, capsule->Radius * uniformScale,
						capsule->HalfHeight * uniformScale, capsule->Offset);
				}
			}
		}

		bool TryGetBodyTransform(entt::entity entity, glm::vec3* outLocation, glm::quat* outRotation) const
		{
			const auto it = m_Bodies.find(entity);
			if (it == m_Bodies.end()) return false;
			JPH::RVec3 position;
			JPH::Quat rotation;
			m_System.GetBodyInterface().GetPositionAndRotation(it->second, position, rotation);
			if (outLocation) *outLocation = ToGlm(position);
			if (outRotation) *outRotation = ToGlmQuat(rotation);
			return true;
		}

		void EmitContact(bool added, entt::entity entityA, entt::entity entityB)
		{
			m_Owner.EmitContact(added, entityA, entityB);
		}

	private:
		// Kinematic 跟随 Transform:位姿与组件不同才推给 Jolt(避免每帧广播相位/唤醒)。
		void PushKinematicTransforms(const entt::registry& registry)
		{
			JPH::BodyInterface& bodyInterface = m_System.GetBodyInterface();
			for (const auto& [entity, bodyId] : m_Bodies)
			{
				const auto* rigidBody = registry.try_get<RigidBody3DComponent>(entity);
				if (!rigidBody || rigidBody->Type != RigidBody3DComponent::MotionType::Kinematic) continue;
				const auto* transform = registry.try_get<TransformComponent>(entity);
				if (!transform) continue;
				JPH::RVec3 position;
				JPH::Quat rotation;
				bodyInterface.GetPositionAndRotation(bodyId, position, rotation);
				if (!ComponentPoseDiffers(*transform, position, rotation)) continue;
				bodyInterface.SetPositionAndRotation(bodyId, ToJoltPosition(transform->Location),
					ToJoltQuat(transform->RotationQuat), JPH::EActivation::Activate);
			}
		}

	public:
		Physics3DWorld& m_Owner;
		JPH::PhysicsSystem m_System;
		JPH::TempAllocatorImpl m_TempAllocator { kTempAllocatorBytes };
		JPH::JobSystemSingleThreaded m_JobSystem { kMaxJobs };
		BroadPhaseLayerInterfaceImpl m_BroadPhaseLayers;
		ObjectVsBroadPhaseLayerFilterImpl m_ObjectVsBroadPhaseFilter;
		ObjectLayerPairFilterImpl m_ObjectLayerPairFilter;
		ContactListenerImpl m_ContactListener { *this };
		Scene* m_Scene = nullptr;
		std::unordered_map<entt::entity, JPH::BodyID> m_Bodies;
		std::unordered_map<uint32_t, entt::entity> m_BodyEntities;
	};

	Physics3DWorld::Physics3DWorld() = default;

	Physics3DWorld::~Physics3DWorld()
	{
		Stop();
	}

	bool Physics3DWorld::ValidateScene(const Scene& scene, std::string* error)
	{
		if (error) error->clear();
		const entt::registry& registry = scene.GetRegistry();

		const auto reject = [error](const std::string& message)
		{
			if (error) *error = message;
			return false;
		};

		// 同一实体同时挂 2D 与 3D 物理组件 → 运行时拒绝(可读错误)。
		const auto conflicts2DAnd3D = [&registry](entt::entity entity)
		{
			const bool has2D = registry.any_of<RigidBody2DComponent, BoxCollider2DComponent, CircleCollider2DComponent>(entity);
			const bool has3D = registry.any_of<RigidBody3DComponent, BoxCollider3DComponent, SphereCollider3DComponent,
				CapsuleCollider3DComponent, MeshCollider3DComponent>(entity);
			return has2D && has3D;
		};
		const auto check3DPools = [&](auto&& visit)
		{
			visit(registry.view<RigidBody3DComponent>());
			visit(registry.view<BoxCollider3DComponent>());
			visit(registry.view<SphereCollider3DComponent>());
			visit(registry.view<CapsuleCollider3DComponent>());
			visit(registry.view<MeshCollider3DComponent>());
		};
		entt::entity conflicting = entt::null;
		check3DPools([&](const auto& view)
		{
			for (const entt::entity entity : view)
				if (conflicting == entt::null && conflicts2DAnd3D(entity))
					conflicting = entity;
		});
		if (conflicting != entt::null)
			return reject("entity " + EntityLabel(conflicting) +
				" mixes 2D and 3D physics components (RigidBody2D/BoxCollider2D/CircleCollider2D vs "
				"RigidBody3D/BoxCollider3D/SphereCollider3D/CapsuleCollider3D/MeshCollider3D); "
				"remove one side before starting the scene");

		const auto meshColliders = registry.view<MeshCollider3DComponent>();
		for (const entt::entity entity : meshColliders)
		{
			const MeshCollider3DComponent& mesh = meshColliders.get<MeshCollider3DComponent>(entity);
			if (mesh.Mode != MeshCollider3DComponent::ColliderMode::StaticTriangles) continue;
			const RigidBody3DComponent* rigidBody = registry.try_get<RigidBody3DComponent>(entity);
			if (!rigidBody || rigidBody->Type != RigidBody3DComponent::MotionType::Static)
				return reject("entity " + EntityLabel(entity) +
					" MeshCollider3D StaticTriangles requires a Static rigid body (Jolt mesh shapes are static-only)");
		}

		try
		{
			const auto rigidBodies = registry.view<RigidBody3DComponent>();
			for (const entt::entity entity : rigidBodies)
			{
				const TransformComponent* transform = registry.try_get<TransformComponent>(entity);
				if (transform) ValidatePositiveScale(entity, *transform);
				ValidateBoxCollider(registry, entity);
				ValidateSphereCollider(registry, entity);
				ValidateCapsuleCollider(registry, entity);
			}
		}
		catch (const std::exception& exception)
		{
			return reject(exception.what());
		}
		return true;
	}

	void Physics3DWorld::Start(Scene& scene)
	{
		if (m_Impl)
			throw std::logic_error("Physics3DWorld::Start requires a stopped world; call Stop() first");

		std::string error;
		if (!ValidateScene(scene, &error))
			throw std::logic_error(error);

		AcquireJoltRuntime();
		try
		{
			m_Impl = std::make_unique<Impl>(*this);
			m_Impl->Start(scene);
		}
		catch (...)
		{
			if (m_Impl) m_Impl->Stop();
			m_Impl.reset();
			ReleaseJoltRuntime();
			throw;
		}
	}

	void Physics3DWorld::Stop()
	{
		if (!m_Impl) return;
		m_Impl->Stop();
		m_Impl.reset();
		ReleaseJoltRuntime();
	}

	void Physics3DWorld::RequireStarted(const char* operation) const
	{
		if (!m_Impl)
			throw std::logic_error(std::string("Physics3DWorld::") + operation +
				" requires a running 3D physics world; call Start(Scene&) first");
	}

	void Physics3DWorld::Step(float fixedDeltaSeconds)
	{
		RequireStarted("Step");
		if (!(fixedDeltaSeconds > 0.0f)) return;
		m_Impl->Step(fixedDeltaSeconds);
	}

	void Physics3DWorld::SyncTransforms()
	{
		RequireStarted("SyncTransforms");
		m_Impl->SyncTransforms(*m_Impl->m_Scene);
	}

	void Physics3DWorld::SetContactCallback(std::function<void(bool added, entt::entity entityA, entt::entity entityB)> callback)
	{
		m_ContactCallback = std::move(callback);
	}

	void Physics3DWorld::EmitContact(bool added, entt::entity entityA, entt::entity entityB) const
	{
		if (!m_ContactCallback) return;
		try
		{
			m_ContactCallback(added, entityA, entityB);
		}
		catch (const std::exception& exception)
		{
			WLD_CORE_ERROR("[Physics3D] contact callback failed: {0}", exception.what());
		}
		catch (...)
		{
			WLD_CORE_ERROR("[Physics3D] contact callback failed: unknown exception");
		}
	}

	void Physics3DWorld::CollectDebugLines(std::vector<DebugLine>& outLines) const
	{
		RequireStarted("CollectDebugLines");
		m_Impl->CollectDebugLines(outLines);
	}

	void Physics3DWorld::DestroyBody(entt::entity entity)
	{
		if (!m_Impl) return;
		m_Impl->DestroyBody(entity);
	}

	bool Physics3DWorld::TryGetBodyTransform(entt::entity entity, glm::vec3* outLocation, glm::quat* outRotation) const
	{
		if (!m_Impl) return false;
		return m_Impl->TryGetBodyTransform(entity, outLocation, outRotation);
	}
}
