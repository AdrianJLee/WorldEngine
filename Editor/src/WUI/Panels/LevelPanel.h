#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Level/Authoring/LevelCatalog.h"

#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	// 关卡面板 —— 列出内容根下 `scenes/*.wlevel` 的关卡入口,一键打开;
	// 也负责**新建 / 重命名 / 删除**三件套(关卡的唯一创建入口)。
	//
	// 关卡 = **入口文件**(`scenes/<Id>.wlevel`,存 manifest)+ **内容目录**
	// (`levels/<Id>/`,一实体一文件),二者按 Id 同名成对。因此新建/改名/删除都是
	// **成对事务** —— 只动一半会留下被目录校验拒绝的残骸。
	class LevelPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "levels"; }
		const char* Title() const override { return "Levels"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		void EnsureLoaded();
		// 重新扫描并刷新列表(新建之后调用)。
		void Reload();
		// 重命名 / 删除:成对作用于入口 `.wlevel` 与内容目录 `levels/<Id>/`。
		// 失败(含"目标已存在"与需要回滚的情形)返回 false,*error 说明原因。
		bool RenameLevel(PanelHost& host, const std::string& oldId, const std::string& newId,
			std::string* error);
		bool DeleteLevel(PanelHost& host, const std::string& id, std::string* error);
		// 数据层编辑区(只对**当前打开的关卡**):草稿 → 写回。规则(归一化/掩码分配)在引擎侧。
		void DrawDataLayers(Wui::WuiContext& ctx, float& y, const Wui::WuiRect& rect, PanelHost& host);
		// 把草稿写回宿主;成功 ⇒ 草稿失效(下一帧从宿主重读归一化后的结果)。
		bool CommitLayerDraft(PanelHost& host, std::string* error);
		// 清单里的 start_scene 是否指向该关卡(三种历史写法都认)。命中时改名要同步清单。
		bool IsCurrentStartScene(const std::string& id) const;

		bool m_Loaded = false;
		std::filesystem::path m_ContentRoot;
		std::vector<Level::LevelInfo> m_Levels;
		// 清单快照:改名命中 start_scene 时用它把启动项同步过去(其余字段原样回写)。
		std::string m_ManifestRenderer;
		std::string m_ManifestContentRoot;
		std::string m_StartScene;

		// 新建表单
		std::string m_NewLevelName = "NewLevel";
		std::string m_NewLevelError;
		// 行内改名状态(空 = 没有在改名的行)。
		std::string m_RenameId;
		std::string m_RenameText;
		// 行级操作(改名/删除)的失败原因 —— 与新建表单的错误分开显示,互不覆盖。
		std::string m_RowError;

		// ---- 数据层编辑(草稿模型)----
		// 为什么留草稿:改名是逐字符输入,不可能每敲一个字就写一次 manifest。文本框回车提交、
		// 勾选/增删立即提交 —— 提交 = 整份草稿归一化后写回入口 manifest。
		std::vector<Level::DataLayerDefinition> m_LayerDraft;
		bool m_LayerDraftValid = false;      // false ⇒ 下一帧从宿主重读(打开/切换关卡、提交后)
		std::string m_NewLayerName;
		std::string m_LayerError;            // 数据层区的错误(与上面的 m_RowError 分开)
		std::string m_LayerNotice;           // 成功回执(写盘路径/层数)
		std::filesystem::path m_LayerDraftFor;   // 草稿对应的关卡入口(换关卡即失效)
	};
}
