#pragma once

// 游戏 UI 框架(GameUI)— 动效:状态机 + 属性补间 + 注册制曲线(工作包 M6)。
//
// 契约:`contract.ui-runtime` §7(动画)。
// 边界(硬):
//   * 纯数值状态机:不偷看输入(状态由调用方 `SetState` 喂)、不发绘制命令、不改 `.wui` 文档;
//   * 产出"属性覆盖值",由调用方在绘制前覆盖进节点属性(值编码 = 属性文本协议);
//   * 时间由调用方给(`Update(dtSeconds)`,真实秒,不是固定步);`dt < 0` 视为 0;
//   * 确定性:同一 dt 序列 → 逐值相同(无随机、无墙钟、无隐藏步长)。
// 明确不做(方案 §9):不做缓动库、不做动画混合树;颜色 / 二维量由调用方按通道自行组合。

#include "World/Core/Export.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace World::UI
{
	inline constexpr const char* kUiCurveLinear = "Linear";
	inline constexpr const char* kUiCurveEaseInOut = "EaseInOut";

	// 曲线 = 归一化时间 [0,1] → 归一化进度(`f(0)=0`,`f(1)=1`)。
	using UiCurveFn = float (*)(float t);

	class WLD_API UiAnimator
	{
	public:
		// ---- 曲线(注册制;内置 Linear / EaseInOut)----
		// 空名 / 空函数 = false;同名重复注册 = 覆盖。
		static bool RegisterCurve(std::string_view name, UiCurveFn fn);
		static UiCurveFn FindCurve(std::string_view name);

		// ---- 节点登记(让"未知节点"可读报错,而不是静默建轨道)----
		bool RegisterNode(std::string_view nodeId);
		bool HasNode(std::string_view nodeId) const;

		// ---- 补间 ----
		// from → to;duration <= 0 = 立即落到 to;curve 未注册 / 节点未登记 → false + error(不建轨道)。
		bool Tween(std::string_view nodeId, std::string_view property,
			float from, float to, float duration,
			std::string_view curve = kUiCurveLinear, std::string* error = nullptr);

		// ---- 状态机(状态由调用方喂)----
		// 注册"某状态(S)下某属性(P)的目标值";`SetState(S)` 时 P 从当前值补到目标。
		bool DefineState(std::string_view nodeId, std::string_view state, std::string_view property,
			float target, float duration,
			std::string_view curve = kUiCurveLinear, std::string* error = nullptr);
		// 切换状态:对每个已定义属性开启补间;属性尚无轨道时以目标值为起点(不伪造运动)。
		// 未知节点 / 空状态 → false + error;状态本身无需预先 `DefineState`(允许无目标状态)。
		bool SetState(std::string_view nodeId, std::string_view state, std::string* error = nullptr);
		std::string_view State(std::string_view nodeId) const;

		// ---- 推进(时间由调用方给;`dt < 0` 视为 0)----
		void Update(float dtSeconds);

		// ---- 查询 ----
		// 未登记节点 / 无该属性轨道 → false + 可读 error(不猜默认值)。
		bool Current(std::string_view nodeId, std::string_view property, float& out, std::string* error = nullptr) const;
		// 覆盖值文本(2 位小数、去尾零/尾点,同属性文本协议;可直接写回节点属性)。
		bool CurrentText(std::string_view nodeId, std::string_view property, std::string& out, std::string* error = nullptr) const;
		bool IsAnimating(std::string_view nodeId, std::string_view property) const;

		std::size_t NodeCount() const { return m_Nodes.size(); }
		void Clear();

	private:
		struct Track
		{
			std::string Property;
			float From = 0.0f;
			float To = 0.0f;
			float Duration = 0.0f;
			float Elapsed = 0.0f;
			float Value = 0.0f;
			UiCurveFn Curve = nullptr;
			std::string CurveName;
			bool Active = false;
		};
		struct StateTarget
		{
			std::string State;
			std::string Property;
			float Target = 0.0f;
			float Duration = 0.0f;
			UiCurveFn Curve = nullptr;
			std::string CurveName;
		};
		struct Node
		{
			std::string Id;
			std::string State;
			std::vector<Track> Tracks;
			std::vector<StateTarget> StateTargets;
		};

		Node* FindNodeMut(std::string_view nodeId);
		const Node* FindNode(std::string_view nodeId) const;
		static Track* FindTrack(Node& node, std::string_view property);
		static const Track* FindTrack(const Node& node, std::string_view property);

		std::vector<Node> m_Nodes;
	};
}
