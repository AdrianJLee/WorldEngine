#include "WUI/Panels/NavMeshPanel.h"
#include "WUI/Panels/ViewportPanel.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Terrain/TerrainRenderer.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Core/Log.h"

#include <cstdio>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <sstream>

namespace World
{
	bool NavMeshPanel::ShowPolygons = true;
	bool NavMeshPanel::FillPolygons = true;
	bool NavMeshPanel::ShowPaths = true;
	bool NavMeshPanel::ShowObstacles = true;
	bool NavMeshPanel::ShowAreaCosts = true;
	bool NavMeshPanel::ShowOffMeshLinks = true;
	bool NavMeshPanel::ShowLoSRaycast = true;

	NavMeshPanel::NavMeshPanel()
		: m_ShellHost(nullptr)
	{
		ApplyAgentProfile(0);
	}

	NavMeshPanel::NavMeshPanel(PanelHost& host)
		: m_ShellHost(&host)
	{
		ApplyAgentProfile(0);
	}

	bool NavMeshPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		(void)keyCode; (void)ctrl; (void)shift; (void)alt;
		return false;
	}

	void NavMeshPanel::ApplyAgentProfile(int profileIdx)
	{
		m_AgentProfileIdx = profileIdx;
		switch (profileIdx)
		{
		case 0:
			m_AgentRadius = 0.5f;
			m_AgentHeight = 2.0f;
			m_MaxSlope = 45.0f;
			m_StepHeight = 0.45f;
			break;
		case 1:
			m_AgentRadius = 0.25f;
			m_AgentHeight = 0.8f;
			m_MaxSlope = 50.0f;
			m_StepHeight = 0.25f;
			break;
		case 2:
			m_AgentRadius = 1.5f;
			m_AgentHeight = 3.5f;
			m_MaxSlope = 30.0f;
			m_StepHeight = 0.8f;
			break;
		case 3:
		default:
			break;
		}
	}

	void NavMeshPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		m_ShellHost = &host;
		const Wui::WuiTheme& theme = host.Theme();
		// 去除冗余嵌套的 Wui::Panel(ctx, rect, Title(), theme) —— DockTabBar 已提供外层标签和标题

		const float pad = theme.Pad > 0.0f ? theme.Pad : 8.0f;
		const float startX = rect.X + pad;
		const float contentW = std::max(100.0f, rect.W - pad * 2.0f);
		float curY = rect.Y + pad;

		// 轮询后台异步多线程烘焙状态 (UI 主线程 60+ FPS 流畅不卡顿)
		if (m_AsyncBake.IsRunning && m_AsyncBake.Future.valid())
		{
			if (m_AsyncBake.Future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
			{
				auto newNav = m_AsyncBake.Future.get();
				m_AsyncBake.IsRunning = false;

				const auto now = std::chrono::high_resolution_clock::now();
				const double elapsed = std::chrono::duration<double>(now - m_AsyncBake.StartTime).count();

				if (Ref<Scene> activeScene = host.GetActiveScene())
				{
					activeScene->SetNavMesh(newNav);
					activeScene->SaveNavMesh();
					host.MarkDocumentDirty();

					ViewportPanel::SetNavMeshVisible(true);
					ShowPolygons = true;

					char msgBuf[256];
					if (newNav->GetPolygonCount() > 0)
					{
						std::snprintf(msgBuf, sizeof(msgBuf), "[OK] 3D 导航网格后台烘焙成功！耗时: %.2fs，多边形: %zu，顶点: %zu",
							elapsed, newNav->GetPolygonCount(), newNav->GetVertexCount());
					}
					else
					{
						std::snprintf(msgBuf, sizeof(msgBuf), "烘焙完成 (%.2fs) 但多边形为 0: 场景未找到符合坡度要求的静态几何体", elapsed);
					}
					m_StatusFeedback = msgBuf;
					host.Notify(m_StatusFeedback);
				}
			}
		}

		Ref<Scene> activeScene = host.GetActiveScene();
		std::shared_ptr<NavMesh> navMesh = activeScene ? activeScene->GetNavMesh() : nullptr;
		if (!navMesh && activeScene)
			navMesh = activeScene->GetOrCreateNavMesh();

		RenderMetricsCard(ctx, startX, curY, contentW, theme, navMesh.get());

		const std::vector<std::string> tabs = { "烘焙配置", "视口显示", "空间探针" };
		const int prevTab = m_ActiveTabIdx;
		Wui::TabBar(ctx, Wui::HashId("navmesh.tabs"), { startX, curY, contentW, 28.0f }, tabs, m_ActiveTabIdx, theme);
		if (m_ActiveTabIdx != prevTab)
		{
			m_ScrollY = 0.0f;
		}
		curY += 34.0f;

		// 计算可用高度与内容所需高度，支持自然滚动与优雅高度自适应，消除卡片下方大面积空黑感
		const float availH = std::max(40.0f, (rect.Y + rect.H) - curY - pad);
		float contentNeededH = 280.0f;
		if (m_ActiveTabIdx == 0)
			contentNeededH = 280.0f + (m_StatusFeedback.empty() ? 0.0f : 24.0f);
		else if (m_ActiveTabIdx == 1)
			contentNeededH = (contentW < 340.0f ? 200.0f : 175.0f);
		else if (m_ActiveTabIdx == 2)
			contentNeededH = 340.0f + (m_PathQueryStatus.empty() ? 0.0f : 20.0f) + (m_LoSStatus.empty() ? 0.0f : 20.0f) + (m_WanderStatus.empty() ? 0.0f : 20.0f);

		const float totalContentH = std::max(contentNeededH, availH);
		const Wui::WuiRect scrollViewport{ startX, curY, contentW, availH };

		Wui::BeginScrollArea(ctx, scrollViewport, totalContentH, m_ScrollY, theme, Wui::HashId("navmesh.scroll"));
		const float tabY = curY - m_ScrollY;
		switch (m_ActiveTabIdx)
		{
		case 0:
			RenderBakeTab(ctx, startX, tabY, contentW, totalContentH, theme, host, navMesh.get());
			break;
		case 1:
			RenderVisibilityTab(ctx, startX, tabY, contentW, totalContentH, theme);
			break;
		case 2:
			RenderProbesTab(ctx, startX, tabY, contentW, totalContentH, theme, host, navMesh.get());
			break;
		default:
			break;
		}
		Wui::EndScrollArea(ctx);

		if (totalContentH > availH + 1.0f)
		{
			const float sbW = 6.0f;
			const Wui::WuiRect sbRect{ startX + contentW - sbW, curY, sbW, availH };
			Wui::ScrollBar(ctx, Wui::HashId("navmesh.scrollbar"), sbRect, totalContentH, availH, m_ScrollY, theme, false);
		}
	}

	void NavMeshPanel::RenderMetricsCard(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, NavMesh* navMesh)
	{
		const float cardH = 64.0f;
		const Wui::WuiRect cardRect { startX, curY, contentW, cardH };
		Wui::PanelBackground(ctx, cardRect, theme.CardBg, theme.Radius);
		Wui::HighlightOutline(ctx, cardRect, theme.Border, theme.Radius, 1.0f);

		const bool hasPolys = (navMesh && navMesh->GetPolygonCount() > 0);
		const char* badgeText = hasPolys ? "[Ready] 导航网格已就绪 (Baked)" : "[Empty] 导航网格未烘焙 / 空 (Empty)";
		const Wui::WuiColor badgeColor = hasPolys ? theme.Success : theme.TextMuted;
		Wui::Label(ctx, { startX + 8.0f, curY + 6.0f }, badgeText, badgeColor, theme.FontSizeBody);

		size_t vertCount = navMesh ? navMesh->GetVertexCount() : 0;
		size_t polyCount = navMesh ? navMesh->GetPolygonCount() : 0;
		size_t linkCount = navMesh ? navMesh->GetOffMeshLinks().size() : 0;
		size_t modCount = navMesh ? navMesh->GetAreaModifiers().size() : 0;
		float walkableArea = 0.0f;
		size_t blockedCount = 0;
		if (navMesh)
		{
			for (size_t i = 0; i < polyCount; ++i)
			{
				const auto& p = navMesh->GetPolygon(static_cast<uint32_t>(i));
				if (p.Blocked) ++blockedCount;
				else walkableArea += p.Area;
			}
		}

		char statsBuf1[128] = {};
		std::snprintf(statsBuf1, sizeof(statsBuf1), "多边形: %zu   顶点: %zu   面积: %.1f m2",
			polyCount, vertCount, walkableArea);
		Wui::Label(ctx, { startX + 8.0f, curY + 26.0f }, statsBuf1, theme.Text, theme.FontSizeSmall);

		char statsBuf2[128] = {};
		std::snprintf(statsBuf2, sizeof(statsBuf2), "阻挡: %zu   跳跃链接: %zu   修饰: %zu",
			blockedCount, linkCount, modCount);
		Wui::Label(ctx, { startX + 8.0f, curY + 44.0f }, statsBuf2, theme.TextMuted, theme.FontSizeSmall);

		curY += cardH + 8.0f;
	}

	void NavMeshPanel::RenderBakeTab(Wui::WuiContext& ctx, float startX, float curY, float contentW, float cardH, const Wui::WuiTheme& theme, PanelHost& host, NavMesh* navMesh)
	{
		const Wui::WuiRect cardRect { startX, curY, contentW, cardH };
		Wui::PanelBackground(ctx, cardRect, theme.CardBg, theme.Radius);
		Wui::HighlightOutline(ctx, cardRect, theme.Border, theme.Radius, 1.0f);

		float innerY = curY + 8.0f;
		Wui::Label(ctx, { startX + 8.0f, innerY }, "[Bake] 全场景烘焙源与代理规格 (Bake Sources & Profiles)", theme.Accent, theme.FontSizeSmall);
		innerY += 24.0f;

		const float labelW = std::min(115.0f, contentW * 0.42f);
		const float fieldW = std::max(60.0f, contentW - labelW - 16.0f);

		// 1. 代理规格预设
		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "代理规格预设:", theme.Text, theme.FontSizeSmall);
		static const std::vector<std::string> s_AgentProfiles = {
			"人形单位 (Humanoid: R:0.5, H:2.0)",
			"小型爬虫 (Small: R:0.25, H:0.8)",
			"大型Boss/载具 (Boss: R:1.5, H:3.5)",
			"自定义规格 (Custom)"
		};
		const int beforeProfile = m_AgentProfileIdx;
		if (Wui::Combo(ctx, Wui::HashId("navmesh.bake.profile"), { startX + labelW, innerY, fieldW, 22.0f }, "",
			s_AgentProfiles, m_AgentProfileIdx, theme) && m_AgentProfileIdx != beforeProfile)
		{
			ApplyAgentProfile(m_AgentProfileIdx);
		}
		Wui::Tooltip(ctx, { startX + labelW, innerY, fieldW, 22.0f }, "切换预设时自动同步更新半径、身高、最大爬坡坡度与台阶跨越高度");
		innerY += 24.0f;

		// 2. 烘焙空间范围
		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "烘焙空间范围:", theme.Text, theme.FontSizeSmall);
		static const std::vector<std::string> s_BakeScopes = {
			"全场景静态几何 (Full Scene)",
			"视口关注区 (64m 瓦片)"
		};
		Wui::Combo(ctx, Wui::HashId("navmesh.bake.scope"), { startX + labelW, innerY, fieldW, 22.0f }, "",
			s_BakeScopes, m_BakeScopeIdx, theme);
		Wui::Tooltip(ctx, { startX + labelW, innerY, fieldW, 22.0f }, "超大开放世界可选择视口关注区 (64m 对齐 World Partition 瓦片)，避免整张大地图全量体素化消耗");
		innerY += 24.0f;

		// 3. Agent 半径
		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "Agent 半径 (m):", theme.Text, theme.FontSizeSmall);
		const float prevRadius = m_AgentRadius;
		Wui::DragFloat(ctx, Wui::HashId("navmesh.bake.agent_radius"), { startX + labelW, innerY, fieldW, 22.0f },
			m_AgentRadius, 0.05f, 0.1f, 5.0f, theme);
		if (std::abs(m_AgentRadius - prevRadius) > 1e-4f) m_AgentProfileIdx = 3;
		Wui::Tooltip(ctx, { startX + labelW, innerY, fieldW, 22.0f }, "寻路代理圆柱体碰撞半径，用于边缘避障膨胀");
		innerY += 24.0f;

		// 4. Agent 高度
		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "Agent 高度 (m):", theme.Text, theme.FontSizeSmall);
		const float prevHeight = m_AgentHeight;
		Wui::DragFloat(ctx, Wui::HashId("navmesh.bake.agent_height"), { startX + labelW, innerY, fieldW, 22.0f },
			m_AgentHeight, 0.1f, 0.5f, 10.0f, theme);
		if (std::abs(m_AgentHeight - prevHeight) > 1e-4f) m_AgentProfileIdx = 3;
		Wui::Tooltip(ctx, { startX + labelW, innerY, fieldW, 22.0f }, "代理通过高度净空要求");
		innerY += 24.0f;

		// 5. 最大坡度
		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "最大爬坡坡度 (度):", theme.Text, theme.FontSizeSmall);
		const float prevSlope = m_MaxSlope;
		Wui::DragFloat(ctx, Wui::HashId("navmesh.bake.max_slope"), { startX + labelW, innerY, fieldW, 22.0f },
			m_MaxSlope, 1.0f, 5.0f, 85.0f, theme);
		if (std::abs(m_MaxSlope - prevSlope) > 1e-4f) m_AgentProfileIdx = 3;
		Wui::Tooltip(ctx, { startX + labelW, innerY, fieldW, 22.0f }, "可通行三角面最大允许坡度角（度）");
		innerY += 24.0f;

		// 6. 台阶最大跨越高度 (StepHeight)
		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "台阶跨越高度 (m):", theme.Text, theme.FontSizeSmall);
		const float prevStep = m_StepHeight;
		Wui::DragFloat(ctx, Wui::HashId("navmesh.bake.step_height"), { startX + labelW, innerY, fieldW, 22.0f },
			m_StepHeight, 0.05f, 0.05f, 2.0f, theme);
		if (std::abs(m_StepHeight - prevStep) > 1e-4f) m_AgentProfileIdx = 3;
		Wui::Tooltip(ctx, { startX + labelW, innerY, fieldW, 22.0f }, "两相邻多边形高度差在此阈值内时视为连通台阶，支持楼梯连续跨步");
		innerY += 24.0f;

		// 7. 体素网格步长 (VoxelSize)
		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "体素网格步长 (m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.bake.voxel_size"), { startX + labelW, innerY, fieldW, 22.0f },
			m_VoxelSize, 0.1f, 0.2f, 8.0f, theme);
		Wui::Tooltip(ctx, { startX + labelW, innerY, fieldW, 22.0f }, "3D 空间几何离散光栅化分辨率（步长越小网格精度越高）");
		innerY += 26.0f;

		// 8. 烘焙操作按钮组
		const float btnH = 26.0f;
		const float btnW = (contentW - 16.0f - 8.0f) * 0.5f;

		if (m_AsyncBake.IsRunning)
		{
			char btnBuf[64];
			const auto now = std::chrono::high_resolution_clock::now();
			const double elapsed = std::chrono::duration<double>(now - m_AsyncBake.StartTime).count();
			std::snprintf(btnBuf, sizeof(btnBuf), "[烘焙中]... (%.1fs)", elapsed);
			Wui::Button(ctx, Wui::HashId("navmesh.btn.baking"), { startX + 8.0f, innerY, btnW, btnH }, btnBuf, theme);
			Wui::Tooltip(ctx, { startX + 8.0f, innerY, btnW, btnH }, "导航网格正在后台独立工作线程中体素化与构建，UI 保持 60+ FPS 流畅，完成后将自动注入场景");
		}
		else
		{
			if (Wui::Button(ctx, Wui::HashId("navmesh.btn.bake_scene"), { startX + 8.0f, innerY, btnW, btnH }, "[Bake] 异步烘焙 (Bake)", theme))
			{
				if (host.IsReadOnlyMode())
				{
					m_StatusFeedback = "Play/Simulate 运行中：无法在运行时重烘焙网格。";
					host.Notify(m_StatusFeedback);
				}
				else if (Ref<Scene> activeScene = host.GetActiveScene())
				{
					NavMeshBakeSettings settings;
					settings.AgentRadius = m_AgentRadius;
					settings.AgentHeight = m_AgentHeight;
					settings.MaxClimbSlopeDeg = m_MaxSlope;
					settings.StepHeight = m_StepHeight;
					settings.VoxelSize = m_VoxelSize;

					settings.IncludeMeshes = true;
					settings.IncludeTerrain = true;
					settings.IncludeWhitebox = true;
					settings.IncludeColliders = true;

					if (m_BakeScopeIdx == 1) // 视口关注区 64m 瓦片 (世界分区流送对齐)
					{
						settings.UseCustomBounds = true;
						const auto cam = host.GetGizmoCamera();
						const glm::vec3 focusPt = cam.Position + cam.Forward * cam.Distance;
						settings.BoundsMin = focusPt - glm::vec3(64.0f, 32.0f, 64.0f);
						settings.BoundsMax = focusPt + glm::vec3(64.0f, 32.0f, 64.0f);
					}

					m_AsyncBake.Future = NavMesh::BakeSceneAsync(*activeScene, settings);
					m_AsyncBake.StartTime = std::chrono::high_resolution_clock::now();
					m_AsyncBake.IsRunning = true;
					m_StatusFeedback = "已启动后台多线程异步烘焙，主线程交互保持 60+ FPS...";
					host.Notify("已启动 3D 导航网格后台多线程异步烘焙...");
				}
			}
			Wui::Tooltip(ctx, { startX + 8.0f, innerY, btnW, btnH }, "在后台独立线程提取场景几何体（Whitebox、Collider、Terrain）、体素化烘焙 3D 凸多边形并建立拓扑邻接");
		}

		if (Wui::Button(ctx, Wui::HashId("navmesh.btn.clear"), { startX + 8.0f + btnW + 8.0f, innerY, btnW, btnH }, "清空 (Clear)", theme))
		{
			if (navMesh)
			{
				navMesh->Clear();
				s_TestPath.clear();
				if (Ref<Scene> activeScene = host.GetActiveScene())
					activeScene->SaveNavMesh();
				host.MarkDocumentDirty();
				m_StatusFeedback = "已清空当前场景 3D 导航网格。";
				host.Notify(m_StatusFeedback);
			}
		}
		Wui::Tooltip(ctx, { startX + 8.0f + btnW + 8.0f, innerY, btnW, btnH }, "清空全部 3D 导航网格多边形、顶点与动态障碍物");

		innerY += btnH + 8.0f;
		if (!m_StatusFeedback.empty())
		{
			const bool isSuccess = (navMesh && navMesh->GetPolygonCount() > 0);
			const Wui::WuiColor statusColor = isSuccess ? theme.Success : theme.Warning;
			Wui::Label(ctx, { startX + 8.0f, innerY }, m_StatusFeedback, statusColor, theme.FontSizeSmall);
		}
	}

	void NavMeshPanel::RenderVisibilityTab(Wui::WuiContext& ctx, float startX, float curY, float contentW, float cardH, const Wui::WuiTheme& theme)
	{
		const Wui::WuiRect cardRect { startX, curY, contentW, cardH };
		Wui::PanelBackground(ctx, cardRect, theme.CardBg, theme.Radius);
		Wui::HighlightOutline(ctx, cardRect, theme.Border, theme.Radius, 1.0f);

		float innerY = curY + 8.0f;
		Wui::Label(ctx, { startX + 8.0f, innerY }, "[3D] 视口 3D 调试可视化开关 (Viewport Overlays)", theme.Accent, theme.FontSizeSmall);
		innerY += 24.0f;

		const bool narrow = contentW < 340.0f;
		const int cols = narrow ? 2 : 3;
		const float checkW = (contentW - 16.0f - (cols - 1) * 6.0f) / static_cast<float>(cols);

		const float col0X = startX + 8.0f;
		const float col1X = col0X + checkW + 6.0f;
		const float col2X = col1X + checkW + 6.0f;

		// 1. 显示导航网格 (P)
		Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.polys"), { col0X, innerY, checkW, 22.0f }, "显示导航网格 (P)", ShowPolygons, theme);
		Wui::Tooltip(ctx, { col0X, innerY, checkW, 22.0f }, "在视口中开启/关闭 3D 导航多边形着色网格 (快捷键 P)");

		// 2. 半透明全涂色填充
		Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.fill"), { col1X, innerY, checkW, 22.0f }, "半透明填充", FillPolygons, theme);
		Wui::Tooltip(ctx, { col1X, innerY, checkW, 22.0f }, "以 UE5/Unity 风格半透明色彩涂色铺满可行走表面，配合近细远大自适应网格");

		if (!narrow)
		{
			// 3. 区域代价色彩
			Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.area_costs"), { col2X, innerY, checkW, 22.0f }, "区域代价色彩", ShowAreaCosts, theme);
			Wui::Tooltip(ctx, { col2X, innerY, checkW, 22.0f }, "按区域类型着色：普通(翡翠绿)、道路(亮绿)、泥潭(蓝紫)、阻挡(红)");
			innerY += 28.0f;

			// 4. 显示跳跃点弧线
			Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.links"), { col0X, innerY, checkW, 22.0f }, "跳跃点弧线", ShowOffMeshLinks, theme);
			Wui::Tooltip(ctx, { col0X, innerY, checkW, 22.0f }, "在视口中绘制洋红色抛物线跳跃弧线与箭头指示");

			// 5. 显示路径折线
			Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.paths"), { col1X, innerY, checkW, 22.0f }, "路径折线", ShowPaths, theme);
			Wui::Tooltip(ctx, { col1X, innerY, checkW, 22.0f }, "在视口中绘制金黄色平滑折线走廊与航点圆环");

			// 6. 显示动态障碍物
			Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.obstacles"), { col2X, innerY, checkW, 22.0f }, "动态障碍物", ShowObstacles, theme);
			Wui::Tooltip(ctx, { col2X, innerY, checkW, 22.0f }, "在视口中绘制动态障碍物的橙色 3D 盒体包围线框");
			innerY += 32.0f;
		}
		else
		{
			innerY += 26.0f;
			// 3. 区域代价色彩
			Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.area_costs"), { col0X, innerY, checkW, 22.0f }, "区域代价色彩", ShowAreaCosts, theme);
			Wui::Tooltip(ctx, { col0X, innerY, checkW, 22.0f }, "按区域类型着色：普通(翡翠绿)、道路(亮绿)、泥潭(蓝紫)、阻挡(红)");

			// 4. 显示跳跃点弧线
			Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.links"), { col1X, innerY, checkW, 22.0f }, "跳跃点弧线", ShowOffMeshLinks, theme);
			Wui::Tooltip(ctx, { col1X, innerY, checkW, 22.0f }, "在视口中绘制洋红色抛物线跳跃弧线与箭头指示");
			innerY += 26.0f;

			// 5. 显示路径折线
			Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.paths"), { col0X, innerY, checkW, 22.0f }, "路径折线", ShowPaths, theme);
			Wui::Tooltip(ctx, { col0X, innerY, checkW, 22.0f }, "在视口中绘制金黄色平滑折线走廊与航点圆环");

			// 6. 显示动态障碍物
			Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.obstacles"), { col1X, innerY, checkW, 22.0f }, "动态障碍物", ShowObstacles, theme);
			Wui::Tooltip(ctx, { col1X, innerY, checkW, 22.0f }, "在视口中绘制动态障碍物的橙色 3D 盒体包围线框");
			innerY += 32.0f;
		}

		// 视口性能与快捷键说明卡片 (提升设计感与专业感)
		const float tipH = 58.0f;
		const Wui::WuiRect tipRect { startX + 8.0f, innerY, contentW - 16.0f, tipH };
		Wui::PanelBackground(ctx, tipRect, theme.PanelHeader, theme.Radius);
		Wui::HighlightOutline(ctx, tipRect, theme.Border, theme.Radius, 1.0f);
		Wui::Label(ctx, { tipRect.X + 8.0f, tipRect.Y + 6.0f }, "[*] 视口性能削峰与工业级规范", theme.Accent, theme.FontSizeSmall);
		Wui::Label(ctx, { tipRect.X + 8.0f, tipRect.Y + 23.0f }, "- 视距硬截断 65m，自适应距离粗细网格", theme.TextMuted, theme.FontSizeSmall);
		Wui::Label(ctx, { tipRect.X + 8.0f, tipRect.Y + 39.0f }, "- 单帧 Quad 安全限额 350，确保 100+ FPS 流畅", theme.TextMuted, theme.FontSizeSmall);
	}

	void NavMeshPanel::RenderProbesTab(Wui::WuiContext& ctx, float startX, float curY, float contentW, float cardH, const Wui::WuiTheme& theme, PanelHost& host, NavMesh* navMesh)
	{
		(void)host;
		const Wui::WuiRect cardRect { startX, curY, contentW, cardH };
		Wui::PanelBackground(ctx, cardRect, theme.CardBg, theme.Radius);
		Wui::HighlightOutline(ctx, cardRect, theme.Border, theme.Radius, 1.0f);

		float innerY = curY + 8.0f;
		Wui::Label(ctx, { startX + 8.0f, innerY }, "[Query] 通用空间感知与探针测试 (Spatial Query Probes)", theme.Accent, theme.FontSizeSmall);
		innerY += 24.0f;

		const float colLabelW = 55.0f;
		const float valW = std::max(30.0f, (contentW - 16.0f - colLabelW - 8.0f) / 3.0f);

		// 1. 寻路走廊规划
		Wui::Label(ctx, { startX + 8.0f, innerY }, "1. 寻路走廊规划 (A* Path Corridor)", theme.Text, theme.FontSizeSmall);
		innerY += 20.0f;

		// 起点
		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "起点 (S):", theme.TextMuted, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.sx"), { startX + 8.0f + colLabelW, innerY, valW, 20.0f }, m_StartPos.x, 0.5f, -500.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.sy"), { startX + 8.0f + colLabelW + valW + 4.0f, innerY, valW, 20.0f }, m_StartPos.y, 0.5f, -100.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.sz"), { startX + 8.0f + colLabelW + (valW + 4.0f) * 2.0f, innerY, valW, 20.0f }, m_StartPos.z, 0.5f, -500.0f, 500.0f, theme);
		innerY += 24.0f;

		// 终点
		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "终点 (E):", theme.TextMuted, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.ex"), { startX + 8.0f + colLabelW, innerY, valW, 20.0f }, m_EndPos.x, 0.5f, -500.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.ey"), { startX + 8.0f + colLabelW + valW + 4.0f, innerY, valW, 20.0f }, m_EndPos.y, 0.5f, -100.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.ez"), { startX + 8.0f + colLabelW + (valW + 4.0f) * 2.0f, innerY, valW, 20.0f }, m_EndPos.z, 0.5f, -500.0f, 500.0f, theme);
		innerY += 24.0f;

		if (Wui::Button(ctx, Wui::HashId("navmesh.probe.btn_find_path"), { startX + 8.0f, innerY, contentW - 16.0f, 22.0f }, "规划 A* 路径折线 (Plan Path)", theme))
		{
			if (!navMesh || navMesh->GetPolygonCount() == 0)
			{
				m_PathQueryStatus = "导航网格未烘焙，无法规划路径。";
				s_TestPath.clear();
			}
			else
			{
				s_TestPath.clear();
				m_LastPathFound = navMesh->FindPath(m_StartPos, m_EndPos, s_TestPath);
				if (m_LastPathFound)
				{
					m_LastPathDistance = 0.0f;
					for (size_t i = 0; i < s_TestPath.size() - 1; ++i)
						m_LastPathDistance += glm::distance(s_TestPath[i], s_TestPath[i + 1]);
					m_PathQueryStatus = "路径规划成功！航点: " + std::to_string(s_TestPath.size()) +
						" 总里程: " + std::to_string(m_LastPathDistance).substr(0, 5) + " m";
				}
				else
				{
					m_PathQueryStatus = "未找到可达路径（两点间被阻隔或位于不可通行区域）。";
				}
			}
		}
		innerY += 24.0f;
		if (!m_PathQueryStatus.empty())
		{
			const Wui::WuiColor statColor = m_LastPathFound ? theme.Success : theme.Danger;
			Wui::Label(ctx, { startX + 8.0f, innerY }, m_PathQueryStatus, statColor, theme.FontSizeSmall);
			innerY += 20.0f;
		}

		// 2. 视线通视测试
		Wui::Label(ctx, { startX + 8.0f, innerY }, "2. 视线通视测试 (LoS Raycast Probe)", theme.Text, theme.FontSizeSmall);
		innerY += 20.0f;

		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "视点 (A):", theme.TextMuted, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.los_sx"), { startX + 8.0f + colLabelW, innerY, valW, 20.0f }, m_LoSStart.x, 0.5f, -500.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.los_sy"), { startX + 8.0f + colLabelW + valW + 4.0f, innerY, valW, 20.0f }, m_LoSStart.y, 0.5f, -100.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.los_sz"), { startX + 8.0f + colLabelW + (valW + 4.0f) * 2.0f, innerY, valW, 20.0f }, m_LoSStart.z, 0.5f, -500.0f, 500.0f, theme);
		innerY += 24.0f;

		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "目标 (B):", theme.TextMuted, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.los_ex"), { startX + 8.0f + colLabelW, innerY, valW, 20.0f }, m_LoSEnd.x, 0.5f, -500.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.los_ey"), { startX + 8.0f + colLabelW + valW + 4.0f, innerY, valW, 20.0f }, m_LoSEnd.y, 0.5f, -100.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.los_ez"), { startX + 8.0f + colLabelW + (valW + 4.0f) * 2.0f, innerY, valW, 20.0f }, m_LoSEnd.z, 0.5f, -500.0f, 500.0f, theme);
		innerY += 24.0f;

		if (Wui::Button(ctx, Wui::HashId("navmesh.probe.btn_los_test"), { startX + 8.0f, innerY, contentW - 16.0f, 22.0f }, "检测视线通达性 (Test Line of Sight)", theme))
		{
			if (!navMesh || navMesh->GetPolygonCount() == 0)
			{
				m_LoSStatus = "导航网格未烘焙，无法检测视线通达性。";
			}
			else
			{
				glm::vec3 hitPoint { 0.0f };
				const bool visible = navMesh->Raycast(m_LoSStart, m_LoSEnd, &hitPoint);
				s_RaycastProbe.Start = m_LoSStart;
				s_RaycastProbe.End = m_LoSEnd;
				s_RaycastProbe.HasTested = true;
				s_RaycastProbe.IsVisible = visible;
				s_RaycastProbe.HitPoint = hitPoint;

				if (visible)
				{
					m_LoSStatus = "[OK] 视线通达 (Visible - 无几何与障碍阻隔)";
				}
				else
				{
					char buf[128] = {};
					std::snprintf(buf, sizeof(buf), "[X] 视线阻断 (Blocked at X: %.2f, Y: %.2f, Z: %.2f)",
						hitPoint.x, hitPoint.y, hitPoint.z);
					m_LoSStatus = buf;
				}
			}
		}
		innerY += 24.0f;
		if (!m_LoSStatus.empty())
		{
			const Wui::WuiColor losColor = s_RaycastProbe.IsVisible ? theme.Success : theme.Danger;
			Wui::Label(ctx, { startX + 8.0f, innerY }, m_LoSStatus, losColor, theme.FontSizeSmall);
			innerY += 20.0f;
		}

		// 3. 随机漫游采样
		Wui::Label(ctx, { startX + 8.0f, innerY }, "3. 随机漫游采样 (Random Wander Sampling)", theme.Text, theme.FontSizeSmall);
		innerY += 20.0f;

		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "中心 (C):", theme.TextMuted, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.w_cx"), { startX + 8.0f + colLabelW, innerY, valW, 20.0f }, m_WanderCenter.x, 0.5f, -500.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.w_cy"), { startX + 8.0f + colLabelW + valW + 4.0f, innerY, valW, 20.0f }, m_WanderCenter.y, 0.5f, -100.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.w_cz"), { startX + 8.0f + colLabelW + (valW + 4.0f) * 2.0f, innerY, valW, 20.0f }, m_WanderCenter.z, 0.5f, -500.0f, 500.0f, theme);
		innerY += 24.0f;

		const float radLabelW = 55.0f;
		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "半径 (R):", theme.TextMuted, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.w_rad"), { startX + 8.0f + radLabelW, innerY, contentW - 16.0f - radLabelW, 20.0f }, m_WanderRadius, 0.5f, 1.0f, 100.0f, theme);
		innerY += 24.0f;

		if (Wui::Button(ctx, Wui::HashId("navmesh.probe.btn_sample_wander"), { startX + 8.0f, innerY, contentW - 16.0f, 22.0f }, "采样随机漫游目标 (Sample Wander Point)", theme))
		{
			if (!navMesh || navMesh->GetPolygonCount() == 0)
			{
				m_WanderStatus = "导航网格未烘焙，无法采样漫游目标。";
			}
			else
			{
				glm::vec3 outPoint { 0.0f };
				const bool ok = navMesh->GetRandomPointAround(m_WanderCenter, m_WanderRadius, outPoint);
				s_WanderProbe.Center = m_WanderCenter;
				s_WanderProbe.Radius = m_WanderRadius;
				s_WanderProbe.HasSampled = ok;
				s_WanderProbe.SampledPoint = outPoint;

				if (ok)
				{
					char buf[128] = {};
					std::snprintf(buf, sizeof(buf), "[OK] 采样目标: (%.2f, %.2f, %.2f)",
						outPoint.x, outPoint.y, outPoint.z);
					m_WanderStatus = buf;
				}
				else
				{
					m_WanderStatus = "[X] 采样失败 (无法在指定半径内找到可通行多边形)";
				}
			}
		}
		innerY += 24.0f;
		if (!m_WanderStatus.empty())
		{
			const Wui::WuiColor wanderColor = s_WanderProbe.HasSampled ? theme.Accent : theme.Warning;
			Wui::Label(ctx, { startX + 8.0f, innerY }, m_WanderStatus, wanderColor, theme.FontSizeSmall);
		}
	}
}
