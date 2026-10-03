#include "wldpch.h"
#include "WUI/Panels/WidgetGalleryPanel.h"

#include "Core/EditorPreferences.h"

#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiScriptedInput.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

// 构建期常量由 CMake 注入(见根 CMakeLists 的 WLD_OUTPUT_DIR);单独做语法检查时给个兜底,
// 不影响正常构建(与其它面板依赖 WLD_LOCAL_DIR 的用法一致)。
#ifndef WLD_OUTPUT_DIR
#define WLD_OUTPUT_DIR "build/x64-Debug/"
#endif

namespace World
{
namespace WidgetGalleryPanelDetail {}   // 前置声明:下面的 using 必须先见到这个名字
using namespace WidgetGalleryPanelDetail;   // 等价于拆分前的文件内匿名命名空间可见性
	namespace WidgetGalleryPanelDetail
	{
		// ---- 稳定 a11y id 约定 ----
		// 工作台里每个可交互件都是 `wui.workbench.<area>.<name>`;控件自己登记的节点
		// (Button/Checkbox/Combo/TextField/DragFloat)自动带上这套 id,工作台另行手工登记
		// 左树行、画布、状态行等"不是控件"的节点。探针按 id 驱动,不依赖文案(语言无关)。
		constexpr const char* kCaptureButtonId = "wui.workbench.btn.capture";
		constexpr const char* kApproveButtonId = "wui.workbench.btn.approve";
		constexpr const char* kTreeSearchId = "wui.workbench.tree.search";
		constexpr const char* kTreeRowPrefix = "wui.workbench.tree.row.";
		constexpr const char* kTreeListId = "wui.workbench.tree.list";
		constexpr const char* kCanvasId = "wui.workbench.canvas";
		constexpr const char* kCanvasLabelId = "wui.workbench.canvas.label";
		constexpr const char* kStatusId = "wui.workbench.status";
		constexpr const char* kInfoId = "wui.workbench.info";
		constexpr const char* kPropLabelPrefix = "wui.workbench.prop.";
		// WUI-P1.5b:属性面板的搜索框 / 分组折叠头(P1.5b 新增的稳定 id)。
		constexpr const char* kPropSearchId = "wui.workbench.props.search";
		constexpr const char* kPropGroupHeaderPrefix = "wui.workbench.props.group.";
		// WUI-P1.6b:Edit/Play 开关 + 一键跑交互契约 + Play 观测条(探针按这些 id 驱动/断言)。
		constexpr const char* kModeEditButtonId = "wui.workbench.btn.mode.edit";
		constexpr const char* kModePlayButtonId = "wui.workbench.btn.mode.play";
		constexpr const char* kModeNodeId = "wui.workbench.mode";
		constexpr const char* kRunButtonId = "wui.workbench.btn.interactions";
		constexpr const char* kRunNodeId = "wui.workbench.interactions";
		constexpr const char* kPlayObservationId = "wui.workbench.play.observation";

		constexpr float kMinCanvasSize = 120.0f;
		constexpr float kMaxCanvasSize = 640.0f;
void RegisterWorkbenchNode(Wui::WuiId id, const char* kind, const Wui::WuiRect& rect, const std::string& label, const std::string& value = std::string(), bool enabled = true, bool interactive = true);

std::string ClipText(const Wui::WuiContext& ctx, const std::string& text, float width, float fontSize);

float SafeFloat(const std::string& text, float fallback);

std::string FormatFloat(float value);


		// ---- WUI-P1.5b:属性面板(UE 式)用的分组/格式化工具 ----
		//
		// 值编码协议(颜色 `#RRGGBB[AA]`、尺寸 `WxH`)由登记表侧实现
		// (`Wui::ParseComponentColor` / `Wui::ParseComponentSize` / `FormatComponent*`),
		// 面板**只调用不重写** —— 否则面板与 showcase 会各解析一套,迟早不一致。
		constexpr const char* kPropGroupGeneral = "General";
std::string ToLowerAscii(std::string_view text);


