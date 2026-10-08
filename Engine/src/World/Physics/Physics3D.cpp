#include "wldpch.h"
#include "World/Core/StringPool.h"
#include "World/Physics/Physics3D.h"

#include "World/Asset/WModelIO.h"
#include "World/Physics/PhysicsJobSystem.h"
#include "World/Physics/PhysicsSettings.h"
#include "World/Core/Log.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Physics/PhysicsEvents.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"

// D6 硬约束:Jolt 头文件只允许出现在 3D 物理模块内部 —— 本 TU 与 PhysicsJobSystem.{h,cpp}
// (JOBSYS:引擎 JobSystem 适配器必须继承 JPH::JobSystemWithBarrier)。Physics3D.h / Scene.h
// 仍然都不 include Jolt。
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/MotionQuality.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Geometry/IndexedTriangle.h>
#include <Jolt/Math/Float3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace World
{
	namespace
	{
		// P5:per-body (Layer,Mask) 碰撞过滤。Jolt 的 ObjectLayerPairFilter 只拿得到 ObjectLayer
		// 序号,所以按"(Layer,Mask) 唯一组合 ↔ ObjectLayer 序号"的注册表实现(见 Impl::RegisterPair):
		//     ShouldCollide(a, b) = (MaskA & LayerB) != 0 && (MaskB & LayerA) != 0
		// broadphase 层只做分片与命名(ObjectLayer % kBroadPhaseLayerCount);真正决定"能不能碰"
		// 的永远是 ObjectLayerPairFilter,所以 ObjectVsBroadPhaseLayerFilter 必须保守(见下)。
		constexpr JPH::uint kBroadPhaseLayerCount = 8;
		constexpr JPH::uint kMaxBodies = 4096;
		constexpr JPH::uint kMaxBodyPairs = 2048;
		constexpr JPH::uint kMaxContactConstraints = 2048;
		constexpr JPH::uint kTempAllocatorBytes = 8u * 1024u * 1024u;
		constexpr JPH::uint kMaxJobs = 1024;
		// JOBSYS:PhysicsJobSystem 的 barrier 容量(与 Jolt 物理样例的 8 同量级;
		// PhysicsSystem::Update 一次只用其中一个)。
		constexpr uint32_t kMaxPhysicsBarriers = 8;

		constexpr float kLocationEpsilon = 1e-4f;
		constexpr float kRotationEpsilon = 1e-4f;
		constexpr int kDebugCircleSegments = 24;

		std::string EntityLabel(entt::entity entity)
		{
			return std::to_string(static_cast<uint32_t>(entity));
		}

		// P5:实体句柄 ↔ Jolt 刚体 userData 的既有约定(body userData = 实体索引本体,不做 ±1 偏移;
		// Jolt 用 uint64 userData,不存在 2D 侧 PackEntityUserData 那种 nullptr 歧义)。
		entt::entity EntityFromBodyUserData(const JPH::Body& body)
		{
			return static_cast<entt::entity>(static_cast<uint32_t>(body.GetUserData()));
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
			return std::fabs(glm::dot(transform.Rotation, ToGlmQuat(rotation))) < 1.0f - kRotationEpsilon;
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

		PathId ResolveMeshColliderMesh(const entt::registry& registry, entt::entity entity, const MeshCollider3DComponent& collider)
		{
			if (collider.Mesh.HasPath()) return collider.Mesh.Path;
			if (const auto* renderer = registry.try_get<MeshRendererComponent>(entity))
				if (renderer->Mesh.HasPath()) return renderer->Mesh.Path;
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
				const std::string path = StringPool::Get().PathOf(ResolveMeshColliderMesh(registry, entity, *mesh));
				if (path.empty())
					throw std::logic_error("[Physics3D] entity " + EntityLabel(entity) +
						" MeshCollider3D has no mesh: set Mesh or add a MeshRendererComponent with a .wmodel asset");

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
		// P5:(Layer,Mask) 唯一组合 ↔ ObjectLayer 序号。Jolt 的 filter 只看到序号,所以过滤语义
		// 必须在"注册表 + filter"里自己实现(语义 = (MaskA & LayerB) != 0 && (MaskB & LayerA) != 0)。
		struct PairEntry
		{
			uint32_t Layer;
			uint32_t Mask;
		};

		// broadphase 层 = ObjectLayer % kBroadPhaseLayerCount:只为分片与调试命名,不承担过滤语义。
		struct BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
		{
			JPH::uint GetNumBroadPhaseLayers() const override { return kBroadPhaseLayerCount; }
			JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inObjectLayer) const override
			{
				return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(inObjectLayer % kBroadPhaseLayerCount));
			}
		#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
			const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inBroadPhaseLayer) const override
			{
				static const char* const kNames[kBroadPhaseLayerCount] = {
					"ObjectShard0", "ObjectShard1", "ObjectShard2", "ObjectShard3",
					"ObjectShard4", "ObjectShard5", "ObjectShard6", "ObjectShard7",
				};
				const JPH::BroadPhaseLayer::Type index = static_cast<JPH::BroadPhaseLayer::Type>(inBroadPhaseLayer);
				return index < kBroadPhaseLayerCount ? kNames[index] : "InvalidObjectShard";
			}
		#endif
		};

		// Jolt 的 FindCollidingPairs 只对"通过 ObjectVsBroadPhaseLayerFilter 的 broadphase 层"发起查询,
		// 所以这里必须保守:只要该层里存在任一能跟 inObjectLayer 配对的注册组合就返回 true,
		// 精确判定交给 ObjectLayerPairFilter —— 否则 (MaskA & LayerB) 允许但分片序号不同的配对会被漏掉。
		struct ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
		{
			explicit ObjectVsBroadPhaseLayerFilterImpl(const std::vector<PairEntry>* entries) : m_Entries(entries) {}

			bool ShouldCollide(JPH::ObjectLayer inObjectLayer, JPH::BroadPhaseLayer inBroadPhaseLayer) const override
			{
				if (inObjectLayer >= m_Entries->size()) return false;
				const PairEntry& layer = (*m_Entries)[inObjectLayer];
				const size_t shard = static_cast<size_t>(static_cast<JPH::BroadPhaseLayer::Type>(inBroadPhaseLayer));
				for (size_t index = shard; index < m_Entries->size(); index += kBroadPhaseLayerCount)
					if (((*m_Entries)[index].Mask & layer.Layer) != 0)
						return true;
				return false;
			}

			const std::vector<PairEntry>* m_Entries = nullptr;
		};

		// 精确语义:(MaskA & LayerB) != 0 && (MaskB & LayerA) != 0(与 2D Box2D 口径一致)。
		struct ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
		{
			explicit ObjectLayerPairFilterImpl(const std::vector<PairEntry>* entries) : m_Entries(entries) {}

			bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::ObjectLayer inLayer2) const override
			{
				if (inLayer1 >= m_Entries->size() || inLayer2 >= m_Entries->size()) return false;
				const PairEntry& entry1 = (*m_Entries)[inLayer1];
				const PairEntry& entry2 = (*m_Entries)[inLayer2];
				return (entry1.Mask & entry2.Layer) != 0 && (entry2.Mask & entry1.Layer) != 0;
			}

			const std::vector<PairEntry>* m_Entries = nullptr;
		};

		// 接触监听:Jolt 的三个回调(Added / Persisted / Removed)都接。
		//  * Added/Persisted 能读 body 与流形 ⇒ 解析实体 + 几何量;
		//  * Removed 只有 SubShapeIDPair(拿不到流形,且 Jolt 文档禁止读 body)⇒ 查本模块的
		//    BodyID → 实体 与 BodyID → IsSensor 副本,几何量一律为零;
		//  * 回调发生在 PhysicsSystem::Update 内部(不能改物理状态,也不能直接入场景队列),
		//    所以只推进 pending 缓冲,Step 里 Update 返回后按序 flush(顺序确定 ⇒ 可复现)。
		class ContactListenerImpl final : public JPH::ContactListener
		{
		public:
			explicit ContactListenerImpl(Impl& impl) : m_Impl(impl) {}

			void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2,
				const JPH::ContactManifold& inManifold, JPH::ContactSettings&) override
			{
				m_Impl.PushContactEvent(inBody1, inBody2, inManifold, Physics::ContactPhase::Begin);
			}

			void OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2,
				const JPH::ContactManifold& inManifold, JPH::ContactSettings&) override
			{
				m_Impl.PushContactEvent(inBody1, inBody2, inManifold, Physics::ContactPhase::Persist);
			}

			void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override
			{
				m_Impl.PushContactEnd(inSubShapePair);
			}

		private:
			Impl& m_Impl;
		};

		// gravityY 由 Physics3DWorld::Start(Scene&) 解析(P4-U4):场景头 `World.physics_gravity`
		// 优先,未覆盖才落到项目清单 `physics.gravity`,最后才是引擎常量。
		explicit Impl(Physics3DWorld& owner, float gravityY) : m_Owner(owner)
		{
			m_System.Init(kMaxBodies, /*inNumBodyMutexes=*/0, kMaxBodyPairs, kMaxContactConstraints,
				m_BroadPhaseLayers, m_ObjectVsBroadPhaseFilter, m_ObjectLayerPairFilter);
			m_System.SetGravity(JPH::Vec3(0.0f, gravityY, 0.0f));
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

				// P5:per-body 过滤 —— (Layer,Mask) 唯一组合注册成 ObjectLayer;传感器是刚体级开关,
				// 用 BodyCreationSettings::mIsSensor 一次性带上(不额外调 BodyInterface::SetIsSensor)。
				JPH::BodyCreationSettings settings(shapeResult.Get(), ToJoltPosition(transform->Location),
					ToJoltQuat(transform->Rotation), ToJoltMotionType(rigidBody.Type),
					RegisterPair(rigidBody.Layer, rigidBody.Mask));
				settings.mUserData = static_cast<JPH::uint64>(static_cast<uint32_t>(entity));
				settings.mIsSensor = rigidBody.IsSensor;
				// P7 CCD:RigidBody3DComponent::Ccd ⇒ Jolt 运动质量档(Discrete / LinearCast)。
				settings.mMotionQuality = rigidBody.Ccd ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
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
				m_BodySensors.emplace(bodyId.GetIndexAndSequenceNumber(), rigidBody.IsSensor);
			}

			// P7:EnableCollision == false 的关节要禁用两体碰撞。Jolt 没有每约束的 collide-connected
			// 开关,只能用 GroupFilterTable(见 CreateJoint)。子组数 = 刚体数;真正参与"禁碰"关节的
			// 刚体在 CreateJoint 时才惰性分配唯一 SubGroupID(总数不会超过这里的子组数)。
			m_JointSubGroups.clear();
			m_FreeJointSubGroups.clear();
			m_NextJointSubGroup = 0;
			m_JointSubGroupCapacity = static_cast<JPH::uint>(m_Bodies.size());
			m_JointCollisionFilter = new JPH::GroupFilterTable(m_JointSubGroupCapacity);
		}

		void Stop()
		{
			// 先解全部约束:既清掉悬垂 Constraint*,也避免移除仍被约束引用的刚体(Jolt 不允许)。
			DestroyAllJoints();
			m_JointCollisionFilter = nullptr;
			JPH::BodyInterface& bodyInterface = m_System.GetBodyInterface();
			for (const auto& [entity, bodyId] : m_Bodies)
			{
				if (bodyId.IsInvalid()) continue;
				if (bodyInterface.IsAdded(bodyId)) bodyInterface.RemoveBody(bodyId);
				bodyInterface.DestroyBody(bodyId);
			}
			m_Bodies.clear();
			m_BodyEntities.clear();
			m_BodySensors.clear();
			m_JointSubGroups.clear();
			m_JointRecords.clear();
			m_PendingContacts.clear();
			m_PendingTriggers.clear();
			// JOBSYS:世界停下就释放多线程适配器(Jolt 的 barrier 此时都已被归还)。
			m_JobSystemMulti.reset();
			m_Scene = nullptr;
		}

		// JOBSYS:每步按项目清单的 physics.multithreaded 选 job system。默认(false)恒为单线程 ——
		// 行为与改动前逐字节相同;打开后还要 IsUsable()(引擎 JobSystem 在跑)才真的走多线程,
		// 否则同样回退单线程。
		JPH::JobSystem* SelectJobSystem()
		{
			if (!PhysicsSettings::Get().Multithreaded)
				return &m_JobSystem;
			// 全局串行 A/B 开关:WLD_NO_PARALLEL=1 时连物理也走单线程 ⇒ 可用来做
			// "并行结果 == 串行结果"的逐字节对照。
			if (!JobSystem::ParallelAllowed())
				return &m_JobSystem;
			// 只在该开关首次为真时建一次(barrier 数组不小),之后每步只做可用性判断。
			if (!m_JobSystemMulti)
				m_JobSystemMulti = std::make_unique<PhysicsJobSystem>(kMaxPhysicsBarriers);
			if (m_JobSystemMulti->IsUsable())
				return m_JobSystemMulti.get();
			return &m_JobSystem;
		}

		// 与 SelectJobSystem() 同口径的只读查询(测试用:证明 MT 分支真的被选到)。
		bool IsMultithreadedSelected() const
		{
			return m_JobSystemMulti != nullptr && m_JobSystemMulti->IsUsable() &&
				PhysicsSettings::Get().Multithreaded;
		}

		void Step(float deltaSeconds)
		{
			// 只读遍历必须走 const 重载:活动场景的非 const GetRegistry 会触发结构写断言。
			const entt::registry& registry = static_cast<const Scene&>(*m_Scene).GetRegistry();
			PushKinematicTransforms(registry);
			// 上一次 Update 抛异常时可能留下半截事件;先清空,保证 flush 的永远是本次 Update 的事实。
			m_PendingContacts.clear();
			m_PendingTriggers.clear();
			m_System.Update(deltaSeconds, /*inCollisionSteps=*/1, &m_TempAllocator, SelectJobSystem());
			// 回调只推进 pending 缓冲;Update 返回后在这里按序 flush(= Jolt 回调顺序 ⇒ 可复现)。
			FlushEvents();
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
			// 先解该实体参与的约束,否则 Jolt 不允许移除仍被约束引用的刚体。
			DestroyJoints(entity);
			const auto it = m_Bodies.find(entity);
			if (it == m_Bodies.end()) return;
			JPH::BodyInterface& bodyInterface = m_System.GetBodyInterface();
			if (bodyInterface.IsAdded(it->second)) bodyInterface.RemoveBody(it->second);
			m_BodyEntities.erase(it->second.GetIndexAndSequenceNumber());
			m_BodySensors.erase(it->second.GetIndexAndSequenceNumber());
			bodyInterface.DestroyBody(it->second);
			m_Bodies.erase(it);
		}

		void CreateJoint(entt::entity owner, entt::entity other, int kind,
			const glm::vec3& anchorSelf, const glm::vec3& anchorOther, const glm::vec3& axis,
			float minDistance, float maxDistance, bool enableCollision)
		{
			if (owner == other)
				throw std::logic_error("Physics3DWorld::CreateJoint requires two different entities");

			const auto ownerIt = m_Bodies.find(owner);
			if (ownerIt == m_Bodies.end())
				throw std::logic_error("Physics3DWorld::CreateJoint: entity " + EntityLabel(owner) +
					" has no 3D rigid body in this world");
			const auto otherIt = m_Bodies.find(other);
			if (otherIt == m_Bodies.end())
				throw std::logic_error("Physics3DWorld::CreateJoint: entity " + EntityLabel(other) +
					" has no 3D rigid body in this world");

			JPH::Body* body1 = m_System.GetBodyLockInterfaceNoLock().TryGetBody(ownerIt->second);
			JPH::Body* body2 = m_System.GetBodyLockInterfaceNoLock().TryGetBody(otherIt->second);
			if (body1 == nullptr || body2 == nullptr)
				throw std::logic_error("Physics3DWorld::CreateJoint: Jolt body lookup failed");

			JPH::RVec3 position1;
			JPH::RVec3 position2;
			JPH::Quat rotation1;
			JPH::Quat rotation2;
			JPH::BodyInterface& bodyInterface = m_System.GetBodyInterface();
			bodyInterface.GetPositionAndRotation(ownerIt->second, position1, rotation1);
			bodyInterface.GetPositionAndRotation(otherIt->second, position2, rotation2);

			// 局部 → 世界:锚点/轴按 JointComponent 约定写在实体局部空间,而 Jolt 在
			// EConstraintSpace::WorldSpace 下要世界量,所以用刚体当前世界位姿(权威)换算:
			//   worldPoint = bodyPosition + bodyRotation * localPoint
			// 参考系也用**各自**刚体的世界旋转来旋转同一个局部系 ⇒ 相对姿态被记进约束(不会出现
			// "创建即对齐/弹跳")。
			const JPH::RVec3 worldPoint1 = position1 + rotation1 * ToJoltVec3(anchorSelf);
			const JPH::RVec3 worldPoint2 = position2 + rotation2 * ToJoltVec3(anchorOther);

			JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
			switch (kind)
			{
			case 0: // Fixed
			{
				JPH::FixedConstraintSettings* fixed = new JPH::FixedConstraintSettings();
				fixed->mSpace = JPH::EConstraintSpace::WorldSpace;
				fixed->mAutoDetectPoint = false;
				fixed->mPoint1 = worldPoint1;
				fixed->mPoint2 = worldPoint2;
				fixed->mAxisX1 = rotation1 * JPH::Vec3::sAxisX();
				fixed->mAxisY1 = rotation1 * JPH::Vec3::sAxisY();
				fixed->mAxisX2 = rotation2 * JPH::Vec3::sAxisX();
				fixed->mAxisY2 = rotation2 * JPH::Vec3::sAxisY();
				settings = fixed;
				break;
			}
			case 1: // Distance
			{
				JPH::DistanceConstraintSettings* distance = new JPH::DistanceConstraintSettings();
				distance->mSpace = JPH::EConstraintSpace::WorldSpace;
				distance->mPoint1 = worldPoint1;
				distance->mPoint2 = worldPoint2;
				// < 0 原样传:Jolt 用两锚点实际距离(仅 WorldSpace 生效),不自己猜数值。
				distance->mMinDistance = minDistance;
				distance->mMaxDistance = maxDistance;
				settings = distance;
				break;
			}
			case 2: // Hinge
			{
				const JPH::Vec3 axisLocal = ToJoltVec3(axis);
				if (axisLocal.LengthSq() < 1e-12f)
					throw std::logic_error("Physics3DWorld::CreateJoint: hinge axis must be non-zero");
				// 与轴垂直的法线轴(局部空间);轴接近 X 时退到 Y,避免 cross 退化。
				const JPH::Vec3 helper = std::abs(axisLocal.GetX()) < 0.9f ? JPH::Vec3::sAxisX() : JPH::Vec3::sAxisY();
				const JPH::Vec3 normalLocal = axisLocal.Cross(helper).Normalized();

				JPH::HingeConstraintSettings* hinge = new JPH::HingeConstraintSettings();
				hinge->mSpace = JPH::EConstraintSpace::WorldSpace;
				hinge->mPoint1 = worldPoint1;
				hinge->mPoint2 = worldPoint2;
				hinge->mHingeAxis1 = (rotation1 * axisLocal).Normalized();
				hinge->mHingeAxis2 = (rotation2 * axisLocal).Normalized();
				hinge->mNormalAxis1 = (rotation1 * normalLocal).Normalized();
				hinge->mNormalAxis2 = (rotation2 * normalLocal).Normalized();
				settings = hinge;
				break;
			}
			default:
				throw std::logic_error("Physics3DWorld::CreateJoint: unknown joint kind " + std::to_string(kind));
			}

			JPH::TwoBodyConstraint* constraint = settings->Create(*body1, *body2);
			if (constraint == nullptr)
				throw std::logic_error("Physics3DWorld::CreateJoint: Jolt failed to create the constraint");
			// 所有权:AddConstraint 把约束存进 Array<Ref<Constraint>>(AddRef);记录里再持一份 Ref。
			// 销毁时 RemoveConstraint + 释放记录的 Ref ⇒ 引用计数归零自动 delete,不手动 delete。
			m_System.AddConstraint(constraint);

			JointRecord record;
			record.Owner = owner;
			record.Other = other;
			record.Constraint = constraint;

			if (!enableCollision)
			{
				// Jolt 没有每约束的 collide-connected 开关(ConstraintSettings::mEnabled 只是
				// "约束是否生效");禁用两体碰撞要用 GroupFilterTable(见 EnsureJointSubGroup)。
				const JPH::CollisionGroup::SubGroupID subOwner = EnsureJointSubGroup(owner);
				const JPH::CollisionGroup::SubGroupID subOther = EnsureJointSubGroup(other);
				if (m_JointCollisionFilter != nullptr
					&& subOwner != JPH::CollisionGroup::cInvalidSubGroup
					&& subOther != JPH::CollisionGroup::cInvalidSubGroup)
				{
					m_JointCollisionFilter->DisableCollision(subOwner, subOther);
					record.CollisionDisabled = true;
				}
			}

			m_JointRecords.push_back(std::move(record));
		}

		void DestroyJoints(entt::entity entity)
		{
			for (auto it = m_JointRecords.begin(); it != m_JointRecords.end(); )
			{
				if (it->Owner != entity && it->Other != entity) { ++it; continue; }
				ReleaseJointRecord(*it);
				it = m_JointRecords.erase(it);
			}
			ResetUnusedJointCollisionGroups();
		}

		void DestroyAllJoints()
		{
			for (JointRecord& record : m_JointRecords) ReleaseJointRecord(record);
			m_JointRecords.clear();
			ResetUnusedJointCollisionGroups();
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

		bool GetBodyMotionQualityIsLinearCast(entt::entity entity) const
		{
			const auto found = m_Bodies.find(entity);
			if (found == m_Bodies.end()) return false;
			return m_System.GetBodyInterface().GetMotionQuality(found->second) == JPH::EMotionQuality::LinearCast;
		}

		// P5:运行时改 Layer/Mask ⇒ 重映射 ObjectLayer 并应用(否则过滤静默不生效)。
		void RefreshBodyFilter(entt::entity entity)
		{
			const auto found = m_Bodies.find(entity);
			if (found == m_Bodies.end())
				return;
			if (m_Scene == nullptr)
				return;
			const entt::registry& registry = static_cast<const Scene&>(*m_Scene).GetRegistry();
			const auto* rigidBody = registry.try_get<RigidBody3DComponent>(entity);
			if (rigidBody == nullptr)
				return;
			const JPH::ObjectLayer target = RegisterPair(rigidBody->Layer, rigidBody->Mask);
			JPH::BodyInterface& bodyInterface = m_System.GetBodyInterface();
			if (bodyInterface.GetObjectLayer(found->second) == target)
				return;
			// 注意:改层会让 Jolt 丢弃该刚体的现有接触缓存 —— 这正是"改过滤立即生效"的语义
			// (旧接触不再满足新掩码),与 Box2D 侧 b2Shape_SetFilter 同口径。
			bodyInterface.SetObjectLayer(found->second, target);
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

		// P5:(Layer,Mask) 唯一组合 ↔ ObjectLayer 序号(组合键 = (layer << 32) | mask)。
		// 注册表在 Impl 构造时就已存在(地址稳定);filter 持有它的指针,所以 PhysicsSystem::Init
		// 之后新增的组合也照样被看到 —— Init 在 ctor 里,body 直到 Start 才逐个注册。
		JPH::ObjectLayer RegisterPair(uint32_t layer, uint32_t mask)
		{
			const uint64_t key = (static_cast<uint64_t>(layer) << 32) | static_cast<uint64_t>(mask);
			const auto found = m_PairToObjectLayer.find(key);
			if (found != m_PairToObjectLayer.end()) return found->second;
			if (m_PairEntries.size() >= static_cast<size_t>(JPH::cObjectLayerInvalid))
			{
				// JPH_OBJECT_LAYER_BITS == 16 时 ObjectLayer 只有 65535 个;真实场景远达不到。
				WLD_CORE_WARN("[Physics3D] more than 65535 distinct (Layer, Mask) pairs; falling back to object layer 0");
				return 0;
			}
			const JPH::ObjectLayer objectLayer = static_cast<JPH::ObjectLayer>(m_PairEntries.size());
			m_PairEntries.push_back(PairEntry { layer, mask });
			m_PairToObjectLayer.emplace(key, objectLayer);
			return objectLayer;
		}

		entt::entity FindEntity(const JPH::BodyID& bodyId) const
		{
			const auto it = m_BodyEntities.find(bodyId.GetIndexAndSequenceNumber());
			return it == m_BodyEntities.end() ? entt::null : it->second;
		}

		bool IsSensorBody(const JPH::BodyID& bodyId) const
		{
			const auto it = m_BodySensors.find(bodyId.GetIndexAndSequenceNumber());
			return it != m_BodySensors.end() && it->second;
		}

		// Jolt 接触回调只把事实推进 pending 缓冲(回调顺序 = 事件顺序);Step 里 Update 返回后 flush。
		static uint8_t ResolveSubShapeColliderIndex(const JPH::Body& body, const JPH::SubShapeID& subShapeID)
		{
			const JPH::Shape* shape = body.GetShape();
			if (shape && shape->GetType() == JPH::EShapeType::Compound)
			{
				const auto* compound = static_cast<const JPH::CompoundShape*>(shape);
				JPH::SubShapeID remainder;
				return static_cast<uint8_t>(compound->GetSubShapeIndexFromID(subShapeID, remainder));
			}
			return 0;
		}

		void PushContactEvent(const JPH::Body& inBody1, const JPH::Body& inBody2,
			const JPH::ContactManifold& inManifold, Physics::ContactPhase phase)
		{
			const entt::entity entityA = EntityFromBodyUserData(inBody1);
			const entt::entity entityB = EntityFromBodyUserData(inBody2);

			if (inBody1.IsSensor() || inBody2.IsSensor())
			{
				Physics::TriggerEvent event;
				event.SensorEntity = inBody1.IsSensor() ? entityA : entityB;
				event.OtherEntity = inBody1.IsSensor() ? entityB : entityA;
				event.Phase = phase;
				for (const auto& existing : m_PendingTriggers)
				{
					if (existing.SensorEntity == event.SensorEntity &&
						existing.OtherEntity == event.OtherEntity &&
						existing.Phase == phase)
						return;
				}
				m_PendingTriggers.push_back(event);
				return;
			}

			const uint8_t colliderIdxA = ResolveSubShapeColliderIndex(inBody1, inManifold.mSubShapeID1);
			const uint8_t colliderIdxB = ResolveSubShapeColliderIndex(inBody2, inManifold.mSubShapeID2);
			const uint32_t subShapeIdA = inManifold.mSubShapeID1.GetValue();
			const uint32_t subShapeIdB = inManifold.mSubShapeID2.GetValue();
			const glm::vec3 worldNormal = ToGlm(inManifold.mWorldSpaceNormal);

			// 方案 A 实体对去重 + 方案 C 接触点明细聚合
			auto it = std::find_if(m_PendingContacts.begin(), m_PendingContacts.end(),
				[&](const Physics::ContactEvent& existing)
				{
					return existing.Phase == phase &&
						((existing.EntityA == entityA && existing.EntityB == entityB) ||
						 (existing.EntityA == entityB && existing.EntityB == entityA));
				});

			if (it != m_PendingContacts.end())
			{
				Physics::ContactEvent& existing = *it;
				const bool isSameDirection = (existing.EntityA == entityA);

				for (std::size_t i = 0; i < inManifold.mRelativeContactPointsOn1.size() && existing.PointCount < Physics::kMaxContactPoints; ++i)
				{
					Physics::ContactPoint cp;
					cp.Position = ToGlm(inManifold.GetWorldSpaceContactPointOn1(static_cast<JPH::uint>(i)));
					cp.Normal = isSameDirection ? worldNormal : -worldNormal;
					cp.PenetrationDepth = inManifold.mPenetrationDepth;
					cp.ColliderIndexA = isSameDirection ? colliderIdxA : colliderIdxB;
					cp.ColliderIndexB = isSameDirection ? colliderIdxB : colliderIdxA;
					cp.SubShapeIdA = isSameDirection ? subShapeIdA : subShapeIdB;
					cp.SubShapeIdB = isSameDirection ? subShapeIdB : subShapeIdA;
					existing.Points[existing.PointCount++] = cp;
				}

				if (inManifold.mPenetrationDepth > existing.PenetrationDepth)
				{
					existing.PenetrationDepth = inManifold.mPenetrationDepth;
					existing.Normal = isSameDirection ? worldNormal : -worldNormal;
					if (!inManifold.mRelativeContactPointsOn1.empty())
						existing.Point = ToGlm(inManifold.GetWorldSpaceContactPointOn1(0));
				}
				return;
			}

			Physics::ContactEvent event;
			event.EntityA = entityA;
			event.EntityB = entityB;
			event.Phase = phase;
			event.Normal = worldNormal;
			event.PenetrationDepth = inManifold.mPenetrationDepth;
			event.PointCount = 0;

			for (std::size_t i = 0; i < inManifold.mRelativeContactPointsOn1.size() && event.PointCount < Physics::kMaxContactPoints; ++i)
			{
				Physics::ContactPoint cp;
				cp.Position = ToGlm(inManifold.GetWorldSpaceContactPointOn1(static_cast<JPH::uint>(i)));
				cp.Normal = worldNormal;
				cp.PenetrationDepth = inManifold.mPenetrationDepth;
				cp.ColliderIndexA = colliderIdxA;
				cp.ColliderIndexB = colliderIdxB;
				cp.SubShapeIdA = subShapeIdA;
				cp.SubShapeIdB = subShapeIdB;
				event.Points[event.PointCount++] = cp;
			}
			if (event.PointCount > 0)
				event.Point = event.Points[0].Position;

			m_PendingContacts.push_back(event);
		}

		void PushContactEnd(const JPH::SubShapeIDPair& inSubShapePair)
		{
			const entt::entity entityA = FindEntity(inSubShapePair.GetBody1ID());
			const entt::entity entityB = FindEntity(inSubShapePair.GetBody2ID());
			if (entityA == entt::null || entityB == entt::null) return;

			const bool sensorA = IsSensorBody(inSubShapePair.GetBody1ID());
			if (sensorA || IsSensorBody(inSubShapePair.GetBody2ID()))
			{
				Physics::TriggerEvent event;
				event.SensorEntity = sensorA ? entityA : entityB;
				event.OtherEntity = sensorA ? entityB : entityA;
				event.Phase = Physics::ContactPhase::End;
				for (const auto& existing : m_PendingTriggers)
				{
					if (existing.SensorEntity == event.SensorEntity &&
						existing.OtherEntity == event.OtherEntity &&
						existing.Phase == Physics::ContactPhase::End)
						return;
				}
				m_PendingTriggers.push_back(event);
				return;
			}

			auto it = std::find_if(m_PendingContacts.begin(), m_PendingContacts.end(),
				[&](const Physics::ContactEvent& existing)
				{
					return existing.Phase == Physics::ContactPhase::End &&
						((existing.EntityA == entityA && existing.EntityB == entityB) ||
						 (existing.EntityA == entityB && existing.EntityB == entityA));
				});
			if (it != m_PendingContacts.end())
				return;

			Physics::ContactEvent event;
			event.EntityA = entityA;
			event.EntityB = entityB;
			event.Phase = Physics::ContactPhase::End;
			m_PendingContacts.push_back(event);
		}

		void FlushEvents()
		{
			for (const Physics::ContactEvent& event : m_PendingContacts) m_Owner.EmitContactEvent(event);
			for (const Physics::TriggerEvent& event : m_PendingTriggers) m_Owner.EmitTriggerEvent(event);
			m_PendingContacts.clear();
			m_PendingTriggers.clear();
		}

	private:
		// P7:一条关节的 Jolt 句柄 + 该书否禁用了两体碰撞。Constraint 由 PhysicsSystem 以
		// Array<Ref<Constraint>> 持有(AddConstraint 会 AddRef),这里再持一份 ⇒ 释放记录的 Ref
		// 后引用计数归零自动 delete(约束是 RefTarget,不手动 delete)。
		struct JointRecord
		{
			entt::entity Owner = entt::null;
			entt::entity Other = entt::null;
			JPH::Ref<JPH::Constraint> Constraint;
			bool CollisionDisabled = false;
		};

		// P7:给参与"禁碰"关节的刚体惰性分配唯一 SubGroupID,并挂进世界共享的 GroupFilterTable
		// (GroupID 固定 0,不同 entity 靠 SubGroupID 区分)。号优先从空位表复用,否则用递增计数器;
		// 上界 = Start 时的刚体数(表容量),因此永远不会访问表外行。
		JPH::CollisionGroup::SubGroupID EnsureJointSubGroup(entt::entity entity)
		{
			if (m_JointCollisionFilter == nullptr) return JPH::CollisionGroup::cInvalidSubGroup;

			auto found = m_JointSubGroups.find(entity);
			if (found == m_JointSubGroups.end())
			{
				JPH::CollisionGroup::SubGroupID sub = JPH::CollisionGroup::cInvalidSubGroup;
				if (!m_FreeJointSubGroups.empty())
				{
					sub = m_FreeJointSubGroups.back();
					m_FreeJointSubGroups.pop_back();
				}
				else if (m_NextJointSubGroup < m_JointSubGroupCapacity)
				{
					sub = static_cast<JPH::CollisionGroup::SubGroupID>(m_NextJointSubGroup++);
				}
				if (sub == JPH::CollisionGroup::cInvalidSubGroup)
				{
					WLD_CORE_WARN("[Physics3D] joint collision subgroup table is full ({0} bodies); EnableCollision=false is ignored for entity {1}",
						m_JointSubGroupCapacity, EntityLabel(entity));
					return sub;
				}
				found = m_JointSubGroups.emplace(entity, sub).first;
			}

			const auto bodyIt = m_Bodies.find(entity);
			if (bodyIt == m_Bodies.end()) return JPH::CollisionGroup::cInvalidSubGroup;
			m_System.GetBodyInterface().SetCollisionGroup(bodyIt->second,
				JPH::CollisionGroup(m_JointCollisionFilter.GetPtr(), /*inGroupID=*/0, found->second));
			return found->second;
		}

		// 同一对实体上还有没有**别的**关节仍禁用碰撞(重复关节时不能提前恢复)。
		bool IsCollisionStillDisabled(entt::entity owner, entt::entity other, const JointRecord* except) const
		{
			for (const JointRecord& record : m_JointRecords)
			{
				if (&record == except || !record.CollisionDisabled) continue;
				if ((record.Owner == owner && record.Other == other) || (record.Owner == other && record.Other == owner))
					return true;
			}
			return false;
		}

		void ReleaseJointRecord(JointRecord& record)
		{
			if (record.CollisionDisabled && m_JointCollisionFilter != nullptr
				&& !IsCollisionStillDisabled(record.Owner, record.Other, &record))
			{
				const auto subOwner = m_JointSubGroups.find(record.Owner);
				const auto subOther = m_JointSubGroups.find(record.Other);
				if (subOwner != m_JointSubGroups.end() && subOther != m_JointSubGroups.end())
					m_JointCollisionFilter->EnableCollision(subOwner->second, subOther->second);
			}
			if (record.Constraint != nullptr)
				m_System.RemoveConstraint(record.Constraint);
			record.Constraint = nullptr;
			record.CollisionDisabled = false;
		}

		// 不再参与任何关节的刚体恢复默认碰撞组(无过滤 ⇒ 与其它刚体正常碰撞),并回收子组号。
		void ResetUnusedJointCollisionGroups()
		{
			JPH::BodyInterface& bodyInterface = m_System.GetBodyInterface();
			for (auto it = m_JointSubGroups.begin(); it != m_JointSubGroups.end(); )
			{
				bool used = false;
				for (const JointRecord& record : m_JointRecords)
					if (record.Owner == it->first || record.Other == it->first) { used = true; break; }
				if (used) { ++it; continue; }
				const auto bodyIt = m_Bodies.find(it->first);
				if (bodyIt != m_Bodies.end())
					bodyInterface.SetCollisionGroup(bodyIt->second, JPH::CollisionGroup());
				m_FreeJointSubGroups.push_back(it->second);
				it = m_JointSubGroups.erase(it);
			}
		}

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
					ToJoltQuat(transform->Rotation), JPH::EActivation::Activate);
			}
		}

	public:
		Physics3DWorld& m_Owner;
		// P5 过滤注册表:必须在两个 filter 成员之前声明(filter 初始化时取它的地址)。
		std::vector<PairEntry> m_PairEntries;
		std::unordered_map<uint64_t, JPH::ObjectLayer> m_PairToObjectLayer;
		JPH::PhysicsSystem m_System;
		JPH::TempAllocatorImpl m_TempAllocator { kTempAllocatorBytes };
		JPH::JobSystemSingleThreaded m_JobSystem { kMaxJobs };
		// JOBSYS:physics.multithreaded = true 且引擎 JobSystem 可用时才建(懒建一次)。
		// 默认恒为空 ⇒ 始终走 m_JobSystem,行为与今天逐字节相同。
		std::unique_ptr<PhysicsJobSystem> m_JobSystemMulti;
		BroadPhaseLayerInterfaceImpl m_BroadPhaseLayers;
		ObjectVsBroadPhaseLayerFilterImpl m_ObjectVsBroadPhaseFilter { &m_PairEntries };
		ObjectLayerPairFilterImpl m_ObjectLayerPairFilter { &m_PairEntries };
		ContactListenerImpl m_ContactListener { *this };
		Scene* m_Scene = nullptr;
		std::unordered_map<entt::entity, JPH::BodyID> m_Bodies;
		std::unordered_map<uint32_t, entt::entity> m_BodyEntities;
		std::unordered_map<uint32_t, bool> m_BodySensors;
		std::vector<Physics::ContactEvent> m_PendingContacts;
		std::vector<Physics::TriggerEvent> m_PendingTriggers;
		std::vector<JointRecord> m_JointRecords;
		// entity → GroupFilterTable 的 SubGroupID(只给"有 EnableCollision == false 关节"的刚体分配)。
		std::unordered_map<entt::entity, JPH::CollisionGroup::SubGroupID> m_JointSubGroups;
		JPH::Ref<JPH::GroupFilterTable> m_JointCollisionFilter;
		// 子组号分配器:优先复用已释放的号,否则用递增计数器;上界 = Start 时的刚体数(表容量)。
		std::vector<JPH::CollisionGroup::SubGroupID> m_FreeJointSubGroups;
		JPH::uint m_JointSubGroupCapacity = 0;
		JPH::uint m_NextJointSubGroup = 0;
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

		// P7:两个实体互相引用(A.Connected == B 且 B.Connected == A)会重复建同一关节 —— 配置错误,
		// 拒绝启动;单向引用是正常用法。
		const auto jointComponents = registry.view<JointComponent>();
		for (const entt::entity jointEntity : jointComponents)
		{
			const JointComponent& joint = jointComponents.get<JointComponent>(jointEntity);
			if (joint.Connected == entt::null || joint.Connected == jointEntity) continue;
			const JointComponent* other = registry.try_get<JointComponent>(joint.Connected);
			if (other != nullptr && other->Connected == jointEntity)
				return reject("entities " + EntityLabel(jointEntity) + " and " + EntityLabel(joint.Connected) +
					" mutually reference each other via JointComponent.Connected; author the joint on exactly one "
					"of the two bodies (mutual references would create the same constraint twice)");
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
			// P4-U4:重力解析顺序 = 场景覆盖 > 项目清单 > 引擎常量。
			// 非法值(非有限)一律往下一级退化,保证"清单/场景头写坏"时行为可预测。
			float gravity = PhysicsSettings::Get().Gravity;
			if (!std::isfinite(gravity))
			{
				WLD_CORE_WARN("Physics3D: physics.gravity 非有限({0}),退回默认 {1}",
					gravity, Physics3DWorld::kGravity);
				gravity = Physics3DWorld::kGravity;
			}
			const float sceneOverride = scene.GetWorldSettings().Gravity;
			if (std::isfinite(sceneOverride))
				gravity = sceneOverride;
			m_Impl = std::make_unique<Impl>(*this, gravity);
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

	bool Physics3DWorld::IsUsingMultithreadedJobSystem() const
	{
		return m_Impl != nullptr && m_Impl->IsMultithreadedSelected();
	}

	void Physics3DWorld::SetContactCallback(std::function<void(const Physics::ContactEvent&)> callback)
	{
		m_ContactCallback = std::move(callback);
	}

	void Physics3DWorld::SetTriggerCallback(std::function<void(const Physics::TriggerEvent&)> callback)
	{
		m_TriggerCallback = std::move(callback);
	}

	void Physics3DWorld::EmitContactEvent(const Physics::ContactEvent& event) const
	{
		if (!m_ContactCallback) return;
		try
		{
			m_ContactCallback(event);
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

	void Physics3DWorld::EmitTriggerEvent(const Physics::TriggerEvent& event) const
	{
		if (!m_TriggerCallback) return;
		try
		{
			m_TriggerCallback(event);
		}
		catch (const std::exception& exception)
		{
			WLD_CORE_ERROR("[Physics3D] trigger callback failed: {0}", exception.what());
		}
		catch (...)
		{
			WLD_CORE_ERROR("[Physics3D] trigger callback failed: unknown exception");
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

	void Physics3DWorld::RefreshBodyFilter(entt::entity entity)
	{
		if (!m_Impl) return;
		m_Impl->RefreshBodyFilter(entity);
	}

	void Physics3DWorld::CreateJoint(entt::entity owner, entt::entity other, int kind,
		const glm::vec3& anchorSelf, const glm::vec3& anchorOther, const glm::vec3& axis,
		float minDistance, float maxDistance, bool enableCollision)
	{
		RequireStarted("CreateJoint");
		m_Impl->CreateJoint(owner, other, kind, anchorSelf, anchorOther, axis, minDistance, maxDistance, enableCollision);
	}

	void Physics3DWorld::DestroyJoints(entt::entity entity)
	{
		if (!m_Impl) return;
		m_Impl->DestroyJoints(entity);
	}

	void Physics3DWorld::DestroyAllJoints()
	{
		if (!m_Impl) return;
		m_Impl->DestroyAllJoints();
	}

	bool Physics3DWorld::TryGetBodyTransform(entt::entity entity, glm::vec3* outLocation, glm::quat* outRotation) const
	{
		if (!m_Impl) return false;
		return m_Impl->TryGetBodyTransform(entity, outLocation, outRotation);
	}

	bool Physics3DWorld::GetBodyMotionQualityIsLinearCast(entt::entity entity) const
	{
		if (!m_Impl) return false;
		return m_Impl->GetBodyMotionQualityIsLinearCast(entity);
	}
}
