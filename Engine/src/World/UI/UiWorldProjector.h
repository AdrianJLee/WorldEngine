#pragma once

// 游戏 UI 框架(GameUI)— 世界空间 UI(工作包 M8)。
//
// 需求:血条 / 名牌 / 交互提示这类 UI 要"钉在世界里的东西上",而不是固定在屏幕上。
//
// 第一性原理:世界空间 UI **不是**另一套运行时 —— 它只是"锚点求解的另一种输入"。
// 因此这里只做一件事:把节点的锚点输入从"父矩形"换成"实体世界坐标投影到屏幕后的点",
// 求出的仍然是**设计空间矩形**,`UiPainter` 与命中测试一行都不用改。
//
// 契约见 `contract.ui-runtime`(坐标一致:绘制与命中共用同一 `UiViewport` 映射);
// 不变量见 `invariant.ui-presentation-only`(本层只读相机/实体位置,不写场景、不进固定步)。

#include "World/Core/Export.h"
#include "World/UI/UiScreen.h"
#include "World/UI/UiTypes.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

namespace World::UI
{
	// 相机投影输入(纯数据:宿主每帧从主相机取,不持有相机对象)。
	struct UiWorldCamera
	{
		glm::mat4 ViewProjection { 1.0f };
		glm::vec2 ScreenSize { 1920.0f, 1080.0f };   // 物理像素
	};

	// 实体世界坐标解析器(宿主提供:按 `.wui` 里的 `Target` 名字取当前世界位置)。
	// 返回 false = 目标当前不存在(节点本帧隐藏,而不是画在 (0,0))。
	using UiWorldPositionResolver = std::function<bool(std::string_view target, glm::vec3& outWorld)>;

	struct UiWorldResult
	{
		std::size_t Anchored = 0;   // 成功锚定的节点数
		std::size_t Hidden = 0;     // 目标解析失败 ⇒ 收起(不参与绘制/命中)的节点数
		std::vector<std::string> Warnings;
	};

	// 世界坐标 → 屏幕(物理像素)。目标在相机**背后**或退化矩阵 ⇒ false。
	WLD_API bool ProjectWorldToScreen(const UiWorldCamera& camera, const glm::vec3& world, glm::vec2& outScreen);

	// 把 `screen` 里带 `World` 锚点的节点重新定位到"投影点 + Offset"。
	// 坐标口径:相机与目标都按**视口单位**(= 后端绘制前的那套坐标),与 `UiScreen` 的矩形、
	// 无障碍节点同一空间;后端绘制时统一乘 `Wui::UiScale()`(唯一一次换算)。
	// 必须在本帧 `UiScreen::Layout` 之后调用(尺寸取自布局结果,位置被覆盖)。
	// 目标解析失败的节点会被移到内容矩形之外并标记不可见(宿主据此跳过绘制)。
	WLD_API UiWorldResult ApplyWorldAnchors(UiScreen& screen, const UiViewport& viewport,
		const UiWorldCamera& camera, const UiWorldPositionResolver& resolve);
}
