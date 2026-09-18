#pragma once

#include "EditorPanel.h"

#include "World/Script/LuauHighlighter.h"
#include "World/WUI/WuiCodeEditor.h"
#include "World/WUI/WuiTextBuffer.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace World
{
	// P2 W9-2:内置脚本编辑器面板(每个脚本一个实例,id = "script:<逻辑路径>",
	// 与材质编辑器同款动态面板模型)。
	//
	// 职责:
	//  - 打开:ResolveScriptDiskPath 解析磁盘路径 → 二进制读取 → WuiTextBuffer::SetText;
	//    解析失败(包内/非法/不存在)→ 面板仍显示、只读 + 状态行错误文本;
	//  - 保存(工具栏 / Ctrl+S):写 <path>.tmp-<pid> → 同目录 rename 原子替换;失败清理临时文件、
	//    保留编辑器内容;成功后 MarkSaved + 更新磁盘指纹 + 编辑态下热重载场景里的同路径实例;
	//  - 外部改动:0.5s 节流的内容指纹比对(不看 mtime)—— buffer 干净自动重载;
	//    dirty → 状态行提示"磁盘已变化" + Reload(覆盖)/Keep(忽略到指纹再次变化)两个动作;
	//  - 只读:Play/Simulate 或非磁盘来源时禁用 Save/Revert 与编辑(工具栏按钮登记为 disabled);
	//  - 快捷键:OnShortcut 只置位(事件派发在 UI 帧之外,不在派发期改文档),帧内 OnRender 统一消费。
	class ScriptEditorPanel final : public EditorPanel
	{
	public:
		explicit ScriptEditorPanel(std::string logicalPath);

		const char* Id() const override { return m_PanelId.c_str(); }
		const char* Title() const override { return m_PanelTitle.c_str(); }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		// 第 2 层(焦点面板层)快捷键:Ctrl+S = 保存脚本;Ctrl+R = 从磁盘 Reload(同 Revert)。
		bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt) override;

		const std::string& LogicalPath() const { return m_LogicalPath; }
		bool IsDiskBacked() const { return m_DiskBacked; }
		const std::string& StatusText() const { return m_Status; }

	private:
		// 打开/重新解析并载入磁盘内容(SetText 会重置撤销历史与脏标记)。
		void LoadFromDisk();
		// 0.5s 节流:磁盘内容指纹变化检测(自动重载 / Reload+Keep 提示)。
		void PollExternalChange();
		// 保存:临时文件 + 原子替换 + MarkSaved + 场景实例热重载。
		void ApplySave(PanelHost& host);
		// 用磁盘版本覆盖 buffer(工具栏 Revert / Ctrl+R / 冲突提示的 Reload)。
		void ApplyReloadFromDisk(PanelHost& host);
		// 编辑态下把场景里同一逻辑路径的 LuaScriptComponent 走唯一重载入口刷新。
		void ReloadSceneInstances(PanelHost& host);
		void SetStatus(std::string text, bool error);

		std::string m_PanelId;      // "script:<逻辑路径>"
		std::string m_PanelTitle;   // 标签标题(文件名)
		std::string m_LogicalPath;  // 逻辑路径(分隔符统一为 '/')
		std::filesystem::path m_DiskPath;   // 解析出的磁盘绝对路径(仅 m_DiskBacked 时有效)
		bool m_DiskBacked = false;
		Wui::WuiTextBuffer m_Buffer;
		LuauHighlightCache m_Highlight;
		uint64_t m_DiskFingerprint = 0;     // 打开/保存/重载时记录的磁盘内容指纹
		uint64_t m_IgnoredFingerprint = 0;  // Keep 忽略的磁盘版本(0 = 无)
		uint64_t m_ConflictFingerprint = 0; // 触发当前冲突的磁盘版本(Keep 用它登记忽略)
		bool m_ExternalConflict = false;    // 磁盘变了且 buffer 脏:状态行给 Reload/Keep
		double m_NextDiskCheck = 0.0;
		std::string m_Status;
		bool m_StatusIsError = false;
		// W9:代码字号(会话内记忆)。Ctrl+滚轮 / Ctrl+± / Ctrl+0 调整。
		float m_FontSize = 14.0f;
		// OnShortcut 只置位,帧内消费。
		bool m_PendingSave = false;
		bool m_PendingReload = false;
	};
}
