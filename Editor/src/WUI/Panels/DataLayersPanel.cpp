#include "wldpch.h"
#include "WUI/Panels/DataLayersPanel.h"

#include "World/Level/Authoring/LevelPackage.h"
#include <unordered_map>
#include "World/Scene/Components.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

namespace World
{
	// 数据层编辑区(只对**当前打开的关卡**)。
	//
	// 布局(每层一块,行高 22):
	//   [名字 TextField(回车提交)] [Del]
	//   [掩码 N]  [x 默认激活]
	// 底部是"添加层"行;掩码自动取**下一个空闲位**(引擎侧 NextFreeLayerMask),不用手填数字。
	//
	// 三条守卫(都在下面按理由给用户看,不是静默忽略):
	//   ① 有实体仍归属该层 ⇒ 拒绝删除(否则那些实体会静默变成"残留在无效位上"的不可见内容);
	//   ② 至少保留一层(关卡的实体需要可归属的层);
	//   ③ 32 个层位用尽 ⇒ "添加层"拒绝并说明。
	//
	// 行级动作与 Levels 面板同一条纪律:**先记账、循环后执行**(循环里改容器 = 引用悬垂)。
	void DataLayersPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();

		float y = rect.Y + 8.0f;
		Label(ctx, { rect.X + 10.0f, y }, Wui::Tr("panel.data_layers.title", "Data Layers"),
			theme.Text, 15.0f);
		y += 24.0f;

		std::vector<Level::DataLayerDefinition> hostLayers;
		const bool hasLevel = host.GetEditableDataLayers(&hostLayers);

		// 草稿失效条件:换关卡 / 尚未初始化 / 刚提交过。
		const std::filesystem::path entry = host.GetOpenDocumentPath().empty()
			? std::filesystem::path()
			: std::filesystem::path(host.GetOpenDocumentPath());
		if (!m_DraftValid || m_DraftFor != entry)
		{
			m_Draft = hostLayers;
			m_DraftValid = true;
			m_DraftFor = entry;
			m_Error.clear();
			m_Notice.clear();
		}

		if (!hasLevel)
		{
			Label(ctx, { rect.X + 10.0f, y },
				Wui::Tr("panel.data_layers.no_level",
					"No level is open. Open one in the Levels panel to edit its data layers."),
				theme.TextMuted, 12.0f);
			y += 20.0f;
			return;
		}

		// 在编辑谁的层:入口文件名 = 关卡 Id(与 Levels 面板列表里的 Id 同源)。
		Label(ctx, { rect.X + 10.0f, y },
			Wui::TrFormat("panel.data_layers.level", "Level: {id}",
				{ { "id", entry.stem().string() } }),
			theme.Text, 12.0f);
		y += 18.0f;
		Label(ctx, { rect.X + 10.0f, y },
			Wui::Tr("panel.data_layers.hint",
				"One bit per layer; default-active layers are on when the level opens."),
			theme.TextMuted, 11.0f);
		y += 18.0f;

		// 行级动作先记账、循环后执行(循环里改容器 = 引用悬垂)。
		enum class LayerAction { None, Rename, ToggleDefault, Delete, Add };
		LayerAction pending = LayerAction::None;
		uint32_t pendingMask = 0;
		std::string pendingName;

		// 统计当前打开关卡各数据层的实体引用数
		std::unordered_map<uint32_t, size_t> layerEntityCounts;
		if (const Ref<Scene> scene = host.GetActiveScene())
		{
			const entt::registry& registry = std::as_const(*scene).GetRegistry();
			for (const entt::entity handle : registry.view<LevelEntityComponent>())
			{
				const uint32_t mask = registry.get<LevelEntityComponent>(handle).DataLayerMask;
				for (const Level::DataLayerDefinition& layer : m_Draft)
				{
					if ((mask & layer.LayerMask) != 0)
						layerEntityCounts[layer.LayerMask]++;
				}
			}
		}

