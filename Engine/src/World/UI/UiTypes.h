#pragma once

// 游戏 UI 框架(GameUI)— 基础类型:设备无关坐标 / 锚点 / 布局种类。
//
// 契约见 WorldEngine-docs `contract.ui-runtime` + `contract.ui-document-format`;
// 架构决策见 `decision.0011-game-ui-architecture`。
//
// 第一性原理:UI 是表现层,不是模拟层(见 `invariant.ui-presentation-only`)。
// 本头文件是纯数学 + 纯数据,不依赖渲染后端、不依赖 Editor、不碰 ECS。

#include "World/Core/Export.h"
#include "World/WUI/WuiCore.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

namespace World::UI
{
	// ---- 缩放策略(商业级设备无关坐标)----
	//
	// 有效缩放 = 策略缩放 × DPI 系数;设计单位 → 物理像素只走这一条路径,
	// 绘制与命中共用同一个 `UiViewport`,保证"看到的 = 点到的"。
	enum class UiScaleMode : uint8_t
	{
		ConstantPixelSize = 0,   // 策略缩放 = 1(分辨率变化不改 UI 像素尺寸)
		ScaleWithScreenSize,     // 按 Match 在宽/高比例间插值(Unity MatchWidthOrHeight 口径)
		Expand,                  // max(physW/designW, physH/designH):内容至少铺满
		Shrink,                  // min(...):内容完整可见(可能留边)
	};

	const char* UiScaleModeName(UiScaleMode mode);
	bool ParseUiScaleMode(std::string_view name, UiScaleMode& out);

	struct UiDesign
	{
		glm::vec2 Resolution { 1920.0f, 1080.0f };   // 设计分辨率(设计单位)
		UiScaleMode ScaleMode = UiScaleMode::ScaleWithScreenSize;
		float Match = 0.5f;                          // 0 = 全宽,1 = 全高;仅 ScaleWithScreenSize 用
	};

	// 安全区:刘海/圆角/Home 条等不可用区域,单位为**物理像素**,从屏幕四边内缩。
	struct UiSafeArea
	{
		bool Enabled = false;
		float Top = 0.0f;
		float Bottom = 0.0f;
		float Left = 0.0f;
		float Right = 0.0f;
	};

	// ---- 锚点(Unity RectTransform offsetMin/offsetMax 口径,可无歧义测试)----
	//
	//   点锚定(Min == Max):rect.size = Size;rect.min = anchorPoint + Offset - Pivot*Size
	//   拉伸(Min != Max):rect.min = parent.min + Min*parentSize + Offset;
	//                     rect.size = (Max-Min)*parentSize + Size
	//
	// 因此:Min==Max + Size + Pivot 给出"相对某个角/中心定位的固定尺寸";
	// Min=[0,0]/Max=[1,1] + Size=0 给出铺满;Pivot 只在点锚定时参与求解(与 Unity 一致)。
	struct UiAnchor
	{
		glm::vec2 Min { 0.0f, 0.0f };
		glm::vec2 Max { 0.0f, 0.0f };
		glm::vec2 Pivot { 0.5f, 0.5f };
		glm::vec2 Offset { 0.0f, 0.0f };
		glm::vec2 Size { 0.0f, 0.0f };
	// true = 不相对父矩形,而相对视口内容矩形(已扣安全区)锚定。
	bool RelativeToSafeArea = false;
};

	// 世界空间锚点(M8):节点钉在"目标的世界位置投影到屏幕后的点"上。
	// 默认关闭 = 纯屏幕空间;开启后 `Anchor` 仍是同一套五元组,只是"父矩形"换成了
	// "以投影点为中心、尺寸为节点自身布局结果的矩形"(见 UiWorldProjector.h)。
	struct UiWorldAnchor
	{
		bool Enabled = false;
		std::string Target;                 // 宿主解析用的目标名(实体名/路径;空 = 非法)
		glm::vec3 Offset { 0.0f, 0.0f, 0.0f };
		bool KeepOnScreen = false;          // true = 投影点在屏幕外时钳到内容矩形内
	};