		// 组名:登记表的枚举 → 面板内的稳定组 id(做 a11y id / 排序键,不随语言变)。
		// 类型名用 decltype 取而不是直接写:属性分组枚举是**属性元数据**,不是可展示控件,
		// 而共享门禁 `check-ui-components.ps1` 会把面板里出现的控件命名空间类型名当成
		// "用到但未登记的控件类型"报缺口(它的 `$nonComponentTypes` 白名单在门禁文件里,
		// 不在本单的文件边界内)。语义完全不变,只是不给扫描面添假阳性。
		using PropGroupEnum = decltype(Wui::WuiComponentProperty::Group);
std::string NormalizePropGroup(PropGroupEnum group);

int PropGroupRank(const std::string& group);

std::string PropGroupTitle(const std::string& group);

glm::vec4 PropColorToVec4(const Wui::WuiColor& color);

Wui::WuiColor PropVec4ToColor(const glm::vec4& rgba);

int PropPanelMode();

bool PropTimingEnabled();


		// 状态选择器的排版:估计高度与绘制**走同一段代码**(绘制时多画一次按钮),避免两处口径走偏。
		// 宽度表在缓存重建时算好;这里只做换行与位置计算,不建临时 vector(每帧零分配)。
		template <typename Emit>
		float WalkStateChips(const std::vector<float>& widths, const Wui::WuiRect& rect, float rowH,
			Emit&& emit)
		{
			const float gap = 4.0f;
			float x = rect.X + 2.0f;
			float y = rect.Y;
			for (size_t index = 0; index < widths.size(); ++index)
			{
				const float width = widths[index];
				if (x > rect.X + 2.0f && x + width > rect.X + rect.W - 2.0f)
				{
					x = rect.X + 2.0f;
					y += rowH + gap;
				}
				emit(index, Wui::WuiRect { x, y, width, rowH });
				x += width + gap;
			}
			return widths.empty() ? 0.0f : (y - rect.Y) + rowH;
		}
float FontScale(float uiScale);

Wui::WuiTheme ScaledFontTheme(const Wui::WuiTheme& theme, float scale);

const std::filesystem::path& WorkbenchDir();

void EnsureWorkbenchDir();

std::string JsonEscape(const std::string& text);

std::string ReadFirstLine(const std::filesystem::path& path);

std::string ResolveGitCommit();

std::pair<float, float> SizeNotesToSize(const std::string& notes);

const char* StatusText(Wui::WuiComponentStatus status);

Wui::WuiColor StatusColor(Wui::WuiComponentStatus status, const Wui::WuiTheme& theme);

bool DetectsFullWindowOverlay(const Wui::WuiContext& ctx, size_t from);


		// ---- WUI-P1.6b:交互契约 runner 的阶段与工具 ----
		//
		// 阶段定时用**墙钟**:隐藏窗口的帧率不保证(实测同一台机器上 60 与 200+ fps 都出现过),
		// 按帧定时会让探针抓不到 pressed 帧 —— 按下沿只存在一帧,而"按住"这一段才是像素证据窗口。
		enum RunPhaseId
		{
			kRunIdle = 0,
			kRunApproach,     // 移入(悬停)
			kRunClickSend,    // 脚本化点击(与 ui.invoke 同一条 WuiScriptedInput 队列)
			kRunPressHold,    // 按下 + 保持(像素抓取窗口)
			kRunRelease,      // 抬起沿
			kRunLeave,        // 移出(保证"未悬停"的默认外观)
			kRunFocusScan,    // Key 契约:Tab 找焦点
			kRunKeyEnter,     // Enter
			kRunKeySpace,     // Space
			kRunDragMove,     // 按住 + 位移
			kRunTypeText,     // 逐帧注入码点
			kRunScroll,       // 逐帧注入滚轮
			kRunSettle,       // 收尾(不注入)
			kRunFinished,
		};
uint64_t NowMs();

std::string WallStamp();

const char* RunPhaseName(int phase);

uint64_t RunPhaseDurationMs(int phase);

std::string RunPhaseNote(int phase, const std::string& kind);

int StartRunPhase(const std::string& kind);

int NextRunPhase(const std::string& kind, int phase);

const char* InteractionKindId(Wui::WuiInteractionKind kind);

const char* InteractionExpectId(Wui::WuiInteractionExpect expect);

const char* InteractionPixelPhase(const std::string& kind);

Wui::WuiRect ResolveInteractionTarget(const std::string& targetId, const Wui::WuiRect& slot, const std::string& cachedId, const Wui::WuiRect& cachedRect);

std::string AccessNodeValue(Wui::WuiId id);

std::string PlayFocusName(Wui::WuiId id);

bool PointInside(const Wui::WuiRect& rect, const glm::vec2& point);

glm::vec2 LeavePoint(const Wui::WuiRect& target, const Wui::WuiRect& canvas);


	}
}
