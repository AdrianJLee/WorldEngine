#pragma once

// ============================================================================
// WorldEngine 插件 ABI(v1)—— 公共接口的唯一事实源。
//
// 纪律(与 World/Modules/WeModule.h 同一套,但**版本号独立**):
//   * 只含 C 类型与函数指针:不跨边界传 STL / 原始 C++ 对象;
//   * 每个结构以 `StructSize + AbiVersion` 开头,宿主与插件**双向**校验;
//   * **字段只增不改号**:尾部追加字段 = StructSize 增长(不升 AbiVersion);
//     语义变更 = 升 WE_PLUGIN_ABI_VERSION(旧插件被干净拒绝,不按新布局解释)。
//     ⇒ 判据是"**声明的大小覆盖对方要读的前缀**":宿主接受 `plugin.StructSize >= 已知 v1 前缀`
//       (插件用更新的头编译、尾部字段更多时合法);小于该前缀 = 干净拒绝。
//       宿主侧将来追加字段后,读新字段前必须逐项检查 `plugin.StructSize` 是否覆盖该字段。
//
// 与 Game 模块的关系:`WE_MODULE_ABI_VERSION`(Game 模块契约)保持独立演进;
// 插件加载器复用 ModuleManager 底层的 DynamicLibrary 与"等值门"思路,不复制第二套实现。
//
// T2(2026-09-30)在**尾部**追加了宿主注册面(资产类型 / 导入器 / LookupExport):
// 字段只增不改号、`StructSize` 随成员增长,`WE_PLUGIN_ABI_VERSION` 不变 ——
// 旧插件对新宿主仍兼容;新插件对旧宿主必须用 `StructSize >= offsetof(字段) + sizeof(字段)` 自检。
//
// T2b(2026-09-30)继续在**尾部**追加组件 schema 注册面(RegisterComponent /
// UnregisterComponent + WeComponentDesc / WeComponentFieldDesc):同一个纪律,
// 仍然不改既有字段、不升 `WE_PLUGIN_ABI_VERSION`。
//
// T3b(2026-09-30)继续在**尾部**追加编辑器扩展注册面(RegisterEditorCommand /
// UnregisterEditorCommand + RegisterEditorPanel / UnregisterEditorPanel,以及
// 面板 Draw 拿到的小组件表 WeEditorUiApi):同一个纪律,不改既有字段、不升 ABI。
//
// 版本同步点(升级时必须一起改):本文件、`plugin.we.yaml` 的 `abi`、
// `Engine/src/World/Plugins/PluginManager.*`、`docs/dev/plugin-framework.md`。
// 方案:`tools/agents/tasks/20260930-1100-plugin-framework/plan.md`(v2.1)。
// ============================================================================

#include <cstdint>

namespace World
{
	class WorldContext;
}

namespace World::Plugins
{
	// 插件 ABI 版本(独立于 Game 模块的 WE_MODULE_ABI_VERSION)。
	constexpr uint32_t WE_PLUGIN_ABI_VERSION = 1;

	// 导出入口符号名(清单 `entry:` 可覆盖;默认即此名)。
	constexpr const char* WE_PLUGIN_QUERY_SYMBOL = "WePluginQuery";

	// 宿主日志等级(WeHostApi::Log 的 level 取值)。
	enum WePluginLogLevel : int
	{
		WePluginLogInfo = 0,
		WePluginLogWarn = 1,
		WePluginLogError = 2,
	};

	// 插件导出的 C++ 函数库条目(`provides: [cxx.exports]` 的载体)。
	// Function 的解释由调用方与被调用方按 Name+Version 约定,宿主不解析。
	struct WePluginExport
	{
		const char* Name = nullptr;
		uint32_t Version = 0;
		void* Function = nullptr;
	};

	// ---- T2:宿主注册面(资产类型 / 导入器 / C++ 导出查询)--------------------------------
	//
	// 纪律(与 WeHostApi 的 append-only 规则配套):
	//   * 每个回调都带 `void* userData`(插件自己的句柄),宿主只原样回传,不解释;
	//   * 重复注册同 id = 返回 false + 可读日志(不覆盖);注册方必须检查返回值;
	//   * 注销幂等:注销"本插件没注册过"的 id = 返回 true 且不报错;
	//   * 插件卸载时宿主会兜底回收该插件仍注册着的项(记警告)—— 插件不应依赖这一点,
	//     Register/Unregister 必须成对,失败路径自己清干净。

