#pragma once

#include "World/Core/Export.h"
#include "World/WUI/WuiCore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace World::Wui
{
	struct WuiContext;
	struct WuiTheme;
	struct WuiRect;

	// M4-TEX-P10:纹理引用的**唯一实现**(概念归属表 `tools/agents/wui-concept-owners.json` 的
	// owner_files = 本文件 + WuiTexturePicker.cpp)。
	//
	// 为什么:材质槽位下拉 / `.slang` 参数行 / Texture Settings 面板曾经各写一套纹理引用 UI
	// (用户原话:「材质贴图下拉组件统一用一个」),而"用内核基元拼概念级控件"裸绘制与类型覆盖
	// 两条门禁都抓不到 —— 因此概念归属表按 markers 拦面板自建,面板一律改调本件。
	// 口径:知识库 `contracts/wui-component-library.md`(组件库登记/属性/a11y)与
	// `contracts/texture-import.md`(源图 / `.wtex` 资产 / `.wtexc` 产物的显示口径)。
	//
	// 一件 = 一个纹理引用槽:当前值 + 徽标 + 清空 + 定位 + 拖放 + 可搜索列表(搜索匹配**完整**
	// 逻辑路径,越界只在绘制期中间省略 —— 见 WuiWidgets.h EllipsizeMiddleToWidth)。
	struct WuiTexturePickerEntry
	{
		std::string Value;   // 逻辑路径(源图 `textures/Icon.png` 或资产 `textures/Icon.wtex`)
		std::string Label;   // 显示名;空 = 用 Value。约定:资产条目把导入源名写进括号里
		// 该条目的徽标 token 表(逗号/竖线/分号分隔;空 = 无)。组件**不**替调用方决定
		// "当前值该显示什么徽标" —— 把条目设为当前值时,调用方把这一串转交给 Options::Badges
		// (目录里每条都带 token,当前值的那条才画)。
		std::string Badges;
	};

	// 组件的**显式状态**。`Auto` = 由 Value + 徽标 token 推导(见 WuiTexturePickerDeriveState);
	// 显式传入时一律以调用方为准(调用方知道磁盘事实,组件不猜)。
	enum class WuiTexturePickerState : uint8_t
	{
		Auto = 0,
		Empty,
		Set,
		Missing,        // 资产/源图文件在磁盘上不存在
		MissingSource,  // 资产在,但它指向的源图缺(或解析不出来)
		Stale,          // 有产物但源/设置改过 → 需重烘
		Unbaked,        // 没有产物
		Legacy,         // 旧式设置(没有内嵌源字节)
		Container,      // 单文件 `.wtex`(设置 + 源字节都在资产里)
	};

	// 稳定 token(`auto/empty/set/missing/missing-source/stale/unbaked/legacy/container`):
	// 进 a11y 节点的 value 与工作台属性,探针按它断言(不用中文/英文文案)。
	WLD_API const char* WuiTexturePickerStateToken(WuiTexturePickerState state);
	// Value + 徽标 token → 状态(Auto 用)。`missing-source`/`no-source`/`unreadable` 优先于 `missing`。
	WLD_API WuiTexturePickerState WuiTexturePickerDeriveState(const std::string& value,
		const std::string& badges);
	// 徽标 token → 本地化短标签(`Wui::Tr`)。未知 token **原样返回**(不静默吞掉调用方的自定义标记)。
	WLD_API std::string WuiTexturePickerBadgeText(const std::string& token);

	struct WuiTexturePickerOptions
	{
		// ---- Content ----
		std::string Label;        // 控件标签(a11y label + 列表搜索框的占位)。空 = 本地化默认
		std::string IdPrefix;     // a11y id 前缀(如 "material.albedo")。派生:
		                          //   "<IdPrefix>.state"  = 状态节点;  "<IdPrefix>.locate" = 定位按钮
		                          // 空 = 不登记这两个节点(调用方的既有探针 id 保持逐字节不变)。
		std::string ImportSourceName;  // 当前值的导入源名(括号里的那一半,如 "Icon.png")
		std::vector<WuiTexturePickerEntry> Entries;  // 候选清单(排序由调用方决定:资产在前)
		std::string Badges;       // 当前值的徽标 token 表(空 = 用命中条目的 Badges)
		std::string NoneLabel;    // 清空项文案(空 = 本地化默认 "(none)")
		std::string DroppedValue;  // 跨窗口拖放的**交接值**(调用方用 Editor::AssetDropBridge 取到
		                          // 一次投放后转换出的逻辑路径;非空 = 本帧有一次投放)。
		                          // 跨窗口的"命中/投递"留在调用方(核心 WUI 不认识屏幕物理像素);
		                          // 同窗口拖放由组件自己用 ctx.DropTarget/AcceptDrop("file:") 接收。

		// ---- Behavior ----
		bool AllowClear = true;   // 列表里给 "(none)" 项,选中即清空
		bool AllowReveal = true;  // 右侧定位按钮(在资源管理器中显示)
		bool AllowPick = true;    // 点击展开搜索列表(关闭 = 只展示,不可挑选)
		bool ReadOnly = false;    // 只读:不可挑选、不可拖放、不响应键盘,节点 enabled=false
		WuiTexturePickerState State = WuiTexturePickerState::Auto;

		// ---- Style(徽标排版;未覆盖 = 组件默认,不影响"无徽标"时的命令流)----
		float BadgeFontSize = 11.0f;    // 8..16,越界夹取
		float BadgeFillAlpha = 0.22f;   // 0..1,徽标底透明度,越界夹取

		// ---- 非赋值动作的输出(可空;返回值只表示 Value 有没有变) ----
		bool* RevealRequested = nullptr;  // 本帧点了定位(此时 Value 未变)
	};

	// 画一件纹理引用。`rect` = **整个槽位**(组件内部按需切出列表/徽标/定位按钮)。
	// 返回值 = Value 是否变化(选中新条目 / 清空 / 拖放);定位等动作走 Options 的输出指针。
	//
	// 像素口径:AllowClear/AllowReveal 都关、Badges/State/IdPrefix 都空、Value 命中候选条目时,
	// 本件产生的绘制命令与"直接调用 SearchableCombo(同一 options/selected)"**逐字段相同**
	// (徽标、定位按钮、清空项只在真的启用/需要时才新增命令)。
	WLD_API bool WuiTexturePicker(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& value,
		const WuiTexturePickerOptions& options, const WuiTheme& theme);
}
