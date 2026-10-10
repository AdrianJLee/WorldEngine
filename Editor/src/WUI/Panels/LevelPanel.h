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
	};
}