	// 资产"新建"回调(WeAssetTypeDesc::Create)。
	//   * directoryUtf8 = 目标目录绝对路径(UTF-8,仅调用期间有效);
	//   * 成功 = 至少落一份磁盘产物(失败不得留半成品)并返回 true;
	//   * 失败 = 返回 false,并把可读原因写进 errorBuffer(UTF-8、含结尾 0,可截断)。
	using WeAssetTypeCreateFn = bool (*)(void* userData, const char* directoryUtf8,
		char* errorBuffer, uint32_t errorCapacity);

	// 资产类型描述(T2):只含 C 类型;宿主侧映射到 World::AssetTypeDesc 一次注册。
	struct WeAssetTypeDesc
	{
		uint32_t StructSize = sizeof(WeAssetTypeDesc);
		uint32_t AbiVersion = WE_PLUGIN_ABI_VERSION;

		const char* Id = nullptr;         // 稳定 id(必需;空 = 拒绝注册)
		const char* Label = nullptr;      // 内联显示名(可空 = Id)
		const char* Term = nullptr;       // 术语/别名(可空 = Label)
		const char* Extension = nullptr;  // 默认扩展名(".whello";可空 = 无)
		uint64_t Icon = 0;                // 图标纹理 id(0 = 无图标)
		int32_t SortOrder = 100;          // 菜单排序(与 AssetTypeDesc::SortOrder 同口径)
		int32_t IsFolder = 0;             // 非 0 = 文件夹语义
		WeAssetTypeCreateFn Create = nullptr;  // 可空 = 只声明类型、不能"新建"
		void* UserData = nullptr;         // 只回传给 Create
	};

	// 导入产物写出面(Import 调用期间由**宿主**提供;插件只调用,不得保留 sink、
	// UserData 或 bytes 指针)。
	//   * logicalPathUtf8 非空 = 多产物之一(相对 content_root 的逻辑路径);
	//     空 = 单产物(宿主映射到 ImportResult::Data;单产物只能写一次);
	//   * 返回 false = 宿主拒收(插件应让本次 Import 整体失败)。
	struct WeImportSink
	{
		uint32_t StructSize = sizeof(WeImportSink);
		uint32_t AbiVersion = WE_PLUGIN_ABI_VERSION;
		void* UserData = nullptr;
		bool (*Write)(void* userData, const char* logicalPathUtf8, const void* bytes,
			uint64_t size) = nullptr;
	};

	// 匹配回调:sourceUtf8 = 源文件绝对路径(UTF-8);非 0 = 本导入器接手(顺序同内建导入器,
	// 谁是"第一个匹配"由宿主交给 CookPipeline 的顺序决定)。
	using WeAssetImporterMatchesFn = bool (*)(void* userData, const char* sourceUtf8);
	// 导入回调:成功 = 至少向 sink 写一份产物并返回 true;失败 = 返回 false,
	// errorBuffer(UTF-8、含结尾 0、可截断)带可读原因。
	using WeAssetImportFn = bool (*)(void* userData, const char* logicalPathUtf8,
		const char* sourceUtf8, const WeImportSink* sink, char* errorBuffer, uint32_t errorCapacity);
	// 逐源设置指纹(可空 = 0 = 该导入器没有逐源设置;与 IAssetImporter::SettingsFingerprint 同语义)。
	using WeAssetImporterFingerprintFn = uint64_t (*)(void* userData, const char* sourceUtf8);

	// 资产导入器描述(T2)。Extensions 只是诊断/展示元数据(插件管理器用);
	// "谁接手"始终以 Matches 回调为准,宿主不替插件判扩展名。
	struct WeAssetImporterDesc
	{
		uint32_t StructSize = sizeof(WeAssetImporterDesc);
		uint32_t AbiVersion = WE_PLUGIN_ABI_VERSION;

