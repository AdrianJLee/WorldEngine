#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Framework/InputMap.h"
#include "World/Scene/Scene.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <string>

namespace World::Gameplay
{
	// WP5:输入采样服务(帧首采样一次 -> 快照 -> PreFixed/Fixed 只消费)。
	//
	// 语义(工业口径,保 P4/P5 的确定性):
	//   * 宿主在**可变帧开头、固定步循环之前**调用 Sample 恰好一次;
	//   * 同一可变帧内固定步循环跑 0..N 次,而快照只在 Sample 时替换
	//     ⇒ 每个固定步读到的输入逐位相同,可复现;
	//   * `PreFixed` / `Fixed` 内**不允许**再采样(那里每帧跑 0..N 次,会重复采样/丢输入)。
	//
	// 三态语义:`IsDown` 读场景快照;`WasPressed` / `WasReleased` 用采样时记下的本帧边沿表
	// (边沿由 InputService 的上一帧状态给出,InputSystem 只做搬运,不重复推导)。
	// 运行态只读:本服务没有设置输入的口子。
	//
	// 存储:按场景寻址(引擎主线程使用)。场景销毁后条目允许残留,查询前用
	// Scene 的 slot/generation 判活,地址被新场景复用时不会读到旧状态。
	class WLD_API InputSystem
	{
	public:
		// 帧首采样:把 InputService 的动作/轴 + 平台鼠标位置折算成 Scene::InputSnapshot。
		// mousePosition 由宿主在**有窗口**时传入(无窗口的 headless 宿主传 {0,0});
		// 鼠标增量由本服务按"上一可变帧位置"求差;
		// scrollDelta 由宿主从平台滚轮累积读入(无窗口宿主传 {0,0}),写入快照的 ScrollDelta。
		static const Scene::InputSnapshot& Sample(Scene& scene, const InputService& service,
			glm::vec2 mousePosition = glm::vec2(0.0f), glm::vec2 scrollDelta = glm::vec2(0.0f));

		// 丢弃某场景的边沿/鼠标缓存并清空快照(场景停止/换场景时调用):
		// 运行结束后不应再能读到上一场运行的输入;不调用只会多留一份可判活的缓存。
		static void Forget(Scene& scene);

		// ---- 模拟侧只读查询 ----
		static bool IsDown(const Scene& scene, const std::string& action);
		static bool WasPressed(const Scene& scene, const std::string& action);
		static bool WasReleased(const Scene& scene, const std::string& action);
		static float GetAxis(const Scene& scene, const std::string& axis);

		// 可观测性(反假证据用):最近一次采样所在的可变帧号 / 该场景累计采样次数。
		static uint64_t GetLastSampledFrame(const Scene& scene);
		static uint64_t GetSampleCount(const Scene& scene);
	};
}
