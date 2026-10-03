#include "wldpch.h"
#include "World/WUI/WuiCodeEditor.h"

#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiWidgets.h"   // DrawFocusRing / CurrentTheme(焦点环与其它控件同一套 token)

#include <algorithm>
#include <cmath>

namespace World::Wui
{
namespace WuiCodeEditorDetail {}   // 前置声明:下面的 using 必须先见到这个名字
using namespace WuiCodeEditorDetail;   // 等价于拆分前的文件内匿名命名空间可见性
	namespace WuiCodeEditorDetail
	{
		constexpr float kScrollbarWidth = 10.0f;
		constexpr float kGutterPaddingLeft = 6.0f;
		constexpr float kGutterPaddingRight = 8.0f;
		constexpr float kThumbMinHeight = 24.0f;
		// M4-TEX-P6b:水平滚动条与竖向同一档尺寸(上下各留 2px 内边距 = thumb 高 6px)。
		constexpr float kThumbMinWidth = 24.0f;
		// caret 横向跟随 / 内容右内边距共用的一条口径:caret(或行尾)与可视区边缘之间至少保留的
		// 字符数。两处用同一个值,"滚到最右"时行尾也正好留出这么多,不会出现"跟随留 2 字符、
		// 上限只留 0"那种自相矛盾的口径。
		constexpr float kCaretScrollMarginChars = 2.0f;

		const WuiColor kBackground { 0.10f, 0.105f, 0.11f, 1.0f };
		const WuiColor kGutterBackground { 0.13f, 0.135f, 0.14f, 1.0f };
		const WuiColor kGutterText { 0.44f, 0.46f, 0.50f, 1.0f };
		const WuiColor kGutterTextCurrent { 0.78f, 0.81f, 0.85f, 1.0f };
		const WuiColor kCurrentLine { 1.0f, 1.0f, 1.0f, 0.05f };
		const WuiColor kCaretColor { 0.90f, 0.92f, 0.95f, 1.0f };
		const WuiColor kScrollbarTrack { 0.15f, 0.155f, 0.16f, 1.0f };
		const WuiColor kScrollbarThumb { 0.33f, 0.35f, 0.38f, 1.0f };
		const WuiColor kSuggestBackground { 0.145f, 0.152f, 0.165f, 0.985f };
		const WuiColor kSuggestBorder { 0.30f, 0.32f, 0.36f, 1.0f };
		const WuiColor kSuggestSelected { 0.20f, 0.30f, 0.46f, 1.0f };
		const WuiColor kSuggestName { 0.86f, 0.89f, 0.93f, 1.0f };
		// WUI-MAT-INTEL4:类型列用"类型色"(teal)而不是灰 —— 与名字(近白)一眼分开。
		const WuiColor kSuggestType { 0.31f, 0.79f, 0.69f, 1.0f };
		const WuiColor kSuggestDoc { 0.66f, 0.69f, 0.74f, 1.0f };
		// ---- MAT-UI6a:查找/替换条 + 同词高亮的配色(与正文同一条暗色系,不引入主题依赖)----
		const WuiColor kFindBarBackground { 0.135f, 0.142f, 0.152f, 0.995f };
		const WuiColor kFindBarButton { 0.19f, 0.20f, 0.22f, 1.0f };
		const WuiColor kFindBarButtonHover { 0.26f, 0.28f, 0.31f, 1.0f };
		const WuiColor kFindBarButtonActive { 0.20f, 0.34f, 0.52f, 1.0f };
		const WuiColor kFindBarText { 0.80f, 0.83f, 0.87f, 1.0f };
		const WuiColor kFindBarTextActive { 0.92f, 0.95f, 0.99f, 1.0f };
		const WuiColor kFindBarNoMatch { 0.92f, 0.55f, 0.50f, 1.0f };
		// MAT-UI6c:空查询时框内的占位文案(比 kFindBarText 低一档,不与真实输入混淆)。
		const WuiColor kFindBarPlaceholder { 0.52f, 0.55f, 0.60f, 1.0f };
		constexpr float kFindBarRowHeight = 22.0f;
		constexpr float kFindBarPad = 4.0f;
		constexpr float kFindBarGap = 3.0f;
		constexpr float kFindBarFontSize = 13.0f;
		// 同词高亮:当前词一档更强(可辨的两级底色)。
		const WuiColor kOccurrenceStrong { 0.26f, 0.42f, 0.62f, 0.65f };
		const WuiColor kOccurrenceWeak { 0.34f, 0.37f, 0.42f, 0.32f };
		// WUI-MAT-INTEL4 配色(VS Code Dark+ 风格):名字(Default)保持中性;类型=青绿、函数=暖黄、
		// 字段/全局=浅蓝、注解关键字=紫 —— 与关键字(蓝)/字符串/数字/注释/运算符分开。
		const WuiColor kTokenColors[16] = {
			{ 0.83f, 0.83f, 0.83f, 1.0f },   // Default(变量/名字)
			{ 0.34f, 0.61f, 0.84f, 1.0f },   // Keyword
			{ 0.81f, 0.57f, 0.47f, 1.0f },   // String
			{ 0.42f, 0.60f, 0.33f, 1.0f },   // Comment
			{ 0.71f, 0.81f, 0.66f, 1.0f },   // Number
			{ 0.61f, 0.86f, 1.00f, 1.0f },   // Global
			{ 0.83f, 0.83f, 0.83f, 1.0f },   // Operator
			{ 0.31f, 0.79f, 0.69f, 1.0f },   // Type(类型标识:teal)
			{ 0.86f, 0.86f, 0.67f, 1.0f },   // Function(函数:暖黄)
			{ 0.67f, 0.84f, 0.94f, 1.0f },   // Field(成员/字段:浅蓝)
			{ 0.77f, 0.53f, 0.75f, 1.0f },   // Annotation(注解关键字:紫)
			{ 0.34f, 0.71f, 0.76f, 1.0f },   // Constant(true/false/nil)
			{ 0.77f, 0.53f, 0.75f, 1.0f },   // Self(self)
			{ 0.67f, 0.84f, 0.94f, 1.0f },   // Parameter
			{ 0.63f, 0.66f, 0.70f, 1.0f },   // Punctuation
			// VEC-A7:引擎外部类名(vec2/vec3/vec4/mat3/mat4/Entity/WorldScript)= 柔紫,
			// 与 Keyword 蓝 / Type 青 / Function 暖黄 / Global 浅蓝 / Annotation 紫红都可区分。
			{ 0.70f, 0.62f, 0.95f, 1.0f },   // EngineType(引擎外部类)
		};