		const char* Id = nullptr;              // 注册键 + 诊断名(必需;插件间唯一)
		const char* DisplayName = nullptr;     // 显示名(可空 = Id)
		const char* const* Extensions = nullptr;  // 诊断元数据(".whello";可空)
		uint32_t ExtensionCount = 0;
		uint32_t Version = 1;                  // 导入器版本(cook 复合指纹的参与项)
		WeAssetImporterMatchesFn Matches = nullptr;      // 必需
		WeAssetImportFn Import = nullptr;                // 必需
		WeAssetImporterFingerprintFn SettingsFingerprint = nullptr;  // 可空
		void* UserData = nullptr;              // 只回传给上述回调
	};

	// ---- T2b:组件 schema 注册面 --------------------------------------------------------
	//
	// 组件注册 = 插件把**自己的组件布局**告诉宿主:每个字段给 (Kind, Offset, Size),
	// 宿主据此生成 `Schema::FieldSchema` 并按偏移读写实例内存 —— 插件不必链接 World,
	// 也不必把组件的 C++ 类型交给宿主。实例指针由消费方(插件自己的存储 / 会话 / 测试)提供。
	//
	// 语义边界(T2b):
	//   * 注册产物 = `Schema::SchemaRegistry` 里的 `TypeCategory::Component` 类型
	//     (List/Find/序列化 API 可见);**没有 entt 存储绑定**(Storage == nullptr),
	//     所以该组件暂时不能挂到场景实体上(存储桥不在本版 ABI 里,见下面的 ComponentId);
	//   * 支持的 Kind = 固定大小 POD(布尔/整型/浮点/向量/四元数/矩阵,见下表);
	//     String / Enum / Asset / Object 与容器字段 = 干净拒绝(返回 false + 可读日志);
	//   * 字段名在同一个组件内必须唯一(它同时是 `.wd`/YAML 的键)。

	// 字段类型。**数值 = `World::Schema::Kind` 的稳定值**(Engine/src/World/Schema/Schema.h;
	// PluginManager.cpp 用 static_assert 钉住这份映射)—— 追加新类型只能排在末尾。
	enum WeComponentFieldKind : uint32_t
	{
		WeComponentKindNone = 0,
		WeComponentKindBool,
		WeComponentKindInt8, WeComponentKindInt16, WeComponentKindInt32, WeComponentKindInt64,
		WeComponentKindUInt8, WeComponentKindUInt16, WeComponentKindUInt32, WeComponentKindUInt64,
		WeComponentKindFloat, WeComponentKindDouble,
		WeComponentKindVec2, WeComponentKindVec3, WeComponentKindVec4,
		WeComponentKindIVec2, WeComponentKindIVec3, WeComponentKindIVec4,
		WeComponentKindUVec2, WeComponentKindUVec3, WeComponentKindUVec4,
		WeComponentKindQuat, WeComponentKindMat3, WeComponentKindMat4,
		// 以下类型 T2b 不支持注册(声明在这里只为与 Schema::Kind 对齐值):
		WeComponentKindString, WeComponentKindEnum, WeComponentKindAsset, WeComponentKindObject,
	};

	// 字段的编辑期提示(只影响属性面板怎么显示,不进序列化);未知位 = 忽略(前向兼容)。
	enum WeComponentFieldFlags : uint32_t
	{
		WeComponentFieldFlagNone = 0,
		WeComponentFieldFlagColor = 1u << 0,      // Vec3/Vec4 = 颜色 → 取色器
		WeComponentFieldFlagReadOnly = 1u << 1,   // 只读行
	};

	// 组件字段描述(只含 C 类型)。Offset/Size 用 offsetof/sizeof 填:Offset 是字段在插件
	// 组件结构里的字节偏移,Size 必须等于 Kind 的规范大小(否则注册被拒绝 —— 宿主不做
	// "按声明大小瞎 memcpy" 的事)。宿主不校验组件结构的总大小(布局由插件拥有)。
	struct WeComponentFieldDesc
	{
		uint32_t StructSize = sizeof(WeComponentFieldDesc);
		uint32_t AbiVersion = WE_PLUGIN_ABI_VERSION;

