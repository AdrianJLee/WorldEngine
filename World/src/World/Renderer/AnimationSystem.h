#pragma once

// D5c-4a:场景级骨骼动画系统(纯 CPU:无 RHI 设备、无窗口依赖,headless 可断言)。
//
// 每帧流程(由 SceneRenderer 在收集 3D 绘制之前调用一次):
//   Time += dt * Speed(Playing 时;按 Loop/时长回绕或夹取)
//     → 采样 clip 覆盖节点 TRS
//     → 按节点父链求节点世界矩阵(节点下标空间)
//     → World::BuildJointMatrices 得调色板(jointWorld 按 skin.JointNodes[j] 索引)
// 结果按实体缓存在系统里,SceneRenderer 当帧用 GetPalette 取走交给 Renderer3D::SubmitSkinned。

#include "World/Core/Asset/WModelIO.h"
#include "World/Core/Export.h"

#include <entt.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace World
{
	class Scene;

	class WLD_API AnimationSystem
	{
	public:
		// 推进所有带 SkinnedMeshRendererComponent 的实体:Time 写回组件,调色板按实体缓存。
		// 模型读取走 Asset::WModelIO::ReadFile(VFS 优先 + 内容根回退,与 Mesh::LoadWModel 同一条路径解析),
		// 读失败 → warn 一次并跳过该实体;非蒙皮网格(该 mesh 的 SkinIndex = -1)不产生调色板。
		static void Update(Scene& scene, float deltaSeconds);

		// 上一次 Update 算出的该实体调色板(长度 = 该 skin 的关节数);nullptr = 本帧取不到
		// (模型读失败 / 非蒙皮 / 组件不存在)。指针在下次 Update 前有效。
		static const std::vector<glm::mat4>* GetPalette(entt::entity entity);

		// 纯函数(无 Scene/组件依赖,测试/预览用):给一个已解析的 WModelData + 时间,
		// 直接算调色板。skinIndex 越界 / 关节集合为空 → 空调色板;
		// Channels 为空的 animation = 绑定姿态(节点 TRS 原样),不是失败。
		static std::vector<glm::mat4> ComputePalette(const Asset::WModelData& model,
			uint32_t skinIndex, const Asset::WModelAnimation& animation, float time);

		// 时间推进的纯逻辑(Update 的组件读写之外只有这一段,单测直接断言):
		//   - Playing = false → 原值返回;
		//   - next = time + deltaSeconds * Speed;
		//   - duration > 0 且 Loop → fmod 回绕到 [0, duration); 非 Loop → 夹到 duration;
		//   - duration = 0(无 clip / 零时长)→ 不回绕,直接累加。
		// 非有限输入按 0 处理(不把 NaN 写进组件)。
		static float AdvanceTime(float time, float deltaSeconds, float speed, bool playing,
			bool loop, float duration);

		// 清空模型缓存、失败标记与调色板(重新导入/热重载后由调用方清)。
		static void ClearCache();
	};
}
