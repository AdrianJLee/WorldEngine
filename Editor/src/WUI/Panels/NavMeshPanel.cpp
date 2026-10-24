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
		Wui::Panel(ctx, rect, Title(), theme);

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 32.0f + pad;

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
						std::snprintf(msgBuf, sizeof(msgBuf), "✦ 3D 导航网格后台烘焙成功！耗时: %.2fs，多边形: %zu，顶点: %zu",
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
		Wui::TabBar(ctx, Wui::HashId("navmesh.tabs"), { startX, curY, contentW, 28.0f }, tabs, m_ActiveTabIdx, theme);
		curY += 34.0f;

		switch (m_ActiveTabIdx)
		{
		case 0:
			RenderBakeTab(ctx, startX, curY, contentW, theme, host, navMesh.get());
			break;
		case 1:
			RenderVisibilityTab(ctx, startX, curY, contentW, theme);
			break;
		case 2:
			RenderProbesTab(ctx, startX, curY, contentW, theme, host, navMesh.get());
			break;
		default:
			break;
		}


	}

	void NavMeshPanel::RenderMetricsCard(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, NavMesh* navMesh)
	{
		const float cardH = 58.0f;
		const Wui::WuiRect cardRect { startX, curY, contentW, cardH };
		Wui::PanelBackground(ctx, cardRect, theme.CardBg, theme.Radius);

		const bool hasPolys = (navMesh && navMesh->GetPolygonCount() > 0);
		const char* badgeText = hasPolys ? "● 导航网格已烘焙 (Baked)" : "○ 导航网格未烘焙 / 空 (Empty)";
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

		char statsBuf[256] = {};
		std::snprintf(statsBuf, sizeof(statsBuf), "顶点: %zu   多边形: %zu   面积: %.1f m²   阻挡: %zu   跳跃链接: %zu   区域修饰: %zu",
			vertCount, polyCount, walkableArea, blockedCount, linkCount, modCount);
		Wui::Label(ctx, { startX + 8.0f, curY + 32.0f }, statsBuf, theme.TextMuted, theme.FontSizeSmall);

		curY += cardH + 8.0f;
	}

	void NavMeshPanel::RenderBakeTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host, NavMesh* navMesh)
	{
		const float cardH = 270.0f;
		const Wui::WuiRect cardRect { startX, curY, contentW, cardH };
		Wui::PanelBackground(ctx, cardRect, theme.CardBg, theme.Radius);

		float innerY = curY + 8.0f;
		Wui::Label(ctx, { startX + 8.0f, innerY }, "✦ 全场景烘焙源与代理规格 (Bake Sources & Profiles)", theme.Accent, theme.FontSizeSmall);
		innerY += 24.0f;

		const float labelW = 120.0f;
		const float fieldW = contentW - labelW - 16.0f;

		// 1. 代理规格预设
		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "代理规格预设:", theme.Text, theme.FontSizeSmall);
		static const std::vector<std::string> s_AgentProfiles = {
			"人形单位 (Humanoid: R:0.5m, H:2.0m, Slope:45°, Step:0.45m)",
			"小型爬虫 (Small Creature: R:0.25m, H:0.8m, Slope:50°, Step:0.25m)",
			"大型载具/Boss (Heavy Boss: R:1.5m, H:3.5m, Slope:30°, Step:0.8m)",
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

		// 2. 烘焙空间范围 (Bake Scope: 对标 UE5 NavMeshBoundsVolume 与大世界 World Partition 瓦片)
		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "烘焙空间范围:", theme.Text, theme.FontSizeSmall);
		static const std::vector<std::string> s_BakeScopes = {
			"全场景静态几何 (Full Scene: 烘焙当前关卡全部实体)",
			"视口关注区 (Active Viewport: 仅烘焙相机中心 64m 分区瓦片)"
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
		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "最大爬坡坡度 (°):", theme.Text, theme.FontSizeSmall);
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
		innerY += 26.0f;

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
			std::snprintf(btnBuf, sizeof(btnBuf), "⏳ 后台异步烘焙中... (%.1fs)", elapsed);
			Wui::Button(ctx, Wui::HashId("navmesh.btn.baking"), { startX + 8.0f, innerY, btnW, btnH }, btnBuf, theme);
			Wui::Tooltip(ctx, { startX + 8.0f, innerY, btnW, btnH }, "导航网格正在后台独立工作线程中体素化与构建，UI 保持 60+ FPS 流畅，完成后将自动注入场景");
		}
		else
		{
			if (Wui::Button(ctx, Wui::HashId("navmesh.btn.bake_scene"), { startX + 8.0f, innerY, btnW, btnH }, "✦ 异步烘焙 (Bake)", theme))
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

		innerY += btnH + 6.0f;
		if (!m_StatusFeedback.empty())
		{
			const bool isSuccess = (navMesh && navMesh->GetPolygonCount() > 0);
			const Wui::WuiColor statusColor = isSuccess ? theme.Success : theme.Warning;
			Wui::Label(ctx, { startX + 8.0f, innerY }, m_StatusFeedback, statusColor, theme.FontSizeSmall);
		}

		curY += cardH + 8.0f;
	}

	void NavMeshPanel::RenderProbesTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host, NavMesh* navMesh)
	{
		(void)host;
		const float cardH = 340.0f;
		const Wui::WuiRect cardRect { startX, curY, contentW, cardH };
		Wui::PanelBackground(ctx, cardRect, theme.CardBg, theme.Radius);

		float innerY = curY + 8.0f;
		Wui::Label(ctx, { startX + 8.0f, innerY }, "✦ 通用空间感知与探针测试 (Spatial Query Probes)", theme.Accent, theme.FontSizeSmall);
		innerY += 24.0f;

		const float colLabelW = 70.0f;
		const float colValW = (contentW - 16.0f - colLabelW * 2.0f) * 0.5f;

		// 1. 寻路走廊规划
		Wui::Label(ctx, { startX + 8.0f, innerY }, "1. 寻路走廊规划 (A* Path Corridor)", theme.Text, theme.FontSizeSmall);
		innerY += 20.0f;

		// 起点
		Wui::Label(ctx, { startX + 8.0f, innerY + 2.0f }, "起点 (S):", theme.TextMuted, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.sx"), { startX + colLabelW, innerY, colValW / 3.0f, 20.0f }, m_StartPos.x, 0.5f, -500.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.sy"), { startX + colLabelW + colValW / 3.0f, innerY, colValW / 3.0f, 20.0f }, m_StartPos.y, 0.5f, -100.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.sz"), { startX + colLabelW + colValW * 2.0f / 3.0f, innerY, colValW / 3.0f, 20.0f }, m_StartPos.z, 0.5f, -500.0f, 500.0f, theme);

		// 终点
		const float endLabelX = startX + colLabelW + colValW + 8.0f;
		Wui::Label(ctx, { endLabelX, innerY + 2.0f }, "终点 (E):", theme.TextMuted, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.ex"), { endLabelX + colLabelW, innerY, colValW / 3.0f, 20.0f }, m_EndPos.x, 0.5f, -500.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.ey"), { endLabelX + colLabelW + colValW / 3.0f, innerY, colValW / 3.0f, 20.0f }, m_EndPos.y, 0.5f, -100.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.ez"), { endLabelX + colLabelW + colValW * 2.0f / 3.0f, innerY, colValW / 3.0f, 20.0f }, m_EndPos.z, 0.5f, -500.0f, 500.0f, theme);
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
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.los_sx"), { startX + colLabelW, innerY, colValW / 3.0f, 20.0f }, m_LoSStart.x, 0.5f, -500.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.los_sy"), { startX + colLabelW + colValW / 3.0f, innerY, colValW / 3.0f, 20.0f }, m_LoSStart.y, 0.5f, -100.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.los_sz"), { startX + colLabelW + colValW * 2.0f / 3.0f, innerY, colValW / 3.0f, 20.0f }, m_LoSStart.z, 0.5f, -500.0f, 500.0f, theme);

		Wui::Label(ctx, { endLabelX, innerY + 2.0f }, "目标 (B):", theme.TextMuted, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.los_ex"), { endLabelX + colLabelW, innerY, colValW / 3.0f, 20.0f }, m_LoSEnd.x, 0.5f, -500.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.los_ey"), { endLabelX + colLabelW + colValW / 3.0f, innerY, colValW / 3.0f, 20.0f }, m_LoSEnd.y, 0.5f, -100.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.los_ez"), { endLabelX + colLabelW + colValW * 2.0f / 3.0f, innerY, colValW / 3.0f, 20.0f }, m_LoSEnd.z, 0.5f, -500.0f, 500.0f, theme);
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
					m_LoSStatus = "✓ 视线通达 (Visible - 无几何与障碍阻隔)";
				}
				else
				{
					char buf[128] = {};
					std::snprintf(buf, sizeof(buf), "✗ 视线阻断 (Blocked at X: %.2f, Y: %.2f, Z: %.2f)",
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
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.w_cx"), { startX + colLabelW, innerY, colValW / 3.0f, 20.0f }, m_WanderCenter.x, 0.5f, -500.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.w_cy"), { startX + colLabelW + colValW / 3.0f, innerY, colValW / 3.0f, 20.0f }, m_WanderCenter.y, 0.5f, -100.0f, 500.0f, theme);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.w_cz"), { startX + colLabelW + colValW * 2.0f / 3.0f, innerY, colValW / 3.0f, 20.0f }, m_WanderCenter.z, 0.5f, -500.0f, 500.0f, theme);

		Wui::Label(ctx, { endLabelX, innerY + 2.0f }, "半径 (R):", theme.TextMuted, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("navmesh.probe.w_rad"), { endLabelX + colLabelW, innerY, colValW, 20.0f }, m_WanderRadius, 0.5f, 1.0f, 100.0f, theme);
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
					std::snprintf(buf, sizeof(buf), "✓ 采样目标 (Sampled Point): (%.2f, %.2f, %.2f)",
						outPoint.x, outPoint.y, outPoint.z);
					m_WanderStatus = buf;
				}
				else
				{
					m_WanderStatus = "✗ 采样失败 (无法在指定半径内找到可通行多边形)";
				}
			}
		}
		innerY += 24.0f;
		if (!m_WanderStatus.empty())
		{
			const Wui::WuiColor wanderColor = s_WanderProbe.HasSampled ? theme.Accent : theme.Warning;
			Wui::Label(ctx, { startX + 8.0f, innerY }, m_WanderStatus, wanderColor, theme.FontSizeSmall);
		}

		curY += cardH + 8.0f;
	}

	void NavMeshPanel::RenderVisibilityTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme)
	{
		const float cardH = 110.0f;
		const Wui::WuiRect cardRect { startX, curY, contentW, cardH };
		Wui::PanelBackground(ctx, cardRect, theme.CardBg, theme.Radius);

		float innerY = curY + 8.0f;
		Wui::Label(ctx, { startX + 8.0f, innerY }, "👁 视口 3D 调试可视化开关 (Viewport Overlays)", theme.Accent, theme.FontSizeSmall);
		innerY += 24.0f;

		const float checkW = (contentW - 16.0f) / 3.0f;

		// 第一行开关
		const bool prevShowPolys = ShowPolygons;
		Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.polys"), { startX + 8.0f, innerY, checkW, 22.0f }, "显示多边形网格", ShowPolygons, theme);
		if (ShowPolygons != prevShowPolys)
		{
			ViewportPanel::SetNavMeshVisible(ShowPolygons);
		}
		Wui::Tooltip(ctx, { startX + 8.0f, innerY, checkW, 22.0f }, "在视口中绘制 3D 绿色可通行多边形网格与红色阻挡线框");

		Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.area_costs"), { startX + 8.0f + checkW, innerY, checkW, 22.0f }, "显示区域代价色彩", ShowAreaCosts, theme);
		Wui::Tooltip(ctx, { startX + 8.0f + checkW, innerY, checkW, 22.0f }, "按区域类型着色：普通(翡翠绿)、道路(亮绿)、泥潭(蓝紫)、阻挡(红)");

		Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.links"), { startX + 8.0f + checkW * 2.0f, innerY, checkW, 22.0f }, "显示跳跃点弧线", ShowOffMeshLinks, theme);
		Wui::Tooltip(ctx, { startX + 8.0f + checkW * 2.0f, innerY, checkW, 22.0f }, "在视口中绘制洋红色抛物线跳跃弧线与箭头指示");
		innerY += 28.0f;

		// 第二行开关
		Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.los"), { startX + 8.0f, innerY, checkW, 22.0f }, "显示通视测试线", ShowLoSRaycast, theme);
		Wui::Tooltip(ctx, { startX + 8.0f, innerY, checkW, 22.0f }, "在视口中绘制视线通视射线（绿色高亮通达，红色截断阻挡）");

		Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.paths"), { startX + 8.0f + checkW, innerY, checkW, 22.0f }, "显示路径折线", ShowPaths, theme);
		Wui::Tooltip(ctx, { startX + 8.0f + checkW, innerY, checkW, 22.0f }, "在视口中绘制金黄色平滑折线走廊与航点圆环");

		Wui::Checkbox(ctx, Wui::HashId("navmesh.vis.obstacles"), { startX + 8.0f + checkW * 2.0f, innerY, checkW, 22.0f }, "显示动态障碍物", ShowObstacles, theme);
		Wui::Tooltip(ctx, { startX + 8.0f + checkW * 2.0f, innerY, checkW, 22.0f }, "在视口中绘制动态障碍物的橙色 3D 盒体包围线框");

		curY += cardH + 8.0f;
	}
}