		const char* Name = nullptr;                 // 字段名(必需;同一组件内唯一)
		uint32_t Kind = WeComponentKindNone;        // WeComponentFieldKind
		uint32_t Offset = 0;                        // 字节偏移(插件用 offsetof 填)
		uint32_t Size = 0;                          // 字节数(必须等于 Kind 的规范大小)
		uint32_t Flags = WeComponentFieldFlagNone;  // WeComponentFieldFlags(编辑期提示)
		const char* const* Choices = nullptr;       // 固定取值下拉(可空)
		uint32_t ChoicesCount = 0;
		const char* Doc = nullptr;                  // 行悬停提示(英文 canonical,可空)
	};

	// 组件类型描述(只含 C 类型)。Id = 类型全名(如 `com.example.foo.Health`;宿主用它
	// Find/ListByModule,也是 `.wd` 里的类型键)。
	struct WeComponentDesc
	{
		uint32_t StructSize = sizeof(WeComponentDesc);
		uint32_t AbiVersion = WE_PLUGIN_ABI_VERSION;

		const char* Id = nullptr;                   // 类型全名(必需;空 = 拒绝注册)
		const char* DisplayName = nullptr;          // 可空 = Id
		// T2b **必须为 0**:组件存储(entt 桥)不在本版 ABI 里;非 0 = 干净拒绝并给可读诊断。
		// 将来追加 Add/Copy 回调后,这里才作为组件存储 id 启用(字段只增不改号,不升 ABI)。
		uint32_t ComponentId = 0;
		const WeComponentFieldDesc* Fields = nullptr;  // FieldCount == 0 时可为空
		uint32_t FieldCount = 0;
	};

	// 宿主能力表(函数指针表;T1 含最小集合,T2 起注册面**尾部追加**)。
	// ---- T3b:编辑器扩展(命令 / 面板)-----------------------------------------------
	//
	// 语义:
	//   * 命令 = 具名动作,进宿主的编辑器命令面;AI 通道用 `plugin.command.run <id>` 触发
	//     (宿主命令名 = plugin.command.<pluginId>.<id>),插件面板里的按钮走
	//     WeEditorUiApi::InvokeCommand 触发同一条执行路径(宿主统计成功次数)。
	//   * 面板 = 独立窗口形态(与插件管理器同款:默认附加到主窗口的标签,可拖出为
	//     独立 OS 窗口);宿主在渲染时调用 WeEditorPanelDesc::Draw,并把**只读上下文**
	//     (WeEditorUiState:插件 id + 面板 id)与小组件表(WeEditorUiApi)交给插件。
	//   * Draw 是**只读渲染**:不得在 Draw 里改宿主状态;按钮点击等交互要经
	//     WeEditorUiApi::InvokeCommand 回到宿主命令路径。
	//   * 首次版本的小组件表:Label / Button / Separator / Checkbox(不做全量 WUI 暴露)。
	//     控件的 id 由宿主按面板命名空间隔离,插件只需给面板内的唯一短 id
	//     (a11y id 形如 `plugin.panel.<pluginId>.<控件 id>`)。
	//   * 禁用/被拒绝的插件不参与:宿主在面板能打开之前就调用 RegisterEditorPanel。
	//   * 卸载路径:PluginManager 调用 UnregisterEditorCommand / UnregisterEditorPanel,
	//     卸载**回收**该插件注册的全部命令与面板(不留悬空回调)。
	//
	// 反向约束(与 HostApi 其余注册面一致):注册失败 = 返回 false + 可读日志;
	// 插件必须在 Register 里检查返回值;Unregister 幂等(注销"本插件没注册过"的 id = true)。

	// 插件面板里的单值 / 动作控件(只回传 bool 结果;文本 / 状态由插件自己保存):
	//   * Button 返回 true = 本帧被点击(按下并释放命中);
	//   * Checkbox 返回 true = 本帧被点击改值(新值写回 *value;值由插件自己保存)。
	using WeEditorUiLabelFn = void (*)(void* uiContext, const char* text);
	using WeEditorUiButtonFn = bool (*)(void* uiContext, const char* id, const char* label);
	using WeEditorUiSeparatorFn = void (*)(void* uiContext);
	using WeEditorUiCheckboxFn = bool (*)(void* uiContext, const char* id, const char* label, bool* value);
	// 从面板里触发宿主命令面里的一条命令(宿主按命令名解析;成功 = true)。
	// 用于"按钮 → 命令"：插件面板把按钮点击转成命令调用,执行统计与 AI 通道同源。
	using WeEditorUiInvokeCommandFn = bool (*)(void* uiContext, const char* commandId);

