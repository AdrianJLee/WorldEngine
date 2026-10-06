#pragma once

#include "World/WUI/WuiWidget.h"
#include <cstddef>

namespace World::Wui
{
	// 折线图/柱状图:数据由调用方持有,控件**不拷贝样本**。
	class WuiPlot final : public WuiWidget
	{
	public:
		enum class Kind { Line, Bars };

		const float* Samples = nullptr;   // 只读,不拥有
		size_t SampleCount = 0;
		Kind Style = Kind::Line;
		float MinValue = 0.0f;            // <=0 时按数据自动定标
		float MaxValue = 0.0f;
		float WarnThreshold = -1.0f;      // >=0 时画一条水平告警线(帧时间用:掉帧阈值)
		WuiColor LineColor { 0.3f, 0.7f, 1.0f, 1.0f };
		WuiColor FillColor { 0.3f, 0.7f, 1.0f, 0.18f };
		WuiColor GridColor { 1.0f, 1.0f, 1.0f, 0.08f };
		WuiColor ThresholdColor { 1.0f, 0.45f, 0.35f, 0.9f };
		float MinHeight = 80.0f;

		// ---- 第二序列(可选;默认不画)----
		// 用途:同图对比两条曲线(内存面板的"堆内存 vs 显存")。
		// 第二序列**默认不填充**,避免半透明色块遮住主序列。
		const float* Samples2 = nullptr;
		size_t SampleCount2 = 0;
		WuiColor LineColor2 { 1.0f, 0.72f, 0.35f, 1.0f };
		bool FillSecond = false;
		WuiColor FillColor2 { 1.0f, 0.72f, 0.35f, 0.14f };

		// ---- 参考线(最多 4 条)----
		// 与 WarnThreshold 的分工:WarnThreshold 是"掉帧告警"的**专用**语义(一条、红);
		// Guides 是**通用**多条标记(如 p50/p95/p99),颜色各自指定。
		// 契约:值落在 [量程] 之外时**不画** —— 画到边缘会让人误读成"刚好到顶"。
		struct Guide
		{
			float Value = 0.0f;
			WuiColor Color { 1.0f, 1.0f, 1.0f, 0.35f };
		};
		static constexpr uint32_t kMaxGuides = 4;
		Guide Guides[kMaxGuides];
		uint32_t GuideCount = 0;

		WuiMeasure Measure(const WuiConstraints& constraints) override;
		void Paint(WuiPaintContext& context) override;
	};
}
