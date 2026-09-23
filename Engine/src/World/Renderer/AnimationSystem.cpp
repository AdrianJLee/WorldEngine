#include "wldpch.h"

#include "World/Renderer/AnimationSystem.h"

#include "World/Renderer/Skinning.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace World
{
	namespace
	{
		// 进程级静态状态(同一宿主里的所有 SceneRenderer 共用同一份;Update 每帧重建调色板表)。
		std::unordered_map<std::string, Asset::WModelData>& ModelCache()
		{
			static std::unordered_map<std::string, Asset::WModelData> cache;
			return cache;
		}

		std::unordered_map<entt::entity, std::vector<glm::mat4>>& Palettes()
		{
			static std::unordered_map<entt::entity, std::vector<glm::mat4>> palettes;
			return palettes;
		}

		// 读失败的路径不再逐帧重试(磁盘别再被刷屏);ClearCache 后可以重试。
		std::unordered_set<std::string>& FailedModels()
		{
			static std::unordered_set<std::string> failed;
			return failed;
		}

		std::unordered_set<std::string>& WarnedKeys()
		{
			static std::unordered_set<std::string> warned;
			return warned;
		}

		void WarnOnce(const std::string& key, const std::string& message)
		{
			if (WarnedKeys().insert(key).second)
				WLD_CORE_WARN("{0}", message);
		}

		// 缓存键用规范化路径(与 Mesh 的 .wmodel 缓存同一口径)。
		std::string ModelKey(const std::string& path)
		{
			return std::filesystem::path(path).lexically_normal().generic_string();
		}

		// 与 SceneRenderer 的 MeshIndex 语义一致:越界(资产被替换/手填)回退 mesh 0。
		uint32_t SelectMeshIndex(const Asset::WModelData& model, int32_t requested)
		{
			if (requested > 0 && static_cast<size_t>(requested) < model.Meshes.size())
				return static_cast<uint32_t>(requested);
			return 0;
		}

		// 空名 = 第 0 条;找不到 → warn 一次并按"无动画"(绑定姿态)处理,不打断渲染。
		const Asset::WModelAnimation* SelectClip(const Asset::WModelData& model,
			const std::string& requested, const std::string& key)
		{
			if (model.Animations.empty())
				return nullptr;
			if (requested.empty())
				return &model.Animations.front();
			for (const Asset::WModelAnimation& animation : model.Animations)
				if (animation.Name == requested)
					return &animation;
			WarnOnce("anim-clip:" + key + ":" + requested,
				"骨骼动画 clip 未找到 '" + requested + "'(" + key + "):按绑定姿态渲染");
			return nullptr;
		}

		glm::quat SafeNormalize(const glm::quat& value)
		{
			const float length = glm::length(value);
			if (!std::isfinite(length) || length <= 0.0f)
				return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			return value / length;
		}
	}

	void AnimationSystem::Update(Scene& scene, float deltaSeconds)
	{
		auto& palettes = Palettes();
		palettes.clear();
		if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f)
			deltaSeconds = 0.0f;

		// Play/Simulate 下活动场景的**非 const** GetRegistry() 会触发"运行期结构写"断言,
		// 因此枚举走 const registry,只对组件的运行态字段(Time)做 const_cast 写入 ——
		// 与 ScriptEngine::DrawScriptUi 写 State/LastError 是同一套口径,不增删实体/组件。
		const entt::registry& registry = static_cast<const Scene&>(scene).GetRegistry();
		const auto view = registry.view<SkinnedMeshRendererComponent>();
		for (const entt::entity entity : view)
		{
			const SkinnedMeshRendererComponent* probe = registry.try_get<SkinnedMeshRendererComponent>(entity);
			if (!probe)
				continue;
			SkinnedMeshRendererComponent& component = const_cast<SkinnedMeshRendererComponent&>(*probe);
			if (component.MeshPath.empty())
			{
				WarnOnce("anim-path:" + std::to_string(static_cast<uint32_t>(entity)),
					"SkinnedMeshRendererComponent 缺少 MeshPath(实体 " + std::to_string(static_cast<uint32_t>(entity))
						+ "):跳过骨骼动画");
				continue;
			}

			const std::string key = ModelKey(component.MeshPath);
			auto cached = ModelCache().find(key);
			if (cached == ModelCache().end())
			{
				if (FailedModels().count(key))
					continue;
				Asset::WModelData data;
				std::string error;
				if (!Asset::WModelIO::ReadFile(component.MeshPath, data, &error))
				{
					FailedModels().insert(key);
					WarnOnce("anim-model:" + key, "骨骼动画模型读取失败 '" + component.MeshPath + "': "
						+ error + "(跳过该实体的骨骼动画)");
					continue;
				}
				cached = ModelCache().emplace(key, std::move(data)).first;
			}
			const Asset::WModelData& model = cached->second;

			// 该组件没有 SkinIndex 字段:骨架由所选 mesh 的 SkinIndex 决定(与 GPU 侧同一映射)。
			const uint32_t meshIndex = SelectMeshIndex(model, component.MeshIndex);
			const int32_t skinIndex = meshIndex < model.Meshes.size()
				? model.Meshes[meshIndex].SkinIndex : -1;
			if (skinIndex < 0 || static_cast<size_t>(skinIndex) >= model.Skins.size())
				continue;   // 非蒙皮网格:SceneRenderer 按网格顶点布局回退静态路径(不产生调色板)

			const Asset::WModelAnimation* clip = SelectClip(model, component.AnimationClip, key);
			static const Asset::WModelAnimation kNoAnimation;
			const Asset::WModelAnimation& animation = clip ? *clip : kNoAnimation;
			if (clip)
			{
				component.Time = AdvanceTime(component.Time, deltaSeconds, component.Speed,
					component.Playing, component.Loop, animation.Duration);
			}

			std::vector<glm::mat4> palette = ComputePalette(model, static_cast<uint32_t>(skinIndex),
				animation, component.Time);
			if (!palette.empty())
				palettes.emplace(entity, std::move(palette));
		}
	}

	const std::vector<glm::mat4>* AnimationSystem::GetPalette(entt::entity entity)
	{
		const auto& palettes = Palettes();
		const auto found = palettes.find(entity);
		return found == palettes.end() ? nullptr : &found->second;
	}

	std::vector<glm::mat4> AnimationSystem::ComputePalette(const Asset::WModelData& model,
		uint32_t skinIndex, const Asset::WModelAnimation& animation, float time)
	{
		if (static_cast<size_t>(skinIndex) >= model.Skins.size())
			return {};
		const Asset::WModelSkin& skin = model.Skins[skinIndex];
		// 容器不变量之外的手工数据(0 关节 / 逆绑定不足)→ 空,交给调用方按"无蒙皮"处理。
		if (skin.JointNodes.empty() || skin.InverseBindMatrices.size() < skin.JointNodes.size())
			return {};

		const size_t nodeCount = model.Nodes.size();
		std::vector<glm::vec3> translations(nodeCount);
		std::vector<glm::quat> rotations(nodeCount);
		std::vector<glm::vec3> scales(nodeCount);
		for (size_t index = 0; index < nodeCount; ++index)
		{
			translations[index] = model.Nodes[index].Translation;
			rotations[index] = model.Nodes[index].Rotation;
			scales[index] = model.Nodes[index].Scale;
		}

		// 采样结果直接写回节点 TRS(动画 TargetNode = Nodes[] 下标;越界样本忽略)。
		for (const AnimationSample& sample : SampleAnimation(animation, time))
		{
			if (static_cast<size_t>(sample.Node) >= nodeCount)
				continue;
			switch (sample.Path)
			{
			case Asset::WModelIO::WModelAnimationPath::Translation:
				translations[sample.Node] = glm::vec3(sample.Value);   // T/S 只写 xyz(w 按容器约定保留)
				break;
			case Asset::WModelIO::WModelAnimationPath::Rotation:
				rotations[sample.Node] = SafeNormalize(
					glm::quat(sample.Value.w, sample.Value.x, sample.Value.y, sample.Value.z));
				break;
			case Asset::WModelIO::WModelAnimationPath::Scale:
				scales[sample.Node] = glm::vec3(sample.Value);
				break;
			}
		}

		// 节点世界矩阵:按 Nodes[].Parent 求父链(节点顺序不保证父先子后);
		// 非法父级/自引用/成环一律按"根"处理,绝不递归失控。关节世界矩阵与网格世界矩阵
		// 都从这个数组取 —— BuildJointMatrices 的 jointWorld 按**节点下标**索引。
		std::vector<glm::mat4> nodeWorld(nodeCount, glm::mat4(1.0f));
		std::vector<uint8_t> state(nodeCount, 0);   // 0 = 未求,1 = 求解中,2 = 已求
		const auto localMatrix = [&](size_t index)
		{
			return glm::translate(glm::mat4(1.0f), translations[index])
				* glm::mat4_cast(rotations[index])
				* glm::scale(glm::mat4(1.0f), scales[index]);
		};
		std::function<glm::mat4(size_t)> resolve = [&](size_t index) -> glm::mat4
		{
			if (state[index] == 2)
				return nodeWorld[index];
			if (state[index] == 1)
				return glm::mat4(1.0f);   // 环:环上的父级按单位阵参与,不再深入
			state[index] = 1;
			glm::mat4 parentWorld(1.0f);
			const int32_t parent = model.Nodes[index].Parent;
			if (parent >= 0 && static_cast<size_t>(parent) < nodeCount && static_cast<size_t>(parent) != index)
				parentWorld = resolve(static_cast<size_t>(parent));
			nodeWorld[index] = parentWorld * localMatrix(index);
			state[index] = 2;
			return nodeWorld[index];
		};
		for (size_t index = 0; index < nodeCount; ++index)
			resolve(index);

		// meshWorld:绑定到该 skin 的网格节点的世界矩阵(节点下标空间;纯网格资产没有节点 → 单位阵)。
		// 调色板公式(plan §D5c 冻结):palette[j] = inverse(meshWorld) * jointWorld[JointNodes[j]] * IBM[j],
		// 顶点再由实体世界矩阵(u_Model)变换 —— 网格节点的世界矩阵因此必须被抵消。
		glm::mat4 meshWorld(1.0f);
		for (size_t index = 0; index < nodeCount; ++index)
		{
			const int32_t meshIndex = model.Nodes[index].MeshIndex;
			if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= model.Meshes.size())
				continue;
			if (model.Meshes[static_cast<size_t>(meshIndex)].SkinIndex != static_cast<int32_t>(skinIndex))
				continue;
			meshWorld = nodeWorld[index];
			break;
		}

		return BuildJointMatrices(skin, nodeWorld, meshWorld);
	}

	float AnimationSystem::AdvanceTime(float time, float deltaSeconds, float speed, bool playing,
		bool loop, float duration)
	{
		if (!std::isfinite(time) || time < 0.0f)
			time = 0.0f;
		if (!playing || !std::isfinite(deltaSeconds) || !std::isfinite(speed))
			return time;

		const float step = deltaSeconds * speed;
		if (step == 0.0f)
			return time;
		float next = time + step;
		if (!std::isfinite(next))
			return time;

		if (duration > 0.0f)
		{
			if (loop)
			{
				next = std::fmod(next, duration);
				if (next < 0.0f)
					next += duration;
			}
			else
			{
				next = std::min(next, duration);
				if (next < 0.0f)
					next = 0.0f;
			}
		}
		return next;
	}

	void AnimationSystem::ClearCache()
	{
		ModelCache().clear();
		FailedModels().clear();
		WarnedKeys().clear();
		Palettes().clear();
	}
}
