#include "wldpch.h"

#include "World/Animation/AnimGraphSystem.h"
#include "World/Animation/AnimGraph.h"
#include "World/Core/Application.h"
#include "World/Core/StringPool.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/AnimationSystem.h"
#include "World/Renderer/Mesh.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Systems/TransformSystem.h"
#include "World/Terrain/TerrainRenderer.h"
#include "World/Utils/Paths.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace World
{
	namespace
	{
		namespace fs = std::filesystem;

		// 与 AnimationSystem 的 MeshIndex 语义一致：越界回退 mesh 0。
		uint32_t SelectMeshIndex(const Asset::WModelData& model, int32_t requested)
		{
			if (requested > 0 && static_cast<size_t>(requested) < model.Meshes.size())
				return static_cast<uint32_t>(requested);
			return 0;
		}

		uint32_t Fnv1a32(const std::string& text)
		{
			uint32_t hash = 2166136261u;
			for (const char c : text)
			{
				hash ^= static_cast<uint32_t>(static_cast<unsigned char>(c));
				hash *= 16777619u;
			}
			return hash;
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
				const JointTransform& jt = (index < pose.Joints.size()) ? pose.Joints[index] : JointTransform{};
				return glm::translate(glm::mat4(1.0f), jt.Translation)
					* glm::mat4_cast(jt.Rotation)
					* glm::scale(glm::mat4(1.0f), jt.Scale);
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

		// ---- 资产解析：VFS → AssetRoot → EngineContentRoot（剥 engine/ 前缀） ----
		std::string StripEnginePrefix(std::string path)
		{
			std::string normalized = StringPool::NormalizePath(path);
			if (normalized.rfind("engine/", 0) == 0)
				normalized.erase(0, 7);
			return normalized;
		}

		bool ReadVfsBytes(const std::string& logicalPath, std::vector<uint8_t>& out)
		{
			if (!Application::HasInstance())
				return false;
			std::error_code error;
			if (Application::Get().GetContext().Vfs().Read(logicalPath, out, error) && !out.empty())
				return true;
			const std::string stripped = StripEnginePrefix(logicalPath);
			if (!stripped.empty() && stripped != logicalPath)
			{
				error.clear();
				if (Application::Get().GetContext().Vfs().Read(stripped, out, error) && !out.empty())
					return true;
			}
			return false;
		}

		bool ResolveGraphPath(const std::string& logicalPath, std::string& outPath, std::string* error)
		{
			outPath.clear();
			if (logicalPath.empty())
			{
				if (error)
					*error = "AnimGraphComponent 未指定 AnimGraphAsset";
				return false;
			}

			const std::string stripped = StripEnginePrefix(logicalPath);

			// 1) 绝对/相对 CWD 的直接路径（测试与编辑器打开路径）。
			std::error_code ec;
			if (fs::is_regular_file(logicalPath, ec))
			{
				outPath = logicalPath;
				return true;
			}

			// 2) VFS（打包内容）。命中但磁盘没有对应文件时落到临时文件（LoadFromFile 只吃路径）。
			std::vector<uint8_t> vfsBytes;
			const bool vfsHit = ReadVfsBytes(logicalPath, vfsBytes);

			const fs::path candidates[] = {
				Paths::AssetRoot() / logicalPath,
				Paths::EngineContentRoot() / logicalPath,
				stripped.empty() ? fs::path() : Paths::EngineContentRoot() / stripped,
			};
			for (const fs::path& candidate : candidates)
			{
				if (candidate.empty())
					continue;
				if (fs::is_regular_file(candidate, ec))
				{
					outPath = candidate.string();
					return true;
				}
			}

			if (vfsHit)
			{
				const fs::path tempDir = fs::temp_directory_path() / "we_animgraph";
				fs::create_directories(tempDir, ec);
				const fs::path tempPath = tempDir / ("vfs_" + std::to_string(Fnv1a32(logicalPath)) + ".wanimgraph");
				std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
				if (stream.is_open())
				{
					stream.write(reinterpret_cast<const char*>(vfsBytes.data()),
						static_cast<std::streamsize>(vfsBytes.size()));
					stream.close();
					outPath = tempPath.string();
					return true;
				}
			}

			if (error)
				*error = "找不到动画图资产 '" + logicalPath + "'";
			return false;
		}

		bool ResolveTerrainFile(const AssetRef& asset, fs::path& outPath)
		{
			if (!asset.HasPath())
				return false;
			const std::string& logical = StringPool::Get().PathOf(asset.Path);
			if (logical.empty())
				return false;

			std::error_code ec;
			if (fs::is_regular_file(logical, ec))
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
				if (fs::is_regular_file(candidate, ec))
				{
					outPath = candidate;
					return true;
				}
			}
			return false;
		}

		struct ParameterOverride
		{
			float Value = 0.0f;
			uint64_t ExpiryFrame = 0;
		};

		struct Instance
		{
			const Scene* Owner = nullptr;
			std::string LogicalPath;
			std::string ResolvedPath;
			fs::file_time_type MTime {};
			bool Loaded = false;
			bool Warned = false;
			std::string Error;
			AnimGraph Graph;
			std::unordered_map<std::string, ParameterOverride> Overrides;
			std::vector<glm::mat4> Palette;
			float Speed = 0.0f;
			float Direction = 0.0f;
			float VerticalSpeed = 0.0f;
			bool Grounded = true;
		};

		std::unordered_map<entt::entity, Instance>& Instances()
		{
			static std::unordered_map<entt::entity, Instance> instances;
			return instances;
		}

		uint64_t& FrameCounter()
		{
			static uint64_t counter = 0;
			return counter;
		}

		void WarnOnce(const std::string& key, const std::string& message)
		{
			static std::unordered_set<std::string> warned;
			if (warned.insert(key).second)
				WLD_CORE_WARN("{0}", message);
		}

		bool SameFileStamp(const std::string& path, fs::file_time_type& out)
		{
			std::error_code ec;
			if (path.empty() || !fs::exists(path, ec))
				return false;
			out = fs::last_write_time(path, ec);
			return !ec;
		}

		// 两骨 IK：在姿态空间（模型空间）把 Thigh→Shin→Foot 送到 target。
		struct LegSolveInput
		{
			int32_t Thigh = -1;
			int32_t Shin = -1;
			int32_t Foot = -1;
			glm::vec3 Target { 0.0f };
			glm::vec3 GroundNormal { 0.0f, 1.0f, 0.0f };
			float Weight = 1.0f;
		};

		bool SolveLeg(const Asset::WModelData& model, AnimPose& pose, const LegSolveInput& input)
		{
			const int32_t thigh = input.Thigh;
			const int32_t shin = input.Shin;
			const int32_t foot = input.Foot;
			if (thigh < 0 || shin < 0 || foot < 0)
				return false;
			if (static_cast<size_t>(thigh) >= pose.Joints.size() || static_cast<size_t>(shin) >= pose.Joints.size()
				|| static_cast<size_t>(foot) >= pose.Joints.size())
				return false;
			if (model.Nodes[static_cast<size_t>(shin)].Parent != thigh || model.Nodes[static_cast<size_t>(foot)].Parent != shin)
			{
				WarnOnce("animgraph-ik-chain:" + model.Nodes[static_cast<size_t>(thigh)].Name
						+ model.Nodes[static_cast<size_t>(shin)].Name + model.Nodes[static_cast<size_t>(foot)].Name,
					"Foot IK 需要 Thigh→Shin→Foot 直接父子链，已跳过该腿");
				return false;
			}

			const float weight = std::clamp(input.Weight, 0.0f, 1.0f);
			if (weight <= 0.0f)
				return false;

			const std::vector<glm::mat4> world = ComputeNodeWorld(model, pose);
			const glm::vec3 thighPos = glm::vec3(world[static_cast<size_t>(thigh)][3]);
			const glm::vec3 shinPos = glm::vec3(world[static_cast<size_t>(shin)][3]);
			const glm::vec3 footPos = glm::vec3(world[static_cast<size_t>(foot)][3]);

			const float length1 = glm::distance(thighPos, shinPos);
			const float length2 = glm::distance(shinPos, footPos);
			if (length1 <= 1e-4f || length2 <= 1e-4f)
				return false;

			const glm::vec3 toTarget = input.Target - thighPos;
			const float targetDistance = glm::length(toTarget);
			if (targetDistance <= 1e-5f)
				return false;
			const float maxReach = length1 + length2 - 1e-4f;
			const float reach = std::clamp(targetDistance, 1e-3f, std::max(1e-3f, maxReach));
			const glm::vec3 direction = toTarget / targetDistance;

			// 膝盖保持当前弯曲平面的方向；退化时用 +Z。
			glm::vec3 kneeOffset = (shinPos - thighPos) - direction * glm::dot(shinPos - thighPos, direction);
			if (glm::length(kneeOffset) < 1e-4f)
			{
				kneeOffset = glm::vec3(0.0f, 0.0f, 1.0f) - direction * direction.z;
				if (glm::length(kneeOffset) < 1e-4f)
					kneeOffset = glm::vec3(1.0f, 0.0f, 0.0f);
			}
			const glm::vec3 kneeDir = glm::normalize(kneeOffset);

			const float cosAlpha = std::clamp(
				(length1 * length1 + reach * reach - length2 * length2) / (2.0f * length1 * reach), -1.0f, 1.0f);
			const float sinAlpha = std::sqrt(std::max(0.0f, 1.0f - cosAlpha * cosAlpha));
			const glm::vec3 kneePos = thighPos + direction * (length1 * cosAlpha) + kneeDir * (length1 * sinAlpha);

			const glm::quat parentFix = RotationBetween(shinPos - thighPos, kneePos - thighPos);
			const glm::quat thighWorldOld = MatrixRotation(world[static_cast<size_t>(thigh)]);
			const glm::quat shinWorldOld = MatrixRotation(world[static_cast<size_t>(shin)]);
			const glm::quat footWorldOld = MatrixRotation(world[static_cast<size_t>(foot)]);

			const glm::quat thighWorldNew = SafeNormalize(parentFix * thighWorldOld);
			const glm::vec3 footAfterParent = kneePos + parentFix * (footPos - shinPos);
			glm::quat shinWorldFixed = SafeNormalize(parentFix * shinWorldOld);
			const glm::vec3 toTargetFromShin = input.Target - kneePos;
			glm::quat shinWorldNew = shinWorldFixed;
			if (glm::length(toTargetFromShin) > 1e-5f && glm::length(footAfterParent - kneePos) > 1e-5f)
			{
				const glm::quat shinFix = RotationBetween(footAfterParent - kneePos, toTargetFromShin);
				shinWorldNew = SafeNormalize(shinFix * shinWorldFixed);
			}

			// 脚掌按地面法线对齐（世界法线已在调用侧转到模型空间）。
			glm::vec3 normal = input.GroundNormal;
			if (glm::length(normal) < 1e-5f)
				normal = glm::vec3(0.0f, 1.0f, 0.0f);
			normal = glm::normalize(normal);
			const glm::quat alignFix = RotationBetween(glm::vec3(0.0f, 1.0f, 0.0f), normal);
			const glm::quat footWorldNew = SafeNormalize(alignFix * shinWorldNew * glm::inverse(shinWorldFixed) * footWorldOld);

			const int32_t thighParent = model.Nodes[static_cast<size_t>(thigh)].Parent;
			const glm::quat thighParentWorld = (thighParent >= 0 && static_cast<size_t>(thighParent) < world.size())
				? MatrixRotation(world[static_cast<size_t>(thighParent)])
				: glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

			const glm::quat thighLocalNew = SafeNormalize(glm::inverse(thighParentWorld) * thighWorldNew);
			const glm::quat shinLocalNew = SafeNormalize(glm::inverse(thighWorldNew) * shinWorldNew);
			const glm::quat footLocalNew = SafeNormalize(glm::inverse(shinWorldNew) * footWorldNew);

			pose.Joints[static_cast<size_t>(thigh)].Rotation =
				SafeNormalize(glm::slerp(pose.Joints[static_cast<size_t>(thigh)].Rotation, thighLocalNew, weight));
			pose.Joints[static_cast<size_t>(shin)].Rotation =
				SafeNormalize(glm::slerp(pose.Joints[static_cast<size_t>(shin)].Rotation, shinLocalNew, weight));
			pose.Joints[static_cast<size_t>(foot)].Rotation =
				SafeNormalize(glm::slerp(pose.Joints[static_cast<size_t>(foot)].Rotation, footLocalNew, weight));
			return true;
		}

		struct TerrainBinding
		{
			const TerrainData* Data = nullptr;
			entt::entity Entity = entt::null;
			glm::mat4 WorldMatrix { 1.0f };
		};

		// 实体世界矩阵：优先用缓存（SceneRenderer/TransformSystem 每帧求解），否则由局部 TRS 合成。
		glm::mat4 EntityWorldMatrix(const entt::registry& registry, entt::entity entity)
		{
			if (const WorldTransformComponent* world = registry.try_get<WorldTransformComponent>(entity))
				return world->Matrix;
			if (const TransformComponent* transform = registry.try_get<TransformComponent>(entity))
				return transform->GetLocalMatrix();
			return glm::mat4(1.0f);
		}

		TerrainBinding FindSceneTerrain(const entt::registry& registry)
		{
			TerrainBinding binding;
			const auto view = registry.view<TerrainComponent>();
			for (const entt::entity entity : view)
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

		void ApplyFootIk(const Asset::WModelData& model, const TerrainData* terrain,
			const glm::mat4& terrainWorld, const glm::mat4& characterWorld,
			const AnimGraphComponent& component, AnimPose& pose, bool grounded)
		{
			// 空中不踩地（否则会把腿拉直去够地面）。无地形时保持动画姿态 = 平地退化的数学等价结果
			// （目标 = 绑定踝高 + 实体地面高度，与当前姿态相同），不会 NaN 或抖动。
			if (!terrain || !grounded || component.EnableFootIk == 0 || component.FootIkWeight <= 0.0f)
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

			const float weight = std::clamp(component.FootIkWeight, 0.0f, 1.0f);
			const glm::mat4 entityMatrix = characterWorld;
			const glm::mat4 inverseEntity = glm::inverse(entityMatrix);

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
			// TerrainData::SampleHeight/SampleNormal 的 x/z 是**地形本地网格坐标**
			// （内部再除 cellSize），不是世界坐标 —— 必须先把脚的世界位置换算到地形实体本地空间，
			// 采样后再换回世界空间。否则平移/旋转/缩放过地形实体的关卡会把腿拉长（P0）。
			const glm::mat4 inverseTerrain = glm::inverse(terrainWorld);
			const glm::mat3 terrainNormalMatrix = glm::mat3(glm::transpose(inverseTerrain));
			const glm::mat3 entityNormalMatrix = glm::mat3(glm::transpose(inverseEntity));
			const float entityScaleY = glm::length(glm::vec3(entityMatrix[1]));

			const auto makeTarget = [&](int32_t footIndex, float restHeight) -> FootTarget
			{
				FootTarget target;
				const glm::vec3 footModel = glm::vec3(world[static_cast<size_t>(footIndex)][3]);
				const glm::vec3 footWorld = glm::vec3(entityMatrix * glm::vec4(footModel, 1.0f));

				const glm::vec3 footTerrainLocal = glm::vec3(inverseTerrain * glm::vec4(footWorld, 1.0f));
				const float groundLocalY = terrain->SampleHeight(footTerrainLocal.x, footTerrainLocal.z);
				const glm::vec3 groundWorld = glm::vec3(terrainWorld
					* glm::vec4(footTerrainLocal.x, groundLocalY, footTerrainLocal.z, 1.0f));
				const glm::vec3 normalTerrainLocal = terrain->SampleNormal(footTerrainLocal.x, footTerrainLocal.z);
				glm::vec3 normalWorld = terrainNormalMatrix * normalTerrainLocal;
				normalWorld = (glm::length(normalWorld) > 1e-5f)
					? glm::normalize(normalWorld) : glm::vec3(0.0f, 1.0f, 0.0f);

				// 踝高沿世界竖直方向抬升（骨骼动画的踝高是模型空间 Y）。
				const glm::vec3 targetWorld(groundWorld.x, groundWorld.y + restHeight * entityScaleY, groundWorld.z);
				target.ModelTarget = glm::vec3(inverseEntity * glm::vec4(targetWorld, 1.0f));
				const glm::vec3 normalModel = entityNormalMatrix * normalWorld;
				target.ModelNormal = (glm::length(normalModel) > 1e-5f)
					? glm::normalize(normalModel) : glm::vec3(0.0f, 1.0f, 0.0f);
				target.CurrentY = footModel.y;
				return target;
			};

			FootTarget leftTarget = makeTarget(footLeft, leftRestHeight);
			FootTarget rightTarget = makeTarget(footRight, rightRestHeight);

			// 骨盆下沉：以两脚最低目标为准（只向下，不向上）。
			const float pelvisDrop = std::min(0.0f, std::min(leftTarget.ModelTarget.y - leftTarget.CurrentY,
				rightTarget.ModelTarget.y - rightTarget.CurrentY) * weight);
			if (hipsIndex >= 0 && static_cast<size_t>(hipsIndex) < pose.Joints.size() && std::fabs(pelvisDrop) > 1e-8f)
			{
				pose.Joints[static_cast<size_t>(hipsIndex)].Translation.y += pelvisDrop;
				world = ComputeNodeWorld(model, pose);
				leftTarget = makeTarget(footLeft, leftRestHeight);
				rightTarget = makeTarget(footRight, rightRestHeight);
			}

			LegSolveInput left;
			left.Thigh = thighLeft;
			left.Shin = shinLeft;
			left.Foot = footLeft;
			left.Target = leftTarget.ModelTarget;
			left.GroundNormal = leftTarget.ModelNormal;
			left.Weight = weight;
			SolveLeg(model, pose, left);

			LegSolveInput right;
			right.Thigh = thighRight;
			right.Shin = shinRight;
			right.Foot = footRight;
			right.Target = rightTarget.ModelTarget;
			right.GroundNormal = rightTarget.ModelNormal;
			right.Weight = weight;
			SolveLeg(model, pose, right);
		}

		float DirectionDegrees(const glm::vec3& localVelocity)
		{
			// 面向 +Z 时为 0，右为 +90（atan2(x, z)）。
			return glm::degrees(std::atan2(localVelocity.x, localVelocity.z));
		}
	}

	void AnimGraphSystem::Update(Scene& scene, float deltaSeconds)
	{
		++FrameCounter();
		if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f)
			deltaSeconds = 0.0f;

		auto& instances = Instances();
		const entt::registry& registry = static_cast<const Scene&>(scene).GetRegistry();
		// 编辑态也会推进动画（视口预览），但 Root Motion 只能改运行态实体 ——
		// 否则在 Edit 里只是看着角色就会把 Transform 挪走（并写进关卡）。
		const bool gameplayActive = scene.IsRunning();
		AnimationSystem::ClearExternalPalettes();

		const auto view = registry.view<AnimGraphComponent, SkinnedMeshRendererComponent>();

		// 本场景的缓存清道夫：实体销毁/组件移除后不能继续返回上一帧的调色板。
		for (auto it = instances.begin(); it != instances.end();)
		{
			if (it->second.Owner != &scene)
			{
				++it;
				continue;
			}
			const entt::entity cached = it->first;
			if (!registry.valid(cached) || !registry.all_of<AnimGraphComponent, SkinnedMeshRendererComponent>(cached))
				it = instances.erase(it);
			else
				++it;
		}

		// 只有确实需要 Foot IK 的实体才去找地形（避免无 AnimGraph 的场景每帧加载地形）。
		TerrainBinding terrainBinding;
		for (const entt::entity candidate : view)
		{
			const AnimGraphComponent* probe = registry.try_get<AnimGraphComponent>(candidate);
			if (probe && probe->EnableFootIk != 0 && probe->FootIkWeight > 0.0f)
			{
				terrainBinding = FindSceneTerrain(registry);
				break;
			}
		}
		for (const entt::entity entity : view)
		{
			const AnimGraphComponent* graphComponent = registry.try_get<AnimGraphComponent>(entity);
			const SkinnedMeshRendererComponent* renderer = registry.try_get<SkinnedMeshRendererComponent>(entity);
			if (!graphComponent || !renderer)
				continue;

			AnimGraphComponent& component = const_cast<AnimGraphComponent&>(*graphComponent);
			SkinnedMeshRendererComponent& skinned = const_cast<SkinnedMeshRendererComponent&>(*renderer);

			Instance& instance = instances[entity];
			if (instance.Owner != &scene)
			{
				instance = Instance{};
				instance.Owner = &scene;
			}

			const std::string logicalPath = component.AnimGraphAsset.HasPath()
				? StringPool::Get().PathOf(component.AnimGraphAsset.Path) : std::string();

			// ---- 资产加载（路径 / mtime 变化或首次尝试时重建；失败不逐帧重读）----
			bool rebuild = instance.LogicalPath != logicalPath;
			if (!rebuild && !instance.Loaded && !instance.Warned && !logicalPath.empty())
				rebuild = true;
			std::string resolved;
			std::string resolveError;
			if (!logicalPath.empty() && ResolveGraphPath(logicalPath, resolved, &resolveError))
			{
				fs::file_time_type stamp {};
				if (SameFileStamp(resolved, stamp) && resolved == instance.ResolvedPath && stamp != instance.MTime)
					rebuild = true;
				if (resolved != instance.ResolvedPath)
					rebuild = true;
			}
			else if (!logicalPath.empty())
			{
				instance.Error = resolveError;
				instance.Loaded = false;
				instance.Palette.clear();
				if (!instance.Warned)
				{
					instance.Warned = true;
					WLD_CORE_WARN("AnimGraph 资产不可用 '{0}': {1}", logicalPath, resolveError);
				}
				continue;
			}

			if (rebuild)
			{
				instance.LogicalPath = logicalPath;
				instance.ResolvedPath = resolved;
				instance.Graph.Clear();
				instance.Loaded = false;
				instance.Error.clear();
				instance.Palette.clear();
				instance.Overrides.clear();
				SameFileStamp(resolved, instance.MTime);

				if (!logicalPath.empty())
				{
					std::string loadError;
					if (instance.Graph.LoadFromFile(resolved, &loadError))
						instance.Loaded = true;
					else
					{
						instance.Error = loadError;
						if (!instance.Warned)
						{
							instance.Warned = true;
							WLD_CORE_WARN("AnimGraph 读取失败 '{0}': {1}", logicalPath, loadError);
						}
					}
				}
			}

			if (!instance.Loaded)
			{
				instance.Palette.clear();
				continue;
			}

			// ---- 参数：override(2 帧) > CharacterController 推导 > 资产默认 ----
			for (const auto& entry : instance.Graph.GetParameterDefaults())
				instance.Graph.SetFloat(entry.first, entry.second);

			const CharacterControllerComponent* controller = registry.try_get<CharacterControllerComponent>(entity);
			instance.Speed = 0.0f;
			instance.Direction = 0.0f;
			instance.VerticalSpeed = 0.0f;
			instance.Grounded = true;
			if (controller)
			{
				const TransformComponent* transform = registry.try_get<TransformComponent>(entity);
				const glm::quat rotation = transform ? SafeNormalize(transform->Rotation) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
				const glm::vec3 localVelocity = glm::inverse(rotation) * controller->Velocity;
				instance.Speed = glm::length(glm::vec2(localVelocity.x, localVelocity.z));
				instance.Direction = DirectionDegrees(localVelocity);
				instance.VerticalSpeed = localVelocity.y;
				instance.Grounded = controller->Grounded;
				instance.Graph.SetFloat("Speed", instance.Speed);
				instance.Graph.SetFloat("Direction", instance.Direction);
				instance.Graph.SetFloat("VerticalSpeed", instance.VerticalSpeed);
				instance.Graph.SetFloat("IsFalling", instance.Grounded ? 0.0f : 1.0f);
			}

			const uint64_t frame = FrameCounter();
			for (auto it = instance.Overrides.begin(); it != instance.Overrides.end();)
			{
				if (it->second.ExpiryFrame < frame)
					it = instance.Overrides.erase(it);
				else
					++it;
			}
			for (const auto& entry : instance.Overrides)
				instance.Graph.SetFloat(entry.first, entry.second.Value);

			// 上层遮罩总开关：EnableUpperBodyLayer 直接驱动每一层的权重参数
			// （开启 = 1，关闭 = 0）。示例脚本只切这个标志位、不写 AimWeight，因此这里必须
			// 兜底；显式 SetParameter 的同名 override 仍然优先（面板滑杆可做平滑过渡）。
			for (const AnimLayer& layer : instance.Graph.GetLayers())
			{
				if (layer.WeightParameter.empty())
					continue;
				if (instance.Overrides.find(layer.WeightParameter) != instance.Overrides.end())
					continue;
				instance.Graph.SetFloat(layer.WeightParameter, component.EnableUpperBodyLayer != 0 ? 1.0f : 0.0f);
			}

			// ---- 模型与骨架 ----
			if (!skinned.Mesh.HasPath())
			{
				instance.Palette.clear();
				instance.Error = "SkinnedMeshRendererComponent 缺少 Mesh";
				continue;
			}
			std::string meshError;
			Ref<Mesh> mesh = Mesh::LoadWModel(skinned.Mesh.Path, &meshError);
			const Asset::WModelData* modelPtr = mesh ? mesh->GetSkinSourceData() : nullptr;
			if (!modelPtr)
			{
				instance.Palette.clear();
				instance.Error = "模型不可用: " + meshError;
				continue;
			}
			const Asset::WModelData& model = *modelPtr;
			const uint32_t meshIndex = SelectMeshIndex(model, skinned.MeshIndex);
			const int32_t skinIndex = meshIndex < model.Meshes.size() ? model.Meshes[meshIndex].SkinIndex : -1;
			if (skinIndex < 0 || static_cast<size_t>(skinIndex) >= model.Skins.size())
			{
				instance.Palette.clear();
				instance.Error = "网格不是蒙皮网格";
				continue;
			}

			// ---- 推进 + 采样 ----
			instance.Graph.Update(deltaSeconds * std::max(0.0f, component.PlaybackSpeed));
			AnimPose pose;
			if (!instance.Graph.EvaluatePose(model, pose))
			{
				instance.Palette.clear();
				instance.Error = "姿态采样失败";
				continue;
			}

			const TransformComponent* transformPtr = registry.try_get<TransformComponent>(entity);
			if (transformPtr)
			{
				TransformComponent& transform = const_cast<TransformComponent&>(*transformPtr);
				ApplyFootIk(model, terrainBinding.Data, terrainBinding.WorldMatrix,
					EntityWorldMatrix(registry, entity), component, pose, instance.Grounded);

				// Root Motion：本帧增量按实体朝向旋转后叠加（仅运行态写 Transform）。
				if (gameplayActive && component.EnableRootMotion != 0 && component.RootMotionWeight > 0.0f)
				{
					const glm::vec3 worldDelta = transform.Rotation * (pose.RootMotionDelta * component.RootMotionWeight);
					if (glm::length(worldDelta) > 1e-8f)
						TransformSystem::SetLocation(transform, transform.Location + worldDelta);

					const glm::quat rotationDelta = SafeNormalize(pose.RootRotationDelta);
					// 只取绕 Y 的分量（yaw）。
					const float yaw = 2.0f * std::atan2(rotationDelta.y, rotationDelta.w);
					if (std::isfinite(yaw) && std::fabs(yaw) > 1e-6f)
					{
						const glm::quat yawRotation = glm::angleAxis(yaw * component.RootMotionWeight, glm::vec3(0.0f, 1.0f, 0.0f));
						TransformSystem::SetRotation(transform, SafeNormalize(yawRotation * transform.Rotation));
					}
				}
			}

			if (!instance.Graph.BuildPalette(model, static_cast<uint32_t>(skinIndex), pose, instance.Palette))
			{
				instance.Palette.clear();
				instance.Error = "调色板构建失败";
				continue;
			}

			AnimationSystem::SetExternalPalette(entity, instance.Palette);
			component.CurrentStateHash = Fnv1a32(instance.Graph.GetCurrentStateName());
			instance.Error.clear();
		}
	}

	const std::vector<glm::mat4>* AnimGraphSystem::GetPalette(entt::entity entity)
	{
		const auto found = Instances().find(entity);
		if (found == Instances().end() || found->second.Palette.empty() || found->second.Owner == nullptr)
			return nullptr;
		return &found->second.Palette;
	}

	bool AnimGraphSystem::SetParameter(entt::entity entity, const std::string& name, float value)
	{
		Instance& instance = Instances()[entity];
		ParameterOverride override;
		override.Value = std::isfinite(value) ? value : 0.0f;
		override.ExpiryFrame = FrameCounter() + 2;
		instance.Overrides[name] = override;
		return true;
	}

	float AnimGraphSystem::GetParameter(entt::entity entity, const std::string& name, float fallback)
	{
		const auto found = Instances().find(entity);
		if (found == Instances().end())
			return fallback;
		const auto overrideIt = found->second.Overrides.find(name);
		if (overrideIt != found->second.Overrides.end())
			return overrideIt->second.Value;
		return found->second.Graph.GetFloat(name, fallback);
	}

	bool AnimGraphSystem::GetRuntimeInfo(entt::entity entity, AnimGraphRuntimeInfo& out)
	{
		const auto found = Instances().find(entity);
		if (found == Instances().end())
			return false;
		const Instance& instance = found->second;
		out.Loaded = instance.Loaded;
		out.AssetPath = instance.LogicalPath;
		out.CurrentState = instance.Graph.GetCurrentStateName();
		out.TargetState = instance.Graph.IsInTransition() ? instance.Graph.GetTransitionTargetState() : std::string();
		out.Error = instance.Error;
		out.StateTime = instance.Graph.GetStateTime();
		out.TransitionProgress = instance.Graph.GetTransitionProgress();
		out.InTransition = instance.Graph.IsInTransition();
		out.Speed = instance.Speed;
		out.Direction = instance.Direction;
		out.Grounded = instance.Grounded;
		out.Parameters.clear();
		for (const auto& entry : instance.Graph.GetParameters())
			out.Parameters.push_back(entry);
		return true;
	}

	bool AnimGraphSystem::ReloadAsset(entt::entity entity)
	{
		const auto found = Instances().find(entity);
		if (found == Instances().end())
			return false;
		found->second.Loaded = false;
		found->second.LogicalPath.clear();
		found->second.ResolvedPath.clear();
		found->second.Palette.clear();
		return true;
	}

	void AnimGraphSystem::ClearCache()
	{
		Instances().clear();
		AnimationSystem::ClearExternalPalettes();
	}
}