	// 宿主每帧提供的物理面信息。
	struct UiSurface
	{
		glm::vec2 PhysicalSize { 1280.0f, 720.0f };   // 后备缓冲/呈现目标的物理像素尺寸
		float DpiScale = 1.0f;                        // 平台缩放系数(1.0 = 100%)
	};

	// 设计空间 ↔ 物理空间 的**唯一**映射。
	struct UiViewport
	{
		glm::vec2 DesignResolution { 1920.0f, 1080.0f };
		glm::vec2 PhysicalSize { 1280.0f, 720.0f };
		glm::vec2 PhysicalOrigin { 0.0f, 0.0f };      // 内容原点在物理面上的位置
		float Scale = 1.0f;                           // 设计单位 → 物理像素
		Wui::WuiRect ContentRect { 0, 0, 1920, 1080 };// 设计空间内容矩形(已扣安全区)

		glm::vec2 DesignToPhysical(glm::vec2 p) const { return PhysicalOrigin + p * Scale; }
		glm::vec2 PhysicalToDesign(glm::vec2 p) const { return (p - PhysicalOrigin) / Scale; }
		Wui::WuiRect DesignRectToPhysical(const Wui::WuiRect& r) const;
	};

	// 计算视口:策略缩放 × DPI 系数;内容矩形扣安全区(物理内缩换算回设计单位)。
	WLD_API UiViewport ComputeUiViewport(const UiDesign& design, const UiSafeArea& safe, const UiSurface& surface);

	// 命中:物理坐标 → 设计坐标 → 落在设计矩形内。
	WLD_API bool HitTestDesign(const Wui::WuiRect& designRect, const UiViewport& viewport, glm::vec2 physicalPoint);

	// 锚点解析:父矩形(设计空间)→ 节点矩形(设计空间)。负尺寸钳到 0。
	WLD_API Wui::WuiRect ResolveAnchor(const UiAnchor& anchor, const Wui::WuiRect& parent);

	// 布局容器种类(注册制:内置这些,第三方可注册更多)。
	enum class UiLayoutKind : uint8_t
	{
		Absolute = 0,   // 子节点各自按锚点摆放
		Column,         // 纵向排布(带 Gap / Padding)
		Row,            // 横向排布
		Overlay,        // 所有子节点铺满父矩形(层叠)
		Grid,           // 按列数切格
		Flex,           // 复用 Wui::SolveFlex
	};

	const char* UiLayoutKindName(UiLayoutKind kind);
	bool ParseUiLayoutKind(std::string_view name, UiLayoutKind& out);

	struct UiLayoutSpec
	{
		UiLayoutKind Kind = UiLayoutKind::Absolute;
		float Gap = 0.0f;
		float Padding[4] = { 0.0f, 0.0f, 0.0f, 0.0f };  // 左 上 右 下
		int Columns = 2;                                  // Grid 用
		bool RowMajor = false;                            // Flex 的主轴:false = 纵,true = 横
		// M45:自动尺寸 —— 布局后按"内容"收紧节点矩形(文本 / 类型自报 / 子的包围盒,
		// 按该优先级取第一个有来源者)。只在**点锚定**(Min == Max)下生效;拉伸锚定下尺寸
		// 由锚框决定,不参与并记一条 warning。缺省 false = 与今天逐值一致(序列化省略默认值)。
		bool AutoSize = false;
	};

	WLD_API Wui::WuiRect ApplyPadding(const UiLayoutSpec& spec, const Wui::WuiRect& parent);
	WLD_API Wui::WuiRect SolveChildSlot(const UiLayoutSpec& spec, const Wui::WuiRect& parent,
		std::size_t index, std::size_t count);

	// 默认值判定(序列化省略默认节 + 容器子槽位是否被锚点覆盖,共用同一份口径)。
	WLD_API bool IsDefaultAnchor(const UiAnchor& anchor);
	WLD_API bool IsDefaultLayout(const UiLayoutSpec& spec);

	// 稳定 Id 生成:文档未写明 Id 时按"父路径/类型/序号"确定性推导。
	WLD_API std::string MakeStableId(std::string_view parentPath, std::string_view type, std::size_t index);
	// Id 合法性:非空、无 '/'、无空白、无坐标语义(纯逻辑身份)。
	WLD_API bool IsValidUiNodeId(std::string_view id);

