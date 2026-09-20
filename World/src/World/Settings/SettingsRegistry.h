#pragma once

#include "World/Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace World::Settings
{
	// P4-UX7:设置注册表(方案 §U4)。
	//
	// 动机:引擎里"已经有实现、但没有界面"的开关只能改文件或设环境变量
	// (热重载、脚本字号、AI 通道端口、日志级别、诊断开关…)。每加一个 knob 就要
	// 手写一遍"标签 + 控件 + tooltip + 无障碍节点 + 复位 + 生效时机徽标",
	// 结果是有的 knob 干脆没有入口、有的入口行为不一致。
	//
	// 口径:一个 knob 一条描述符(Id 稳定、进无障碍节点;Read/Write 绑定真实存储),
	// 面板只负责"按 Group 分组渲染 + 搜索 + 只看已修改";存盘仍由各自的存储负责
	// (项目级 `Game/project.we.yaml`、用户级 `Editor/editor-prefs.json`)。
	enum class SettingType : uint8_t
	{
		Bool = 0,
		Int = 1,
		Float = 2,
		Enum = 3,   // Options 里的 Value 是存盘值,Label 是英文默认文案
		Text = 4,
		Path = 5,
	};

	// 作用域决定"是否存在项目仓库里"与显示在哪个面板(方案 §U4)。
	enum class SettingScope : uint8_t
	{
		Project = 0,   // 随项目提交:Game/project.we.yaml
		Editor = 1,    // 用户级:Editor/editor-prefs.json
		Import = 2,    // 项目级:project.we.yaml 的 imports:
		World = 3,     // 场景级:.wd 场景头
	};

	// 生效时机(面板画黄色徽标;tooltip 里也要写)。
	enum class SettingApply : uint8_t
	{
		Immediate = 0,   // 改完立刻生效
		Restart = 1,     // 下次启动编辑器生效
		NextPlay = 2,    // 下次进入 Play 生效
	};

	struct SettingOption
	{
		std::string Value;   // 存盘值(稳定,不随语言变化)
		std::string Label;   // 英文内联默认文案
	};

	struct SettingDescriptor
	{
		// 稳定 id,建议 "<作用域>.<分组>.<键>"(如 "editor.editor.script_font_size")。
		// 它同时是无障碍节点 id 的一部分,改名等于破坏脚本兼容。
		std::string Id;
		// 分类名(英文,如 "Appearance"/"Workflow"/"Diagnostics")。面板按它分页。
		std::string Group;
		SettingType Type = SettingType::Bool;
		SettingScope Scope = SettingScope::Editor;
		SettingApply Apply = SettingApply::Immediate;
		// 英文内联默认文案 = 默认语言(English)下显示的文本(本地化口径见 WuiLocalization.h)。
		std::string Label;
		// 英文术语(中文界面下对照)。为空或与 Label 相同则不画对照。
		std::string Term;
		// 悬停说明:名称 + 一句用途 + 默认值/范围 + 生效时机(禁用原因)。
		std::string Tooltip;
		std::string Unit;          // 数值单位("x"/"px"/"ms"…)
		double Min = 0.0;
		double Max = 0.0;
		double Step = 0.0;
		std::vector<SettingOption> Options;   // Type == Enum
		bool Advanced = false;                 // 归入"高级/诊断"折叠区

		// 存取绑定(必填)。Read 返回字符串化当前值;Write 写回真实存储
		// (立即生效 + 落盘由存储负责),失败时把原因写进 error。
		std::function<std::string()> Read;
		std::function<bool(const std::string&, std::string*)> Write;
		// 可选:是否仍是默认值(用于"只看已修改"与行内复位圆点)。
		std::function<bool()> IsDefault;
		// 可选:恢复默认值(实现内部应写回存储;返回是否真的改了)。
		std::function<bool()> Reset;
		// 可选:当前是否可用。返回 false 时控件禁用,并把 DisabledReason 写进 tooltip
		// (反模式:"做成禁用按钮却不写原因")。省略 = 始终可用。
		std::function<bool()> IsEnabled;
		std::string DisabledReason;   // 英文默认;中文覆盖键 = settings.<id>.disabled
	};

	// 单例注册表(World 层,Editor 注册、Runtime 只读)。
	class WLD_API SettingsRegistry
	{
	public:
		static SettingsRegistry& Get();

		// 重复 Id = 替换(先注册的丢弃)并打 warn:面板重开/热重载不该堆出重复行。
		void Register(SettingDescriptor descriptor);
		// 只清掉某个作用域的条目(重开面板时重新注册,不影响另一侧)。
		void ClearScope(SettingScope scope);
		void Clear();

		const SettingDescriptor* Find(std::string_view id) const;
		size_t Count() const { return m_Items.size(); }

		// 注册顺序输出;group 为空 = 全部。面板据此分组渲染。
		std::vector<const SettingDescriptor*> OfScope(SettingScope scope, std::string_view group = {}) const;
		// 出现的分组(注册顺序去重)。
		std::vector<std::string> Groups(SettingScope scope) const;
		// 按 id/label/tooltip 子串匹配(不区分大小写);onlyModified 时只留 !IsDefault()。
		std::vector<const SettingDescriptor*> Search(SettingScope scope, std::string_view query, bool onlyModified) const;
		// 是否有被改过的项(标题栏提示点用)。
		bool HasModified(SettingScope scope) const;

		// 写值:找不到 / 没有 Write / Write 失败 → false 并写 error。
		bool Set(std::string_view id, const std::string& value, std::string* error);
		// 恢复默认:没有 Reset 时返回 false。
		bool Reset(std::string_view id, std::string* error);

		// 读取便捷函数(解析失败或找不到时返回 fallback)。
		std::string ReadText(std::string_view id, const std::string& fallback = std::string()) const;
		bool ReadBool(std::string_view id, bool fallback) const;
		int64_t ReadInt(std::string_view id, int64_t fallback) const;
		double ReadFloat(std::string_view id, double fallback) const;

	private:
		SettingsRegistry() = default;
		std::vector<SettingDescriptor> m_Items;
	};
}