	// 首期小组件表(宿主提供;函数指针可为 null = 该控件不可用)。
	struct WeEditorUiApi
	{
		uint32_t StructSize = sizeof(WeEditorUiApi);
		uint32_t AbiVersion = WE_PLUGIN_ABI_VERSION;
		// 本面板的只读上下文句柄(宿主创建 / 销毁;插件只原样回传给上面的函数)。
		void* UserData = nullptr;
		// 只读上下文字符串(插件 id;仅调用期间有效,插件不得保留)。
		const char* PluginId = nullptr;
		// 控件绘制:宿主按 Draw 调用顺序垂直排列(控件 id 由宿主按面板命名空间隔离)。
		WeEditorUiLabelFn Label = nullptr;
		WeEditorUiButtonFn Button = nullptr;
		WeEditorUiSeparatorFn Separator = nullptr;
		WeEditorUiCheckboxFn Checkbox = nullptr;
		// 命令面(可空 = 宿主未接命令面;面板按钮据此给可读提示而不是静默失败)。
		WeEditorUiInvokeCommandFn InvokeCommand = nullptr;
	};

	// 命令描述(只含 C 类型)。Callback 由宿主在命令面里调用(命令被执行时);
	// 回调**不得让异常跨过 ABI 边界**(抛异常 = 契约违约,宿主记错误并继续运行)。
	using WeEditorCommandCallback = void (*)(void* userData);
	struct WeEditorCommandDesc
	{
		uint32_t StructSize = sizeof(WeEditorCommandDesc);
		uint32_t AbiVersion = WE_PLUGIN_ABI_VERSION;

		const char* Id = nullptr;        // 插件内唯一(必需;空 = 拒绝注册)
		const char* Label = nullptr;     // 显示名(可空 = Id)
		const char* Tooltip = nullptr;   // 悬停提示(可空)
		WeEditorCommandCallback Callback = nullptr;  // 必需(空 = 拒绝注册)
		void* UserData = nullptr;        // 只回传给 Callback
	};

	// 面板 Draw 回调:`uiContext` = 本面板的只读上下文句柄(回传给 WeEditorUiApi 的函数);
	// `ui` = 宿主提供的小组件表(仅本次调用期间有效,插件不得保留);`userData` = 注册时给的句柄。
	// 返回 0 = 正常;非 0 = 契约违约(宿主把该面板判为坏面板)。
	// Draw 不得让异常跨过 ABI 边界(抛异常 = 契约违约,宿主停止渲染该面板并记错误)。
	using WeEditorPanelDrawFn = int (*)(void* userData, const WeEditorUiApi* ui, void* uiContext);
	struct WeEditorPanelDesc
	{
		uint32_t StructSize = sizeof(WeEditorPanelDesc);
		uint32_t AbiVersion = WE_PLUGIN_ABI_VERSION;

		const char* Id = nullptr;       // 插件内唯一(必需;空 = 拒绝注册)
		const char* Title = nullptr;    // 窗口/标签标题(可空 = Id)
		WeEditorPanelDrawFn Draw = nullptr;  // 必需(空 = 拒绝注册)
		void* UserData = nullptr;       // 只回传给 Draw
	};

	struct WeHostApi
	{
		uint32_t StructSize = sizeof(WeHostApi);
		uint32_t AbiVersion = WE_PLUGIN_ABI_VERSION;
		// 宿主私有指针(插件只应原样回传给宿主提供的函数)。
		void* UserData = nullptr;
		// 日志(宿主负责加插件身份前缀与落盘;message 为 UTF-8,调用方不保留所有权)。
		void (*Log)(void* userData, int level, const char* message) = nullptr;