	// ---- 滚动(工作包 M10;仅追加)----
	//
	// 判定口径**必须**与 `UiInputRouter::IsScrollContainer` 完全一致(同一份规则,
	// 两处由此保持同口径):`scrollable`/`scroll` 为真值,或 `overflow` ∈ {scroll, auto, Scroll}。
	// 真值集合与 `.wui` 既有 BoolProp 编码一致(1/true/True/yes/on);空串 = 属性缺席。
	WLD_API bool IsUiPropertyTruthy(std::string_view value);
	WLD_API bool IsUiScrollContainerProps(std::string_view scrollable, std::string_view scroll,
		std::string_view overflow);

	// 滚动偏移钳位:offset ∈ [0, max(0, contentSize - containerSize)]。
	// 内容不溢出(或容器非法)⇒ 恒为 0;见 `contract.ui-runtime` §3 与派工单 M10 口径。
	WLD_API float ClampUiScrollOffset(float offset, float contentSize, float containerSize);

	// ---- 主题令牌与样式继承(工作包 M14;仅追加)----
	//
	// 令牌 = 文档级 `Tokens: [{ Id, Props }]`(**优先**)或既有局部 `Styles: [{ Id, Props }]`
	// (兼容回退);两者同形,合并成一张令牌表。节点属性值形如 `$tokenId`(美元前缀,类比
	// 本地化 `@key`)时,在**绘制期**解析成字面值:
	//   `bg: $surface` → 取令牌 `surface` 在属性 `bg` 上的值(样式继承);
	//   令牌只声明一个属性时,该属性即令牌的"单值",`$surface` 在任何属性上都取它;
	//   令牌的值本身可以再是 `$other`(允许多级),因此**必须检测环**;
	//   `$x` 未定义 / 有环 / 引用非法 ⇒ 可读错误,**该属性按"未设置"处理**
	//   (回退到调用方的默认值,绝不静默变成空串或 0)。
	//
	// 口径(唯一解析点):解析只发生在 `UiPainter` 绘制期 —— 读取绘制/无障碍属性时逐值解析。
	// **布局不解析令牌**:`UiScreen::Layout`(锚点 / 容器槽位 / List·Grid 内容尺寸自报)只读
	// 数值属性,`$ref` 在那里等同"未设置"。令牌只影响绘制命令与无障碍文本。
	inline constexpr char kUiTokenPrefix = '$';

	// `$tokenId`:以 `$` 开头且去掉首尾空白后名字非空。`"$"` / `"$  "` / 字面值 = false。
	WLD_API bool IsUiTokenRef(std::string_view value);
	// `$` 之后的令牌名(去掉首尾空白);非令牌引用 = 空。
	WLD_API std::string_view UiTokenRefId(std::string_view value);

	// 令牌表查询:`propName` = 正在请求该值的属性名(样式继承的键)。
	// 返回令牌的**原始值**(可以还是 `$ref`);令牌未定义 / 没有该属性值 = nullptr。
	// 指针在令牌表存活期内有效;不得在回调返回后继续持有。
	using UiTokenFinder = std::function<const std::string*(std::string_view tokenId, std::string_view propName)>;

	struct UiTokenResult
	{
		std::string Value;      // 解析后的字面值;失败 = 空(调用方按"未设置"处理)
		std::string Chain;      // 解析链(诊断),如 "$a -> $b -> $c"
		std::string Error;      // 可读错误(未定义 / 环 / 引用非法 / 链过深);空 = 无错
		bool WasToken = false;  // 输入是 `$...` 引用
		bool Ok = false;        // 非令牌值,或令牌解析成功
	};

	// 解析链格式化(诊断/校验共用):["a","b"] → "$a -> $b"。
	WLD_API std::string FormatUiTokenChain(const std::vector<std::string>& chain);

	// 解析单个属性值:非令牌值原样返回(旧文档零变化);令牌值沿链解析并检测环。
	// `maxDepth <= 0` 时用默认上限 32(防御超长链)。失败时 `Value` 为空、`Error` 可读。
	WLD_API UiTokenResult ResolveUiTokenValue(std::string_view value, std::string_view propName,
		const UiTokenFinder& find, int maxDepth = 32);
}