		const uint32_t nextMask = Level::NextFreeLayerMask(m_Draft);

		for (size_t index = 0; index < m_Draft.size(); ++index)
		{
			Level::DataLayerDefinition& layer = m_Draft[index];
			const float rowY = y;

			const Wui::WuiRect nameRect { rect.X + 8.0f, rowY, rect.W - 16.0f - 56.0f, 22.0f };
			Wui::TextFieldA11y nameA11y;
			nameA11y.Label = "Layer name";
			nameA11y.Placeholder = "Layer" + std::to_string(layer.LayerMask);
			if (Wui::TextField(ctx, Wui::HashId(("level.layer.name." + std::to_string(layer.LayerMask)).c_str()),
				nameRect, layer.Name, theme, nullptr, &nameA11y))
			{
				pending = LayerAction::Rename;      // TextField 返回"回车提交"
				pendingMask = layer.LayerMask;
				pendingName = layer.Name;
			}
			if (Button(ctx, Wui::HashId(("level.layer.delete." + std::to_string(layer.LayerMask)).c_str()),
				{ nameRect.X + nameRect.W + 4.0f, rowY, 52.0f, 22.0f }, "Del", theme))
			{
				pending = LayerAction::Delete;
				pendingMask = layer.LayerMask;
			}

			const size_t entityCount = layerEntityCounts[layer.LayerMask];
			const float secondY = rowY + 24.0f;
			Label(ctx, { rect.X + 10.0f, secondY + 3.0f },
				Wui::TrFormat("panel.data_layers.mask", "mask {mask} ({count})",
					{ { "mask", std::to_string(layer.LayerMask) },
					  { "count", std::to_string(entityCount) } }),
				theme.TextMuted, 11.0f);
			const Wui::WuiRect defaultRect { rect.X + 78.0f, secondY, rect.W - 86.0f - 56.0f, 20.0f };
			bool defaultActive = layer.DefaultActive;
			if (Wui::Checkbox(ctx,
				Wui::HashId(("level.layer.default." + std::to_string(layer.LayerMask)).c_str()),
				defaultRect,
				Wui::Tr("panel.data_layers.default_active", "Default active"),
				defaultActive, theme))
			{
				pending = LayerAction::ToggleDefault;
				pendingMask = layer.LayerMask;
				pendingName = defaultActive ? "1" : "0";
			}

			// 一键指派当前选中实体到此数据层
			Entity selected = host.GetSelectedEntity();
			const bool canAssign = selected.IsValid() && selected.GetScene() == host.GetActiveScene().get();
			const Wui::WuiRect assignRect { rect.X + rect.W - 58.0f, secondY, 50.0f, 20.0f };
			if (Button(ctx, Wui::HashId(("level.layer.assign." + std::to_string(layer.LayerMask)).c_str()),
				assignRect, Wui::Tr("panel.data_layers.assign", "Assign"), theme))
			{
				if (canAssign)
				{
					if (selected.HasComponent<LevelEntityComponent>())
					{
						selected.GetComponent<LevelEntityComponent>().DataLayerMask |= layer.LayerMask;
					}
					else
					{
						selected.AddComponent<LevelEntityComponent>(LevelEntityComponent { layer.LayerMask });
					}
					host.MarkDocumentDirty();
					m_Notice = Wui::TrFormat("panel.data_layers.assigned", "Assigned selected entity to layer '{layer}'.",
						{ { "layer", layer.Name } });
				}
				else
				{
					m_Error = Wui::Tr("panel.data_layers.no_selection", "Select an entity in Hierarchy first.");
				}
			}
			y = secondY + 24.0f;
		}