		// ---- T2 追加(插件必须先自检 StructSize 覆盖到对应字段再调用)----
		// 资产类型注册/注销(宿主侧 → World::AssetTypeRegistry;重复 id = false)。
		bool (*RegisterAssetType)(void* userData, const WeAssetTypeDesc* desc) = nullptr;
		bool (*UnregisterAssetType)(void* userData, const char* id) = nullptr;
		// 导入器注册/注销(宿主侧 → 交给 CookPipeline 的导入器清单;重复 id = false)。
		bool (*RegisterAssetImporter)(void* userData, const WeAssetImporterDesc* desc) = nullptr;
		bool (*UnregisterAssetImporter)(void* userData, const char* id) = nullptr;
		// 按插件 id + 名字 + 最低版本查已加载插件的 C++ 导出表(WePlugin::Exports);
		// 未命中 = nullptr(查询失败是正常分支,不记日志)。
		void* (*LookupExport)(void* userData, const char* pluginId, const char* name,
			uint32_t minVersion) = nullptr;

		// ---- T2b 追加(组件 schema;同上:先自检 StructSize 覆盖到对应字段再调用)----
		// 注册/注销组件类型。重复 Id = false + 警告(不覆盖);注销"本插件没注册过"的 Id =
		// 若该类型属于别的插件 = false + 警告,否则 = true(幂等);字段非法(不支持的 Kind /
		// Size 与 Kind 不符 / 重名 / 空名)= false + 可读警告(不半注册)。
		// 注册成功的类型在插件卸载时由宿主兜底注销(未被插件自己注销则记警告)。
		bool (*RegisterComponent)(void* userData, const WeComponentDesc* desc) = nullptr;
		bool (*UnregisterComponent)(void* userData, const char* id) = nullptr;

		// ---- T3b 追加(编辑器扩展;同上:先自检 StructSize 覆盖到对应字段再调用)----
		// 注册/注销编辑器命令。重复 Id(本插件内)= false + 警告(不覆盖);
		// 注销"本插件没注册过"的 Id = true(幂等);命令在插件卸载时由宿主兜底注销。
		bool (*RegisterEditorCommand)(void* userData, const WeEditorCommandDesc* desc) = nullptr;
		bool (*UnregisterEditorCommand)(void* userData, const char* id) = nullptr;
		// 注册/注销编辑器面板(独立窗口形态)。语义同上(重复 Id / 幂等注销 / 卸载兜底)。
		bool (*RegisterEditorPanel)(void* userData, const WeEditorPanelDesc* desc) = nullptr;
		bool (*UnregisterEditorPanel)(void* userData, const char* id) = nullptr;
	};

	// 插件描述 + 生命周期回调 + 导出表。
	struct WePlugin
	{
		uint32_t StructSize = sizeof(WePlugin);
		uint32_t AbiVersion = WE_PLUGIN_ABI_VERSION;

		// 身份(必须与清单 `id` 一致;宿主以清单为准,不一致 = 拒绝加载)。
		const char* Id = nullptr;
		const char* Name = nullptr;
		const char* Version = nullptr;          // "1.0.0"(semver,仅诊断/依赖用)
		const char* Publisher = nullptr;        // 预留(不强制)
		const char* MinEngineVersion = nullptr; // 预留(与清单 `engine:` 比对)

		// 能力 id 列表(null 结尾的字符串数组;与清单 `provides:` 比对,缺声明 = 警告)。
		const char* const* Provides = nullptr;
		uint32_t ProvidesCount = 0;

		// C++ 函数库导出表(可为空 = 纯行为插件)。
		const WePluginExport* Exports = nullptr;
		uint32_t ExportCount = 0;

		// 生命周期契约:
		//   * Register 返回 true = 注册成功;返回 false = 插件**必须已自行清理**本次注册的
		//     任何部分状态(契约:false 之后宿主**不会**再调 Unregister,防二次释放);
		//   * Register 不得让异常跨过 ABI 边界(抛异常 = 契约违约:宿主按 best-effort 调
		//     Unregister 回滚后再拒绝该插件);
		//   * Unregister 必须销毁插件拥有的一切对象/回调/注册项,且与 Register 成功次数一一对应。
		bool (*Register)(WorldContext& context, const WeHostApi& host) = nullptr;
		void (*Unregister)(WorldContext& context) = nullptr;
	};

	// 入口签名:`WePluginQuery(hostAbiVersion)`。
	// 返回 nullptr = 宿主 ABI 不受支持(插件必须显式拒绝,而不是按旧布局解释)。
	using WePluginQueryFn = const WePlugin* (*)(uint32_t hostAbiVersion);
}
