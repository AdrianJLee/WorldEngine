#pragma once

#include "WUI/Common/EditorPanel.h"

#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	// 数据层面板 —— 编辑**当前打开关卡**的数据层定义(关卡作用域的层元数据)。
	//
	// 与 Levels 面板分成两个栏位(2026-10-10 用户口径:"把数据层UI和关卡栏位分开单独成一个栏位"):
	// Levels 管"有哪些关卡 / 打开哪一个",这一栏管"打开的这个关卡有哪些层"。
	// 层定义写在**关卡入口 manifest** 的 DataLayers 段里(不在实体文件里);
	// 归一化规则(每层恰一位 / 去重 / 排序 / 至少一层)在引擎侧 LevelPackage,面板不做规则。
	class DataLayersPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "data_layers"; }
		const char* Title() const override { return "Data Layers"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		// 把草稿写回宿主;成功 ⇒ 草稿失效(下一帧从宿主重读归一化后的结果)。
		bool CommitLayerDraft(PanelHost& host, std::string* error);

		// ---- 草稿模型 ----
		// 为什么留草稿:改名是逐字符输入,不可能每敲一个字就写一次 manifest。文本框回车提交、
		// 勾选/增删立即提交 —— 提交 = 整份草稿归一化后写回入口 manifest。
		std::vector<Level::DataLayerDefinition> m_Draft;
		bool m_DraftValid = false;          // false ⇒ 下一帧从宿主重读(打开/切换关卡、提交后)
		std::filesystem::path m_DraftFor;   // 草稿对应的关卡入口(换关卡即失效)
		std::string m_NewLayerName;
		std::string m_Error;                // 数据层区的错误
		std::string m_Notice;               // 成功回执(写盘路径/层数)
	};
}