		// ---- 添加层 ----
		const Wui::WuiRect addNameRect { rect.X + 8.0f, y, rect.W - 16.0f - 56.0f, 22.0f };
		{
			Wui::TextFieldA11y addA11y;
			addA11y.Label = "New layer name";
			addA11y.Placeholder = "NewLayer";
			Wui::TextField(ctx, Wui::HashId("level.layer.add.name"), addNameRect, m_NewLayerName, theme,
				nullptr, &addA11y);
		}
		const bool canAdd = nextMask != 0 && !m_NewLayerName.empty();
		if (Button(ctx, Wui::HashId("level.layer.add"),
			{ addNameRect.X + addNameRect.W + 4.0f, y, 52.0f, 22.0f }, "Add", theme))
		{
			if (nextMask == 0)
			{
				m_Error = Wui::Tr("panel.data_layers.full", "All 32 layer bits are in use.");
			}
			else if (m_NewLayerName.empty())
			{
				m_Error = Wui::Tr("panel.data_layers.add_name", "Enter a name for the new layer.");
			}
			else
			{
				pending = LayerAction::Add;
				pendingMask = nextMask;
				pendingName = m_NewLayerName;
			}
			(void)canAdd;
		}
		y += 26.0f;

		// ---- 执行(此刻不再持有 m_Draft 的引用)----
		if (pending != LayerAction::None)
		{
			m_Error.clear();
			m_Notice.clear();
			switch (pending)
			{
			case LayerAction::Rename:
				for (Level::DataLayerDefinition& layer : m_Draft)
					if (layer.LayerMask == pendingMask)
						layer.Name = pendingName;
				break;
			case LayerAction::ToggleDefault:
				for (Level::DataLayerDefinition& layer : m_Draft)
					if (layer.LayerMask == pendingMask)
						layer.DefaultActive = pendingName == "1";
				break;
			case LayerAction::Delete:
			{
				if (m_Draft.size() <= 1)
				{
					m_Error = Wui::Tr("panel.data_layers.keep_one",
						"A level must keep at least one data layer.");
					break;
				}
				// 守卫:仍被实体使用的层不能删(见本函数注释 ①)。查的是**当前打开的场景**,
				// 与保存后渲染用的那份实体集合同源。
				if (const Ref<Scene> scene = host.GetActiveScene())
				{
					const entt::registry& registry = std::as_const(*scene).GetRegistry();
					for (const entt::entity handle : registry.view<LevelEntityComponent>())
					{
						const uint32_t mask = registry.get<LevelEntityComponent>(handle).DataLayerMask;
						if ((mask & pendingMask) != 0)
						{
							m_Error = Wui::Tr("panel.data_layers.in_use",
								"Entities still belong to this layer — reassign them first.");
							break;
						}
					}
				}
				if (!m_Error.empty())
					break;
				for (size_t index = 0; index < m_Draft.size(); ++index)
					if (m_Draft[index].LayerMask == pendingMask)
					{
						m_Draft.erase(m_Draft.begin() + static_cast<ptrdiff_t>(index));
						break;
					}
				break;
			}
			case LayerAction::Add:
				m_Draft.push_back({ pendingMask, pendingName, true });
				m_NewLayerName.clear();
				break;
			case LayerAction::None:
				break;
			}

			if (m_Error.empty())
			{
				std::string message;
				if (!CommitLayerDraft(host, &message))
					m_Error = message;
				else
					m_Notice = message;
			}
		}

		if (!m_Error.empty())
		{
			Label(ctx, { rect.X + 10.0f, y }, m_Error, theme.Danger, 12.0f);
			y += 18.0f;
		}
		else if (!m_Notice.empty())
		{
			Label(ctx, { rect.X + 10.0f, y }, m_Notice, theme.TextMuted, 11.0f);
			y += 16.0f;
		}
	}


	bool DataLayersPanel::CommitLayerDraft(PanelHost& host, std::string* error)
	{
		std::string message;
		if (!host.ApplyDataLayers(m_Draft, &message))
		{
			if (error) *error = message;
			return false;
		}
		// 提交成功 ⇒ 草稿失效:下一帧从宿主重读(归一化后的结果才是盘上的事实)。
		m_DraftValid = false;
		if (error) *error = message;
		return true;
	}
}
