#include "wldpch.h"

#include "World/Animation/IkSystem.h"

#include "World/Core/StringPool.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Terrain/Terrain.h"
#include "World/Terrain/TerrainRenderer.h"
#include "World/Utils/Paths.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace World
{
	namespace
	{
		namespace fs = std::filesystem;

		bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool IsFinite(const glm::quat& value)
		{
			return std::isfinite(value.w) && std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool IsFinite(const glm::mat4& value)
		{
			for (int column = 0; column < 4; ++column)
				for (int row = 0; row < 4; ++row)
					if (!std::isfinite(value[column][row]))
						return false;
			return true;
		}

		glm::quat SafeNormalize(const glm::quat& value)
		{
			const float length = glm::length(value);
			if (!std::isfinite(length) || length <= 1e-8f)
				return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			return value / length;
		}

		glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to)
		{
			const float fromLength = glm::length(from);
			const float toLength = glm::length(to);
			if (fromLength <= 1e-8f || toLength <= 1e-8f)
				return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			const glm::vec3 a = from / fromLength;
			const glm::vec3 b = to / toLength;
			const float dot = glm::clamp(glm::dot(a, b), -1.0f, 1.0f);
			if (dot >= 1.0f - 1e-6f)
				return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			if (dot <= -1.0f + 1e-6f)
			{
				glm::vec3 axis = glm::cross(a, glm::vec3(1.0f, 0.0f, 0.0f));
				if (glm::length(axis) < 1e-4f)
					axis = glm::cross(a, glm::vec3(0.0f, 0.0f, 1.0f));
				return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
			}
			const glm::vec3 axis = glm::cross(a, b);
			return glm::normalize(glm::quat(1.0f + dot, axis.x, axis.y, axis.z));
		}

		glm::quat MatrixRotation(const glm::mat4& matrix)
		{
			glm::mat3 rotation(matrix);
			for (int column = 0; column < 3; ++column)
			{
				const float length = glm::length(rotation[column]);
				if (length > 1e-8f)
					rotation[column] /= length;
			}
			return SafeNormalize(glm::quat_cast(rotation));
		}

		std::vector<glm::mat4> ComputeNodeWorld(const Asset::WModelData& model, const AnimPose& pose)
		{
			const size_t nodeCount = model.Nodes.size();
			std::vector<glm::mat4> world(nodeCount, glm::mat4(1.0f));
			std::vector<uint8_t> state(nodeCount, 0);
			const auto localMatrix = [&](size_t index)
			{
				const JointTransform& joint = (index < pose.Joints.size()) ? pose.Joints[index] : JointTransform{};
				return glm::translate(glm::mat4(1.0f), joint.Translation)
					* glm::mat4_cast(joint.Rotation)
					* glm::scale(glm::mat4(1.0f), joint.Scale);
			};
			std::function<glm::mat4(size_t)> resolve = [&](size_t index) -> glm::mat4
			{
				if (state[index] == 2)
					return world[index];
				if (state[index] == 1)
					return glm::mat4(1.0f);
				state[index] = 1;
				glm::mat4 parentWorld(1.0f);
				const int32_t parent = model.Nodes[index].Parent;
				if (parent >= 0 && static_cast<size_t>(parent) < nodeCount && static_cast<size_t>(parent) != index)
					parentWorld = resolve(static_cast<size_t>(parent));
				world[index] = parentWorld * localMatrix(index);
				state[index] = 2;
				return world[index];
			};
			for (size_t i = 0; i < nodeCount; ++i)
				resolve(i);
			return world;
		}

		// 旧 AnimGraphSystem::FindNode 的别名表（脚步示例保持逐项兼容）。
		int32_t FindNode(const Asset::WModelData& model, std::initializer_list<const char*> names)
		{
			for (const char* name : names)
			{
				for (size_t i = 0; i < model.Nodes.size(); ++i)
					if (model.Nodes[i].Name == name)
						return static_cast<int32_t>(i);
			}
			return -1;
		}

		int32_t FindBoneNode(const Asset::WModelData& model, NameId id)
		{
			if (!id.IsValid())
				return -1;
			const std::string& name = StringPool::Get().NameOf(id);
			if (name.empty())
				return -1;
			for (size_t i = 0; i < model.Nodes.size(); ++i)
				if (model.Nodes[i].Name == name)
					return static_cast<int32_t>(i);
			return -1;
		}

		void WarnOnce(const std::string& key, const std::string& message)
		{
			static std::unordered_set<std::string> warned;
			if (warned.insert(key).second)
				WLD_CORE_WARN("{0}", message);
		}

		bool SolveTwoBoneImpl(const Asset::WModelData& model, AnimPose& pose,
			const IkChainRequest& request, const glm::vec3* endNormalModel)
		{
			const int32_t root = request.Root;
			const int32_t mid = request.Mid;
			const int32_t end = request.End;
			if (root < 0 || mid < 0 || end < 0)
				return false;
			if (static_cast<size_t>(root) >= pose.Joints.size()
				|| static_cast<size_t>(mid) >= pose.Joints.size()
				|| static_cast<size_t>(end) >= pose.Joints.size()
				|| static_cast<size_t>(root) >= model.Nodes.size()
				|| static_cast<size_t>(mid) >= model.Nodes.size()
				|| static_cast<size_t>(end) >= model.Nodes.size())
				return false;

			if (model.Nodes[static_cast<size_t>(mid)].Parent != root
				|| model.Nodes[static_cast<size_t>(end)].Parent != mid)
			{
				WarnOnce("ik-chain:" + model.Nodes[static_cast<size_t>(root)].Name
						+ "/" + model.Nodes[static_cast<size_t>(mid)].Name
						+ "/" + model.Nodes[static_cast<size_t>(end)].Name,
					"IK 需要 Root→Mid→End 直接父子链，已跳过该链");
				return false;
			}

			const float weight = std::isfinite(request.Weight) ? std::clamp(request.Weight, 0.0f, 1.0f) : 0.0f;
			if (weight <= 0.0f)
				return false;

			const std::vector<glm::mat4> world = ComputeNodeWorld(model, pose);
			const glm::vec3 rootPos = glm::vec3(world[static_cast<size_t>(root)][3]);
			const glm::vec3 midPos = glm::vec3(world[static_cast<size_t>(mid)][3]);
			const glm::vec3 endPos = glm::vec3(world[static_cast<size_t>(end)][3]);

			const float length1 = glm::distance(rootPos, midPos);
			const float length2 = glm::distance(midPos, endPos);
			if (!std::isfinite(length1) || !std::isfinite(length2) || length1 <= 1e-4f || length2 <= 1e-4f)
				return false;

			const glm::vec3 toTarget = request.TargetModel - rootPos;
			const float targetDistance = glm::length(toTarget);
			if (!std::isfinite(targetDistance) || targetDistance <= 1e-5f)
				return false;

			// 超出可达范围 → 钳到最大伸展（不产生 NaN）。
			const float maxReach = length1 + length2 - 1e-4f;
			const float reach = std::clamp(targetDistance, 1e-3f, std::max(1e-3f, maxReach));
			const glm::vec3 direction = toTarget / targetDistance;

			// 极向：投影到链轴垂直平面；退化时回退 +Z / +X（与旧 SolveLeg 一致）。
			glm::vec3 pole = IsFinite(request.PoleDirectionModel) ? request.PoleDirectionModel : glm::vec3(0.0f);
			pole -= direction * glm::dot(pole, direction);
			if (glm::length(pole) < 1e-4f)
			{
				pole = glm::vec3(0.0f, 0.0f, 1.0f) - direction * direction.z;
				if (glm::length(pole) < 1e-4f)
					pole = glm::vec3(1.0f, 0.0f, 0.0f);
			}
			if (glm::length(pole) < 1e-6f)
				return false;
			const glm::vec3 kneeDir = glm::normalize(pole);

			const float cosAlpha = std::clamp(
				(length1 * length1 + reach * reach - length2 * length2) / (2.0f * length1 * reach), -1.0f, 1.0f);
			const float sinAlpha = std::sqrt(std::max(0.0f, 1.0f - cosAlpha * cosAlpha));
			const glm::vec3 kneePos = rootPos + direction * (length1 * cosAlpha) + kneeDir * (length1 * sinAlpha);

			const glm::quat parentFix = RotationBetween(midPos - rootPos, kneePos - rootPos);
			const glm::quat rootWorldOld = MatrixRotation(world[static_cast<size_t>(root)]);
			const glm::quat midWorldOld = MatrixRotation(world[static_cast<size_t>(mid)]);
			const glm::quat endWorldOld = MatrixRotation(world[static_cast<size_t>(end)]);

			const glm::quat rootWorldNew = SafeNormalize(parentFix * rootWorldOld);
			const glm::vec3 endAfterParent = kneePos + parentFix * (endPos - midPos);
			glm::quat midWorldFixed = SafeNormalize(parentFix * midWorldOld);
			const glm::vec3 toTargetFromMid = request.TargetModel - kneePos;
			glm::quat midWorldNew = midWorldFixed;
			if (glm::length(toTargetFromMid) > 1e-5f && glm::length(endAfterParent - kneePos) > 1e-5f)
			{
				const glm::quat midFix = RotationBetween(endAfterParent - kneePos, toTargetFromMid);
				midWorldNew = SafeNormalize(midFix * midWorldFixed);
			}

			// 末端朝向：调用方给了地面法线（脚步示例）就对齐法线；通用链保持原世界朝向。
			glm::quat endAlign(1.0f, 0.0f, 0.0f, 0.0f);
			if (endNormalModel && IsFinite(*endNormalModel) && glm::length(*endNormalModel) > 1e-5f)
				endAlign = RotationBetween(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(*endNormalModel));
			const glm::quat endWorldNew = SafeNormalize(endAlign * midWorldNew * glm::inverse(midWorldFixed) * endWorldOld);

			const int32_t rootParent = model.Nodes[static_cast<size_t>(root)].Parent;
			const glm::quat rootParentWorld = (rootParent >= 0 && static_cast<size_t>(rootParent) < world.size())
				? MatrixRotation(world[static_cast<size_t>(rootParent)])
				: glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

			const glm::quat rootLocalNew = SafeNormalize(glm::inverse(rootParentWorld) * rootWorldNew);
			const glm::quat midLocalNew = SafeNormalize(glm::inverse(rootWorldNew) * midWorldNew);
			const glm::quat endLocalNew = SafeNormalize(glm::inverse(midWorldNew) * endWorldNew);

			if (!IsFinite(rootLocalNew) || !IsFinite(midLocalNew) || !IsFinite(endLocalNew))
				return false;

			pose.Joints[static_cast<size_t>(root)].Rotation =
				SafeNormalize(glm::slerp(pose.Joints[static_cast<size_t>(root)].Rotation, rootLocalNew, weight));
			pose.Joints[static_cast<size_t>(mid)].Rotation =
				SafeNormalize(glm::slerp(pose.Joints[static_cast<size_t>(mid)].Rotation, midLocalNew, weight));
			pose.Joints[static_cast<size_t>(end)].Rotation =
				SafeNormalize(glm::slerp(pose.Joints[static_cast<size_t>(end)].Rotation, endLocalNew, weight));
			return true;
		}

		std::string StripEnginePrefix(std::string path)
		{
			std::string normalized = StringPool::NormalizePath(path);
			if (normalized.rfind("engine/", 0) == 0)
				normalized.erase(0, 7);
			return normalized;
		}

		bool ResolveTerrainFile(const AssetRef& asset, fs::path& outPath)
		{
			if (!asset.HasPath())
				return false;
			const std::string& logical = StringPool::Get().PathOf(asset.Path);
			if (logical.empty())
				return false;

			std::error_code error;
			if (fs::is_regular_file(logical, error))
			{
				outPath = logical;
				return true;
			}
			const fs::path candidates[] = {
				Paths::AssetRoot() / logical,
				Paths::EngineContentRoot() / logical,
				Paths::EngineContentRoot() / StripEnginePrefix(logical),
			};
			for (const fs::path& candidate : candidates)
			{
				if (candidate.empty())
					continue;
				if (fs::is_regular_file(candidate, error))
				{
					outPath = candidate;
					return true;
				}
			}
			return false;
		}

		glm::mat4 EntityWorldMatrix(const entt::registry& registry, entt::entity entity)
		{
			if (const WorldTransformComponent* world = registry.try_get<WorldTransformComponent>(entity))
				return world->Matrix;
			if (const TransformComponent* transform = registry.try_get<TransformComponent>(entity))
				return transform->GetLocalMatrix();
			return glm::mat4(1.0f);
		}

		uint64_t EntityUuid(const entt::registry& registry, entt::entity entity)
		{
			if (const UUIDComponent* identity = registry.try_get<UUIDComponent>(entity))
				return static_cast<uint64_t>(identity->ID);
			return 0;
		}

		entt::entity FindEntityByUuid(const entt::registry& registry, uint64_t uuid)
		{
			if (uuid == 0)
				return entt::null;
			for (const entt::entity entity : registry.view<UUIDComponent>())
			{
				const UUIDComponent* identity = registry.try_get<UUIDComponent>(entity);
				if (identity && static_cast<uint64_t>(identity->ID) == uuid)
					return entity;
			}
			return entt::null;
		}

		struct TerrainBinding
		{
			const TerrainData* Data = nullptr;
			entt::entity Entity = entt::null;
			glm::mat4 WorldMatrix { 1.0f };
		};

		TerrainBinding FindSceneTerrain(const entt::registry& registry)
		{
			TerrainBinding binding;
			for (const entt::entity entity : registry.view<TerrainComponent>())
			{
				const TerrainComponent* component = registry.try_get<TerrainComponent>(entity);
				if (!component)
					continue;
				fs::path resolved;
				if (!ResolveTerrainFile(component->TerrainAsset, resolved))
					continue;
				const std::shared_ptr<TerrainData> data =
					TerrainRenderer::GetOrCreateTerrainData(component->TerrainAsset,
						component->CellSize, component->HeightScale);
				if (!data || data->GetWidth() <= 1 || data->GetHeight() <= 1)
					continue;
				binding.Data = data.get();
				binding.Entity = entity;
				binding.WorldMatrix = EntityWorldMatrix(registry, entity);
				return binding;
			}
			return binding;
		}

		struct CachedChain
		{
			entt::entity Self = entt::null;
			uint64_t OwnerUuid = 0;
			TwoBoneIkComponent Component;
		};

		struct FrameState
		{
			const Scene* Owner = nullptr;
			std::vector<CachedChain> Chains;
			TerrainBinding Terrain;
		};

		FrameState& State()
		{
			static FrameState state;
			return state;
		}

		void ApplyGenericChains(const entt::registry& registry, entt::entity owner, uint64_t ownerUuid,
			const Asset::WModelData& model, const glm::mat4& inverseCharacter, AnimPose& pose)
		{
			for (const CachedChain& cached : State().Chains)
			{
				const TwoBoneIkComponent& component = cached.Component;
				const bool matchesOwner = component.OwnerUuid != 0
					? (ownerUuid != 0 && component.OwnerUuid == ownerUuid)
					: cached.Self == owner;
				if (!matchesOwner)
					continue;
				if (component.Enabled == 0 || !(component.Weight > 0.0f))
					continue;

				const int32_t root = FindBoneNode(model, component.RootBone);
				const int32_t mid = FindBoneNode(model, component.MidBone);
				const int32_t end = FindBoneNode(model, component.EndBone);
				if (root < 0 || mid < 0 || end < 0)
				{
					WarnOnce("ik-bones:" + std::to_string(component.OwnerUuid),
						"TwoBoneIkComponent 的 Root/Mid/End 骨骼名在模型中不存在，已跳过该链");
					continue;
				}

				glm::vec3 worldTarget = component.TargetPosition;
				if (component.TargetUuid != 0)
				{
					const entt::entity target = FindEntityByUuid(registry, component.TargetUuid);
					if (target != entt::null)
						worldTarget = glm::vec3(EntityWorldMatrix(registry, target)[3]);
					else
						WarnOnce("ik-target:" + std::to_string(component.TargetUuid),
							"TwoBoneIkComponent 的 TargetUuid 在场景中不存在，回退到 TargetPosition");
				}
				if (!IsFinite(worldTarget))
					continue;

				const glm::vec3 targetModel = glm::vec3(inverseCharacter * glm::vec4(worldTarget, 1.0f));
				const std::vector<glm::mat4> world = ComputeNodeWorld(model, pose);
				const glm::vec3 rootPos = glm::vec3(world[static_cast<size_t>(root)][3]);
				const glm::vec3 midPos = glm::vec3(world[static_cast<size_t>(mid)][3]);
				const glm::vec3 chainAxis = targetModel - rootPos;
				glm::vec3 pole = midPos - rootPos;
				if (std::isfinite(component.PoleAngleDeg) && std::fabs(component.PoleAngleDeg) > 1e-6f
					&& glm::length(chainAxis) > 1e-5f)
				{
					pole = glm::angleAxis(glm::radians(component.PoleAngleDeg), glm::normalize(chainAxis)) * pole;
				}

				IkChainRequest request;
				request.Root = root;
				request.Mid = mid;
				request.End = end;
				request.TargetModel = targetModel;
				request.PoleDirectionModel = pole;
				request.Weight = std::clamp(component.Weight, 0.0f, 1.0f);
				SolveTwoBoneImpl(model, pose, request, nullptr);
			}
		}

		void ApplyFootExample(const entt::registry& registry, entt::entity owner,
			const Asset::WModelData& model, const glm::mat4& characterWorld, AnimPose& pose, bool grounded)
		{
			const TerrainBinding& terrainBinding = State().Terrain;
			if (!terrainBinding.Data || !grounded)
				return;

			const FootIkComponent* footComponent = registry.try_get<FootIkComponent>(owner);
			const AnimGraphComponent* graphComponent = registry.try_get<AnimGraphComponent>(owner);
			float weight = 0.0f;
			float footOffset = 0.0f;
			float maxPelvisDrop = std::numeric_limits<float>::max();

			if (footComponent)
			{
				// 有 FootIkComponent 时以示例组件为准（即使它被禁用，也不回退到旧字段）。
				if (footComponent->Enabled == 0 || !(footComponent->Weight > 0.0f))
					return;
				weight = std::clamp(footComponent->Weight, 0.0f, 1.0f);
				footOffset = std::isfinite(footComponent->FootOffset) ? footComponent->FootOffset : 0.0f;
				maxPelvisDrop = (std::isfinite(footComponent->MaxPelvisDrop) && footComponent->MaxPelvisDrop >= 0.0f)
					? footComponent->MaxPelvisDrop
					: std::numeric_limits<float>::max();
			}
			else
			{
				// 兼容路径：旧 AnimGraphComponent.EnableFootIk/FootIkWeight（行为与旧 ApplyFootIk 一致）。
				if (!graphComponent || graphComponent->EnableFootIk == 0 || !(graphComponent->FootIkWeight > 0.0f))
					return;
				weight = std::clamp(graphComponent->FootIkWeight, 0.0f, 1.0f);
			}
			if (weight <= 0.0f)
				return;

			const int32_t hipsIndex = FindNode(model, { "Hips", "Root" });
			const int32_t thighLeft = FindNode(model, { "Thigh.L", "Thigh_L", "LeftUpLeg" });
			const int32_t thighRight = FindNode(model, { "Thigh.R", "Thigh_R", "RightUpLeg" });
			const int32_t shinLeft = FindNode(model, { "Shin.L", "Shin_L", "LeftLeg" });
			const int32_t shinRight = FindNode(model, { "Shin.R", "Shin_R", "RightLeg" });
			const int32_t footLeft = FindNode(model, { "Foot.L", "Foot_L", "LeftFoot" });
			const int32_t footRight = FindNode(model, { "Foot.R", "Foot_R", "RightFoot" });
			if (thighLeft < 0 || thighRight < 0 || shinLeft < 0 || shinRight < 0 || footLeft < 0 || footRight < 0)
				return;

			const glm::mat4 inverseCharacter = glm::inverse(characterWorld);
			const glm::mat4 inverseTerrain = glm::inverse(terrainBinding.WorldMatrix);
			if (!IsFinite(inverseCharacter) || !IsFinite(inverseTerrain))
				return;

			// 绑定姿态里脚骨的离地高度（模型空间），用于把脚骨送到“地面 + 踝高”。
			AnimPose bindPose;
			bindPose.Joints.resize(model.Nodes.size());
			for (size_t i = 0; i < model.Nodes.size(); ++i)
			{
				bindPose.Joints[i].Translation = model.Nodes[i].Translation;
				bindPose.Joints[i].Rotation = SafeNormalize(model.Nodes[i].Rotation);
				bindPose.Joints[i].Scale = model.Nodes[i].Scale;
			}
			const std::vector<glm::mat4> bindWorld = ComputeNodeWorld(model, bindPose);
			const float leftRestHeight = glm::vec3(bindWorld[static_cast<size_t>(footLeft)][3]).y;
			const float rightRestHeight = glm::vec3(bindWorld[static_cast<size_t>(footRight)][3]).y;

			std::vector<glm::mat4> world = ComputeNodeWorld(model, pose);
			struct FootTarget
			{
				glm::vec3 ModelTarget { 0.0f };
				glm::vec3 ModelNormal { 0.0f, 1.0f, 0.0f };
				float CurrentY = 0.0f;
			};
			// TerrainData::SampleHeight/SampleNormal 的 x/z 是地形本地网格坐标：
			// 先把脚的世界位置换算到地形实体本地空间，采样后再换回世界空间。
			const glm::mat3 terrainNormalMatrix = glm::mat3(glm::transpose(inverseTerrain));
			const glm::mat3 characterNormalMatrix = glm::mat3(glm::transpose(inverseCharacter));
			const float characterScaleY = glm::length(glm::vec3(characterWorld[1]));

			const auto makeTarget = [&](int32_t footIndex, float restHeight) -> FootTarget
			{
				FootTarget target;
				const glm::vec3 footModel = glm::vec3(world[static_cast<size_t>(footIndex)][3]);
				const glm::vec3 footWorld = glm::vec3(characterWorld * glm::vec4(footModel, 1.0f));

				const glm::vec3 footTerrainLocal = glm::vec3(inverseTerrain * glm::vec4(footWorld, 1.0f));
				const float groundLocalY = terrainBinding.Data->SampleHeight(footTerrainLocal.x, footTerrainLocal.z);
				const glm::vec3 groundWorld = glm::vec3(terrainBinding.WorldMatrix
					* glm::vec4(footTerrainLocal.x, groundLocalY, footTerrainLocal.z, 1.0f));
				const glm::vec3 normalTerrainLocal = terrainBinding.Data->SampleNormal(footTerrainLocal.x, footTerrainLocal.z);
				glm::vec3 normalWorld = terrainNormalMatrix * normalTerrainLocal;
				normalWorld = (glm::length(normalWorld) > 1e-5f)
					? glm::normalize(normalWorld) : glm::vec3(0.0f, 1.0f, 0.0f);

				const glm::vec3 targetWorld(groundWorld.x,
					groundWorld.y + restHeight * characterScaleY + footOffset, groundWorld.z);
				target.ModelTarget = glm::vec3(inverseCharacter * glm::vec4(targetWorld, 1.0f));
				const glm::vec3 normalModel = characterNormalMatrix * normalWorld;
				target.ModelNormal = (glm::length(normalModel) > 1e-5f)
					? glm::normalize(normalModel) : glm::vec3(0.0f, 1.0f, 0.0f);
				target.CurrentY = footModel.y;
				return target;
			};

			FootTarget leftTarget = makeTarget(footLeft, leftRestHeight);
			FootTarget rightTarget = makeTarget(footRight, rightRestHeight);

			// 骨盆下沉：以两脚最低目标为准（只向下，不向上）；示例组件可限制最大下沉量。
			float pelvisDrop = std::min(0.0f, std::min(leftTarget.ModelTarget.y - leftTarget.CurrentY,
				rightTarget.ModelTarget.y - rightTarget.CurrentY) * weight);
			if (std::isfinite(maxPelvisDrop) && pelvisDrop < -maxPelvisDrop)
				pelvisDrop = -maxPelvisDrop;
			if (hipsIndex >= 0 && static_cast<size_t>(hipsIndex) < pose.Joints.size()
				&& std::isfinite(pelvisDrop) && std::fabs(pelvisDrop) > 1e-8f)
			{
				pose.Joints[static_cast<size_t>(hipsIndex)].Translation.y += pelvisDrop;
				world = ComputeNodeWorld(model, pose);
				leftTarget = makeTarget(footLeft, leftRestHeight);
				rightTarget = makeTarget(footRight, rightRestHeight);
			}

			const auto solveLeg = [&](int32_t thigh, int32_t shin, int32_t foot, const FootTarget& target)
			{
				const glm::vec3 rootPos = glm::vec3(world[static_cast<size_t>(thigh)][3]);
				const glm::vec3 midPos = glm::vec3(world[static_cast<size_t>(shin)][3]);
				IkChainRequest request;
				request.Root = thigh;
				request.Mid = shin;
				request.End = foot;
				request.TargetModel = target.ModelTarget;
				request.PoleDirectionModel = midPos - rootPos;
				request.Weight = weight;
				SolveTwoBoneImpl(model, pose, request, &target.ModelNormal);
			};
			solveLeg(thighLeft, shinLeft, footLeft, leftTarget);
			solveLeg(thighRight, shinRight, footRight, rightTarget);
		}
	}

	void IkSystem::BeginFrame(Scene& scene)
	{
		FrameState& state = State();
		state = FrameState{};
		state.Owner = &scene;

		const entt::registry& registry = static_cast<const Scene&>(scene).GetRegistry();
		for (const entt::entity entity : registry.view<TwoBoneIkComponent>())
		{
			const TwoBoneIkComponent* component = registry.try_get<TwoBoneIkComponent>(entity);
			if (!component)
				continue;
			CachedChain cached;
			cached.Self = entity;
			cached.OwnerUuid = component->OwnerUuid != 0 ? component->OwnerUuid : EntityUuid(registry, entity);
			cached.Component = *component;
			state.Chains.push_back(cached);
		}

		// 只有确实需要脚步 IK 的实体才去找地形（与旧 Update 的提前剪枝一致）。
		bool needsTerrain = false;
		for (const entt::entity entity : registry.view<AnimGraphComponent>())
		{
			const AnimGraphComponent* graph = registry.try_get<AnimGraphComponent>(entity);
			if (!graph)
				continue;
			if (const FootIkComponent* foot = registry.try_get<FootIkComponent>(entity))
				needsTerrain = needsTerrain || (foot->Enabled != 0 && foot->Weight > 0.0f);
			else
				needsTerrain = needsTerrain || (graph->EnableFootIk != 0 && graph->FootIkWeight > 0.0f);
		}
		if (needsTerrain)
			state.Terrain = FindSceneTerrain(registry);
	}

	bool IkSystem::SolveTwoBone(const Asset::WModelData& model, AnimPose& pose, const IkChainRequest& request)
	{
		return SolveTwoBoneImpl(model, pose, request, nullptr);
	}

	void IkSystem::ApplyToPose(Scene& scene, entt::entity owner, const Asset::WModelData& model,
		const glm::mat4& characterWorld, AnimPose& pose, bool grounded)
	{
		FrameState& state = State();
		if (state.Owner != &scene)
			return;

		const glm::mat4 inverseCharacter = glm::inverse(characterWorld);
		if (!IsFinite(inverseCharacter))
			return;

		const entt::registry& registry = static_cast<const Scene&>(scene).GetRegistry();
		const uint64_t ownerUuid = EntityUuid(registry, owner);
		ApplyGenericChains(registry, owner, ownerUuid, model, inverseCharacter, pose);
		ApplyFootExample(registry, owner, model, characterWorld, pose, grounded);
	}

	void IkSystem::EndFrame()
	{
		State() = FrameState{};
	}
}
