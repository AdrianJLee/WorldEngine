#include "wldpch.h"

#include "World/Animation/AnimGraphSystem.h"
#include "World/Animation/AnimGraph.h"
#include "World/Animation/IkSystem.h"
#include "World/Core/Application.h"
#include "World/Core/StringPool.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/AnimationSystem.h"
#include "World/Renderer/Mesh.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Systems/TransformSystem.h"
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

		// 实体世界矩阵：优先用缓存（SceneRenderer/TransformSystem 每帧求解），否则由局部 TRS 合成。
		glm::mat4 EntityWorldMatrix(const entt::registry& registry, entt::entity entity)
		{
			if (const WorldTransformComponent* world = registry.try_get<WorldTransformComponent>(entity))
				return world->Matrix;
			if (const TransformComponent* transform = registry.try_get<TransformComponent>(entity))
				return transform->GetLocalMatrix();
			return glm::mat4(1.0f);
		}

		bool SameFileStamp(const std::string& path, fs::file_time_type& out)
		{
			std::error_code ec;
			if (path.empty() || !fs::exists(path, ec))
				return false;
			out = fs::last_write_time(path, ec);
			return !ec;
		}

		float DirectionDegrees(const glm::vec3& localVelocity)
		{
			// 面向 +Z 时为 0°，角色自身右侧为 +90°。本工程使用右手系：当角色面向 +Z 时，
			// 其右方是 -X，因此 X 分量取负后才与「右 = +90°」的混合空间横轴语义一致。
			return glm::degrees(std::atan2(-localVelocity.x, localVelocity.z));
		}
	}

	void AnimGraphSystem::Update(Scene& scene, float deltaSeconds)
	{
		++FrameCounter();
		if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f)
			deltaSeconds = 0.0f;

		IkSystem::BeginFrame(scene);

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
			IkSystem::ApplyToPose(scene, entity, model, EntityWorldMatrix(registry, entity), pose,
				instance.Grounded && transformPtr != nullptr);
			if (transformPtr)
			{
				TransformComponent& transform = const_cast<TransformComponent&>(*transformPtr);

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

		IkSystem::EndFrame();
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