		// W9.5 补全浮层:最多 12 行可滚动 + 底部一行 Doc。
		constexpr int kSuggestMaxRows = 12;
		constexpr float kSuggestRowHeight = 22.0f;
		constexpr float kSuggestWidth = 320.0f;
		constexpr float kSuggestFontSize = 13.0f;
		constexpr float kSuggestDocFontSize = 12.0f;
float ClampZoom(float zoom);

float FindBarHeight(bool visible, bool replaceMode, bool readOnly);


		struct WuiCodeEditorState
		{
			float ScrollY = 0.0f;
			// ---- M4-TEX-P6b:水平滚动(像素) ----
			// 0 = 未滚。只有"内容宽(行号槽 + 最长行像素宽 + 右内边距)"超过"可视宽"时才可能 > 0;
			// 装得下时被夹回 0(见布局段与 caret 跟随段)。绘制/命中/光标/浮层锚点全部按 -ScrollX 平移。
			float ScrollX = 0.0f;
			bool DraggingHThumb = false;
			float HThumbGrabOffset = 0.0f;
			// 最长行像素宽缓存:键 = (文本版本, 生效字号, 单字宽)。单字宽随 UiScale 变(度量钩子按
			// 设计单位回答),所以缩放拖动时缓存自动失效;正文没动时不会逐帧全文件重新度量。
			float MaxLineWidth = 0.0f;
			uint64_t MaxLineWidthRevision = 0;
			float MaxLineWidthFontSize = 0.0f;
			float MaxLineWidthDigit = 0.0f;
			bool MaxLineWidthValid = false;
			bool MouseSelecting = false;
			bool DraggingThumb = false;
			float ThumbGrabOffset = 0.0f;
			bool FollowCaret = true;
			// ---- W9.5 补全浮层(状态必须跨帧:打开/选中/滚动)----
			bool PopupVisible = false;
			std::size_t ReplaceStart = 0;   // 前缀起点(buffer 字节偏移)
			std::size_t CaretAtOpen = 0;    // 弹出时的 caret(buffer 字节偏移)
			std::vector<World::LuauCompletionItem> PopupItems;
			std::string PopupLinePrefix;    // 打开浮层时的 linePrefix(无障碍状态用)
			float PopupAnchorX = 0.0f;      // 浮层锚点(caret 左上角),打开时固定避免逐帧漂移
			float PopupAnchorY = 0.0f;
			int PopupSelected = 0;
			int PopupScroll = 0;
			// Enter 是否算"接受":点号/冒号、Ctrl+Space 或手动 Up/Down 之后为 true;
			// 普通打字自动弹出的浮层里 Enter 仍是换行(避免吞换行/把 Enter 当接受)。
			bool PopupAcceptEnter = false;
			bool PopupVisibleLastFrame = false;
			WuiRect PopupBounds {};
			// ---- W9.6 悬停提示(名称/类型/文档) ----
			bool HoverVisible = false;
			uint64_t HoverSinceFrame = 0;
			std::size_t HoverWordStart = 0;   // buffer 字节偏移
			std::size_t HoverWordEnd = 0;
			glm::vec2 HoverMousePos { 0, 0 };
			std::string HoverName;
			std::string HoverType;
			std::string HoverDoc;
			// W9.8:补全函数参数占位(交替 start/end buffer 偏移)与当前选中的参数序号。
			std::vector<std::size_t> SnippetRanges;
			std::size_t SnippetIndex = 0;
			// ---- MAT-UI6a:会话缩放(内核持有;初值取 options.UiZoom,"只有第一次"生效)----
			float UiZoom = 1.0f;
			bool ZoomSeeded = false;
			// ---- MAT-UI6a:查找/替换条 ----
			bool FindVisible = false;
			bool FindReplaceMode = false;
			std::string FindQuery;         // 查找输入框(TextField 直接写这里,跨帧保留)
			std::string FindReplaceQuery;  // 替换输入框
			// 命中缓存:只在"查询/开关/文本版本"变化时重扫(节流;大文件不逐帧全扫)。
			std::vector<WuiCodeFindMatch> FindMatches;
			std::string FindScannedQuery;
			bool FindScannedCase = false;
			bool FindScannedWord = false;
			uint64_t FindScannedRevision = 0;
			bool FindScanned = false;
			int FindCurrent = -1;      // 当前命中下标(FindMatches 内;-1 = 无)
			size_t FindAnchor = 0;     // 打开/上次导航落点:重扫后从这里往后挑当前命中
			// MAT-UI6d:上一帧的代码区选区(码点索引)。条内再按 Ctrl+F 时用它区分
			// "用户刚做出的新选区"(重播种)与"上一次查找留下的旧选区"(不重播种,否则
			// 会把用户刚在查找框里改好的查询改写回旧文字)。
			int LastSelectionStart = -1;
			int LastSelectionEnd = -1;
			// 同词高亮缓存:键 = (词, 文本版本, 大小写口径)。
			std::vector<WuiCodeFindMatch> OccurrenceMatches;
			std::string OccurrenceWord;
			size_t OccurrenceWordStart = 0;
			size_t OccurrenceWordEnd = 0;
			std::string OccurrenceScannedWord;
			uint64_t OccurrenceScannedRevision = 0;
			bool OccurrenceScannedCase = false;
			bool OccurrenceScanned = false;
		};
size_t CodepointLength(unsigned char lead);

uint32_t DecodeCodepoint(std::string_view text, size_t offset, size_t& length);

void EncodeUtf8(std::string& out, uint32_t codepoint);

bool IsWordCodepoint(uint32_t codepoint);

bool IsIdentByte(char c);

bool IsBlankByte(char c);

bool IsAnnotationContextBody(std::string_view body);

bool InsertAsCall(const World::LuauCompletionItem& item);

std::string CompletionInsertName(std::string name);

std::string TruncateBytes(const std::string& text, std::size_t maxBytes);

size_t OffsetAtX(const WuiContext& ctx, std::string_view line, float x, float fontSize);

void SelectWordAt(WuiTextBuffer& buffer, const std::string& text, size_t offset);

std::string_view LineView(const WuiTextBuffer& buffer, int line, size_t& lineStart);

std::size_t CompletionReplaceStart(std::string_view text, std::size_t caret);

std::size_t PrevBoundary(std::string_view text, std::size_t offset);

std::string EllipsizeToWidth(const WuiContext& ctx, std::string_view text, float maxWidth, float fontSize);

std::vector<std::string> WrapToWidth(const WuiContext& ctx, std::string_view text, float maxWidth, float fontSize, size_t maxLines);

char FoldAsciiChar(char c);

bool MatchAt(std::string_view text, size_t at, std::string_view needle, bool caseSensitive);

bool WholeWordAt(std::string_view text, size_t start, size_t end);

void WordAtOffset(std::string_view text, size_t offset, size_t& wordStart, size_t& wordEnd);

bool IsIdentifierRange(std::string_view text, size_t start, size_t end);

	}
std::vector<WuiCodeFindMatch> FindCodeMatches(std::string_view text, std::string_view needle, const WuiCodeFindOptions& options);

int NextCodeMatchIndex(const std::vector<WuiCodeFindMatch>& matches, size_t fromOffset);

int PrevCodeMatchIndex(const std::vector<WuiCodeFindMatch>& matches, size_t fromOffset);


	namespace WuiCodeEditorDetail
	{
bool SeedFindQueryFromSelection(std::string_view text, size_t selStart, size_t selEnd, std::string& out);

bool SelectionIsCurrentFindMatch(const WuiCodeEditorState& state, size_t selStart, size_t selEnd);

bool ApplyFindQueryFromSelection(WuiCodeEditorState& state, const WuiTextBuffer& buffer);

	}
WuiCodeEditorResult CodeEditor(WuiContext& ctx, WuiId id, const WuiRect& rect, WuiTextBuffer& buffer, const WuiCodeEditorOptions& options);

}
