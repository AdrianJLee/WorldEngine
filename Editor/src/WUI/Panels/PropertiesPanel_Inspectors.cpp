#include "World/Foliage/FoliageRenderer.h"
#include "World/Foliage/Foliage.h"
#include "World/Command/CommandHistory.h"
#include "World/Physics/PhysicsMaterial.h"
#include "World/Audio/AudioSystem.h"
#include "World/Renderer/Tilemap.h"
#include "World/Renderer/Mesh.h"
#include "World/Asset/WModelIO.h"
#include "World/Renderer/MeshLOD.h"
#include "World/Renderer/MeshLODGenerator.h"
#include "World/Renderer/ParticleSystem.h"
#include "World/Renderer/ParticleAsset.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include <unordered_set>
#include "World/Core/StringPool.h"
#include "World/Utils/Paths.h"
#include "PropertiesPanel_Internal.h"
#include "WUI/Panels/BehaviorTreeEditorPanel.h"
#include "WUI/Common/AssetThumbnailRenderer.h"
#include "NavMeshPanel.h"
#include "World/Level/WhiteboxBuilder.h"
#include "World/Level/Spline3D.h"
#include <chrono>
#include <future>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "World/Environment/PCGBiome.h"
#include "World/Level/AI/EQSCoverSystem.h"
#include "World/Foliage/FoliageInteraction.h"
#include "World/Terrain/Terrain.h"
#include "World/Terrain/TerrainRenderer.h"


namespace World
{
	struct PCGPreviewState
	{
		bool Active = false;
		entt::entity TargetEntity = entt::null;
		std::vector<PCGSpawnPoint> Points;
		size_t DominantBiomeCounts[5] = { 0 };
		std::string DominantBiomeName = "None";
		float Density = 0.0f;
	};

	struct EQSCandidatePreview
	{
		glm::vec3 Position { 0.0f };
		float TotalScore { 0.0f };
		bool Occluded { false };
	};

	struct EQSPreviewState
	{
		bool Evaluated = false;
		entt::entity TargetEntity = entt::null;
		glm::vec3 QuerierPos { 0.0f };
		glm::vec3 ThreatPos { 15.0f, 2.0f, 15.0f };
		glm::vec3 BestCoverPos { 0.0f };
		float BestScore { 0.0f };
		std::vector<EQSCandidatePreview> Candidates;
	};

	struct FoliagePreviewState
	{
		bool Active = false;
		entt::entity TargetEntity = entt::null;
		glm::vec3 Displacement { 0.0f };
		glm::vec3 PushDirection { 1.0f, 0.0f, 0.0f };
		float PushMagnitude { 0.0f };
		float LastImpulseTime { 0.0f };
	};

	PCGPreviewState g_PCGPreviewState;
	EQSPreviewState g_EQSPreviewState;
	FoliagePreviewState g_FoliagePreviewState;

namespace
{
	template<typename T, typename = void>
	struct HasCollisionMember : std::false_type {};

	template<typename T>
	struct HasCollisionMember<T, std::void_t<decltype(std::declval<T>().Collision)>> : std::true_type {};

	struct CachedBakedCollision
	{
		std::string Path;
		bool HasBaked = false;
		uint32_t HullCount = 0;
		float Concavity = 0.05f;
		uint32_t MaxVerts = 32;
		std::string Profile = "Default";
	};
	static CachedBakedCollision s_BakedCache;
}


using namespace PropertiesPanelDetail;

static std::string s_LODGenNotification;
static uint32_t s_LODGenEntityId = 0;

	static float DrawTerrainInspector(PanelHost& host, Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		Entity entity, void* instance, const Schema::TypeSchema&, const Wui::WuiRect&,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto* comp = static_cast<TerrainComponent*>(instance);
		if (!comp)
			return 0.0f;

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// 1) 醒目大按钮: 进入地貌编辑模式
		const Wui::WuiRect editBtnRect { startX, curY, contentW, 28.0f };
		auto editTheme = theme;
		editTheme.ButtonBg = theme.ActiveBg;
		editTheme.Border = theme.Accent;
		if (Wui::Button(ctx, Wui::HashId("inspect.terrain.edit_mode"), editBtnRect,
			"[Edit] 进入地貌编辑模式 (Edit Landscape)", editTheme))
		{
			host.SetEditorToolMode(EditorToolMode::Landscape);
			host.ActivatePanel("terrain_editor");
		}
		curY += 32.0f;

		// 2) 视口聚焦地貌按钮
		const Wui::WuiRect focusBtnRect { startX, curY, contentW, 24.0f };
		if (Wui::Button(ctx, Wui::HashId("inspect.terrain.focus"), focusBtnRect,
			"[Focus] 视口聚焦地貌 (Focus In Viewport)", theme))
		{
			host.SetSelectedEntity(entity);
			host.FocusIndependentWindow("view");
		}
		curY += 28.0f;

		// 3) 地貌规格与分块统计
		Wui::Label(ctx, { startX, curY + 2.0f }, "地貌规格与分块统计", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		// 分块分辨率
		Wui::Label(ctx, { startX, curY + 2.0f }, "分块分辨率:", theme.Text, theme.FontSizeSmall);
		int64_t chunkRes = static_cast<int64_t>(comp->ChunkResolution);
		if (Wui::DragInt(ctx, Wui::HashId("inspect.terrain.chunk_res"),
			{ startX + 110.0f, curY, contentW - 110.0f, 22.0f }, chunkRes, 16, 256, theme))
		{
			comp->ChunkResolution = static_cast<uint32_t>(chunkRes);
			if (changedFields) changedFields->push_back("ChunkResolution");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 单元格格距
		Wui::Label(ctx, { startX, curY + 2.0f }, "格距 (CellSize m):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.terrain.cell_size"),
			{ startX + 110.0f, curY, contentW - 110.0f, 22.0f }, comp->CellSize, 0.1f, 0.1f, 50.0f, theme))
		{
			if (changedFields) changedFields->push_back("CellSize");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 高度倍率
		Wui::Label(ctx, { startX, curY + 2.0f }, "高度倍率 (Scale):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.terrain.height_scale"),
			{ startX + 110.0f, curY, contentW - 110.0f, 22.0f }, comp->HeightScale, 0.5f, 0.1f, 500.0f, theme))
		{
			if (changedFields) changedFields->push_back("HeightScale");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 材质图层总数
		Wui::Label(ctx, { startX, curY + 2.0f }, "材质图层总数:", theme.Text, theme.FontSizeSmall);
		int64_t layerCount = static_cast<int64_t>(comp->LayerCount);
		if (Wui::DragInt(ctx, Wui::HashId("inspect.terrain.layer_count"),
			{ startX + 110.0f, curY, contentW - 110.0f, 22.0f }, layerCount, 1, 4, theme))
		{
			comp->LayerCount = static_cast<uint32_t>(layerCount);
			if (changedFields) changedFields->push_back("LayerCount");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// CDLOD 距离比
		Wui::Label(ctx, { startX, curY + 2.0f }, "CDLOD 距离比:", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.terrain.lod_ratio"),
			{ startX + 110.0f, curY, contentW - 110.0f, 22.0f }, comp->LodDistanceRatio, 0.1f, 0.5f, 10.0f, theme))
		{
			if (changedFields) changedFields->push_back("LodDistanceRatio");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 投射阴影
		bool castShadow = (comp->CastShadow != 0);
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.terrain.cast_shadow"),
			{ startX, curY, contentW, 22.0f }, "投射阴影 (Cast Shadow)", castShadow, theme))
		{
			comp->CastShadow = castShadow ? 1 : 0;
			if (changedFields) changedFields->push_back("CastShadow");
			host.MarkDocumentDirty();
		}
		curY += 24.0f;

		// 接收阴影
		bool recvShadow = (comp->ReceiveShadow != 0);
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.terrain.recv_shadow"),
			{ startX, curY, contentW, 22.0f }, "接收阴影 (Receive Shadow)", recvShadow, theme))
		{
			comp->ReceiveShadow = recvShadow ? 1 : 0;
			if (changedFields) changedFields->push_back("ReceiveShadow");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 关联资产
		std::string assetPath = "(未绑定)";
		if (comp->TerrainAsset.HasPath())
			assetPath = StringPool::Get().PathOf(comp->TerrainAsset.Path);
		Wui::Label(ctx, { startX, curY }, "关联资产: " + assetPath, theme.TextMuted, theme.FontSizeSmall);
		curY += 22.0f;

		return curY - rect.Y;
	}

	static float DrawNavMeshAgentInspector(PanelHost& host, Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		Entity entity, void* instance, const Schema::TypeSchema&, const Wui::WuiRect&,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto* agent = static_cast<NavMeshAgentComponent*>(instance);
		if (!agent)
			return 0.0f;

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// =========================================================================
		// 1. 代理体型与运动属性 (Authoring Properties - 实体组件数据)
		// =========================================================================
		Wui::Label(ctx, { startX, curY + 2.0f }, "[配置] 代理体型与运动属性 (Agent Properties)", theme.Accent, theme.FontSizeSmall);
		curY += 22.0f;

		const float paramLabelW = 120.0f;
		const float paramFieldW = contentW - paramLabelW;

		// 移动速度 Speed
		Wui::Label(ctx, { startX, curY + 2.0f }, "移动速度 (m/s):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.nav_agent.speed"),
			{ startX + paramLabelW, curY, paramFieldW, 22.0f }, agent->Speed, 0.1f, 0.1f, 50.0f, theme))
		{
			if (changedFields) changedFields->push_back("Speed");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 加速度 Acceleration
		Wui::Label(ctx, { startX, curY + 2.0f }, "加速度 (m/s2):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.nav_agent.accel"),
			{ startX + paramLabelW, curY, paramFieldW, 22.0f }, agent->Acceleration, 0.2f, 0.5f, 100.0f, theme))
		{
			if (changedFields) changedFields->push_back("Acceleration");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 到达判定距离 StoppingDistance
		Wui::Label(ctx, { startX, curY + 2.0f }, "停止距离 (m):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.nav_agent.stop_dist"),
			{ startX + paramLabelW, curY, paramFieldW, 22.0f }, agent->StoppingDistance, 0.05f, 0.01f, 10.0f, theme))
		{
			if (changedFields) changedFields->push_back("StoppingDistance");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 代理半径 AgentRadius
		Wui::Label(ctx, { startX, curY + 2.0f }, "代理半径 (Radius m):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.nav_agent.radius"),
			{ startX + paramLabelW, curY, paramFieldW, 22.0f }, agent->AgentRadius, 0.05f, 0.1f, 5.0f, theme))
		{
			if (changedFields) changedFields->push_back("AgentRadius");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 站立高度 AgentHeight
		Wui::Label(ctx, { startX, curY + 2.0f }, "站立高度 (Height m):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.nav_agent.height"),
			{ startX + paramLabelW, curY, paramFieldW, 22.0f }, agent->AgentHeight, 0.05f, 0.1f, 10.0f, theme))
		{
			if (changedFields) changedFields->push_back("AgentHeight");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 蹲伏高度 CrouchHeight
		Wui::Label(ctx, { startX, curY + 2.0f }, "蹲伏高度 (Crouch m):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.nav_agent.crouch_height"),
			{ startX + paramLabelW, curY, paramFieldW, 22.0f }, agent->CrouchHeight, 0.05f, 0.1f, 10.0f, theme))
		{
			if (changedFields) changedFields->push_back("CrouchHeight");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 区域掩码 AreaMask
		Wui::Label(ctx, { startX, curY + 2.0f }, "区域掩码 (AreaMask):", theme.Text, theme.FontSizeSmall);
		int64_t maskVal = static_cast<int64_t>(agent->AreaMask);
		if (Wui::DragInt(ctx, Wui::HashId("inspect.nav_agent.area_mask"),
			{ startX + paramLabelW, curY, paramFieldW, 22.0f }, maskVal, 0, 0xFFFFFFFF, theme))
		{
			agent->AreaMask = static_cast<uint32_t>(maskVal);
			if (changedFields) changedFields->push_back("AreaMask");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 自动重规划 AutoRepath
		bool autoRepath = (agent->AutoRepath != 0);
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.nav_agent.auto_repath"),
			{ startX, curY, contentW, 22.0f }, "目标变动或遇阻时自动重规划 (Auto Repath)", autoRepath, theme))
		{
			agent->AutoRepath = autoRepath ? 1 : 0;
			if (changedFields) changedFields->push_back("AutoRepath");
			host.MarkDocumentDirty();
		}
		curY += 32.0f;

		// =========================================================================
		// 2. 运行时调试与模拟 (Runtime Debug & Simulation - 区分开的独立调试面板)
		// =========================================================================
		const float dbgCardPad = 6.0f;
		const float dbgCardH = 248.0f;
		const Wui::WuiRect dbgRect { startX, curY, contentW, dbgCardH };
		Wui::PanelBackground(ctx, dbgRect, theme.CardBg, theme.Radius);
		Wui::HighlightOutline(ctx, dbgRect, theme.Border, theme.Radius, 1.0f);

		float dbgY = curY + dbgCardPad;
		Wui::Label(ctx, { startX + 8.0f, dbgY + 2.0f }, "[调试] 运行时寻路调试与模拟 (Runtime Debug)", theme.Warning, theme.FontSizeSmall);
		dbgY += 22.0f;

		// 状态指示徽标
		const char* statusDesc = "[Idle] 空闲 / 待命 (Idle)";
		Wui::WuiColor statusColor = theme.TextMuted;
		if (agent->ReachedDestination)
		{
			statusDesc = "[Arrived] 已到达目标位置 (Reached Destination)";
			statusColor = theme.Success;
		}
		else if (agent->HasPath)
		{
			statusDesc = "[Navigating] 正在寻路导航 (Navigating)";
			statusColor = theme.Info;
		}
		Wui::Label(ctx, { startX + 8.0f, dbgY + 2.0f }, statusDesc, statusColor, theme.FontSizeBody);
		dbgY += 24.0f;

		// 蹲伏姿态测试切换开关
		bool isCrouching = (agent->Crouching != 0);
		char crouchLabel[64];
		std::snprintf(crouchLabel, sizeof(crouchLabel), "测试姿态: %s (有效身高: %.2fm)",
			isCrouching ? "蹲伏中 [Crouching]" : "站立中 [Standing]",
			agent->GetEffectiveHeight());
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.nav_agent.dbg_crouch"),
			{ startX + 8.0f, dbgY, contentW - 16.0f, 20.0f }, crouchLabel, isCrouching, theme))
		{
			agent->Crouching = isCrouching ? 1 : 0;
			if (changedFields) changedFields->push_back("Crouching");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + 8.0f, dbgY, contentW - 16.0f, 20.0f },
			"运行时测试切换蹲伏姿态：蹲伏时使用 CrouchHeight，站立时使用 AgentHeight，观察低矮通道通行能力");
		dbgY += 24.0f;

		// 目标坐标 Destination (X, Y, Z)
		const float coordLabelW = 16.0f;
		const float coordFieldW = (contentW - 16.0f - coordLabelW * 3.0f - 12.0f) / 3.0f;

		Wui::Label(ctx, { startX + 8.0f, dbgY + 2.0f }, "X", theme.AxisX, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.nav_agent.dest_x"),
			{ startX + 8.0f + coordLabelW, dbgY, coordFieldW, 20.0f }, agent->Destination.x, 0.2f, -2000.0f, 2000.0f, theme))
		{
			if (changedFields) changedFields->push_back("Destination");
			host.MarkDocumentDirty();
		}

		Wui::Label(ctx, { startX + 8.0f + coordLabelW + coordFieldW + 6.0f, dbgY + 2.0f }, "Y", theme.AxisY, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.nav_agent.dest_y"),
			{ startX + 8.0f + coordLabelW * 2.0f + coordFieldW + 6.0f, dbgY, coordFieldW, 20.0f }, agent->Destination.y, 0.2f, -500.0f, 500.0f, theme))
		{
			if (changedFields) changedFields->push_back("Destination");
			host.MarkDocumentDirty();
		}

		Wui::Label(ctx, { startX + 8.0f + (coordLabelW + coordFieldW + 6.0f) * 2.0f, dbgY + 2.0f }, "Z", theme.AxisZ, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.nav_agent.dest_z"),
			{ startX + 8.0f + (coordLabelW + coordFieldW + 6.0f) * 2.0f + coordLabelW, dbgY, coordFieldW, 20.0f }, agent->Destination.z, 0.2f, -2000.0f, 2000.0f, theme))
		{
			if (changedFields) changedFields->push_back("Destination");
			host.MarkDocumentDirty();
		}
		dbgY += 24.0f;

		// 设为视口相机目标点
		const Wui::WuiRect pickBtnRect { startX + 8.0f, dbgY, contentW - 16.0f, 22.0f };
		if (Wui::Button(ctx, Wui::HashId("inspect.nav_agent.btn_cam_dest"), pickBtnRect, "[Focus] 设为当前视口相机焦点 (Set to Viewport Focus)", theme))
		{
			const auto cam = host.GetGizmoCamera();
			agent->Destination = cam.Position + cam.Forward * cam.Distance;
			if (changedFields) changedFields->push_back("Destination");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, pickBtnRect, "直接提取视口 3D 相机的观察焦点作为寻路代理的新目标点");
		dbgY += 26.0f;

		// 代理标记与路径折线自定义色彩 (两个独立调色板 ColorField)
		const float clrFieldW = contentW - 16.0f - 85.0f;
		const float resetBtnW = 78.0f;

		// ① 路径折线颜色 (Path Line Color)
		Wui::Label(ctx, { startX + 8.0f, dbgY + 2.0f }, "路径折线颜色 (Path Line Color):", theme.Text, theme.FontSizeSmall);
		dbgY += 20.0f;

		const Wui::WuiRect pathClrRect { startX + 8.0f, dbgY, clrFieldW, 20.0f };
		const glm::vec4 defaultPathColor { 1.00f, 0.85f, 0.20f, 0.95f };
		glm::vec4 curPathCol = agent->HasCustomColor()
			? agent->GetCustomColor()
			: defaultPathColor;
		if (Wui::ColorField(ctx, Wui::HashId("inspect.nav_agent.path_clr_field"), pathClrRect, curPathCol, theme))
		{
			agent->SetCustomColor(curPathCol);
			if (changedFields) changedFields->push_back("PathColor");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, pathClrRect, "点击打开 2D 调色板面板，自由调节本代理在视口中的平滑路径走廊折线颜色");

		const Wui::WuiRect resetPathClrBtn { startX + 8.0f + clrFieldW + 6.0f, dbgY, resetBtnW, 20.0f };
		if (Wui::Button(ctx, Wui::HashId("inspect.nav_agent.btn_reset_path_clr"), resetPathClrBtn, "重置默认", theme))
		{
			agent->ClearCustomColor();
			if (changedFields) changedFields->push_back("PathColor");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, resetPathClrBtn, "清除本代理的路径折线自定义颜色，恢复使用金黄色默认预设");
		dbgY += 26.0f;

		// ② 代理标记颜色 (Agent Marker Color)
		Wui::Label(ctx, { startX + 8.0f, dbgY + 2.0f }, "代理标记颜色 (Agent Marker Color):", theme.Text, theme.FontSizeSmall);
		dbgY += 20.0f;

		const Wui::WuiRect agentClrRect { startX + 8.0f, dbgY, clrFieldW, 20.0f };
		const glm::vec4 defaultAgentColor { 0.20f, 0.75f, 1.00f, 0.90f };
		glm::vec4 curAgentCol = agent->HasCustomAgentColor()
			? agent->GetCustomAgentColor()
			: defaultAgentColor;
		if (Wui::ColorField(ctx, Wui::HashId("inspect.nav_agent.agent_clr_field"), agentClrRect, curAgentCol, theme))
		{
			agent->SetCustomAgentColor(curAgentCol);
			if (changedFields) changedFields->push_back("AgentColor");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, agentClrRect, "点击打开 2D 调色板面板，自由调节本代理在视口中的 3D 圆柱体线框标记颜色");

		const Wui::WuiRect resetAgentClrBtn { startX + 8.0f + clrFieldW + 6.0f, dbgY, resetBtnW, 20.0f };
		if (Wui::Button(ctx, Wui::HashId("inspect.nav_agent.btn_reset_agent_clr"), resetAgentClrBtn, "重置默认", theme))
		{
			agent->ClearCustomAgentColor();
			if (changedFields) changedFields->push_back("AgentColor");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, resetAgentClrBtn, "清除本代理的标记自定义颜色，恢复使用姿态自适应默认预设 (站立蓝绿/下蹲灰蓝)");
		dbgY += 26.0f;

				curY += dbgCardH + 8.0f;
		return curY - rect.Y;
	}

	static float DrawNavMeshObstacleInspector(PanelHost& host, Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		Entity entity, void* instance, const Schema::TypeSchema&, const Wui::WuiRect&,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto* obs = static_cast<NavMeshObstacleComponent*>(instance);
		if (!obs)
			return 0.0f;

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// 1. 雕刻状态指示
		Wui::Label(ctx, { startX, curY + 2.0f }, "动态雕刻状态 (Carving Status):", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		if (obs->CarveNavMesh)
		{
			Wui::Label(ctx, { startX + 4.0f, curY + 2.0f }, "[Active] 动态雕刻生效中 (Carving Active - 实时打洞)", theme.Warning, theme.FontSizeBody);
		}
		else
		{
			Wui::Label(ctx, { startX + 4.0f, curY + 2.0f }, "[Disabled] 雕刻已关闭 (Pass-Through - 允许穿越)", theme.TextMuted, theme.FontSizeBody);
		}
		curY += 24.0f;

		// 2. Extents 半长宽高调节
		Wui::Label(ctx, { startX, curY + 2.0f }, "盒体半尺寸 Extents (Half Size m):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;

		const float coordLabelW = 16.0f;
		const float coordFieldW = (contentW - coordLabelW * 3.0f - 12.0f) / 3.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "X", theme.AxisX, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.nav_obstacle.ext_x"),
			{ startX + coordLabelW, curY, coordFieldW, 22.0f }, obs->Extents.x, 0.1f, 0.1f, 50.0f, theme))
		{
			if (changedFields) changedFields->push_back("Extents");
			host.MarkDocumentDirty();
		}

		Wui::Label(ctx, { startX + coordLabelW + coordFieldW + 6.0f, curY + 2.0f }, "Y", theme.AxisY, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.nav_obstacle.ext_y"),
			{ startX + coordLabelW * 2.0f + coordFieldW + 6.0f, curY, coordFieldW, 22.0f }, obs->Extents.y, 0.1f, 0.1f, 50.0f, theme))
		{
			if (changedFields) changedFields->push_back("Extents");
			host.MarkDocumentDirty();
		}

		Wui::Label(ctx, { startX + (coordLabelW + coordFieldW + 6.0f) * 2.0f, curY + 2.0f }, "Z", theme.AxisZ, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.nav_obstacle.ext_z"),
			{ startX + (coordLabelW + coordFieldW + 6.0f) * 2.0f + coordLabelW, curY, coordFieldW, 22.0f }, obs->Extents.z, 0.1f, 0.1f, 50.0f, theme))
		{
			if (changedFields) changedFields->push_back("Extents");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 3. 预设尺寸按钮 (路障、木箱、巨石)
		Wui::Label(ctx, { startX, curY + 2.0f }, "预设规格快捷应用:", theme.TextMuted, theme.FontSizeSmall);
		curY += 18.0f;

		const float presetBtnGap = 6.0f;
		const float presetBtnW = (contentW - presetBtnGap * 2.0f) / 3.0f;
		const float presetBtnH = 24.0f;

		if (Wui::Button(ctx, Wui::HashId("inspect.nav_obstacle.preset_roadblock"),
			{ startX, curY, presetBtnW, presetBtnH }, "路障 (2x1x0.5m)", theme))
		{
			obs->Extents = glm::vec3(2.0f, 1.0f, 0.5f);
			if (changedFields) changedFields->push_back("Extents");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, presetBtnW, presetBtnH }, "应用典型峡谷/路口阻挡铁门或路障半尺寸 (2.0m, 1.0m, 0.5m)");

		if (Wui::Button(ctx, Wui::HashId("inspect.nav_obstacle.preset_crate"),
			{ startX + presetBtnW + presetBtnGap, curY, presetBtnW, presetBtnH }, "木箱 (0.8m)", theme))
		{
			obs->Extents = glm::vec3(0.8f, 0.8f, 0.8f);
			if (changedFields) changedFields->push_back("Extents");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + presetBtnW + presetBtnGap, curY, presetBtnW, presetBtnH }, "应用方形木箱/物资箱半尺寸 (0.8m, 0.8m, 0.8m)");

		if (Wui::Button(ctx, Wui::HashId("inspect.nav_obstacle.preset_boulder"),
			{ startX + (presetBtnW + presetBtnGap) * 2.0f, curY, presetBtnW, presetBtnH }, "巨石 (1.5m)", theme))
		{
			obs->Extents = glm::vec3(1.5f, 1.5f, 1.5f);
			if (changedFields) changedFields->push_back("Extents");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + (presetBtnW + presetBtnGap) * 2.0f, curY, presetBtnW, presetBtnH }, "应用大型山体滑坡巨石半尺寸 (1.5m, 1.5m, 1.5m)");

		curY += presetBtnH + 8.0f;

		// 4. 动态打洞开关
		bool carve = (obs->CarveNavMesh != 0);
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.nav_obstacle.carve_chk"),
			{ startX, curY, contentW, 22.0f }, "在导航网格中实时雕刻打洞 (Carve NavMesh)", carve, theme))
		{
			obs->CarveNavMesh = carve ? 1 : 0;
			if (changedFields) changedFields->push_back("CarveNavMesh");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		return curY - rect.Y;
	}


	struct AsyncInspectLODResult
	{
		bool Success = false;
		std::string Error;
		std::vector<GeneratedLODAsset> Generated;
		float Distances[7] = { 15.0f, 30.0f, 50.0f, 75.0f, 105.0f, 140.0f, 180.0f };
		float CullDist = 240.0f;
	};
	static std::future<AsyncInspectLODResult> s_AsyncInspectLODFuture;
	static bool s_AsyncInspectLODRunning = false;
	static std::chrono::time_point<std::chrono::high_resolution_clock> s_AsyncInspectLODStartTime;
	static uint32_t s_AsyncInspectEntityId = 0;

	static float DrawMeshLODComponentInspector(PanelHost& host, Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		Entity entity, void* instance, const Schema::TypeSchema&, const Wui::WuiRect&,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto* lod = static_cast<MeshLODComponent*>(instance);
		if (!lod)
			return 0.0f;

		// 异步生成任务非阻塞轮询 (UI 保持 60+ FPS)
		if (s_AsyncInspectLODRunning)
		{
			if (s_AsyncInspectLODFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
			{
				s_AsyncInspectLODRunning = false;
				auto res = s_AsyncInspectLODFuture.get();
				if (res.Success)
				{
					if (entity.IsValid() && static_cast<uint32_t>(entity) == s_AsyncInspectEntityId)
					{
						lod->LOD1Distance = res.Distances[0];
						lod->LOD2Distance = res.Distances[1];
						lod->LOD3Distance = res.Distances[2];
						lod->LOD4Distance = res.Distances[3];
						lod->LOD5Distance = res.Distances[4];
						lod->LOD6Distance = res.Distances[5];
						lod->LOD7Distance = res.Distances[6];
						lod->CullDistance = res.CullDist;
						if (changedFields)
						{
							changedFields->push_back("LOD1Distance");
							changedFields->push_back("LOD2Distance");
							changedFields->push_back("LOD3Distance");
							changedFields->push_back("LOD4Distance");
							changedFields->push_back("LOD5Distance");
							changedFields->push_back("LOD6Distance");
							changedFields->push_back("LOD7Distance");
							changedFields->push_back("CullDistance");
						}
						host.MarkDocumentDirty();
					}
					for (const auto& gen : res.Generated)
					{
						host.RefreshAssetCatalog(gen.RelativePath);
					}
					s_LODGenNotification = "已异步生成 " + std::to_string(res.Generated.size()) + " 级 LOD 资产并完成零配置自动绑定！";
					s_LODGenEntityId = s_AsyncInspectEntityId;
					host.Notify(s_LODGenNotification);
				}
				else
				{
					s_LODGenNotification = "生成失败: " + res.Error;
					s_LODGenEntityId = s_AsyncInspectEntityId;
					host.Notify(s_LODGenNotification);
				}
			}
		}

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// =========================================================================
		// 1. 顶部当前相机距离与激活状态横幅 (8 级光谱色支持)
		// =========================================================================
		const glm::vec3 cameraPos = host.GetGizmoCamera().Position;
		glm::vec3 entPos(0.0f);
		if (entity.HasComponent<TransformComponent>())
			entPos = entity.GetComponent<TransformComponent>().Location;
		const float distance = glm::length(cameraPos - entPos);

		const uint8_t activeLOD = lod->CurrentLOD;
		std::string badgeText = "[LOD 0 (Base 原模)]";
		Wui::WuiColor badgeCol { 0.20f, 0.90f, 0.20f, 1.0f };
		Wui::WuiColor bgCol { 0.05f, 0.22f, 0.08f, 0.90f };

		if (activeLOD == 255)
		{
			badgeText = "[CULLED 视距剔除]";
			badgeCol = Wui::WuiColor{ 0.95f, 0.20f, 0.20f, 1.0f };
			bgCol = Wui::WuiColor{ 0.25f, 0.05f, 0.05f, 0.90f };
		}
		else if (activeLOD >= 1 && activeLOD <= 7)
		{
			static const char* s_LodNames[8] = {
				"[LOD 0 (Base)]", "[LOD 1 (高精)]", "[LOD 2 (中模)]", "[LOD 3 (过渡)]",
				"[LOD 4 (简模)]", "[LOD 5 (极简)]", "[LOD 6 (剪影)]", "[LOD 7 (微模)]"
			};
			badgeText = s_LodNames[activeLOD];
			const glm::vec4 col = MeshLODManager::Get().GetLODColor(activeLOD);
			badgeCol = Wui::WuiColor{ col.r, col.g, col.b, 1.0f };
			bgCol = Wui::WuiColor{ col.r * 0.25f, col.g * 0.25f, col.b * 0.25f, 0.90f };
		}

		const Wui::WuiRect bannerRect { startX, curY, contentW, 28.0f };
		Wui::PanelBackground(ctx, bannerRect, bgCol, 4.0f);
		Wui::HighlightOutline(ctx, bannerRect, badgeCol, 4.0f, 1.2f);
		Wui::Label(ctx, { bannerRect.X + 8.0f, bannerRect.Y + 6.0f }, badgeText, badgeCol, theme.FontSizeBody);

		char distText[96];
		std::snprintf(distText, sizeof(distText), "Dist: %.1fm | Bias: %.2f | Hyst: %.1fm",
			distance, lod->LODBias, lod->Hysteresis);
		const float distW = ctx.MeasureTextWidth(distText, theme.FontSizeSmall);
		Wui::Label(ctx, { bannerRect.X + bannerRect.W - distW - 8.0f, bannerRect.Y + 7.0f }, distText, theme.Text, theme.FontSizeSmall);
		curY += 34.0f;

		// =========================================================================
		// 1.5 智能一键生成并配置 LOD (异步多线程并发，0 卡顿)
		// =========================================================================
		if (s_AsyncInspectLODRunning)
		{
			const Wui::WuiRect runningRect { startX, curY, contentW, 26.0f };
			Wui::PanelBackground(ctx, runningRect, Wui::WuiColor{ 0.12f, 0.22f, 0.35f, 0.90f }, 3.0f);
			Wui::HighlightOutline(ctx, runningRect, theme.Accent, 3.0f, 1.0f);
			const auto now = std::chrono::high_resolution_clock::now();
			const double elapsed = std::chrono::duration<double>(now - s_AsyncInspectLODStartTime).count();
			char b[128];
			std::snprintf(b, sizeof(b), "[后台生成中...] 正在异步减面生成 LOD 资产 (%.1fs，UI 60+ FPS)", elapsed);
			Wui::Label(ctx, { runningRect.X + 8.0f, runningRect.Y + 5.0f }, b, theme.Accent, theme.FontSizeSmall);
			curY += 32.0f;
		}
		else if (s_LODGenEntityId == static_cast<uint32_t>(entity) && !s_LODGenNotification.empty())
		{
			const Wui::WuiRect noteRect { startX, curY, contentW, 24.0f };
			Wui::PanelBackground(ctx, noteRect, Wui::WuiColor{ 0.10f, 0.35f, 0.15f, 0.90f }, 3.0f);
			Wui::HighlightOutline(ctx, noteRect, Wui::WuiColor{ 0.25f, 0.85f, 0.35f, 1.0f }, 3.0f, 1.0f);
			Wui::Label(ctx, { noteRect.X + 8.0f, noteRect.Y + 4.0f }, s_LODGenNotification, Wui::WuiColor{ 0.40f, 0.98f, 0.50f, 1.0f }, theme.FontSizeSmall);
			curY += 28.0f;
		}

		const Wui::WuiRect autoGenRect { startX, curY, contentW, 28.0f };
		auto autoGenTheme = theme;
		autoGenTheme.ButtonBg = theme.ActiveBg;
		autoGenTheme.Border = theme.Accent;
		if (Wui::Button(ctx, Wui::HashId("inspect.lod.autogen_bind"), autoGenRect,
			s_AsyncInspectLODRunning ? "[后台生成中，请稍候...]" : "[智能一键生成并配置 LOD (后台异步 60+ FPS)]", autoGenTheme))
		{
			if (!s_AsyncInspectLODRunning)
			{
				if (!entity.HasComponent<MeshRendererComponent>())
				{
					s_LODGenNotification = "错误: 实体未挂载 MeshRendererComponent";
					s_LODGenEntityId = static_cast<uint32_t>(entity);
					host.Notify(s_LODGenNotification);
				}
				else
				{
					const auto& mr = entity.GetComponent<MeshRendererComponent>();
					std::string sourceMeshPath;
					if (mr.Mesh.HasPath())
						sourceMeshPath = StringPool::Get().PathOf(mr.Mesh.Path);

					if (sourceMeshPath.empty())
					{
						s_LODGenNotification = "错误: 实体无有效 .wmodel 网格资产 (内置几何体请先保存为资产)";
						s_LODGenEntityId = static_cast<uint32_t>(entity);
						host.Notify(s_LODGenNotification);
					}
					else
					{
						glm::vec3 scale(1.0f);
						if (entity.HasComponent<TransformComponent>())
							scale = glm::abs(entity.GetComponent<TransformComponent>().Scale);
						const float maxScale = std::max({ scale.x, scale.y, scale.z, 0.001f });

						float maxExtent = 1.0f;
						std::string loadErr;
						Ref<Mesh> mesh = Mesh::LoadWModel(mr.Mesh.Path, &loadErr);
						if (mesh)
						{
							const MeshBounds& bounds = mesh->GetBounds();
							const glm::vec3 ext = bounds.Max - bounds.Min;
							maxExtent = std::max({ ext.x, ext.y, ext.z, 0.1f });
						}

						const float S = maxScale * maxExtent;
						float d[7] = {
							std::max(12.0f, S * 12.0f),
							std::max(24.0f, S * 24.0f),
							std::max(40.0f, S * 40.0f),
							std::max(60.0f, S * 60.0f),
							std::max(85.0f, S * 85.0f),
							std::max(115.0f, S * 115.0f),
							std::max(150.0f, S * 150.0f)
						};
						float cullD = std::max(200.0f, S * 200.0f);

						std::vector<LODLevelConfig> configs = {
							{ 1, 0.70f, d[0] },
							{ 2, 0.45f, d[1] },
							{ 3, 0.28f, d[2] },
							{ 4, 0.16f, d[3] },
							{ 5, 0.09f, d[4] },
							{ 6, 0.05f, d[5] },
							{ 7, 0.025f, d[6] }
						};

						s_AsyncInspectEntityId = static_cast<uint32_t>(entity);
						s_AsyncInspectLODStartTime = std::chrono::high_resolution_clock::now();
						s_AsyncInspectLODRunning = true;

						s_AsyncInspectLODFuture = std::async(std::launch::async, [sourceMeshPath, configs, d, cullD]() -> AsyncInspectLODResult {
							AsyncInspectLODResult res;
							std::string genErr;
							if (MeshLODGenerator::GenerateAndSaveLODFiles(sourceMeshPath, configs, res.Generated, &genErr))
							{
								res.Success = true;
								for (int i = 0; i < 7; ++i) res.Distances[i] = d[i];
								res.CullDist = cullD;
							}
							else
							{
								res.Success = false;
								res.Error = genErr;
							}
							return res;
						});
					}
				}
			}
		}
		Wui::Tooltip(ctx, autoGenRect, "根据实体网格与尺度在后台异步减面输出多级 LOD 资产并推导视距阶梯 (0 卡顿)");
		curY += 34.0f;

		const float paramLabelW = 120.0f;
		const float paramFieldW = contentW - paramLabelW;

		// =========================================================================
		// 2. 强制预览档位下拉框 (Force LOD Preview)
		// =========================================================================
		const std::vector<std::string> forceOptions = {
			"Auto (自动距离评估)",
			"Force LOD 0 (原模)",
			"Force LOD 1 (高精)",
			"Force LOD 2 (中模)",
			"Force LOD 3 (过渡)",
			"Force LOD 4 (简模)",
			"Force LOD 5 (极简)",
			"Force LOD 6 (剪影)",
			"Force LOD 7 (微模)"
		};
		const int8_t curForced = (lod->ForcedLOD >= 0) ? lod->ForcedLOD : MeshLODManager::Get().GetForcedLOD();
		int selectedForce = (curForced >= 0 && curForced <= 7) ? (curForced + 1) : 0;
		const int beforeForce = selectedForce;
		Wui::Label(ctx, { startX, curY + 2.0f }, "强制预览 (Force LOD):", theme.Text, theme.FontSizeSmall);
		if (Wui::Combo(ctx, Wui::HashId("inspect.lod.force_combo"),
			{ startX + paramLabelW, curY, paramFieldW, 22.0f }, "", forceOptions, selectedForce, theme))
		{
			if (selectedForce != beforeForce)
			{
				const int8_t newForced = (selectedForce == 0) ? -1 : static_cast<int8_t>(selectedForce - 1);
				lod->ForcedLOD = newForced;
				if (changedFields) changedFields->push_back("ForcedLOD");
				host.MarkDocumentDirty();
			}
		}
		curY += 26.0f;

		// =========================================================================
		// 3. 距离阶梯与调优滑块 (8 级完整阶梯)
		// =========================================================================
		auto drawDistField = [&](const char* label, const char* fieldName, float& val, float minV, float maxV, uint32_t idHash)
		{
			Wui::Label(ctx, { startX, curY + 2.0f }, label, theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, idHash,
				{ startX + paramLabelW, curY, paramFieldW, 22.0f }, val, 0.5f, minV, maxV, theme))
			{
				if (changedFields) changedFields->push_back(fieldName);
				host.MarkDocumentDirty();
			}
			curY += 26.0f;
		};

		drawDistField("LOD1 距离 (m):", "LOD1Distance", lod->LOD1Distance, 1.0f, 1000.0f, Wui::HashId("inspect.lod.dist1"));
		drawDistField("LOD2 距离 (m):", "LOD2Distance", lod->LOD2Distance, 2.0f, 1500.0f, Wui::HashId("inspect.lod.dist2"));
		drawDistField("LOD3 距离 (m):", "LOD3Distance", lod->LOD3Distance, 3.0f, 2000.0f, Wui::HashId("inspect.lod.dist3"));
		drawDistField("LOD4 距离 (m):", "LOD4Distance", lod->LOD4Distance, 4.0f, 2500.0f, Wui::HashId("inspect.lod.dist4"));
		drawDistField("LOD5 距离 (m):", "LOD5Distance", lod->LOD5Distance, 5.0f, 3000.0f, Wui::HashId("inspect.lod.dist5"));
		drawDistField("LOD6 距离 (m):", "LOD6Distance", lod->LOD6Distance, 6.0f, 3500.0f, Wui::HashId("inspect.lod.dist6"));
		drawDistField("LOD7 距离 (m):", "LOD7Distance", lod->LOD7Distance, 7.0f, 4000.0f, Wui::HashId("inspect.lod.dist7"));
		drawDistField("超距剔除 (m):", "CullDistance", lod->CullDistance, 10.0f, 5000.0f, Wui::HashId("inspect.lod.cull_dist"));
		drawDistField("防抖缓冲 (m):", "Hysteresis", lod->Hysteresis, 0.1f, 50.0f, Wui::HashId("inspect.lod.hysteresis"));
		drawDistField("视距偏置 (Bias):", "LODBias", lod->LODBias, 0.1f, 10.0f, Wui::HashId("inspect.lod.bias"));

		// 平滑淡入淡出过渡开关
		bool crossFade = (lod->EnableCrossFade != 0);
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.lod.cross_fade"),
			{ startX, curY, contentW, 22.0f }, "平滑淡入淡出过渡 (Cross Fade)", crossFade, theme))
		{
			lod->EnableCrossFade = crossFade ? 1 : 0;
			if (changedFields) changedFields->push_back("EnableCrossFade");
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		// =========================================================================
		// 4. 资产槽位与零配置自动继承 (Zero-Config LOD Binding)
		// =========================================================================
		std::string lod0MeshName = "(无网格)";
		std::string sourceMeshPath;
		if (entity.HasComponent<MeshRendererComponent>())
		{
			const auto& mr = entity.GetComponent<MeshRendererComponent>();
			if (mr.Mesh.HasPath())
			{
				lod0MeshName = StringPool::Get().PathOf(mr.Mesh.Path);
				sourceMeshPath = lod0MeshName;
			}
			else
			{
				switch (mr.Primitive)
				{
				case MeshRendererComponent::PrimitiveShape::Cube: lod0MeshName = "Cube (内置立方体)"; break;
				case MeshRendererComponent::PrimitiveShape::Sphere: lod0MeshName = "Sphere (内置球体)"; break;
				case MeshRendererComponent::PrimitiveShape::Plane: lod0MeshName = "Plane (内置平面)"; break;
				}
			}
		}
		Wui::Label(ctx, { startX, curY + 2.0f }, "LOD0 基准网格:", theme.Text, theme.FontSizeSmall);
		const Wui::WuiRect lod0Rect { startX + paramLabelW, curY, paramFieldW, 22.0f };
		Wui::PanelBackground(ctx, lod0Rect, theme.CardBg, 3.0f);
		Wui::HighlightOutline(ctx, lod0Rect, theme.Border, 3.0f, 1.0f);
		Wui::Label(ctx, { lod0Rect.X + 6.0f, lod0Rect.Y + 3.0f }, lod0MeshName, theme.TextDisabled, theme.FontSizeSmall);
		curY += 26.0f;

		// 检查基础模型同目录下是否存在自动派生的 LOD 资产文件 (LOD1 到 LOD8)
		int discoveredLODCount = 0;
		if (!sourceMeshPath.empty())
		{
			std::filesystem::path srcP(sourceMeshPath);
			std::filesystem::path srcParent = srcP.parent_path();
			std::string srcStem = srcP.stem().string();

			auto checkAutoFile = [](const std::string& rel) -> bool {
				if (rel.empty()) return false;
				std::error_code ec;
				std::filesystem::path p(rel);
				if (std::filesystem::exists(p, ec) && !std::filesystem::is_directory(p, ec)) return true;
				if (std::filesystem::exists(World::Paths::AssetRoot() / p, ec) && !std::filesystem::is_directory(World::Paths::AssetRoot() / p, ec)) return true;
				if (std::filesystem::exists(World::Paths::EngineContentRoot() / p, ec) && !std::filesystem::is_directory(World::Paths::EngineContentRoot() / p, ec)) return true;
				if (std::filesystem::exists(World::Paths::ProjectDir() / p, ec) && !std::filesystem::is_directory(World::Paths::ProjectDir() / p, ec)) return true;
				const auto& catalog = Editor::AssetCatalog::PathsForName("Mesh");
				return std::find(catalog.begin(), catalog.end(), rel) != catalog.end();
			};

			for (int lvl = 1; lvl <= 8; ++lvl)
			{
				std::string rel = (srcParent.empty() ? (srcStem + "_LOD" + std::to_string(lvl) + ".wmodel") : (srcParent / (srcStem + "_LOD" + std::to_string(lvl) + ".wmodel"))).generic_string();
				if (checkAutoFile(rel))
					++discoveredLODCount;
			}
		}

		if (discoveredLODCount > 0)
		{
			const Wui::WuiRect autoBadge { startX, curY, contentW, 24.0f };
			Wui::PanelBackground(ctx, autoBadge, Wui::WuiColor{ 0.08f, 0.25f, 0.12f, 0.90f }, 3.0f);
			Wui::HighlightOutline(ctx, autoBadge, theme.Success, 3.0f, 1.0f);
			std::string badgeStr = "[✓ 零配置自动继承就绪: 已发现 " + std::to_string(discoveredLODCount) + " 级同名减面资产，渲染器全自动挂接]";
			Wui::Label(ctx, { autoBadge.X + 8.0f, autoBadge.Y + 5.0f }, badgeStr, theme.Success, theme.FontSizeSmall);
			Wui::Tooltip(ctx, autoBadge, "现代 AAA 约定式资产驱动体系: 实体无需手动配置槽位，渲染器自动匹配同名 LOD 资产");
			curY += 28.0f;
		}

		// 自定义替代网格 (可选覆盖，默认留空自动继承)
		{
			const std::vector<std::string>& meshPaths = Editor::AssetCatalog::PathsForName("Mesh");
			const float pickBtnW = 26.0f;
			const std::string curCustom = lod->CustomMeshOverride.HasPath() ? std::string(StringPool::Get().PathOf(lod->CustomMeshOverride.Path)) : "";
			std::vector<std::string> opts;
			opts.push_back("(None / 零配置自动继承)");
			opts.insert(opts.end(), meshPaths.begin(), meshPaths.end());
			int sel = 0;
			auto it = std::find(meshPaths.begin(), meshPaths.end(), curCustom);
			if (it != meshPaths.end())
				sel = static_cast<int>(it - meshPaths.begin()) + 1;
			else if (!curCustom.empty())
			{
				opts.push_back(curCustom);
				sel = static_cast<int>(opts.size()) - 1;
			}
			const int beforeSel = sel;
			Wui::Label(ctx, { startX, curY + 2.0f }, "特殊网格覆盖 (可选):", theme.Text, theme.FontSizeSmall);
			const Wui::WuiRect comboRect { startX + paramLabelW, curY, paramFieldW - pickBtnW - 4.0f, 22.0f };
			const Wui::WuiRect pickRect { startX + paramLabelW + paramFieldW - pickBtnW, curY, pickBtnW, 22.0f };
			if (Wui::Combo(ctx, Wui::HashId("inspect.lod.custom_combo"), comboRect, "", opts, sel, theme))
			{
				if (sel != beforeSel)
				{
					if (sel == 0)
						lod->CustomMeshOverride = AssetRef{};
					else
						lod->CustomMeshOverride = AssetRef{ StringPool::Get().InternPath(opts[sel]), AssetId{} };
					if (changedFields) changedFields->push_back("CustomMeshOverride");
					host.MarkDocumentDirty();
				}
			}
			if (Wui::Button(ctx, Wui::HashId("inspect.lod.custom_btn"), pickRect, "...", theme))
			{
				host.OpenAssetPicker("Select Custom Override Mesh", AssetCategory::Mesh, [lod, &host, changedFields](const std::string& picked) {
					lod->CustomMeshOverride = AssetRef{ StringPool::Get().InternPath(picked), AssetId{} };
					if (changedFields) changedFields->push_back("CustomMeshOverride");
					host.MarkDocumentDirty();
				});
			}
			curY += 26.0f;
		}

		// 快速测试辅助按钮与聚焦
		const float btnHalfW = (contentW - 6.0f) * 0.5f;
		const Wui::WuiRect quickSetRect { startX, curY, btnHalfW, 26.0f };
		const Wui::WuiRect focusRect { startX + btnHalfW + 6.0f, curY, btnHalfW, 26.0f };

		if (Wui::Button(ctx, Wui::HashId("inspect.lod.quick_set"), quickSetRect, "[Quick Set] 8级标准阶梯", theme))
		{
			lod->LOD1Distance = 15.0f;
			lod->LOD2Distance = 30.0f;
			lod->LOD3Distance = 50.0f;
			lod->LOD4Distance = 75.0f;
			lod->LOD5Distance = 105.0f;
			lod->LOD6Distance = 140.0f;
			lod->LOD7Distance = 180.0f;
			lod->CullDistance = 240.0f;
			lod->Hysteresis = 2.0f;
			lod->LODBias = 1.0f;
			lod->ForcedLOD = -1;
			lod->MaxLODLevel = 7;

			if (changedFields)
			{
				changedFields->push_back("LOD1Distance");
				changedFields->push_back("LOD2Distance");
				changedFields->push_back("LOD3Distance");
				changedFields->push_back("LOD4Distance");
				changedFields->push_back("LOD5Distance");
				changedFields->push_back("LOD6Distance");
				changedFields->push_back("LOD7Distance");
				changedFields->push_back("CullDistance");
				changedFields->push_back("Hysteresis");
				changedFields->push_back("LODBias");
			}
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, quickSetRect, "一键应用标准 8 级视距阶梯 (15m~180m)，零配置自动继承同名 LOD 资产");

		if (Wui::Button(ctx, Wui::HashId("inspect.lod.focus"), focusRect, "[Focus] 视口聚焦实体", theme))
		{
			host.ResetViewportCamera();
		}
		Wui::Tooltip(ctx, focusRect, "重置视口相机并平滑聚焦对齐该实体");

		curY += 32.0f;
		return curY - rect.Y;
	}

	static float DrawDecalInspector(PanelHost& host, Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		Entity entity, void* instance, const Schema::TypeSchema&, const Wui::WuiRect&,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto* comp = static_cast<DecalComponent*>(instance);
		if (!comp)
			return 0.0f;

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// 1. 材质与贴图资产下拉选择器 (DecalMaterial - 通用资产下拉)
		Wui::Label(ctx, { startX, curY + 2.0f }, "投影材质/贴图 (Decal Material / Texture):", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		// 收集工程中的所有可用材质 (.wmat) 与纹理 (.png/.jpg/.wtex)
		const std::vector<std::string>& matPaths = Editor::AssetCatalog::PathsForName("Material");
		const std::vector<std::string>& texPaths = Editor::AssetCatalog::PathsForName("Texture");
		std::vector<std::string> decalOptions;
		decalOptions.reserve(matPaths.size() + texPaths.size() + 2);
		decalOptions.push_back("(Default Decal / 引擎默认贴花)");
		for (const auto& p : matPaths)
			decalOptions.push_back(p);
		for (const auto& p : texPaths)
			decalOptions.push_back(p);

		const std::string curMat = comp->DecalMaterial.HasPath()
			? std::string(StringPool::Get().PathOf(comp->DecalMaterial.Path)) : "";

		int selectedMatIdx = 0;
		if (!curMat.empty())
		{
			const auto found = std::find(decalOptions.begin() + 1, decalOptions.end(), curMat);
			if (found != decalOptions.end())
			{
				selectedMatIdx = static_cast<int>(found - decalOptions.begin());
			}
			else
			{
				decalOptions.push_back(curMat);
				selectedMatIdx = static_cast<int>(decalOptions.size()) - 1;
			}
		}

		const float thumbSize = 22.0f;
		const float pickBtnW = 26.0f;
		const float clearBtnW = 22.0f;
		const float comboW = contentW - thumbSize - pickBtnW - clearBtnW - 12.0f;

		// 资产缩略图小方块
		const Wui::WuiRect thumbRect { startX, curY, thumbSize, thumbSize };
		if (!curMat.empty())
		{
			const bool isWmat = curMat.size() >= 5 && curMat.compare(curMat.size() - 5, 5, ".wmat") == 0;
			Editor::AssetThumbnailRenderer::DrawThumbnail(ctx, thumbRect, curMat,
				isWmat ? AssetCategory::Material : AssetCategory::Texture, theme);
			Wui::HighlightOutline(ctx, thumbRect, theme.BorderStrong, 3.0f, 1.0f);
		}
		else
		{
			Wui::PanelBackground(ctx, thumbRect, theme.ContentBg, 3.0f);
			Wui::HighlightOutline(ctx, thumbRect, theme.Border, 3.0f, 1.0f);
			Wui::Label(ctx, { thumbRect.X + 7.0f, thumbRect.Y + 4.0f }, "*", theme.Accent, 12.0f);
		}
		Wui::Tooltip(ctx, thumbRect, curMat.empty() ? "当前使用引擎内置默认贴花" : curMat);

		// 通用资产下拉框 (SearchableCombo)
		const Wui::WuiRect comboRect { startX + thumbSize + 4.0f, curY, comboW, 22.0f };
		Wui::Tooltip(ctx, comboRect, "选择贴花投射的材质 (.wmat) 或贴图资产");
		const int beforeMatIdx = selectedMatIdx;
		if (Wui::SearchableCombo(ctx, Wui::HashId("inspect.decal.mat_combo"), comboRect, "",
			decalOptions, selectedMatIdx, theme, "", true) && selectedMatIdx != beforeMatIdx)
		{
			if (selectedMatIdx <= 0)
			{
				comp->DecalMaterial = AssetRef{};
			}
			else if (static_cast<size_t>(selectedMatIdx) < decalOptions.size())
			{
				comp->DecalMaterial = AssetRef{ StringPool::Get().InternPath(decalOptions[selectedMatIdx]), AssetId{} };
			}
			if (changedFields) changedFields->push_back("DecalMaterial");
			host.MarkDocumentDirty();
		}

		// 资产选择器浏览按钮 (...)
		const Wui::WuiRect pickBtnRect { startX + thumbSize + 4.0f + comboW + 4.0f, curY, pickBtnW, 22.0f };
		if (Wui::Button(ctx, Wui::HashId("inspect.decal.mat_pick"), pickBtnRect, "...", theme))
		{
			host.OpenAssetPicker("Select Decal Material or Texture", AssetCategory::Material,
				[comp, &host, changedFields](const std::string& picked)
				{
					comp->DecalMaterial = AssetRef{ StringPool::Get().InternPath(picked), AssetId{} };
					if (changedFields) changedFields->push_back("DecalMaterial");
					host.MarkDocumentDirty();
				});
		}
		Wui::Tooltip(ctx, pickBtnRect, "打开资产选择器挑选贴花材质 (.wmat) 或纹理 (.png/.jpg)");

		// 清除按钮 (x)
		const Wui::WuiRect clearBtnRect { startX + thumbSize + 4.0f + comboW + 4.0f + pickBtnW + 4.0f, curY, clearBtnW, 22.0f };
		if (Wui::Button(ctx, Wui::HashId("inspect.decal.mat_clear"), clearBtnRect, "x", theme))
		{
			comp->DecalMaterial = AssetRef{};
			if (changedFields) changedFields->push_back("DecalMaterial");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, clearBtnRect, "清空自定义资产，恢复引擎内置默认贴花");
		curY += 28.0f;

		// 2. 投影半尺寸 Extents (Half Size in meters) - 通用三轴几何输入
		Wui::Label(ctx, { startX, curY + 2.0f }, "投影尺寸 Extents (X/Z 面宽, Y 深度 m):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;

		const float coordLabelW = 16.0f;
		const float coordFieldW = (contentW - coordLabelW * 3.0f - 12.0f) / 3.0f;

		// X: 半宽
		Wui::Label(ctx, { startX, curY + 2.0f }, "X", theme.AxisX, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.decal.ext_x"),
			{ startX + coordLabelW, curY, coordFieldW, 22.0f }, comp->Extents.x, 0.02f, 0.01f, 100.0f, theme))
		{
			if (changedFields) changedFields->push_back("Extents");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + coordLabelW, curY, coordFieldW, 22.0f }, "X 轴半尺寸 (米)：投影面局部水平半宽度");

		// Y: 半深度（投影方向）
		Wui::Label(ctx, { startX + coordLabelW + coordFieldW + 6.0f, curY + 2.0f }, "Y", theme.AxisY, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.decal.ext_y"),
			{ startX + coordLabelW * 2.0f + coordFieldW + 6.0f, curY, coordFieldW, 22.0f }, comp->Extents.y, 0.02f, 0.01f, 100.0f, theme))
		{
			if (changedFields) changedFields->push_back("Extents");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + coordLabelW * 2.0f + coordFieldW + 6.0f, curY, coordFieldW, 22.0f }, "Y 轴半尺寸 (米)：沿投影法向穿透地表与网格的半深度");

		// Z: 半高
		Wui::Label(ctx, { startX + (coordLabelW + coordFieldW + 6.0f) * 2.0f, curY + 2.0f }, "Z", theme.AxisZ, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.decal.ext_z"),
			{ startX + (coordLabelW + coordFieldW + 6.0f) * 2.0f + coordLabelW, curY, coordFieldW, 22.0f }, comp->Extents.z, 0.02f, 0.01f, 100.0f, theme))
		{
			if (changedFields) changedFields->push_back("Extents");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + (coordLabelW + coordFieldW + 6.0f) * 2.0f + coordLabelW, curY, coordFieldW, 22.0f }, "Z 轴半尺寸 (米)：投影面局部垂直半长度");
		curY += 28.0f;

		// 3. 透明衰减 FadeFactor (0.0 ~ 1.0)
		Wui::Label(ctx, { startX, curY + 2.0f }, "透明衰减 (Fade Factor):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.decal.fade_factor"),
			{ startX + 130.0f, curY, contentW - 130.0f, 22.0f }, comp->FadeFactor, 0.01f, 0.0f, 1.0f, theme))
		{
			if (changedFields) changedFields->push_back("FadeFactor");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + 130.0f, curY, contentW - 130.0f, 22.0f }, "贴花整体不透明度倍率 (0.0 = 完全透明，1.0 = 完全不透明)");
		curY += 26.0f;

		// 4. 掠射角法线消隐 NormalAngleCutoff (0° ~ 89°)
		Wui::Label(ctx, { startX, curY + 2.0f }, "掠射角消隐 (Normal Cutoff °):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.decal.angle_cutoff"),
			{ startX + 160.0f, curY, contentW - 160.0f, 22.0f }, comp->NormalAngleCutoff, 0.5f, 0.0f, 89.0f, theme))
		{
			if (changedFields) changedFields->push_back("NormalAngleCutoff");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + 160.0f, curY, contentW - 160.0f, 22.0f },
			"表面法线与投影方向的最大夹角阈值 (推荐 60°)。当受体表面过度倾斜时平滑消隐，杜绝侧面拉伸走样失真");
		curY += 26.0f;

		// 5. 投影受影响目标 (AffectTerrain / AffectMeshes)
		Wui::Label(ctx, { startX, curY + 2.0f }, "投影目标受体掩码 (Target Mask):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;

		const float chkHalfW = (contentW - 6.0f) * 0.5f;
		bool affectTerrain = comp->AffectTerrain != 0;
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.decal.chk_terrain"),
			{ startX, curY, chkHalfW, 22.0f }, "影响地形 (Terrain)", affectTerrain, theme))
		{
			comp->AffectTerrain = affectTerrain ? 1 : 0;
			if (changedFields) changedFields->push_back("AffectTerrain");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, chkHalfW, 22.0f }, "是否投射到 Landscape 3D 地貌表面");

		bool affectMeshes = comp->AffectMeshes != 0;
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.decal.chk_meshes"),
			{ startX + chkHalfW + 6.0f, curY, chkHalfW, 22.0f }, "影响网格 (Meshes)", affectMeshes, theme))
		{
			comp->AffectMeshes = affectMeshes ? 1 : 0;
			if (changedFields) changedFields->push_back("AffectMeshes");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + chkHalfW + 6.0f, curY, chkHalfW, 22.0f }, "是否投射到静态与动态 3D 网格表面");
		curY += 28.0f;

		return curY - rect.Y;
	}

	static float DrawConvexCollider3DInspector(PanelHost& host, Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		Entity entity, void* instance, const Schema::TypeSchema&, const Wui::WuiRect&,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto* comp = static_cast<ConvexCollider3DComponent*>(instance);
		if (!comp)
			return 0.0f;

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// 0. 模型资产预烘焙碰撞状态检测与同步
		std::string targetModelPath;
		if (comp->MeshSource.HasPath())
		{
			targetModelPath = StringPool::Get().PathOf(comp->MeshSource.Path);
		}
		else if (entity.IsValid() && entity.HasComponent<MeshRendererComponent>())
		{
			const auto& mr = entity.GetComponent<MeshRendererComponent>();
			if (mr.Mesh.HasPath())
				targetModelPath = StringPool::Get().PathOf(mr.Mesh.Path);
		}

		if (!targetModelPath.empty())
		{
			if (s_BakedCache.Path != targetModelPath)
			{
				s_BakedCache.Path = targetModelPath;
				s_BakedCache.HasBaked = false;

				Asset::WModelData mdata;
				std::string loadErr;
				if (Asset::WModelIO::ReadFile(targetModelPath, mdata, &loadErr))
				{
					if constexpr (HasCollisionMember<Asset::WModelData>::value)
					{
						if (mdata.Collision.CollisionType != 0 && !mdata.Collision.ConvexHulls.empty())
						{
							s_BakedCache.HasBaked = true;
							s_BakedCache.HullCount = static_cast<uint32_t>(mdata.Collision.ConvexHulls.size());
							s_BakedCache.Concavity = mdata.Collision.ConcavityThreshold;
							s_BakedCache.MaxVerts = mdata.Collision.MaxVerticesPerHull;
							s_BakedCache.Profile = mdata.Collision.DefaultCollisionProfile;
						}
					}
				}
			}

			if (s_BakedCache.HasBaked)
			{
				const Wui::WuiRect badgeRect { startX, curY, contentW, 24.0f };
				Wui::PanelBackground(ctx, badgeRect, Wui::WuiColor{ 0.08f, 0.28f, 0.12f, 0.90f }, 3.0f);
				Wui::HighlightOutline(ctx, badgeRect, theme.Success, 3.0f, 1.0f);
				char badgeText[128];
				std::snprintf(badgeText, sizeof(badgeText), "[Pre-baked in Asset: %u Hulls | %s]",
					s_BakedCache.HullCount, s_BakedCache.Profile.c_str());
				Wui::Label(ctx, { badgeRect.X + 8.0f, badgeRect.Y + 4.0f }, badgeText, theme.Success, theme.FontSizeSmall);
				curY += 28.0f;

				const Wui::WuiRect syncBtnRect { startX, curY, contentW, 22.0f };
				if (Wui::Button(ctx, Wui::HashId("inspect.convex.sync_asset"), syncBtnRect,
					"Sync Parameters from Asset (从模型资产同步参数)", theme))
				{
					// 用户主动点击同步时，重新读取一次磁盘最新模型资产状态
					Asset::WModelData mdata;
					std::string loadErr;
					if (Asset::WModelIO::ReadFile(targetModelPath, mdata, &loadErr))
					{
						if constexpr (HasCollisionMember<Asset::WModelData>::value)
						{
							if (mdata.Collision.CollisionType != 0 && !mdata.Collision.ConvexHulls.empty())
							{
								s_BakedCache.HasBaked = true;
								s_BakedCache.HullCount = static_cast<uint32_t>(mdata.Collision.ConvexHulls.size());
								s_BakedCache.Concavity = mdata.Collision.ConcavityThreshold;
								s_BakedCache.MaxVerts = mdata.Collision.MaxVerticesPerHull;
								s_BakedCache.Profile = mdata.Collision.DefaultCollisionProfile;
							}
						}
					}

					comp->HullCount = s_BakedCache.HullCount;
					comp->ConcavityThreshold = s_BakedCache.Concavity;
					comp->MaxVerticesPerHull = s_BakedCache.MaxVerts;
					if (changedFields)
					{
						changedFields->push_back("HullCount");
						changedFields->push_back("ConcavityThreshold");
						changedFields->push_back("MaxVerticesPerHull");
					}
					host.MarkDocumentDirty();
				}
				Wui::Tooltip(ctx, syncBtnRect, "同步模型资产预烘焙时配置的凸包数量与凹度容差参数");
				curY += 26.0f;
			}
			else
			{
				const Wui::WuiRect badgeRect { startX, curY, contentW, 24.0f };
				Wui::PanelBackground(ctx, badgeRect, Wui::WuiColor{ 0.28f, 0.20f, 0.08f, 0.90f }, 3.0f);
				Wui::HighlightOutline(ctx, badgeRect, theme.Warning, 3.0f, 1.0f);
				Wui::Label(ctx, { badgeRect.X + 8.0f, badgeRect.Y + 4.0f },
					"[Runtime Decomposed (CPU)]", theme.Warning, theme.FontSizeSmall);
				curY += 26.0f;

				Wui::Label(ctx, { startX, curY + 2.0f },
					"Tip: Double-click model in Content Browser to pre-bake in Model Editor.",
					theme.TextMuted, theme.FontSizeCaption);
				curY += 20.0f;
			}
		}

		// 1. 源模型网格选择 (MeshSource)
		Wui::Label(ctx, { startX, curY + 2.0f }, "源网格模型 (Source Mesh Asset):", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		const std::string curMesh = comp->MeshSource.HasPath()
			? std::string(StringPool::Get().PathOf(comp->MeshSource.Path)) : "";
		const float pickBtnW = 26.0f;
		const float clearBtnW = 24.0f;
		const float meshFieldW = contentW - pickBtnW - clearBtnW - 8.0f;

		const Wui::WuiRect meshTextRect { startX, curY, meshFieldW, 22.0f };
		const Wui::WuiRect pickBtnRect { startX + meshFieldW + 4.0f, curY, pickBtnW, 22.0f };
		const Wui::WuiRect clearBtnRect { startX + meshFieldW + pickBtnW + 8.0f, curY, clearBtnW, 22.0f };

		const std::string displayMesh = curMesh.empty() ? "(Use Entity Mesh / 使用实体网格)" : curMesh;
		Wui::Label(ctx, { startX + 4.0f, curY + 3.0f }, displayMesh, curMesh.empty() ? theme.TextMuted : theme.Text, theme.FontSizeSmall);
		Wui::Tooltip(ctx, meshTextRect, curMesh.empty()
			? "当前未特化网格源，自动采用该实体上 MeshRendererComponent 的网格几何进行凸包分解"
			: ("指定凸包分解几何源: " + curMesh));

		if (Wui::Button(ctx, Wui::HashId("inspect.convex.mesh_pick"), pickBtnRect, "...", theme))
		{
			host.OpenAssetPicker("Select Source Mesh for Convex Decomposition", AssetCategory::Mesh,
				[comp, &host, changedFields](const std::string& picked)
				{
					comp->MeshSource = AssetRef{ StringPool::Get().InternPath(picked), AssetId{} };
					if (changedFields) changedFields->push_back("MeshSource");
					host.MarkDocumentDirty();
				});
		}
		Wui::Tooltip(ctx, pickBtnRect, "挑选用于凸包多面体分解的网格资产 (.wmodel / .gltf)");

		if (Wui::Button(ctx, Wui::HashId("inspect.convex.mesh_clear"), clearBtnRect, "x", theme))
		{
			comp->MeshSource = AssetRef{};
			if (changedFields) changedFields->push_back("MeshSource");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, clearBtnRect, "清空特化网格，回退至实体自身网格");
		curY += 26.0f;

		// 2. 复合凸包数量 (HullCount 1 ~ 16)
		Wui::Label(ctx, { startX, curY + 2.0f }, "子凸包数量 (Hull Count 1~16):", theme.Text, theme.FontSizeSmall);
		int64_t hullCount = static_cast<int64_t>(comp->HullCount);
		if (Wui::DragInt(ctx, Wui::HashId("inspect.convex.hull_count"),
			{ startX + 160.0f, curY, contentW - 160.0f, 22.0f }, hullCount, 1, 16, theme))
		{
			comp->HullCount = static_cast<uint32_t>(std::clamp(hullCount, int64_t(1), int64_t(16)));
			if (changedFields) changedFields->push_back("HullCount");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + 160.0f, curY, contentW - 160.0f, 22.0f },
			"复合凸包数量上限。1 为标准单极值凸包 (QuickHull)；≥2 时启用凹面自动切分，构建复合刚体");
		curY += 26.0f;

		// 3. 凹陷容差阈值 (ConcavityThreshold)
		Wui::Label(ctx, { startX, curY + 2.0f }, "凹陷容差阈值 (Concavity Error):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.convex.concavity"),
			{ startX + 160.0f, curY, contentW - 160.0f, 22.0f }, comp->ConcavityThreshold, 0.005f, 0.001f, 1.0f, theme))
		{
			if (changedFields) changedFields->push_back("ConcavityThreshold");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + 160.0f, curY, contentW - 160.0f, 22.0f },
			"切分容差阈值 (默认 0.05)。值越小切分越严格精细，值越大合并更激进，减小物理开销");
		curY += 26.0f;

		// 4. 每个凸包最大顶点数 (MaxVerticesPerHull)
		Wui::Label(ctx, { startX, curY + 2.0f }, "单凸包顶点上限 (Max Vertices):", theme.Text, theme.FontSizeSmall);
		int64_t maxVerts = static_cast<int64_t>(comp->MaxVerticesPerHull);
		if (Wui::DragInt(ctx, Wui::HashId("inspect.convex.max_verts"),
			{ startX + 160.0f, curY, contentW - 160.0f, 22.0f }, maxVerts, 8, 128, theme))
		{
			comp->MaxVerticesPerHull = static_cast<uint32_t>(std::clamp(maxVerts, int64_t(8), int64_t(128)));
			if (changedFields) changedFields->push_back("MaxVerticesPerHull");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + 160.0f, curY, contentW - 160.0f, 22.0f },
			"单个子凸包允许的最大顶点数 (8 ~ 128，推荐 32)。防止物理 Jolt 碰撞矩阵顶点爆炸");
		curY += 26.0f;

		// 5. 触发器属性 (IsTrigger)
		bool isTrigger = comp->IsTrigger != 0;
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.convex.is_trigger"),
			{ startX, curY, contentW, 22.0f }, "作为触发体积 (Is Trigger Volume)", isTrigger, theme))
		{
			comp->IsTrigger = isTrigger ? 1 : 0;
			if (changedFields) changedFields->push_back("IsTrigger");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "仅触发物理接触感知事件 (OnTriggerEnter/Exit)，不产生刚体推力阻挡");
		curY += 26.0f;

		// 快捷规格预设
		const float pBtnGap = 4.0f;
		const float pBtnW = (contentW - pBtnGap * 2.0f) / 3.0f;
		if (Wui::Button(ctx, Wui::HashId("inspect.convex.preset_single"), { startX, curY, pBtnW, 22.0f }, "单凸包极值 (1 Hull)", theme))
		{
			comp->HullCount = 1;
			comp->ConcavityThreshold = 0.05f;
			comp->MaxVerticesPerHull = 32;
			if (changedFields) { changedFields->push_back("HullCount"); changedFields->push_back("ConcavityThreshold"); }
			host.MarkDocumentDirty();
		}
		if (Wui::Button(ctx, Wui::HashId("inspect.convex.preset_standard"), { startX + (pBtnW + pBtnGap), curY, pBtnW, 22.0f }, "标准复合 (4 Hulls)", theme))
		{
			comp->HullCount = 4;
			comp->ConcavityThreshold = 0.03f;
			comp->MaxVerticesPerHull = 32;
			if (changedFields) { changedFields->push_back("HullCount"); changedFields->push_back("ConcavityThreshold"); }
			host.MarkDocumentDirty();
		}
		if (Wui::Button(ctx, Wui::HashId("inspect.convex.preset_precise"), { startX + (pBtnW + pBtnGap) * 2.0f, curY, pBtnW, 22.0f }, "高精切分 (8 Hulls)", theme))
		{
			comp->HullCount = 8;
			comp->ConcavityThreshold = 0.01f;
			comp->MaxVerticesPerHull = 48;
			if (changedFields) { changedFields->push_back("HullCount"); changedFields->push_back("ConcavityThreshold"); }
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		return curY - rect.Y;
	}

	// ---- 白盒几何体专属检视器 (DrawWhiteboxInspector) ----
	static float DrawWhiteboxInspector(PanelHost& host, Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		Entity entity, void* instance, const Schema::TypeSchema&, const Wui::WuiRect&,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto* comp = static_cast<WhiteboxComponent*>(instance);
		if (!comp)
			return 0.0f;

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// 1. 形状类型 (Shape Type) 下拉框
		Wui::Label(ctx, { startX, curY + 2.0f }, "白盒几何形状 (Whitebox Shape):", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		const std::vector<std::string> shapeOptions = {
			"Box (立方体 / 盒体)",
			"Cylinder (圆柱体)",
			"Stairs (楼梯 / 台阶)",
			"Wedge (斜坡 / 楔体)",
			"Arch (拱门 / 弧形廊道)"
		};
		int currentShapeIdx = static_cast<int>(comp->Shape);
		if (currentShapeIdx < 0 || currentShapeIdx >= static_cast<int>(shapeOptions.size()))
			currentShapeIdx = 0;

		const Wui::WuiRect shapeRect { startX, curY, contentW, 22.0f };
		Wui::Tooltip(ctx, shapeRect, "选择参数化程序几何体原型形状");
		const int beforeShape = currentShapeIdx;
		if (Wui::SearchableCombo(ctx, Wui::HashId("inspect.whitebox.shape_combo"), shapeRect, "",
			shapeOptions, currentShapeIdx, theme, "", false) && currentShapeIdx != beforeShape)
		{
			comp->Shape = static_cast<uint8_t>(currentShapeIdx);
			if (changedFields) changedFields->push_back("Shape");
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		// 2. 尺寸 Dimensions (X, Y, Z)
		Wui::Label(ctx, { startX, curY + 2.0f }, "尺寸 Dimensions (X 宽, Y 高, Z 长 m):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;

		const float coordLabelW = 14.0f;
		const float coordFieldW = (contentW - (coordLabelW + 6.0f) * 3.0f) / 3.0f;

		// X
		Wui::Label(ctx, { startX, curY + 3.0f }, "X", theme.TextMuted, theme.FontSizeSmall);
		float dimX = comp->Dimensions.x;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.whitebox.dim_x"),
			{ startX + coordLabelW + 2.0f, curY, coordFieldW, 22.0f }, dimX, 0.1f, 0.1f, 500.0f, theme))
		{
			comp->Dimensions.x = std::max(0.1f, dimX);
			if (changedFields) changedFields->push_back("Dimensions");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + coordLabelW + 2.0f, curY, coordFieldW, 22.0f }, "宽度 X (米)");

		// Y
		const float col1X = startX + coordLabelW + coordFieldW + 6.0f;
		Wui::Label(ctx, { col1X, curY + 3.0f }, "Y", theme.TextMuted, theme.FontSizeSmall);
		float dimY = comp->Dimensions.y;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.whitebox.dim_y"),
			{ col1X + coordLabelW + 2.0f, curY, coordFieldW, 22.0f }, dimY, 0.1f, 0.1f, 500.0f, theme))
		{
			comp->Dimensions.y = std::max(0.1f, dimY);
			if (changedFields) changedFields->push_back("Dimensions");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { col1X + coordLabelW + 2.0f, curY, coordFieldW, 22.0f }, "高度 Y (米)");

		// Z
		const float col2X = col1X + coordLabelW + coordFieldW + 6.0f;
		Wui::Label(ctx, { col2X, curY + 3.0f }, "Z", theme.TextMuted, theme.FontSizeSmall);
		float dimZ = comp->Dimensions.z;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.whitebox.dim_z"),
			{ col2X + coordLabelW + 2.0f, curY, coordFieldW, 22.0f }, dimZ, 0.1f, 0.1f, 500.0f, theme))
		{
			comp->Dimensions.z = std::max(0.1f, dimZ);
			if (changedFields) changedFields->push_back("Dimensions");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { col2X + coordLabelW + 2.0f, curY, coordFieldW, 22.0f }, "长度/深度 Z (米)");
		curY += 28.0f;

		// 3. 智能字段展示 (根据形状展现不同参数)
		if (comp->GetShapeType() == WhiteboxComponent::ShapeType::Stairs)
		{
			Wui::Label(ctx, { startX, curY + 2.0f }, "台阶阶数 (Steps):", theme.Text, theme.FontSizeSmall);
			curY += 20.0f;
			int64_t stepsVal = comp->Steps;
			if (Wui::DragInt(ctx, Wui::HashId("inspect.whitebox.steps"),
				{ startX, curY, contentW, 22.0f }, stepsVal, 1, 64, theme))
			{
				comp->Steps = static_cast<uint8_t>(std::clamp(stepsVal, int64_t(1), int64_t(64)));
				if (changedFields) changedFields->push_back("Steps");
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "楼梯台阶划分数量 (1 ~ 64 阶)");
			curY += 28.0f;
		}
		else if (comp->GetShapeType() == WhiteboxComponent::ShapeType::Cylinder || comp->GetShapeType() == WhiteboxComponent::ShapeType::Arch)
		{
			Wui::Label(ctx, { startX, curY + 2.0f }, "圆周分段数 (Subdivisions):", theme.Text, theme.FontSizeSmall);
			curY += 20.0f;
			int64_t subVal = comp->Subdivisions;
			if (Wui::DragInt(ctx, Wui::HashId("inspect.whitebox.subdivs"),
				{ startX, curY, contentW, 22.0f }, subVal, 3, 64, theme))
			{
				comp->Subdivisions = static_cast<uint8_t>(std::clamp(subVal, int64_t(3), int64_t(64)));
				if (changedFields) changedFields->push_back("Subdivisions");
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "圆柱/弧面多边形细分段数 (3 ~ 64)");
			curY += 28.0f;
		}
		else if (comp->GetShapeType() == WhiteboxComponent::ShapeType::Box)
		{
			Wui::Label(ctx, { startX, curY + 2.0f }, "边缘倒角大小 (Bevel Size):", theme.Text, theme.FontSizeSmall);
			curY += 20.0f;
			float bevel = comp->BevelSize;
			if (Wui::DragFloat(ctx, Wui::HashId("inspect.whitebox.bevel"),
				{ startX, curY, contentW, 22.0f }, bevel, 0.01f, 0.0f, 0.5f, theme))
			{
				comp->BevelSize = std::clamp(bevel, 0.0f, 0.5f);
				if (changedFields) changedFields->push_back("BevelSize");
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "盒体边缘斜切倒角半径 (0.0 ~ 0.5 米)");
			curY += 28.0f;
		}

		// 4. 碰撞体生成开关 GenerateCollision
		bool genCol = comp->GenerateCollision != 0;
		const Wui::WuiRect colRect { startX, curY, contentW, 22.0f };
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.whitebox.gencol"), colRect, "自动生成物理碰撞体 (Generate Collision)", genCol, theme))
		{
			comp->GenerateCollision = genCol ? 1 : 0;
			if (changedFields) changedFields->push_back("GenerateCollision");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, colRect, "是否为该白盒实体同步生成静态物理碰撞体");
		curY += 28.0f;

		// 5. 烘焙按钮 Bake to Mesh
		const Wui::WuiRect bakeBtnRect { startX, curY, contentW, 26.0f };
		if (Wui::Button(ctx, Wui::HashId("inspect.whitebox.bake_btn"), bakeBtnRect, "Bake to Static Mesh (烘焙为静态网格)", theme))
		{
			WhiteboxMesh mesh;
			switch (comp->GetShapeType())
			{
			case WhiteboxComponent::ShapeType::Box:
				WhiteboxBuilder::BuildBox(comp->Dimensions, comp->BevelSize, mesh);
				break;
			case WhiteboxComponent::ShapeType::Cylinder:
				WhiteboxBuilder::BuildCylinder(comp->Dimensions.x * 0.5f, comp->Dimensions.y, comp->Subdivisions, mesh);
				break;
			case WhiteboxComponent::ShapeType::Stairs:
				WhiteboxBuilder::BuildStairs(comp->Dimensions, comp->Steps, mesh);
				break;
			case WhiteboxComponent::ShapeType::Wedge:
				WhiteboxBuilder::BuildWedge(comp->Dimensions, mesh);
				break;
			case WhiteboxComponent::ShapeType::Arch:
				WhiteboxBuilder::BuildArch(comp->Dimensions, comp->Dimensions.x * 0.25f, mesh);
				break;
			}

			if (!mesh.Vertices.empty())
			{
				std::filesystem::create_directories("assets/models");
				std::string modelRelPath = "assets/models/whitebox_" + std::to_string(static_cast<uint32_t>(entity)) + ".wmodel";

				Asset::WModelData mdata;
				mdata.Meta.Valid = true;
				mdata.VertexLayoutId = Asset::WModelIO::kVertexLayoutStandard;
				mdata.Vertices.resize(mesh.Vertices.size());
				for (size_t i = 0; i < mesh.Vertices.size(); ++i)
				{
					mdata.Vertices[i].Position = mesh.Vertices[i];
					mdata.Vertices[i].Normal = i < mesh.Normals.size() ? mesh.Normals[i] : glm::vec3(0, 1, 0);
					mdata.Vertices[i].TexCoord = i < mesh.UVs.size() ? mesh.UVs[i] : glm::vec2(0, 0);
				}
				mdata.Indices = mesh.Indices;

				Asset::WModelSubmesh submesh;
				submesh.IndexOffset = 0;
				submesh.IndexCount = static_cast<uint32_t>(mesh.Indices.size());
				submesh.MaterialSlot = -1;
				mdata.Submeshes.push_back(submesh);

				Asset::WModelMeshRange mrange;
				mrange.FirstSubmesh = 0;
				mrange.SubmeshCount = 1;
				mdata.Meshes.push_back(mrange);

				Asset::WModelNode rootNode;
				rootNode.Parent = -1;
				rootNode.MeshIndex = 0;
				mdata.Nodes.push_back(rootNode);

				std::string writeErr;
				if (Asset::WModelIO::WriteFile(modelRelPath, mdata, &writeErr))
				{
					if (!entity.HasComponent<MeshRendererComponent>())
						entity.AddComponent<MeshRendererComponent>();
					auto& mr = entity.GetComponent<MeshRendererComponent>();
					mr.Mesh = AssetRef{ StringPool::Get().InternPath(modelRelPath), AssetId{} };
					host.Notify("成功烘焙白盒网格: " + modelRelPath);
					host.MarkDocumentDirty();
				}
				else
				{
					host.Notify("烘焙失败: " + writeErr);
				}
			}
		}
		Wui::Tooltip(ctx, bakeBtnRect, "将当前参数化白盒几何体固定并导出为独立的 .wmodel 静态模型资产");
		curY += 32.0f;

		return curY - rect.Y;
	}

	// ---- 3D 样条线道路专属检视器 (DrawSplineInspector) ----
	static float DrawSplineInspector(PanelHost& host, Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		Entity entity, void* instance, const Schema::TypeSchema&, const Wui::WuiRect&,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto* comp = static_cast<SplineComponent*>(instance);
		if (!comp)
			return 0.0f;

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "3D 样条线道路参数 (Spline Road Settings):", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		// 1. 路面宽度 RoadWidth
		Wui::Label(ctx, { startX, curY + 2.0f }, "路面宽度 (Road Width):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;
		float roadW = comp->RoadWidth;
		const Wui::WuiRect widthRect { startX, curY, contentW, 22.0f };
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.spline.road_width"), widthRect, roadW, 0.1f, 0.5f, 50.0f, theme))
		{
			comp->RoadWidth = std::clamp(roadW, 0.5f, 50.0f);
			if (changedFields) changedFields->push_back("RoadWidth");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, widthRect, "样条挤出路面截面双侧总宽度 (米)");
		curY += 28.0f;

		// 2. 采样步长 MeshSpacing
		Wui::Label(ctx, { startX, curY + 2.0f }, "采样步长 (Mesh Spacing):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;
		float spacing = comp->MeshSpacing;
		const Wui::WuiRect spaceRect { startX, curY, contentW, 22.0f };
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.spline.mesh_spacing"), spaceRect, spacing, 0.1f, 0.2f, 20.0f, theme))
		{
			comp->MeshSpacing = std::clamp(spacing, 0.2f, 20.0f);
			if (changedFields) changedFields->push_back("MeshSpacing");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, spaceRect, "沿样条线弧长离散放样的多边形分段步长 (米)");
		curY += 28.0f;

		// 3. 闭合环路 ClosedLoop & 自动挤出 ExtrudeRoad & 碰撞体 GenerateCollision
		bool closed = comp->ClosedLoop != 0;
		const Wui::WuiRect closedRect { startX, curY, contentW, 22.0f };
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.spline.closed"), closedRect, "闭合首尾环路 (Closed Loop)", closed, theme))
		{
			comp->ClosedLoop = closed ? 1 : 0;
			if (changedFields) changedFields->push_back("ClosedLoop");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, closedRect, "是否将样条曲线首尾控制点闭合相连");
		curY += 26.0f;

		bool extrude = comp->ExtrudeRoad != 0;
		const Wui::WuiRect extrudeRect { startX, curY, contentW, 22.0f };
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.spline.extrude"), extrudeRect, "自动挤出道路带状网格 (Extrude Road)", extrude, theme))
		{
			comp->ExtrudeRoad = extrude ? 1 : 0;
			if (changedFields) changedFields->push_back("ExtrudeRoad");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, extrudeRect, "是否自动沿曲线生成连续的双侧放样路面几何体");
		curY += 26.0f;

		bool genCol = comp->GenerateCollision != 0;
		const Wui::WuiRect colRect { startX, curY, contentW, 22.0f };
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.spline.gencol"), colRect, "生成路面物理碰撞体 (Generate Collision)", genCol, theme))
		{
			comp->GenerateCollision = genCol ? 1 : 0;
			if (changedFields) changedFields->push_back("GenerateCollision");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, colRect, "是否为挤出路面生成物理碰撞网格");
		curY += 28.0f;

		// 4. 控制点拓扑列表 (Knots List)
		Wui::Label(ctx, { startX, curY + 2.0f }, "控制点拓扑列表 (Control Knots):", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		Ref<Scene> scene = host.GetActiveScene();
		std::vector<Entity> knots;
		if (scene && entity.IsValid())
		{
			const auto& children = Hierarchy::ChildrenOf(scene->GetRegistry(), static_cast<entt::entity>(entity));
			for (const entt::entity h : children)
			{
				knots.push_back(Entity(scene.get(), h));
			}
		}

		const float knotsBoxH = std::max(60.0f, static_cast<float>(knots.size() * 26.0f + 36.0f));
		const Wui::WuiRect knotsBoxRect { startX, curY, contentW, knotsBoxH };
		Wui::PanelBackground(ctx, knotsBoxRect, theme.ContentBg, 4.0f);
		Wui::HighlightOutline(ctx, knotsBoxRect, theme.Border, 4.0f, 1.0f);

		float innerY = knotsBoxRect.Y + 6.0f;
		for (size_t i = 0; i < knots.size(); ++i)
		{
			Entity knot = knots[i];
			std::string knotName = "Knot_" + std::to_string(i);
			if (knot.HasComponent<TagComponent>())
				knotName = StringPool::Get().NameOf(knot.GetComponent<TagComponent>().Tag);

			glm::vec3 pos = knot.GetComponent<TransformComponent>().Location;
			char posBuf[64];
			std::snprintf(posBuf, sizeof(posBuf), "(%.1f, %.1f, %.1f)", pos.x, pos.y, pos.z);

			const Wui::WuiRect rowRect { knotsBoxRect.X + 4.0f, innerY, knotsBoxRect.W - 8.0f, 22.0f };
			Wui::HoverRow(ctx, rowRect, ctx.IsHovered(rowRect), false, theme);

			Wui::Label(ctx, { rowRect.X + 6.0f, rowRect.Y + 3.0f }, knotName, theme.Text, theme.FontSizeSmall);
			Wui::Label(ctx, { rowRect.X + 75.0f, rowRect.Y + 3.0f }, posBuf, theme.TextMuted, theme.FontSizeSmall);

			// 选中按钮
			const Wui::WuiRect selBtn { rowRect.X + rowRect.W - 54.0f, rowRect.Y + 1.0f, 24.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId(("spline.knot.sel." + std::to_string(i)).c_str()), selBtn, "Q", theme))
			{
				host.SetSelectedEntity(knot);
			}
			Wui::Tooltip(ctx, selBtn, "在视口中选中并定位该控制点");

			// 删除按钮
			const Wui::WuiRect delBtn { rowRect.X + rowRect.W - 26.0f, rowRect.Y + 1.0f, 24.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId(("spline.knot.del." + std::to_string(i)).c_str()), delBtn, "x", theme))
			{
				scene->DeferStructuralChange([knot](Scene& s) {
					Entity k = knot;
					s.DestroyEntity(k);
				});
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, delBtn, "删除此控制点");

			innerY += 24.0f;
		}

		// 「+ 添加控制点」按钮
		const Wui::WuiRect addKnotBtn { knotsBoxRect.X + 4.0f, innerY + 2.0f, knotsBoxRect.W - 8.0f, 22.0f };
		if (Wui::Button(ctx, Wui::HashId("inspect.spline.add_knot"), addKnotBtn, "+ 添加控制点 (Add Knot)", theme))
		{
			glm::vec3 newPos { 0.0f };
			if (!knots.empty())
			{
				glm::vec3 lastPos = knots.back().GetComponent<TransformComponent>().Location;
				newPos = lastPos + glm::vec3(10.0f, 0.0f, 0.0f);
			}
			else if (entity.HasComponent<TransformComponent>())
			{
				newPos = entity.GetComponent<TransformComponent>().Location;
			}

			scene->DeferStructuralChange([entity, newPos, knotCount = knots.size()](Scene& s) {
				Entity newKnot = Entity::CreateEntity(&s, "Knot_" + std::to_string(knotCount));
				auto& xf = newKnot.AddComponent<TransformComponent>();
				xf.SetLocation(newPos);
				Hierarchy::SetParent(s.GetRegistry(), static_cast<entt::entity>(newKnot), static_cast<entt::entity>(entity));
			});
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, addKnotBtn, "在样条末尾延伸添加一个新的控制节点");

		curY += knotsBoxH + 8.0f;

		// 5. 一键生成与烘焙道路网格按钮
		const Wui::WuiRect bakeRoadBtn { startX, curY, contentW, 26.0f };
		if (Wui::Button(ctx, Wui::HashId("inspect.spline.bake_road"), bakeRoadBtn, "Bake Road to Static Mesh (烘焙道路网格)", theme))
		{
			if (knots.size() >= 2)
			{
				Spline3D spline;
				for (Entity k : knots)
				{
					spline.AddPoint(k.GetComponent<TransformComponent>().Location);
				}

				std::vector<glm::vec3> verts;
				std::vector<glm::vec3> norms;
				std::vector<glm::vec2> uvs;
				std::vector<uint32_t> indices;
				spline.GenerateRoadMesh(comp->RoadWidth, comp->MeshSpacing, verts, norms, uvs, indices);

				if (!verts.empty())
				{
					std::filesystem::create_directories("assets/models");
					std::string modelRelPath = "assets/models/road_" + std::to_string(static_cast<uint32_t>(entity)) + ".wmodel";

					Asset::WModelData mdata;
					mdata.Meta.Valid = true;
					mdata.VertexLayoutId = Asset::WModelIO::kVertexLayoutStandard;
					mdata.Vertices.resize(verts.size());
					for (size_t i = 0; i < verts.size(); ++i)
					{
						mdata.Vertices[i].Position = verts[i];
						mdata.Vertices[i].Normal = i < norms.size() ? norms[i] : glm::vec3(0, 1, 0);
						mdata.Vertices[i].TexCoord = i < uvs.size() ? uvs[i] : glm::vec2(0, 0);
					}
					mdata.Indices = indices;

					Asset::WModelSubmesh submesh;
					submesh.IndexOffset = 0;
					submesh.IndexCount = static_cast<uint32_t>(indices.size());
					submesh.MaterialSlot = -1;
					mdata.Submeshes.push_back(submesh);

					Asset::WModelMeshRange mrange;
					mrange.FirstSubmesh = 0;
					mrange.SubmeshCount = 1;
					mdata.Meshes.push_back(mrange);

					Asset::WModelNode rootNode;
					rootNode.Parent = -1;
					rootNode.MeshIndex = 0;
					mdata.Nodes.push_back(rootNode);

					std::string writeErr;
					if (Asset::WModelIO::WriteFile(modelRelPath, mdata, &writeErr))
					{
						if (!entity.HasComponent<MeshRendererComponent>())
							entity.AddComponent<MeshRendererComponent>();
						auto& mr = entity.GetComponent<MeshRendererComponent>();
						mr.Mesh = AssetRef{ StringPool::Get().InternPath(modelRelPath), AssetId{} };
						host.Notify("成功烘焙样条道路网格: " + modelRelPath);
						host.MarkDocumentDirty();
					}
					else
					{
						host.Notify("烘焙失败: " + writeErr);
					}
				}
			}
			else
			{
				host.Notify("样条道路需要至少 2 个控制点方可生成路面！");
			}
		}
		Wui::Tooltip(ctx, bakeRoadBtn, "沿样条曲线放样生成高精度带状网格并写入 .wmodel 资产");
		curY += 32.0f;

		return curY - rect.Y;
	}






		// ---- 组件检视器 ----


	// ---- 物理海洋水体专属检视器 (DrawWaterBodyInspector) ----
	static float DrawWaterBodyInspector(PanelHost& host, Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		Entity entity, void* instance, const Schema::TypeSchema&, const Wui::WuiRect&,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto* comp = static_cast<WaterBodyComponent*>(instance);
		if (!comp)
			return 0.0f;

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// 1. 头部徽章与概要
		const Wui::WuiRect badgeRect { startX, curY, 144.0f, 18.0f };
		const Wui::WuiColor badgeBg { 0.05f, 0.40f, 0.65f, 0.28f };
		const Wui::WuiColor badgeBorder { 0.15f, 0.70f, 0.95f, 0.90f };
		Wui::Badge(ctx, badgeRect, "PHYSICAL WATER BODY", badgeBg, badgeBorder, theme, 10.5f, true, 3.0f);

		char subTitle[64];
		std::snprintf(subTitle, sizeof(subTitle), "阶数: %d 谐波 | 细分: %dx%d",
			static_cast<int>(comp->WaveOctaves),
			static_cast<int>(comp->GridResolution),
			static_cast<int>(comp->GridResolution));
		Wui::Label(ctx, { startX + 152.0f, curY + 2.0f }, subTitle, theme.TextMuted, theme.FontSizeSmall);
		curY += 26.0f;

		const float fieldLabelW = 150.0f;
		const float fieldControlW = contentW - fieldLabelW;

		// 2. 几何网格与细分域 (Geometry Domain & Mesh Tessellation)
		Wui::Label(ctx, { startX, curY + 2.0f }, "几何网格与细分域 (Domain & Mesh):", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		// Grid Resolution
		Wui::Label(ctx, { startX, curY + 3.0f }, "网格细分密度 (Tessellation):", theme.Text, theme.FontSizeSmall);
		int64_t gridRes = comp->GridResolution;
		if (Wui::DragInt(ctx, Wui::HashId("inspect.water.grid_res"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, gridRes, 8, 64, theme))
		{
			comp->GridResolution = static_cast<uint8_t>(std::clamp<int64_t>(gridRes, 8, 64));
			if (changedFields) changedFields->push_back("GridResolution");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "水面网格单轴细分数量 [8, 64]，数值越大水面微浪越细腻真实");
		curY += 26.0f;

		// 3. Gerstner 多阶物理波动动力学 (Multi-Octave Wave Dynamics)
		Wui::Label(ctx, { startX, curY + 2.0f }, "Gerstner 物理波动动力学 (Wave Dynamics):", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		// Wave Direction Angle
		Wui::Label(ctx, { startX, curY + 3.0f }, "主传播向角 (Heading deg):", theme.Text, theme.FontSizeSmall);
		float waveHeading = comp->WaveDirectionAngle;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.water.heading"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, waveHeading, 1.0f, 0.0f, 360.0f, theme))
		{
			comp->WaveDirectionAngle = std::fmod(waveHeading + 360.0f, 360.0f);
			if (changedFields) changedFields->push_back("WaveDirectionAngle");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "海浪主传播航向角 [0, 360] 度");
		curY += 26.0f;

		// Wave Coupling to Atmospheric Wind
		bool coupleWind = comp->WaveCouplingWind != 0;
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.water.chk_couple_wind"),
			{ startX, curY, contentW, 20.0f }, "风浪动态耦合 (Couple Wave Heading to Atmospheric Wind)", coupleWind, theme))
		{
			comp->WaveCouplingWind = coupleWind ? 1 : 0;
			if (changedFields) changedFields->push_back("WaveCouplingWind");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 20.0f }, "开启后海浪传播方向与波峰陡度将由场景天气系统的风场矢量实时动态驱动");
		curY += 24.0f;

		// Wave Steepness
		Wui::Label(ctx, { startX, curY + 3.0f }, "波峰陡峭度 (Steepness Q):", theme.Text, theme.FontSizeSmall);
		float steepness = comp->WaveSteepness;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.water.steepness"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, steepness, 0.01f, 0.01f, 1.0f, theme))
		{
			comp->WaveSteepness = std::clamp(steepness, 0.01f, 1.0f);
			if (changedFields) changedFields->push_back("WaveSteepness");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "Gerstner 波浪波峰尖锐聚拢系数 [0.01, 1.0]");
		curY += 26.0f;

		// Wave Length
		Wui::Label(ctx, { startX, curY + 3.0f }, "主波长 (Wavelength m):", theme.Text, theme.FontSizeSmall);
		float waveLen = comp->WaveLength;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.water.wavelength"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, waveLen, 0.5f, 1.0f, 300.0f, theme))
		{
			comp->WaveLength = std::clamp(waveLen, 1.0f, 500.0f);
			if (changedFields) changedFields->push_back("WaveLength");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "基础波浪波峰间距 [1.0, 300.0] 米");
		curY += 26.0f;

		// Wave Speed
		Wui::Label(ctx, { startX, curY + 3.0f }, "波速 (Wave Speed m/s):", theme.Text, theme.FontSizeSmall);
		float speed = comp->WaveSpeed;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.water.wavespeed"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, speed, 0.1f, 0.1f, 30.0f, theme))
		{
			comp->WaveSpeed = std::clamp(speed, 0.1f, 50.0f);
			if (changedFields) changedFields->push_back("WaveSpeed");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "主波前行速度 [0.1, 30.0] 米/秒");
		curY += 26.0f;

		// Wave Octaves
		Wui::Label(ctx, { startX, curY + 3.0f }, "谐波阶数 (Wave Octaves):", theme.Text, theme.FontSizeSmall);
		int64_t octaves = comp->WaveOctaves;
		if (Wui::DragInt(ctx, Wui::HashId("inspect.water.octaves"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, octaves, 1, 8, theme))
		{
			comp->WaveOctaves = static_cast<uint8_t>(std::clamp<int64_t>(octaves, 1, 8));
			if (changedFields) changedFields->push_back("WaveOctaves");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "叠加的 Gerstner 谐波层数 [1, 8]，层数越多海浪微表面越自然丰富");
		curY += 30.0f;

		// 4. 光学吸收与水面着色 (Optics & PBR Shading)
		Wui::Label(ctx, { startX, curY + 2.0f }, "光学吸收与水体着色 (Optics & Material):", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		// Water Color
		Wui::Label(ctx, { startX, curY + 3.0f }, "深层吸收水色 (Absorption Tint):", theme.Text, theme.FontSizeSmall);
		const Wui::WuiRect clrRect { startX + fieldLabelW, curY, fieldControlW, 22.0f };
		glm::vec4 curCol(comp->WaterColor.r, comp->WaterColor.g, comp->WaterColor.b, 1.0f);
		if (Wui::ColorField(ctx, Wui::HashId("inspect.water.color"), clrRect, curCol, theme))
		{
			comp->WaterColor = glm::vec3(curCol.r, curCol.g, curCol.b);
			if (changedFields) changedFields->push_back("WaterColor");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, clrRect, "水体光学深层 Beer-Lambert 吸收底色");
		curY += 26.0f;

		// Water Clarity
		Wui::Label(ctx, { startX, curY + 3.0f }, "光学透光深度 (Clarity Depth m):", theme.Text, theme.FontSizeSmall);
		float clarity = comp->WaterClarity;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.water.clarity"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, clarity, 0.2f, 0.1f, 100.0f, theme))
		{
			comp->WaterClarity = std::max(0.1f, clarity);
			if (changedFields) changedFields->push_back("WaterClarity");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "水体透光消亡深度距离 [0.1, 100.0] 米");
		curY += 26.0f;

		// Enable Foam & Caustics
		bool foam = comp->EnableFoam != 0;
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.water.chk_foam"),
			{ startX, curY, contentW, 20.0f }, "启用波尖碎浪与海岸泡沫 (Wave Crest Foam)", foam, theme))
		{
			comp->EnableFoam = foam ? 1 : 0;
			if (changedFields) changedFields->push_back("EnableFoam");
			host.MarkDocumentDirty();
		}
		curY += 22.0f;

		bool caustics = comp->EnableCaustics != 0;
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.water.chk_caustics"),
			{ startX, curY, contentW, 20.0f }, "启用浅水动态焦散投射 (Caustics Projection)", caustics, theme))
		{
			comp->EnableCaustics = caustics ? 1 : 0;
			if (changedFields) changedFields->push_back("EnableCaustics");
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		// 5. 流体物理与浮力系统 (Hydrodynamics & Buoyancy)
		Wui::Label(ctx, { startX, curY + 2.0f }, "流体物理与浮力模拟 (Hydrodynamics & Buoyancy):", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		bool buoyancy = comp->EnableBuoyancy != 0;
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.water.chk_buoyancy"),
			{ startX, curY, contentW, 20.0f }, "启用阿基米德物理浮力解算 (Enable Physics Buoyancy)", buoyancy, theme))
		{
			comp->EnableBuoyancy = buoyancy ? 1 : 0;
			if (changedFields) changedFields->push_back("EnableBuoyancy");
			host.MarkDocumentDirty();
		}
		curY += 24.0f;

		// Fluid Density
		Wui::Label(ctx, { startX, curY + 3.0f }, "流体密度 (Density kg/m³):", theme.Text, theme.FontSizeSmall);
		float density = comp->FluidDensity;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.water.density"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, density, 5.0f, 500.0f, 2000.0f, theme))
		{
			comp->FluidDensity = std::clamp(density, 500.0f, 2500.0f);
			if (changedFields) changedFields->push_back("FluidDensity");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "流体介质质量密度 (淡水 1000, 海水 1025, 重油 900)");
		curY += 26.0f;

		// Fluid Damping
		Wui::Label(ctx, { startX, curY + 3.0f }, "流体阻尼系数 (Viscous Drag):", theme.Text, theme.FontSizeSmall);
		float damping = comp->FluidDamping;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.water.damping"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, damping, 0.02f, 0.05f, 1.0f, theme))
		{
			comp->FluidDamping = std::clamp(damping, 0.05f, 1.0f);
			if (changedFields) changedFields->push_back("FluidDamping");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "物体在流体中运动所受粘滞阻尼系数 [0.05, 1.0]");
		curY += 30.0f;

		// 6. 实时水动力学遥测卡片 (Telemetry Card)
		const float cardH = 58.0f;
		const Wui::WuiRect cardRect { startX, curY, contentW, cardH };
		Wui::PanelBackground(ctx, cardRect, theme.CardBg, 4.0f);
		Wui::HighlightOutline(ctx, cardRect, theme.Accent, 4.0f, 1.0f);

		const float theoreticalAmp = comp->WaveLength * 0.04f * comp->WaveSteepness;
		const float wavePeriod = comp->WaveSpeed > 0.01f ? (comp->WaveLength / comp->WaveSpeed) : 0.0f;

		char teleLine1[96];
		std::snprintf(teleLine1, sizeof(teleLine1), "理论波幅: %.2fm  |  主波周期: %.2fs", theoreticalAmp, wavePeriod);

		char teleLine2[96];
		std::snprintf(teleLine2, sizeof(teleLine2), "介质密度: %.0f kg/m³ | 浮力状态: %s",
			comp->FluidDensity,
			comp->EnableBuoyancy ? "Active (实时物理力矩)" : "Disabled (未启用)");

		Wui::Label(ctx, { cardRect.X + 8.0f, cardRect.Y + 6.0f }, teleLine1, theme.Accent, 11.5f);
		Wui::Label(ctx, { cardRect.X + 8.0f, cardRect.Y + 28.0f }, teleLine2,
			comp->EnableBuoyancy ? theme.Info : theme.TextMuted, 11.5f);

		curY += cardH + 12.0f;
		return curY - rect.Y;
	}

	// ---- 全动态天气控制中枢专属检视器 (DrawWeatherSystemInspector) ----
	static float DrawWeatherSystemInspector(PanelHost& host, Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		Entity entity, void* instance, const Schema::TypeSchema&, const Wui::WuiRect&,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto* comp = static_cast<WeatherSystemComponent*>(instance);
		if (!comp)
			return 0.0f;

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// 1. 头部徽章
		const Wui::WuiRect badgeRect { startX, curY, 170.0f, 18.0f };
		const Wui::WuiColor badgeBg { 0.55f, 0.40f, 0.10f, 0.28f };
		const Wui::WuiColor badgeBorder { 0.95f, 0.75f, 0.20f, 0.90f };
		Wui::Badge(ctx, badgeRect, "ATMOSPHERIC WEATHER", badgeBg, badgeBorder, theme, 10.5f, true, 3.0f);

		// 时辰显示
		int hour = static_cast<int>(comp->TimeOfDay);
		int minute = static_cast<int>((comp->TimeOfDay - static_cast<float>(hour)) * 60.0f);
		const char* periodStr = (hour >= 5 && hour < 8) ? "Sunrise (金色破晓)" :
			((hour >= 8 && hour < 17) ? "Daytime (正午骄阳)" :
			((hour >= 17 && hour < 20) ? "Sunset (瑰红落日)" : "Night (静谧银月)"));

		char todClock[64];
		std::snprintf(todClock, sizeof(todClock), "%02d:%02d  %s", hour, minute, periodStr);
		Wui::Label(ctx, { startX + 178.0f, curY + 2.0f }, todClock, theme.Accent, theme.FontSizeSmall);
		curY += 26.0f;

		const float fieldLabelW = 150.0f;
		const float fieldControlW = contentW - fieldLabelW;

		// 2. 天球太阳与昼夜时辰 (Celestial Sun & 24h Solar Cycle)
		Wui::Label(ctx, { startX, curY + 2.0f }, "天球太阳与昼夜时辰 (Celestial Sun & Cycle):", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		// Time of Day Slider
		Wui::Label(ctx, { startX, curY + 3.0f }, "24h 连续时辰 (Time of Day):", theme.Text, theme.FontSizeSmall);
		float tod = comp->TimeOfDay;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weather.time_of_day"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, tod, 0.05f, 0.0f, 24.0f, theme))
		{
			comp->TimeOfDay = std::clamp(tod, 0.0f, 24.0f);
			if (changedFields) changedFields->push_back("TimeOfDay");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "连续 24 小时太阳时辰 [0.0, 24.0]，太阳仰角与黑体色温连续精确解算");
		curY += 26.0f;

		// Auto Day/Night Cycle
		bool autoCycle = comp->AutoCycleTime != 0;
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.weather.chk_auto_cycle"),
			{ startX, curY, 180.0f, 20.0f }, "自动推进昼夜循环 (Auto Cycle)", autoCycle, theme))
		{
			comp->AutoCycleTime = autoCycle ? 1 : 0;
			if (changedFields) changedFields->push_back("AutoCycleTime");
			host.MarkDocumentDirty();
		}

		// Cycle Speed
		float cycleSpeed = comp->DayCycleSpeed;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weather.cycle_speed"),
			{ startX + 190.0f, curY - 1.0f, contentW - 190.0f, 22.0f }, cycleSpeed, 0.01f, 0.001f, 5.0f, theme))
		{
			comp->DayCycleSpeed = std::clamp(cycleSpeed, 0.001f, 10.0f);
			if (changedFields) changedFields->push_back("DayCycleSpeed");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + 190.0f, curY - 1.0f, contentW - 190.0f, 22.0f }, "昼夜推进速度 (小时/真实秒)");
		curY += 28.0f;

		// 3. 大气云层与连续降水 (Clouds & Continuous Precipitation)
		Wui::Label(ctx, { startX, curY + 2.0f }, "大气云层与连续降水 (Atmosphere & Precipitation):", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		// Cloud Coverage
		Wui::Label(ctx, { startX, curY + 3.0f }, "云层天空覆盖率 (Cloud Cover):", theme.Text, theme.FontSizeSmall);
		float cloudCover = comp->CloudCoverage;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weather.cloud_cov"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, cloudCover, 0.01f, 0.0f, 1.0f, theme))
		{
			comp->CloudCoverage = std::clamp(cloudCover, 0.0f, 1.0f);
			if (changedFields) changedFields->push_back("CloudCoverage");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "云层天空覆盖比例 [0.0, 1.0]，直接连续衰减直射光并增强散射环境光");
		curY += 26.0f;

		// Cloud Density
		Wui::Label(ctx, { startX, curY + 3.0f }, "云层光学密度 (Cloud Density):", theme.Text, theme.FontSizeSmall);
		float cloudDensity = comp->CloudDensity;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weather.cloud_dense"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, cloudDensity, 0.05f, 0.1f, 10.0f, theme))
		{
			comp->CloudDensity = std::clamp(cloudDensity, 0.1f, 10.0f);
			if (changedFields) changedFields->push_back("CloudDensity");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// Precipitation Intensity
		Wui::Label(ctx, { startX, curY + 3.0f }, "连续降水强度 (Precipitation):", theme.Text, theme.FontSizeSmall);
		float precip = comp->PrecipitationIntensity;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weather.precip"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, precip, 0.01f, 0.0f, 1.0f, theme))
		{
			comp->PrecipitationIntensity = std::clamp(precip, 0.0f, 1.0f);
			if (changedFields) changedFields->push_back("PrecipitationIntensity");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "雨雪降水连续强度 [0.0, 1.0]，实时驱动地面积水积蓄与环境湿度微积分");
		curY += 26.0f;

		// Ground Wetness
		Wui::Label(ctx, { startX, curY + 3.0f }, "地面积水潮湿度 (Ground Wetness):", theme.Text, theme.FontSizeSmall);
		float wetness = comp->Wetness;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weather.wetness"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, wetness, 0.01f, 0.0f, 1.0f, theme))
		{
			comp->Wetness = std::clamp(wetness, 0.0f, 1.0f);
			if (changedFields) changedFields->push_back("Wetness");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "地表积水湿度，降雨时自动积累，无雨时随风速与阳光自然风干蒸发");
		curY += 28.0f;

		// 4. 大气指数雾效 (Atmospheric Exponential Fog)
		Wui::Label(ctx, { startX, curY + 2.0f }, "大气指数雾效 (Atmospheric Fog):", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		Wui::Label(ctx, { startX, curY + 3.0f }, "指数雾浓度 (Fog Density):", theme.Text, theme.FontSizeSmall);
		float fogDensity = comp->FogDensity;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weather.fog_density"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, fogDensity, 0.0002f, 0.0001f, 0.05f, theme))
		{
			comp->FogDensity = std::clamp(fogDensity, 0.0001f, 0.05f);
			if (changedFields) changedFields->push_back("FogDensity");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "大气高度指数雾基准浓度 [0.0001, 0.05]");
		curY += 28.0f;

		// 5. 全局风场矢量 (Global Atmospheric Wind Field)
		Wui::Label(ctx, { startX, curY + 2.0f }, "全局大气风场 (Global Wind Field):", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		// Wind Speed
		Wui::Label(ctx, { startX, curY + 3.0f }, "风速 (Wind Speed m/s):", theme.Text, theme.FontSizeSmall);
		float windSpeed = comp->WindSpeed;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weather.wind_speed"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, windSpeed, 0.2f, 0.0f, 50.0f, theme))
		{
			comp->WindSpeed = std::clamp(windSpeed, 0.0f, 50.0f);
			if (changedFields) changedFields->push_back("WindSpeed");
			host.MarkDocumentDirty();
		}

		const char* beaufortDesc = (comp->WindSpeed < 0.3f) ? "0级 无风 (Calm)" :
			((comp->WindSpeed < 3.4f) ? "2级 轻风 (Light Breeze)" :
			((comp->WindSpeed < 7.9f) ? "4级 和风 (Moderate Breeze)" :
			((comp->WindSpeed < 13.8f) ? "6级 强风 (Strong Breeze)" :
			((comp->WindSpeed < 20.7f) ? "8级 大风 (Gale)" : "10级 暴风 (Violent Storm)"))));
		curY += 24.0f;
		Wui::Label(ctx, { startX + fieldLabelW, curY }, beaufortDesc, theme.Info, 11.5f);
		curY += 20.0f;

		// Wind Direction X / Z
		Wui::Label(ctx, { startX, curY + 3.0f }, "风向矢量 (Dir X / Z):", theme.Text, theme.FontSizeSmall);
		const float halfControlW = (fieldControlW - 4.0f) * 0.5f;
		float dirX = comp->WindDirection.x;
		float dirZ = comp->WindDirection.z;
		bool dirChanged = false;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weather.wind_dir_x"),
			{ startX + fieldLabelW, curY, halfControlW, 22.0f }, dirX, 0.05f, -1.0f, 1.0f, theme))
		{
			comp->WindDirection.x = dirX;
			dirChanged = true;
		}
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weather.wind_dir_z"),
			{ startX + fieldLabelW + halfControlW + 4.0f, curY, halfControlW, 22.0f }, dirZ, 0.05f, -1.0f, 1.0f, theme))
		{
			comp->WindDirection.z = dirZ;
			dirChanged = true;
		}
		if (dirChanged)
		{
			if (glm::length(comp->WindDirection) > 1e-4f)
				comp->WindDirection = glm::normalize(comp->WindDirection);
			if (changedFields) changedFields->push_back("WindDirection");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "大气水平风向矢量 (X / Z 轴)，直接连续引导海浪传播航向");
		curY += 28.0f;

		// 6. 体积云与积水特性开关
		bool volCloud = comp->EnableVolumetricCloud != 0;
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.weather.chk_vol_cloud"),
			{ startX, curY, contentW, 20.0f }, "启用体积云光线追踪渲染 (Volumetric Clouds)", volCloud, theme))
		{
			comp->EnableVolumetricCloud = volCloud ? 1 : 0;
			if (changedFields) changedFields->push_back("EnableVolumetricCloud");
			host.MarkDocumentDirty();
		}
		curY += 22.0f;

		bool puddles = comp->EnablePuddles != 0;
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.weather.chk_puddles"),
			{ startX, curY, contentW, 20.0f }, "启用地面材质积水与潮湿反光联动 (Puddles Reflection)", puddles, theme))
		{
			comp->EnablePuddles = puddles ? 1 : 0;
			if (changedFields) changedFields->push_back("EnablePuddles");
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		return curY - rect.Y;
	}


	static float DrawWeatherVolumeInspector(
		PanelHost& host,
		Wui::WuiContext& ctx,
		const Wui::WuiRect& rect,
		Entity entity,
		void* instance,
		const Schema::TypeSchema& schema,
		const Wui::WuiRect& visibleRect,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto* comp = static_cast<WeatherVolumeComponent*>(instance);
		if (!comp)
			return 0.0f;

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// 1. 头部徽章
		const Wui::WuiRect badgeRect { startX, curY, 190.0f, 18.0f };
		const Wui::WuiColor badgeBg { 0.20f, 0.45f, 0.65f, 0.28f };
		const Wui::WuiColor badgeBorder { 0.40f, 0.75f, 0.95f, 0.90f };
		Wui::Badge(ctx, badgeRect, "BIOME WEATHER VOLUME", badgeBg, badgeBorder, theme, 10.5f, true, 3.0f);
		curY += 26.0f;

		const float fieldLabelW = 140.0f;
		const float fieldControlW = contentW - fieldLabelW;

		// 2. 启用开关
		bool enabled = comp->Enabled != 0;
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.weathervol.enabled"),
			{ startX, curY, contentW, 20.0f }, "启用局部生物群系天气 (Enable Volume)", enabled, theme))
		{
			comp->Enabled = enabled ? 1 : 0;
			if (changedFields) changedFields->push_back("Enabled");
			host.MarkDocumentDirty();
		}
		curY += 24.0f;

		// 3. 目标天气预设 (Target Weather)
		const char* weatherNames[] = { "Clear (晴朗)", "Overcast (多云)", "Rain (阴雨)", "Storm (暴风雨)", "Snow (降雪)", "Custom" };
		int weatherIdx = std::clamp(static_cast<int>(comp->TargetWeather), 0, 5);
		Wui::Label(ctx, { startX, curY + 3.0f }, "目标生物群系天气:", theme.Text, theme.FontSizeSmall);
		if (Wui::Button(ctx, Wui::HashId("inspect.weathervol.target_btn"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, weatherNames[weatherIdx], theme))
		{
			comp->TargetWeather = static_cast<WeatherSystemComponent::WeatherType>((weatherIdx + 1) % 5);
			if (changedFields) changedFields->push_back("TargetWeather");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 4. 半尺寸 (Extents X / Y / Z)
		Wui::Label(ctx, { startX, curY + 3.0f }, "包围盒半尺寸 (Extents):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;
		const float subW = (contentW - 8.0f) / 3.0f;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weathervol.ext_x"), { startX, curY, subW, 22.0f }, comp->Extents.x, 1.0f, 1.0f, 5000.0f, theme))
		{
			comp->Extents.x = std::max(1.0f, comp->Extents.x);
			if (changedFields) changedFields->push_back("Extents");
			host.MarkDocumentDirty();
		}
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weathervol.ext_y"), { startX + subW + 4.0f, curY, subW, 22.0f }, comp->Extents.y, 1.0f, 1.0f, 5000.0f, theme))
		{
			comp->Extents.y = std::max(1.0f, comp->Extents.y);
			if (changedFields) changedFields->push_back("Extents");
			host.MarkDocumentDirty();
		}
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weathervol.ext_z"), { startX + (subW + 4.0f) * 2.0f, curY, subW, 22.0f }, comp->Extents.z, 1.0f, 1.0f, 5000.0f, theme))
		{
			comp->Extents.z = std::max(1.0f, comp->Extents.z);
			if (changedFields) changedFields->push_back("Extents");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 5. 过渡边缘距离 (Blend Distance)
		Wui::Label(ctx, { startX, curY + 3.0f }, "边缘平滑过渡距离:", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weathervol.blend_dist"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, comp->BlendDistance, 0.5f, 0.1f, 500.0f, theme))
		{
			comp->BlendDistance = std::max(0.1f, comp->BlendDistance);
			if (changedFields) changedFields->push_back("BlendDistance");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 6. 权重 (Weight)
		Wui::Label(ctx, { startX, curY + 3.0f }, "影响权重 (Weight):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.weathervol.weight"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, comp->Weight, 0.05f, 0.0f, 1.0f, theme))
		{
			comp->Weight = std::clamp(comp->Weight, 0.0f, 1.0f);
			if (changedFields) changedFields->push_back("Weight");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		return curY - rect.Y;
	}

	static float DrawMeshletInspector(
		PanelHost& host,
		Wui::WuiContext& ctx,
		const Wui::WuiRect& rect,
		Entity entity,
		void* instance,
		const Schema::TypeSchema& schema,
		const Wui::WuiRect& visibleRect,
		std::vector<std::string>* changedFields)
	{
		auto* comp = static_cast<MeshletComponent*>(instance);
		if (!comp) return 0.0f;

		const Wui::WuiTheme& theme = host.Theme();
		const float startX = rect.X + 8.0f;
		const float contentW = rect.W - 16.0f;
		float curY = rect.Y + 4.0f;

		// 1. 硬件集群配置 (Hardware Limits)
		Wui::Label(ctx, { startX, curY + 2.0f }, "GPU 簇硬件预算 (Hardware Budget):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;

		int64_t maxV = comp->MaxVertices;
		if (Wui::DragInt(ctx, Wui::HashId("inspect.meshlet.max_v"),
			{ startX, curY, contentW, 22.0f }, maxV, 8, 64, theme))
		{
			comp->MaxVertices = static_cast<uint8_t>(std::clamp(maxV, int64_t(8), int64_t(64)));
			if (changedFields) changedFields->push_back("MaxVertices");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "单个 Meshlet 最大顶点预算 (NVidia/AMD 硬件上限 64)");
		curY += 26.0f;

		int64_t maxT = comp->MaxTriangles;
		if (Wui::DragInt(ctx, Wui::HashId("inspect.meshlet.max_t"),
			{ startX, curY, contentW, 22.0f }, maxT, 4, 126, theme))
		{
			comp->MaxTriangles = static_cast<uint8_t>(std::clamp(maxT, int64_t(4), int64_t(126)));
			if (changedFields) changedFields->push_back("MaxTriangles");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, contentW, 22.0f }, "单个 Meshlet 最大三角形预算 (NVidia/AMD 硬件上限 126)");
		curY += 28.0f;

		// 2. 调试可视化开关 (Debug Visualization)
		Wui::Label(ctx, { startX, curY + 2.0f }, "视口调试渲染 (Viewport Overlay):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;

		bool showClusters = (comp->ShowClusters != 0);
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.meshlet.show_clusters"),
			{ startX, curY, contentW, 22.0f }, "真彩微型簇 (Cluster Pseudocolors)", showClusters, theme))
		{
			comp->ShowClusters = showClusters ? 1 : 0;
			if (changedFields) changedFields->push_back("ShowClusters");
			host.MarkDocumentDirty();
		}
		curY += 24.0f;

		bool showCones = (comp->ShowNormalCones != 0);
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.meshlet.show_cones"),
			{ startX, curY, contentW, 22.0f }, "法向背面剔除锥 (Normal Cones)", showCones, theme))
		{
			comp->ShowNormalCones = showCones ? 1 : 0;
			if (changedFields) changedFields->push_back("ShowNormalCones");
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		// 3. 实时遥测卡片 (Realtime Cluster Telemetry Card)
		const Wui::WuiRect cardRect { startX, curY, contentW, 64.0f };
		Wui::PanelBackground(ctx, cardRect, Wui::WuiColor{ 0.08f, 0.12f, 0.18f, 0.85f }, 4.0f);
		Wui::HighlightOutline(ctx, cardRect, Wui::WuiColor{ 0.25f, 0.65f, 0.95f, 0.50f }, 4.0f, 1.0f);

		char line1[64], line2[64];
		std::snprintf(line1, sizeof(line1), "编译集群数: %u 簇", comp->ClusterCount);
		std::snprintf(line2, sizeof(line2), "顶点数: %u  |  三角形数: %u", comp->VertexCount, comp->TriangleCount);

		Wui::Label(ctx, { startX + 8.0f, curY + 8.0f }, line1, theme.Accent, theme.FontSizeBody);
		Wui::Label(ctx, { startX + 8.0f, curY + 34.0f }, line2, theme.TextMuted, theme.FontSizeSmall);
		curY += 72.0f;

		return curY - rect.Y;
	}


		static float DrawBiomePCGInspector(
		PanelHost& host,
		Wui::WuiContext& ctx,
		const Wui::WuiRect& rect,
		Entity entity,
		void* instance,
		const Schema::TypeSchema& schema,
		const Wui::WuiRect& visibleRect,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto* comp = static_cast<BiomePCGComponent*>(instance);
		if (!comp)
			return 0.0f;

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// 1. 头部徽章 (Header Badge)
		const Wui::WuiRect badgeRect { startX, curY, 200.0f, 18.0f };
		const Wui::WuiColor badgeBg { 0.35f, 0.30f, 0.08f, 0.35f };
		const Wui::WuiColor badgeBorder { 0.95f, 0.80f, 0.20f, 0.90f };
		Wui::Badge(ctx, badgeRect, "BIOME PCG GENERATOR", badgeBg, badgeBorder, theme, 10.5f, true, 3.0f);
		curY += 26.0f;

		// 2. 规则卡片：标高与坡度范围
		Wui::Label(ctx, { startX, curY + 2.0f }, "生态标高范围 (Elevation Range m):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;
		const float subW = (contentW - 8.0f) * 0.5f;

		if (Wui::DragFloat(ctx, Wui::HashId("inspect.pcg.min_elev"),
			{ startX, curY, subW, 22.0f }, comp->MinElevation, 1.0f, 0.0f, 5000.0f, theme))
		{
			comp->MinElevation = std::clamp(comp->MinElevation, 0.0f, 5000.0f);
			if (changedFields) changedFields->push_back("MinElevation");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, subW, 22.0f }, "最低生态散布标高 (米)");

		if (Wui::DragFloat(ctx, Wui::HashId("inspect.pcg.max_elev"),
			{ startX + subW + 8.0f, curY, subW, 22.0f }, comp->MaxElevation, 1.0f, 0.0f, 5000.0f, theme))
		{
			comp->MaxElevation = std::clamp(comp->MaxElevation, comp->MinElevation, 5000.0f);
			if (changedFields) changedFields->push_back("MaxElevation");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + subW + 8.0f, curY, subW, 22.0f }, "最高生态散布标高 (米)");
		curY += 26.0f;

		// 最大坡度与湿度阈值
		Wui::Label(ctx, { startX, curY + 2.0f }, "极限坡度 (Slope) 与湿度生成阈值 (Moisture):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.pcg.max_slope"),
			{ startX, curY, subW, 22.0f }, comp->MaxSlope, 0.5f, 0.0f, 89.0f, theme))
		{
			comp->MaxSlope = std::clamp(comp->MaxSlope, 0.0f, 89.0f);
			if (changedFields) changedFields->push_back("MaxSlope");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, subW, 22.0f }, "植被与生物群落允许生长的最大地表坡度 (度)");

		if (Wui::DragFloat(ctx, Wui::HashId("inspect.pcg.moisture"),
			{ startX + subW + 8.0f, curY, subW, 22.0f }, comp->MoistureThreshold, 0.02f, 0.0f, 1.0f, theme))
		{
			comp->MoistureThreshold = std::clamp(comp->MoistureThreshold, 0.0f, 1.0f);
			if (changedFields) changedFields->push_back("MoistureThreshold");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + subW + 8.0f, curY, subW, 22.0f }, "湿度生成阈值 (低于该值判定为荒漠/贫瘠)");
		curY += 26.0f;

		// 伪随机种子
		Wui::Label(ctx, { startX, curY + 2.0f }, "伪随机种子 (Seed):", theme.Text, theme.FontSizeSmall);
		int64_t seedVal = static_cast<int64_t>(comp->Seed);
		if (Wui::DragInt(ctx, Wui::HashId("inspect.pcg.seed"),
			{ startX + 130.0f, curY, contentW - 130.0f, 22.0f }, seedVal, 0, 9999999, theme))
		{
			comp->Seed = static_cast<uint32_t>(seedVal);
			if (changedFields) changedFields->push_back("Seed");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 自动道路放坡开关
		bool autoDeform = comp->AutoDeformTerrain != 0;
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.pcg.autodeform"),
			{ startX, curY, contentW, 20.0f }, "样条道路自动挖填放坡平整 (Auto Deform Along Splines)", autoDeform, theme))
		{
			comp->AutoDeformTerrain = autoDeform ? 1 : 0;
			if (changedFields) changedFields->push_back("AutoDeformTerrain");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 3. 交互按钮
		// 按钮 A: 生成生态散布点
		if (Wui::Button(ctx, Wui::HashId("inspect.pcg.btn_generate"),
			{ startX, curY, contentW, 24.0f }, "生成生态散布点 (Generate Biome Scatter)", theme))
		{
			Scene* scene = entity.GetScene();
			if (scene)
			{
				std::shared_ptr<TerrainData> terrainData;
				glm::mat4 terrainWorld(1.0f);
				for (auto [h, tc] : scene->GetRegistry().view<TerrainComponent>().each())
				{
					Entity te(scene, h);
					if (te.HasComponent<TransformComponent>())
						terrainWorld = te.GetComponent<TransformComponent>().GetLocalMatrix();
					if (te.HasComponent<WorldTransformComponent>())
						terrainWorld = te.GetComponent<WorldTransformComponent>().Matrix;
					terrainData = TerrainRenderer::GetOrCreateTerrainData(tc.TerrainAsset, tc.CellSize, tc.HeightScale);
					if (terrainData) break;
				}
				if (!terrainData)
				{
					terrainData = std::make_shared<TerrainData>(128, 128, 2.0f, 30.0f);
				}

				g_PCGPreviewState.TargetEntity = static_cast<entt::entity>(entity);
				PCGBiomeGenerator::GenerateBiomePoints(*terrainData, *comp, g_PCGPreviewState.Points, terrainWorld);
				g_PCGPreviewState.Active = true;

				for (int b = 0; b < 5; ++b) g_PCGPreviewState.DominantBiomeCounts[b] = 0;
				for (const auto& pt : g_PCGPreviewState.Points)
				{
					int idx = static_cast<int>(pt.Biome);
					if (idx >= 0 && idx < 5)
						g_PCGPreviewState.DominantBiomeCounts[idx]++;
				}
				int maxCount = -1;
				int maxIdx = 1;
				for (int b = 0; b < 5; ++b)
				{
					if (static_cast<int>(g_PCGPreviewState.DominantBiomeCounts[b]) > maxCount)
					{
						maxCount = static_cast<int>(g_PCGPreviewState.DominantBiomeCounts[b]);
						maxIdx = b;
					}
				}
				const char* biomeNames[5] = { "Forest (森林)", "Meadow (草甸)", "RockyMountain (岩山)", "Desert (荒漠)", "SnowPeak (雪峰)" };
				g_PCGPreviewState.DominantBiomeName = biomeNames[maxIdx];

				const float area = terrainData->GetWorldWidth() * terrainData->GetWorldHeight();
				g_PCGPreviewState.Density = (area > 1.0f) ? (static_cast<float>(g_PCGPreviewState.Points.size()) / area) : 0.0f;
			}
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		// 按钮 B: 样条道路自动挖填放坡
		if (Wui::Button(ctx, Wui::HashId("inspect.pcg.btn_deform_spline"),
			{ startX, curY, contentW, 24.0f }, "样条道路自动挖填放坡 (Deform Terrain Along Spline)", theme))
		{
			Scene* scene = entity.GetScene();
			if (scene)
			{
				std::shared_ptr<TerrainData> terrainData;
				glm::mat4 terrainWorld(1.0f);
				for (auto [h, tc] : scene->GetRegistry().view<TerrainComponent>().each())
				{
					Entity te(scene, h);
					if (te.HasComponent<TransformComponent>())
						terrainWorld = te.GetComponent<TransformComponent>().GetLocalMatrix();
					if (te.HasComponent<WorldTransformComponent>())
						terrainWorld = te.GetComponent<WorldTransformComponent>().Matrix;
					terrainData = TerrainRenderer::GetOrCreateTerrainData(tc.TerrainAsset, tc.CellSize, tc.HeightScale);
					if (terrainData) break;
				}

				if (terrainData)
				{
					std::shared_ptr<FoliageData> foliageData;
					if (comp->AutoDeformTerrain != 0)
					{
						for (auto [h, folComp] : scene->GetRegistry().view<FoliageComponent>().each())
						{
							foliageData = FoliageRenderer::GetOrCreateFoliageData(folComp.FoliageAsset);
							if (foliageData) break;
						}
					}

					for (auto [sh, splineComp] : scene->GetRegistry().view<SplineComponent>().each())
					{
						Spline3D spline;
						std::vector<glm::vec3> knots;
						glm::mat4 splineWorldMat(1.0f);
						if (const auto* parentXf = scene->GetRegistry().try_get<TransformComponent>(sh))
							splineWorldMat = parentXf->GetLocalMatrix();
						if (const auto* parentWxf = scene->GetRegistry().try_get<WorldTransformComponent>(sh))
							splineWorldMat = parentWxf->Matrix;

						const auto& children = Hierarchy::ChildrenOf(scene->GetRegistry(), sh);
						for (entt::entity ch : children)
						{
							if (const auto* xf = scene->GetRegistry().try_get<TransformComponent>(ch))
								knots.push_back(glm::vec3(splineWorldMat * glm::vec4(xf->Location, 1.0f)));
						}
						if (knots.size() >= 2)
						{
							for (const auto& k : knots)
								spline.AddPoint(k);
							spline.SetClosedLoop(splineComp.ClosedLoop != 0);
							spline.UpdateTangents();
							PCGBiomeGenerator::DeformTerrainAlongSpline(
								*terrainData, spline, splineComp.RoadWidth, 0.5f, 4.0f, terrainWorld);

							if (foliageData && comp->AutoDeformTerrain != 0)
							{
								PCGBiomeGenerator::ClearFoliageAlongSpline(*foliageData, spline, splineComp.RoadWidth + 1.5f);
							}
						}
					}
					TerrainRenderer::ClearCache();
				}
			}
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		// 按钮 C: 一键烘焙散布到植被系统 (Bake to Foliage Data)
		if (Wui::Button(ctx, Wui::HashId("inspect.pcg.btn_bake_foliage"),
			{ startX, curY, contentW, 24.0f }, "✦ 烘焙散布到场景植被 (Bake Scatter to Foliage System)", theme))
		{
			Scene* scene = entity.GetScene();
			if (scene)
			{
				std::shared_ptr<TerrainData> terrainData;
				glm::mat4 terrainWorld(1.0f);
				for (auto [h, tc] : scene->GetRegistry().view<TerrainComponent>().each())
				{
					Entity te(scene, h);
					if (te.HasComponent<TransformComponent>())
						terrainWorld = te.GetComponent<TransformComponent>().GetLocalMatrix();
					if (te.HasComponent<WorldTransformComponent>())
						terrainWorld = te.GetComponent<WorldTransformComponent>().Matrix;
					terrainData = TerrainRenderer::GetOrCreateTerrainData(tc.TerrainAsset, tc.CellSize, tc.HeightScale);
					if (terrainData) break;
				}
				if (!terrainData)
				{
					terrainData = std::make_shared<TerrainData>(128, 128, 2.0f, 30.0f);
				}

			if (g_PCGPreviewState.Points.empty())
			{
				g_PCGPreviewState.TargetEntity = static_cast<entt::entity>(entity);
				PCGBiomeGenerator::GenerateBiomePoints(*terrainData, *comp, g_PCGPreviewState.Points, terrainWorld);
				g_PCGPreviewState.Active = true;
			}

				std::shared_ptr<FoliageData> foliageData;
				for (auto [h, folComp] : scene->GetRegistry().view<FoliageComponent>().each())
				{
					foliageData = FoliageRenderer::GetOrCreateFoliageData(folComp.FoliageAsset);
					if (foliageData) break;
				}

				if (!foliageData)
				{
					// If no FoliageComponent, add to current entity or ground
					auto& folComp = entity.HasComponent<FoliageComponent>() ? entity.GetComponent<FoliageComponent>() : entity.AddComponent<FoliageComponent>();
					folComp.FoliageAsset = AssetRef::FromPath("foliage/default.wfoliage");
					folComp.EnableWind = 1;
					foliageData = FoliageRenderer::GetOrCreateFoliageData(folComp.FoliageAsset);
				}

				if (foliageData)
				{
					const uint32_t count = PCGBiomeGenerator::PopulateFoliageFromBiomePoints(g_PCGPreviewState.Points, *foliageData);
					(void)count;
				}
			}
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		// 4. 状态摘要卡片 (Status Summary Card)
		const Wui::WuiRect cardRect { startX, curY, contentW, 64.0f };
		Wui::PanelBackground(ctx, cardRect, Wui::WuiColor{ 0.08f, 0.12f, 0.16f, 0.85f }, 4.0f);
		Wui::HighlightOutline(ctx, cardRect, Wui::WuiColor{ 0.85f, 0.70f, 0.20f, 0.45f }, 4.0f, 1.0f);

		char line1[128], line2[128];
		if (g_PCGPreviewState.Active && g_PCGPreviewState.TargetEntity == static_cast<entt::entity>(entity))
		{
			std::snprintf(line1, sizeof(line1), "散布密度: %.3f 点/m²  |  采样总点数: %zu 点",
				g_PCGPreviewState.Density, g_PCGPreviewState.Points.size());
			std::snprintf(line2, sizeof(line2), "生态分布: F:%zu M:%zu R:%zu D:%zu S:%zu | 主导: %s",
				g_PCGPreviewState.DominantBiomeCounts[0],
				g_PCGPreviewState.DominantBiomeCounts[1],
				g_PCGPreviewState.DominantBiomeCounts[2],
				g_PCGPreviewState.DominantBiomeCounts[3],
				g_PCGPreviewState.DominantBiomeCounts[4],
				g_PCGPreviewState.DominantBiomeName.c_str());
		}
		else
		{
			std::snprintf(line1, sizeof(line1), "散布预览: 待生成 (点击上方按钮生成散布点)");
			std::snprintf(line2, sizeof(line2), "预估主导生态: Meadow (草甸)  |  种子: %u", comp->Seed);
		}
		Wui::Label(ctx, { startX + 8.0f, curY + 6.0f }, line1, theme.Text, theme.FontSizeSmall);
		Wui::Label(ctx, { startX + 8.0f, curY + 32.0f }, line2, theme.TextMuted, theme.FontSizeSmall);
		curY += 72.0f;

		return curY - rect.Y;
	}

	static float DrawEQSInspector(
		PanelHost& host,
		Wui::WuiContext& ctx,
		const Wui::WuiRect& rect,
		Entity entity,
		void* instance,
		const Schema::TypeSchema& schema,
		const Wui::WuiRect& visibleRect,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto* comp = static_cast<EQSComponent*>(instance);
		if (!comp)
			return 0.0f;

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// 1. 头部徽章
		const Wui::WuiRect badgeRect { startX, curY, 210.0f, 18.0f };
		const Wui::WuiColor badgeBg { 0.12f, 0.35f, 0.45f, 0.35f };
		const Wui::WuiColor badgeBorder { 0.20f, 0.80f, 0.95f, 0.90f };
		Wui::Badge(ctx, badgeRect, "TACTICAL EQS COVER SYSTEM", badgeBg, badgeBorder, theme, 10.5f, true, 3.0f);
		curY += 26.0f;

		const float fieldLabelW = 120.0f;
		const float fieldControlW = contentW - fieldLabelW;

		// 战术查询模式切换
		const std::vector<std::string> queryModeOptions = {
			"Cover 防守掩体",
			"Flank 侧翼包抄",
			"Retreat 安全撤退"
		};
		int selectedQueryMode = std::clamp(static_cast<int>(comp->QueryMode), 0, 2);
		const int beforeQueryMode = selectedQueryMode;
		Wui::Label(ctx, { startX, curY + 2.0f }, "战术查询模式 (Query Mode):", theme.Text, theme.FontSizeSmall);
		if (Wui::Combo(ctx, Wui::HashId("inspect.eqs.query_mode"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, "", queryModeOptions, selectedQueryMode, theme))
		{
			if (selectedQueryMode != beforeQueryMode)
			{
				comp->QueryMode = static_cast<uint8_t>(selectedQueryMode);
				if (changedFields) changedFields->push_back("QueryMode");
				host.MarkDocumentDirty();
			}
		}
		Wui::Tooltip(ctx, { startX + fieldLabelW, curY, fieldControlW, 22.0f }, "战术行为模式：防守掩体 / 侧翼包抄 / 安全撤退");
		curY += 26.0f;

		// 生成器模式切换
		const std::vector<std::string> generatorOptions = {
			"贴地网格 (Projected Grid)",
			"同心圆环 (Concentric Rings)",
			"掩体贴墙混合 (Hybrid Obstacle Edge)"
		};
		int selectedGenMode = std::clamp(static_cast<int>(comp->GeneratorMode), 0, 2);
		const int beforeGenMode = selectedGenMode;
		Wui::Label(ctx, { startX, curY + 2.0f }, "生成器模式 (Generator):", theme.Text, theme.FontSizeSmall);
		if (Wui::Combo(ctx, Wui::HashId("inspect.eqs.gen_mode"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, "", generatorOptions, selectedGenMode, theme))
		{
			if (selectedGenMode != beforeGenMode)
			{
				comp->GeneratorMode = static_cast<uint8_t>(selectedGenMode);
				if (changedFields) changedFields->push_back("GeneratorMode");
				host.MarkDocumentDirty();
			}
		}
		Wui::Tooltip(ctx, { startX + fieldLabelW, curY, fieldControlW, 22.0f }, "候选空间点采样模式：贴地网格 / 同心圆环 / 掩体边缘混合");
		curY += 26.0f;

		// 2. 查询参数卡片：QueryRadius & GridSpacing
		Wui::Label(ctx, { startX, curY + 2.0f }, "查询搜索半径 (Radius):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.eqs.radius"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, comp->QueryRadius, 0.5f, 1.0f, 100.0f, theme))
		{
			comp->QueryRadius = std::clamp(comp->QueryRadius, 1.0f, 100.0f);
			if (changedFields) changedFields->push_back("QueryRadius");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + fieldLabelW, curY, fieldControlW, 22.0f }, "战术掩体查询候选探测范围 (1~100米)");
		curY += 26.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "候选网格格距 (Spacing):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.eqs.spacing"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, comp->GridSpacing, 0.1f, 0.5f, 10.0f, theme))
		{
			comp->GridSpacing = std::clamp(comp->GridSpacing, 0.5f, 10.0f);
			if (changedFields) changedFields->push_back("GridSpacing");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + fieldLabelW, curY, fieldControlW, 22.0f }, "掩体候选采样点空间网格间距 (0.5~10米)");
		curY += 26.0f;

		// 3. AI 行为协同与威胁源绑定 (AI Behavior Synergy)
		Scene* scene = entity.GetScene();

		auto isThreatTargetName = [](const std::string& name) -> bool {
			if (name.empty()) return false;
			std::string lower = name;
			std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return lower.find("threat") != std::string::npos ||
			       lower.find("sniper") != std::string::npos ||
			       lower.find("enemy") != std::string::npos ||
			       lower.find("hostile") != std::string::npos;
		};

		// 1. 若 comp->ThreatEntityHint == 0：自动检索最近敌对威胁实体并锁定为默认威胁源
		if (scene && comp->ThreatEntityHint == 0)
		{
			const glm::vec3 querierPos = entity.HasComponent<TransformComponent>() ?
				entity.GetComponent<TransformComponent>().Location : glm::vec3(0.0f);
			float closestDistSq = 1e18f;
			entt::entity bestThreatEnt = entt::null;

			for (auto [h, tc] : scene->GetRegistry().view<TransformComponent>().each())
			{
				if (h == static_cast<entt::entity>(entity)) continue;
				std::string tagStr;
				if (const auto* tag = scene->GetRegistry().try_get<TagComponent>(h))
				{
					tagStr = std::string(StringPool::Get().NameOf(tag->Tag));
				}
				if (isThreatTargetName(tagStr))
				{
					const glm::vec3 diff = tc.Location - querierPos;
					const float dSq = glm::dot(diff, diff);
					if (dSq < closestDistSq)
					{
						closestDistSq = dSq;
						bestThreatEnt = h;
					}
				}
			}

			if (bestThreatEnt != entt::null)
			{
				comp->ThreatEntityHint = static_cast<uint32_t>(bestThreatEnt);
				if (entity.HasComponent<BehaviorTreeComponent>())
				{
					entity.GetComponent<BehaviorTreeComponent>().TargetEntity = comp->ThreatEntityHint;
				}
				if (changedFields) changedFields->push_back("ThreatEntityHint");
				host.MarkDocumentDirty();
			}
		}

		// 2. 下拉框选项：优先将场景中检测到的敌对目标排在最前面并标注 [敌对威胁]，天空盒等环境实体排在后面
		struct ThreatCandidate
		{
			std::string Label;
			uint32_t RawId;
		};
		std::vector<ThreatCandidate> hostileCandidates;
		std::vector<ThreatCandidate> otherCandidates;

		if (scene)
		{
			for (auto [h, tc] : scene->GetRegistry().view<TransformComponent>().each())
			{
				if (h == static_cast<entt::entity>(entity)) continue;
				const uint32_t rawId = static_cast<uint32_t>(h);
				std::string tagStr;
				if (const auto* tag = scene->GetRegistry().try_get<TagComponent>(h))
				{
					tagStr = std::string(StringPool::Get().NameOf(tag->Tag));
				}
				std::string name = tagStr.empty() ? ("实体 " + std::to_string(rawId)) : (tagStr + " [" + std::to_string(rawId) + "]");

				if (isThreatTargetName(tagStr))
				{
					hostileCandidates.push_back({ "[敌对威胁] " + name, rawId });
				}
				else
				{
					otherCandidates.push_back({ name, rawId });
				}
			}
		}

		std::vector<std::string> threatEntityLabels;
		std::vector<uint32_t> threatEntityRaws;

		threatEntityLabels.push_back("0: 自动检索最近敌对威胁 (Auto Nearest)");
		threatEntityRaws.push_back(0);

		for (const auto& cand : hostileCandidates)
		{
			threatEntityLabels.push_back(cand.Label);
			threatEntityRaws.push_back(cand.RawId);
		}
		for (const auto& cand : otherCandidates)
		{
			threatEntityLabels.push_back(cand.Label);
			threatEntityRaws.push_back(cand.RawId);
		}

		int selectedThreatIdx = 0;
		if (comp->ThreatEntityHint != 0)
		{
			for (size_t i = 1; i < threatEntityRaws.size(); ++i)
			{
				const uint32_t rawId = threatEntityRaws[i];
				if (comp->ThreatEntityHint == rawId || (rawId % 1000) == (comp->ThreatEntityHint % 1000))
				{
					selectedThreatIdx = static_cast<int>(i);
					break;
				}
			}
		}

		Wui::Label(ctx, { startX, curY + 2.0f }, "锁定威胁源 (Threat Target):", theme.Text, theme.FontSizeSmall);
		if (Wui::Combo(ctx, Wui::HashId("inspect.eqs.threat_combo"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, "", threatEntityLabels, selectedThreatIdx, theme))
		{
			if (selectedThreatIdx >= 0 && selectedThreatIdx < static_cast<int>(threatEntityRaws.size()))
			{
				comp->ThreatEntityHint = threatEntityRaws[selectedThreatIdx];
				if (entity.HasComponent<BehaviorTreeComponent>())
				{
					entity.GetComponent<BehaviorTreeComponent>().TargetEntity = comp->ThreatEntityHint;
				}
				if (changedFields) changedFields->push_back("ThreatEntityHint");
				host.MarkDocumentDirty();
			}
		}
		curY += 26.0f;

		// 协同状态卡片
		const Wui::WuiRect synergyCard { startX, curY, contentW, 44.0f };
		if (entity.HasComponent<BehaviorTreeComponent>())
		{
			auto& bt = entity.GetComponent<BehaviorTreeComponent>();
			Wui::PanelBackground(ctx, synergyCard, Wui::WuiColor{ 0.08f, 0.20f, 0.15f, 0.85f }, 4.0f);
			Wui::HighlightOutline(ctx, synergyCard, Wui::WuiColor{ 0.20f, 0.85f, 0.40f, 0.85f }, 4.0f, 1.0f);
			char synBuf[160];
			std::snprintf(synBuf, sizeof(synBuf), "✓ 已与 BehaviorTreeComponent 协同绑定 (黑板目标: %.1f, %.1f, %.1f | Tag: %u)",
				bt.BlackboardTargetPos.x, bt.BlackboardTargetPos.y, bt.BlackboardTargetPos.z, bt.BlackboardTag);
			Wui::Label(ctx, { startX + 8.0f, curY + 4.0f }, synBuf, theme.Success, theme.FontSizeSmall);
			Wui::Label(ctx, { startX + 8.0f, curY + 22.0f }, "战术查询将自动向行为树黑板实时输出掩体最优站位与评分", theme.TextMuted, theme.FontSizeSmall);
		}
		else
		{
			Wui::PanelBackground(ctx, synergyCard, Wui::WuiColor{ 0.22f, 0.16f, 0.08f, 0.85f }, 4.0f);
			Wui::HighlightOutline(ctx, synergyCard, Wui::WuiColor{ 0.95f, 0.70f, 0.20f, 0.85f }, 4.0f, 1.0f);
			Wui::Label(ctx, { startX + 8.0f, curY + 4.0f }, "当前实体尚未挂载 AI 行为树协同组件", theme.Warning, theme.FontSizeSmall);
			if (Wui::Button(ctx, Wui::HashId("inspect.eqs.btn_add_bt"), { startX + contentW - 170.0f, curY + 20.0f, 160.0f, 20.0f }, "+ 挂载行为树协同组件", theme))
			{
				auto& bt = entity.AddComponent<BehaviorTreeComponent>();
				bt.TargetEntity = comp->ThreatEntityHint;
				bt.Status = BehaviorStatus::Running;
				bt.Enabled = 1;
				bt.BlackboardTag = 88;
				host.MarkDocumentDirty();
			}
		}
		curY += 50.0f;

		// 可扩展评分权重配置
		Wui::Label(ctx, { startX, curY + 2.0f }, "可扩展评分权重 (Scoring Weights):", theme.Accent, theme.FontSizeSmall);
		curY += 22.0f;
		auto& weights = EQSCoverSystem::GetMutableCustomWeights();
		const float weightColW = (contentW - theme.PadSmall) * 0.5f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "遮蔽权重 (Occlusion):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("inspect.eqs.w_occl"), { startX + 130.0f, curY, weightColW - 130.0f, 22.0f }, weights.OcclusionWeight, 0.1f, 0.0f, 10.0f, theme);

		Wui::Label(ctx, { startX + weightColW + theme.PadSmall, curY + 2.0f }, "贴墙权重 (WallProx):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("inspect.eqs.w_wall"), { startX + weightColW + theme.PadSmall + 130.0f, curY, weightColW - 130.0f, 22.0f }, weights.WallProximityWeight, 0.1f, 0.0f, 10.0f, theme);
		curY += 26.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "距离权重 (Distance):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("inspect.eqs.w_dist"), { startX + 130.0f, curY, weightColW - 130.0f, 22.0f }, weights.DistanceWeight, 0.1f, 0.0f, 10.0f, theme);

		Wui::Label(ctx, { startX + weightColW + theme.PadSmall, curY + 2.0f }, "制高点权重 (Elev):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("inspect.eqs.w_elev"), { startX + weightColW + theme.PadSmall + 130.0f, curY, weightColW - 130.0f, 22.0f }, weights.ElevationWeight, 0.1f, 0.0f, 10.0f, theme);
		curY += 30.0f;

		// 4. 交互按钮：执行战术掩体查询
		if (Wui::Button(ctx, Wui::HashId("inspect.eqs.btn_evaluate"),
			{ startX, curY, contentW, 24.0f }, "执行战术掩体查询 (Evaluate Tactical Cover)", theme))
		{
			if (scene)
			{
				auto& reg = scene->GetRegistry();
				const glm::vec3 querierPos = entity.HasComponent<TransformComponent>() ? entity.GetComponent<TransformComponent>().Location : comp->QueryCenter;
				comp->QueryCenter = querierPos;
				uint32_t resolvedThreatRaw = 0;
				glm::vec3 threatPos(0.0f);
				if (!EQSCoverSystem::ResolveSceneThreatPosition(*scene, static_cast<uint32_t>(entity), comp->ThreatEntityHint, querierPos, threatPos, &resolvedThreatRaw))
				{
					threatPos = g_EQSPreviewState.ThreatPos;
				}
				g_EQSPreviewState.ThreatPos = threatPos;

				std::vector<CoverBox> obstacles;
				EQSCoverSystem::GatherSceneObstacles(*scene, static_cast<uint32_t>(entity), resolvedThreatRaw, obstacles);
				std::shared_ptr<TerrainData> terrainData;
				glm::mat4 terrainWorld(1.0f);
				for (auto [h, tc] : reg.view<TerrainComponent>().each())
				{
					Entity te(scene, h);
					if (te.HasComponent<TransformComponent>())
						terrainWorld = te.GetComponent<TransformComponent>().GetLocalMatrix();
					if (te.HasComponent<WorldTransformComponent>())
						terrainWorld = te.GetComponent<WorldTransformComponent>().Matrix;
					terrainData = TerrainRenderer::GetOrCreateTerrainData(tc.TerrainAsset, tc.CellSize, tc.HeightScale);
					if (terrainData) break;
				}


				glm::vec3 bestCover(0.0f);
				float bestScore = -1.0f;
				std::vector<EQSCoverCandidate> candidates;

				const bool found = EQSCoverSystem::EvaluateQueryCandidates(
					static_cast<EQSQueryMode>(comp->QueryMode),
					querierPos,
					threatPos,
					comp->QueryRadius,
					comp->GridSpacing,
					obstacles,
					candidates,
					bestCover,
					bestScore,
					terrainData.get(),
					terrainWorld,
					comp->GeneratorMode);

				g_EQSPreviewState.Candidates.clear();
				for (const auto& c : candidates)
				{
					g_EQSPreviewState.Candidates.push_back({ c.Position, c.TotalScore, c.LineOfSightOcclusion > 0.5f });
				}

				g_EQSPreviewState.TargetEntity = static_cast<entt::entity>(entity);
				g_EQSPreviewState.QuerierPos = querierPos;
				g_EQSPreviewState.BestCoverPos = bestCover;
				g_EQSPreviewState.BestScore = bestScore;
				g_EQSPreviewState.Evaluated = found;
				comp->BestCoverFound = found ? 1 : 0;

				if (found && entity.HasComponent<BehaviorTreeComponent>())
				{
					auto& bt = entity.GetComponent<BehaviorTreeComponent>();
					bt.BlackboardTargetPos = bestCover;
					bt.BlackboardValue = bestScore;
					bt.BlackboardTag = 88;
				}
			}
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		// 按钮 B: 一键移动查询者到最佳掩体位置
		if (Wui::Button(ctx, Wui::HashId("inspect.eqs.btn_move_to_cover"),
			{ startX, curY, contentW, 24.0f }, "🏃 移动查询者至最佳掩体 (Move Querier to Cover)", theme))
		{
			if (g_EQSPreviewState.Evaluated)
			{
				if (entity.HasComponent<TransformComponent>())
				{
					entity.GetComponent<TransformComponent>().Location = g_EQSPreviewState.BestCoverPos;
				}
				if (entity.HasComponent<BehaviorTreeComponent>())
				{
					entity.GetComponent<BehaviorTreeComponent>().BlackboardTargetPos = g_EQSPreviewState.BestCoverPos;
				}
				host.MarkDocumentDirty();
			}
		}
		curY += 28.0f;

		// 5. 状态摘要卡片
		const Wui::WuiRect cardRect { startX, curY, contentW, 64.0f };
		Wui::PanelBackground(ctx, cardRect, Wui::WuiColor{ 0.08f, 0.12f, 0.16f, 0.85f }, 4.0f);
		Wui::HighlightOutline(ctx, cardRect, Wui::WuiColor{ 0.20f, 0.80f, 0.95f, 0.45f }, 4.0f, 1.0f);

		char line1[128], line2[128];
		if (comp->BestCoverFound != 0 || (g_EQSPreviewState.Evaluated && g_EQSPreviewState.TargetEntity == static_cast<entt::entity>(entity)))
		{
			const glm::vec3 bpos = g_EQSPreviewState.BestCoverPos;
			const char* modeNames[3] = { "Cover 掩体", "Flank 包抄", "Retreat 撤退" };
			const char* modeName = (comp->QueryMode <= 2) ? modeNames[comp->QueryMode] : "Cover 掩体";
			const char* coverQuality = (g_EQSPreviewState.BestScore >= 2.0f)
				? "Optimal / 优良掩蔽"
				: (g_EQSPreviewState.BestScore >= 1.0f ? "Partial Cover / 部分遮挡" : "Low Cover / 暴露");
			std::snprintf(line1, sizeof(line1), "模式: %s | 最佳得分: %.2f (%s)",
				modeName, g_EQSPreviewState.BestScore, coverQuality);
			std::snprintf(line2, sizeof(line2), "最优目标: (%.1f, %.1f, %.1f) | 采样候选点: %zu 处",
				bpos.x, bpos.y, bpos.z, g_EQSPreviewState.Candidates.size());
		}
		else
		{
			std::snprintf(line1, sizeof(line1), "战术状态: 待评估 (点击上方按钮或在视口拖动AI)");
			std::snprintf(line2, sizeof(line2), "搜索半径: %.1fm | 采样间距: %.1fm", comp->QueryRadius, comp->GridSpacing);
		}
		Wui::Label(ctx, { startX + 8.0f, curY + 6.0f }, line1, theme.Accent, theme.FontSizeSmall);
		Wui::Label(ctx, { startX + 8.0f, curY + 32.0f }, line2, theme.TextMuted, theme.FontSizeSmall);
		curY += 72.0f;

		return curY - rect.Y;
	}

	static float DrawFoliageInteractionInspector(
		PanelHost& host,
		Wui::WuiContext& ctx,
		const Wui::WuiRect& rect,
		Entity entity,
		void* instance,
		const Schema::TypeSchema& schema,
		const Wui::WuiRect& visibleRect,
		std::vector<std::string>* changedFields)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto* comp = static_cast<FoliageInteractionComponent*>(instance);
		if (!comp)
			return 0.0f;

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// 1. 头部徽章
		const Wui::WuiRect badgeRect { startX, curY, 230.0f, 18.0f };
		const Wui::WuiColor badgeBg { 0.08f, 0.35f, 0.25f, 0.35f };
		const Wui::WuiColor badgeBorder { 0.20f, 0.90f, 0.65f, 0.90f };
		Wui::Badge(ctx, badgeRect, "FOLIAGE PHYSICAL INTERACTION", badgeBg, badgeBorder, theme, 10.5f, true, 3.0f);
		curY += 26.0f;

		// 2. 物理推挤与回弹参数
		const float fieldLabelW = 120.0f;
		const float fieldControlW = contentW - fieldLabelW;

		Wui::Label(ctx, { startX, curY + 2.0f }, "推挤交互半径 (Radius):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.foliage.radius"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, comp->PushRadius, 0.1f, 0.2f, 10.0f, theme))
		{
			comp->PushRadius = std::clamp(comp->PushRadius, 0.2f, 10.0f);
			if (changedFields) changedFields->push_back("PushRadius");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + fieldLabelW, curY, fieldControlW, 22.0f }, "推挤弯折半径 (0.2~10.0米)");
		curY += 26.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "推挤倒伏强度 (Strength):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.foliage.strength"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, comp->PushStrength, 0.1f, 0.1f, 5.0f, theme))
		{
			comp->PushStrength = std::clamp(comp->PushStrength, 0.1f, 5.0f);
			if (changedFields) changedFields->push_back("PushStrength");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + fieldLabelW, curY, fieldControlW, 22.0f }, "受力位移强度系数 (0.1~5.0)");
		curY += 26.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "弹性恢复速率 (Recovery):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("inspect.foliage.recovery"),
			{ startX + fieldLabelW, curY, fieldControlW, 22.0f }, comp->RecoverySpeed, 0.2f, 0.5f, 20.0f, theme))
		{
			comp->RecoverySpeed = std::clamp(comp->RecoverySpeed, 0.5f, 20.0f);
			if (changedFields) changedFields->push_back("RecoverySpeed");
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + fieldLabelW, curY, fieldControlW, 22.0f }, "弹簧回弹阻尼恢复速度 (0.5~20.0 /秒)");
		curY += 26.0f;

		bool leaveTracks = comp->LeaveTracks != 0;
		if (Wui::Checkbox(ctx, Wui::HashId("inspect.foliage.tracks"),
			{ startX, curY, contentW, 20.0f }, "保留地表履带与压痕轨迹 (Leave Tracks)", leaveTracks, theme))
		{
			comp->LeaveTracks = leaveTracks ? 1 : 0;
			if (changedFields) changedFields->push_back("LeaveTracks");
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 3. 交互按钮：施加测试推挤冲量
		if (Wui::Button(ctx, Wui::HashId("inspect.foliage.btn_impulse"),
			{ startX, curY, contentW, 24.0f }, "施加测试推挤冲量 (Simulate Push Impulse)", theme))
		{
			const glm::vec3 entPos = entity.HasComponent<TransformComponent>()
				? entity.GetComponent<TransformComponent>().Location : glm::vec3(0.0f);
			const glm::vec3 testFoliage = entPos + glm::vec3(comp->PushRadius * 0.4f, 0.0f, comp->PushRadius * 0.4f);
			const glm::vec3 pusherVel { 2.5f, 0.0f, 1.2f };
			glm::vec3 disp(0.0f);
			FoliageInteractionSystem::ComputePushDisplacement(
				testFoliage, entPos, pusherVel, comp->PushRadius, comp->PushStrength, disp);

			// 直接向植被交互场注入物理冲量，产生视口即时压弯倒伏与阻尼回弹
			FoliageInteractionSystem::TriggerImpulse(entPos, comp->PushRadius, comp->PushStrength, comp->RecoverySpeed);

			g_FoliagePreviewState.Active = true;
			g_FoliagePreviewState.TargetEntity = static_cast<entt::entity>(entity);
			g_FoliagePreviewState.Displacement = disp;
			g_FoliagePreviewState.PushMagnitude = glm::length(disp);
			g_FoliagePreviewState.PushDirection = (g_FoliagePreviewState.PushMagnitude > 0.001f)
				? glm::normalize(disp) : glm::vec3(1.0f, 0.0f, 0.0f);
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		// 按钮 B: 向场景视口植被注入物理受力风场
		if (Wui::Button(ctx, Wui::HashId("inspect.foliage.btn_inject_wind"),
			{ startX, curY, contentW, 24.0f }, "⚡ 向视口植被注入物理冲击 (Inject Impulse to Foliage)", theme))
		{
			const glm::vec3 entPos = entity.HasComponent<TransformComponent>()
				? entity.GetComponent<TransformComponent>().Location : glm::vec3(0.0f);
			FoliageInteractionSystem::TriggerImpulse(entPos, comp->PushRadius * 1.5f, comp->PushStrength * 2.0f, comp->RecoverySpeed);
			FoliageRenderer::AddLocalWind(entPos, comp->PushRadius, comp->PushStrength * 3.0f,
				comp->RecoverySpeed * 0.5f, 1.5f, LocalWindType::Radial, glm::vec3(0.0f, 1.0f, 0.0f));
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		// 4. 实时物理监视卡片
		const Wui::WuiRect cardRect { startX, curY, contentW, 64.0f };
		Wui::PanelBackground(ctx, cardRect, Wui::WuiColor{ 0.08f, 0.14f, 0.12f, 0.85f }, 4.0f);
		Wui::HighlightOutline(ctx, cardRect, Wui::WuiColor{ 0.20f, 0.90f, 0.70f, 0.50f }, 4.0f, 1.0f);

		const size_t activeCells = FoliageInteractionSystem::GetActiveCellCount();
		char line1[128], line2[128];
		if (g_FoliagePreviewState.Active && g_FoliagePreviewState.TargetEntity == static_cast<entt::entity>(entity))
		{
			const glm::vec3 d = g_FoliagePreviewState.Displacement;
			std::snprintf(line1, sizeof(line1), "倒伏位移: (%.2f, %.2f, %.2f) 模长: %.2fm | 活跃场网格: %zu",
				d.x, d.y, d.z, g_FoliagePreviewState.PushMagnitude, activeCells);
			std::snprintf(line2, sizeof(line2), "物理状态: 动力学受力中  |  残存轨迹: %s", comp->LeaveTracks ? "已记录" : "未开启");
		}
		else
		{
			std::snprintf(line1, sizeof(line1), "倒伏位移: 静止就绪 (Displacement: 0.00, 0.00, 0.00) | 活跃场网格: %zu", activeCells);
			std::snprintf(line2, sizeof(line2), "阻尼回弹率: %.1f /s  |  推挤半径: %.1f m", comp->RecoverySpeed, comp->PushRadius);
		}
		Wui::Label(ctx, { startX + 8.0f, curY + 8.0f }, line1, theme.Accent, theme.FontSizeBody);
		Wui::Label(ctx, { startX + 8.0f, curY + 34.0f }, line2, theme.TextMuted, theme.FontSizeSmall);
		curY += 72.0f;

		return curY - rect.Y;
	}

float PropertiesPanel::DrawComponentInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect){
		const Wui::WuiTheme& theme = m_Host.Theme();
		void* instance = entity.GetComponent(schema.Storage->ComponentId);
		if (!instance)
			return 0;
		const Wui::WuiId base = Wui::HashId(schema.DisplayName.c_str());
		// P4-U13b:本分区内被编辑的字段名收在这里,分区画完统一登记成实例覆盖。
		// 归属判定(这个实体属于哪条实例记录)放在 RegisterPrefabOverrides —— 普通实体编辑不产生记录。
		std::vector<std::string> changedFields;
		float height = 0.0f;

		// PURE-ECS:合成 arena 每组件重新开始(`Used` 原本没人清零;接上原生容器字段后,
		// 第 257 个节点起的容器行会静默退化成只读摘要 —— 见 ResetScriptRowArenas)。
		ResetScriptRowArenas();
		// 原生容器元素的 `+`/子行需要 schema(命名 struct 元素按它建子行)。与脚本路径的
		// `m_ScriptInspectingEntity → scene.Schemas()` 同一来源。
		{
			Scene* rowScene = entity.IsValid() ? entity.GetScene() : nullptr;
			m_ContainerElementSchemas = rowScene ? &rowScene->GetContext().Schemas() : nullptr;
		}

		// 自定义检查器:与迁移前一致的三行 Location/Rotation(度)/Scale。
		if (schema.Id.Name == "World::TransformComponent")
			height = DrawTransformInspector(ctx, rect, *static_cast<TransformComponent*>(instance), schema,
				visibleRect, &changedFields);
		// 自定义检查器:Primary / Fixed Aspect Ratio + 投影类型下拉 + 对应参数组。
		else if (schema.Id.Name == "World::PhysicsMaterialComponent")
			height = DrawPhysicsMaterialInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::VfxComponent")
			height = DrawVfxInspector(ctx, rect, entity, instance, schema, visibleRect, &changedFields);

		else if (schema.Id.Name == "World::CameraComponent")
			height = DrawCameraInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::AudioSourceComponent")
			height = DrawAudioSourceInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::SpriteComponent")
			height = DrawSpriteInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::CapsuleCollider2DComponent")
			height = DrawCapsuleCollider2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::PolygonCollider2DComponent")
			height = DrawPolygonCollider2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::ChainCollider2DComponent")
			height = DrawChainCollider2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::CompositeCollider2DComponent")
			height = DrawCompositeCollider2DInspector(ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::PlatformEffector2DComponent")
			height = DrawPlatformEffector2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::AreaEffector2DComponent")
			height = DrawAreaEffector2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::BuoyancyEffector2DComponent")
			height = DrawBuoyancyEffector2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::SurfaceEffector2DComponent")
			height = DrawSurfaceEffector2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::DistanceJoint2DComponent")
			height = DrawDistanceJoint2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::RevoluteJoint2DComponent")
			height = DrawRevoluteJoint2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::PrismaticJoint2DComponent")
			height = DrawPrismaticJoint2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::WheelJoint2DComponent")
			height = DrawWheelJoint2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::BoxCollider2DComponent")
			height = DrawBoxCollider2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::CircleCollider2DComponent")
			height = DrawCircleCollider2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::SpriteAnimatorComponent")
			height = DrawSpriteAnimatorInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::TilemapComponent")
			height = DrawTilemapInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::PixelPerfectCameraComponent")
			height = DrawPixelPerfectCameraInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::ParallaxLayerComponent")
			height = DrawParallaxLayerInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::PointLight2DComponent")
			height = DrawPointLight2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::GlobalLight2DComponent")
			height = DrawGlobalLight2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::ShadowCaster2DComponent")
			height = DrawShadowCaster2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::FreeformLight2DComponent")
			height = DrawFreeformLight2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		// 自定义检查器:关卡数据层归属(勾选替代裸掩码数字);Play/Simulate 只读时退回普通行。
		else if (schema.Id.Name == "World::LevelEntityComponent" && !m_ReadOnly)
			height = DrawLevelEntityInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::TerrainComponent")
			height = DrawTerrainInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::NavMeshAgentComponent")
			height = DrawNavMeshAgentInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::NavMeshObstacleComponent")
			height = DrawNavMeshObstacleInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::MeshLODComponent")
			height = DrawMeshLODComponentInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::BehaviorTreeComponent")
			height = DrawBehaviorTreeInspector(ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::SmartObjectComponent")
			height = DrawSmartObjectInspector(ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::DecalComponent")
			height = DrawDecalInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::ConvexCollider3DComponent")
			height = DrawConvexCollider3DInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::WhiteboxComponent")
			height = DrawWhiteboxInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::SplineComponent")
			height = DrawSplineInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::WaterBodyComponent")
			height = DrawWaterBodyInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::WeatherSystemComponent")
			height = DrawWeatherSystemInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::WeatherVolumeComponent")
			height = DrawWeatherVolumeInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::MeshletComponent")
			height = DrawMeshletInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::BiomePCGComponent")
			height = DrawBiomePCGInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::EQSComponent")
			height = DrawEQSInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::FoliageInteractionComponent")
			height = DrawFoliageInteractionInspector(m_Host, ctx, rect, entity, instance, schema, visibleRect, &changedFields);


		else
			height = DrawSchemaFields(ctx, base, rect, instance, schema.DisplayName, schema, visibleRect,
				&changedFields);

		RegisterPrefabOverrides(entity, changedFields);
		return height;
	}


	// ---- 2026-09-26 脚本组件重写:统一脚本检视器(两个脚本组件共用)----
	//
	// 布局:脚本引用行 → 状态行(State + LastError,Luau 再加 ReloadDiagnostic)→ 属性表
	//      (每个 `PropertyNode` 一行)→ 动作行(Luau 的 Reload,id 保持 `lua.reload`)。
	//
	// 三条硬口径(方案 v2 §3 + 派工单):
	//   ① **编辑态不实例化脚本**:属性直接读写组件里的 `Properties`;不建 VM、不跑 OnCreate、
	//      不构造脚本实例(实例只由 Scene 在 Play/Simulate 按 schema 工厂创建);
	//   ② **Play/Simulate 只读**:控件走既有只读行 / 禁用按钮契约 + 一行只读说明;
	//      C++ 组件在运行实例在场时按**实例**读真实值(只读展示),Luau 用组件里保存的值;
	//   ③ **属性行复用既有类型化行渲染**:每条属性造一条临时 `FieldSchema`(Get/Set 直连属性值),
	//      逐个调用 `DrawSchemaFields` —— 控件 / 只读行 / 无障碍节点 / 悬停说明 / 预制体覆盖登记
	//      都不另写一套。
	//
	// 无障碍 id 契约:`properties.<组件名>.<属性名>`(脚本可直接算);状态与诊断行见各段注释。
float PropertiesPanel::DrawScriptComponentInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity, void* instance, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect, std::vector<std::string>* changedFields){
		(void)ctx;
		(void)rect;
		(void)entity;
		(void)instance;
		(void)schema;
		(void)visibleRect;
		(void)changedFields;
		return 0.0f;
	}


float PropertiesPanel::DrawTransformInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, TransformComponent& transform, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect, std::vector<std::string>* changedFields){
		const Wui::WuiTheme& theme = m_Host.Theme();
		const std::string& typeName = schema.DisplayName;
		const bool writable = !m_ReadOnly;
		// 自定义检查器也走同一份字段标签(schema 字段名 → 显示文案),id 仍是 PropPath。
		const Wui::LocalizedLabel locationLabel = SchemaFieldLabel(schema, "Location");
		const Wui::LocalizedLabel rotationLabel = SchemaFieldLabel(schema, "Rotation");
		const Wui::LocalizedLabel scaleLabel = SchemaFieldLabel(schema, "Scale");

		if (!writable)
		{
			// Play/Simulate:只读展示(与通用只读字段同一契约:properties.<...> 文本节点)。
			DrawReadOnlyRow(ctx, PropPath(typeName, "Location"), ComponentRect(rect, 0, 20),
				locationLabel, FormatFloatText(transform.Location.x, 2) + ", "
					+ FormatFloatText(transform.Location.y, 2) + ", " + FormatFloatText(transform.Location.z, 2), theme);
			const glm::vec3 degrees = glm::degrees(transform.GetEulerAngles());
			DrawReadOnlyRow(ctx, PropPath(typeName, "Rotation"), ComponentRect(rect, 1, 20),
				rotationLabel, FormatFloatText(degrees.x, 2) + ", " + FormatFloatText(degrees.y, 2) + ", "
					+ FormatFloatText(degrees.z, 2), theme);
			DrawReadOnlyRow(ctx, PropPath(typeName, "Scale"), ComponentRect(rect, 2, 20),
				scaleLabel, FormatFloatText(transform.Scale.x, 2) + ", " + FormatFloatText(transform.Scale.y, 2) + ", "
					+ FormatFloatText(transform.Scale.z, 2), theme);
			return 66.0f;
		}

		static struct
		{
			bool Active = false;
			TransformComponent Before;
			Entity EntityHandle;
		} s_EditState;

		Entity selectedEntity = m_Host.GetSelectedEntity();
		if (selectedEntity != s_EditState.EntityHandle)
		{
			s_EditState.Active = false;
			s_EditState.EntityHandle = selectedEntity;
		}

		if (!s_EditState.Active)
		{
			s_EditState.Before = transform;
		}

		bool changed = false;
		// Rotation 面板按度数显示;写回统一走 SetTransform(同步四元数与状态)。
		glm::vec3 rotationDegrees = glm::degrees(transform.GetEulerAngles());
		// 逐行取"这一行是否被编辑":覆盖登记要精确到 Location/Rotation/Scale,
		// 不能只记"Transform 动过"(否则覆盖计数与实际改动对不上)。
		// 行高由库件的横排/竖排决定(窄控件列竖排 = 3 倍行高),下一行用上一行的返回值累加;
		// 行内无障碍节点由库件登记(前面板自算的 reachable 不再被向量行使用)。
		bool locationChanged = false;
		const float locationHeight = DrawVec3Row(ctx, PropPath(typeName, "Location"), rect.X, rect.Y, rect.W,
			locationLabel, transform.Location, theme, locationChanged);
		bool rotationChanged = false;
		const float rotationHeight = DrawVec3Row(ctx, PropPath(typeName, "Rotation"), rect.X,
			rect.Y + locationHeight, rect.W, rotationLabel, rotationDegrees, theme, rotationChanged);
		bool scaleChanged = false;
		const float scaleHeight = DrawVec3Row(ctx, PropPath(typeName, "Scale"), rect.X,
			rect.Y + locationHeight + rotationHeight, rect.W, scaleLabel, transform.Scale, theme, scaleChanged);
		changed |= locationChanged || rotationChanged || scaleChanged;
		if (changed)
		{
			s_EditState.Active = true;
			transform.SetTransform(transform.Location, glm::radians(rotationDegrees), transform.Scale);
			m_Host.MarkDocumentDirty();
			if (changedFields)
			{
				if (locationChanged) changedFields->push_back(typeName + ".Location");
				if (rotationChanged) changedFields->push_back(typeName + ".Rotation");
				if (scaleChanged) changedFields->push_back(typeName + ".Scale");
			}
		}

		if (s_EditState.Active)
		{
			const bool mouseReleased = ctx.Input().MouseReleased[0];
			const bool enterPressed = ctx.IsKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Enter);
			const bool commitWithoutDrag = !ctx.Input().MouseDown[0] && changed;

			if (mouseReleased || enterPressed || commitWithoutDrag)
			{
				if (selectedEntity.IsValid() && (
					transform.Location != s_EditState.Before.Location ||
					transform.Rotation != s_EditState.Before.Rotation ||
					transform.Scale != s_EditState.Before.Scale))
				{
					m_Host.PushCommand(std::make_unique<EntityTransformCommand>(
						selectedEntity, s_EditState.Before, transform, "Inspect Transform"));
				}
				s_EditState.Active = false;
			}
		}
		return locationHeight + rotationHeight + scaleHeight;
	}


// 关卡数据层归属:勾选式(掩码数字 → 层的名字)。层的定义来自**关卡 manifest**(host 提供),
// 所以没打开关卡时本函数不可用(调用方已按 !m_ReadOnly 分派,这里再兜一层 host 判定)。
float PropertiesPanel::DrawLevelEntityInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect, std::vector<std::string>* changedFields){
		const Wui::WuiTheme& theme = m_Host.Theme();

		std::vector<Level::DataLayerDefinition> layers;
		if (!m_Host.GetEditableDataLayers(&layers) || layers.empty())
			return 0.0f;   // 没打开关卡 ⇒ 0 行,由调用方保证这里不会被走到(退化为普通行)

		auto* component = static_cast<LevelEntityComponent*>(instance);
		if (!component)
			return 0.0f;

		const Schema::FieldSchema* maskField = nullptr;
		for (const Schema::FieldSchema& field : schema.Fields)
			if (field.Name == "DataLayerMask")
				maskField = &field;
		if (!maskField)
			return 0.0f;

		const std::string& typeName = schema.DisplayName;
		const auto reachable = [&visibleRect](const Wui::WuiRect& control)
		{
			return control.X + control.W * 0.5f >= visibleRect.X
				&& control.X + control.W * 0.5f <= visibleRect.X + visibleRect.W
				&& control.Y + control.H * 0.5f >= visibleRect.Y
				&& control.Y + control.H * 0.5f <= visibleRect.Y + visibleRect.H;
		};
		const auto markField = [changedFields, &typeName]()
		{
			if (changedFields)
				changedFields->push_back(typeName + ".DataLayerMask");
		};

		float y = 0.0f;
		uint32_t mask = component->DataLayerMask;
		const uint32_t original = mask;

		// 标题行:说明这一区是"关卡数据层归属",末尾给出原始掩码(排查时与 manifest 对得上)。
		const auto hex32 = [](uint32_t value)
		{
			static const char* digits = "0123456789ABCDEF";
			std::string text;
			for (int shift = 28; shift >= 0; shift -= 4)
				text.push_back(digits[(value >> shift) & 0xF]);
			return text;
		};
		Wui::Label(ctx, { rect.X + 4.0f, rect.Y + y + 3.0f },
			Wui::Tr("panel.properties.level_layers", "Level data layers"),
			theme.TextMuted, 13.0f);
		Wui::Label(ctx, { rect.X + rect.W - 74.0f, rect.Y + y + 3.0f }, "0x" + hex32(mask),
			theme.TextMuted, 12.0f);
		y += kRowHeight;

		// 一层一个勾选框;勾选 = 该位。(0 = 常驻可见,由最后那一行表达。)
		for (const Level::DataLayerDefinition& layer : layers)
		{
			const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, kRowHeight };
			if (reachable(row))
			{
				bool on = (mask & layer.LayerMask) != 0;
				const std::string label = layer.Name.empty()
					? ("Layer" + std::to_string(layer.LayerMask)) : layer.Name;
				const std::string term = "mask " + std::to_string(layer.LayerMask);
				const Wui::WuiRect box { row.X + 8.0f, row.Y + 2.0f, row.W - 16.0f, row.H - 4.0f };
				if (Wui::Checkbox(ctx,
					Wui::HashId(("panel.properties.level_layers." + std::to_string(layer.LayerMask)).c_str()),
					box, label, term, on, theme))
				{
					mask = on ? (mask | layer.LayerMask) : (mask & ~layer.LayerMask);
				}
			}
			y += kRowHeight;
		}

		// "常驻可见"(掩码 0):不属于任何层的实体在**所有**层组合下都可见。
		{
			const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, kRowHeight };
			if (reachable(row))
			{
				bool always = mask == 0;
				const Wui::WuiRect box { row.X + 8.0f, row.Y + 2.0f, row.W - 16.0f, row.H - 4.0f };
				if (Wui::Checkbox(ctx, Wui::HashId("panel.properties.level_layers.always"),
					box, Wui::Tr("panel.properties.level_always", "Always visible (no layer)"),
					always, theme))
				{
					mask = always ? 0u : 1u;   // 取消"常驻" ⇒ 回到常驻层(第一个层位)
				}
			}
			y += kRowHeight;
		}

		if (mask != original)
		{
			component->DataLayerMask = mask;
			markField();
		}
		return y;
	}


float PropertiesPanel::DrawCameraInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect, std::vector<std::string>* changedFields){
		const Wui::WuiTheme& theme = m_Host.Theme();
		// CameraComponent 的 schema 字段:Primary / FixedAspectRatio / Camera(Object Of CameraSettings)。
		const Schema::FieldSchema* primaryField = nullptr;
		const Schema::FieldSchema* fixedField = nullptr;
		const Schema::FieldSchema* cameraField = nullptr;
		for (const Schema::FieldSchema& field : schema.Fields)
		{
			if (field.Name == "Primary") primaryField = &field;
			else if (field.Name == "FixedAspectRatio") fixedField = &field;
			else if (field.K == Schema::Kind::Object) cameraField = &field;
		}
		void* cameraInstance = cameraField && cameraField->GetPtr ? cameraField->GetPtr(instance) : nullptr;
		const Schema::TypeSchema* cameraSchema = cameraField && cameraField->GetNested ? cameraField->GetNested() : nullptr;
		if (!primaryField || !fixedField || !cameraInstance || !cameraSchema)
			return 0;
		auto* camera = static_cast<CameraSettings*>(cameraInstance);

		const std::string& typeName = schema.DisplayName;
		const std::string& cameraType = cameraSchema->DisplayName;
		const bool writable = !m_ReadOnly;
		const auto reachable = [&visibleRect](const Wui::WuiRect& control)
		{
			return control.X + control.W * 0.5f >= visibleRect.X
				&& control.X + control.W * 0.5f <= visibleRect.X + visibleRect.W
				&& control.Y + control.H * 0.5f >= visibleRect.Y
				&& control.Y + control.H * 0.5f <= visibleRect.Y + visibleRect.H;
		};
		float y = 0.0f;
		bool changed = false;
		// 覆盖字段名与属性行 id 同一口径(PropPath 去掉 "properties." 前缀)。
		const auto markField = [changedFields](const std::string& field)
		{
			if (changedFields)
				changedFields->push_back(field);
		};

		const Wui::WuiRect primaryRow { rect.X, rect.Y + y, rect.W, kRowHeight };
		const Wui::LocalizedLabel primaryLabel = SchemaFieldLabel(*primaryField);
		if (writable)
		{
			bool primary = std::get<bool>(primaryField->Get(instance));
			const bool before = primary;
			// 复选框列固定在 +140(标签列宽 140):术语对照预算 136,不压到控件上。
			Wui::LabelWithTerm(ctx, { primaryRow.X + 4, primaryRow.Y + 3 }, primaryLabel.Text, primaryLabel.Term,
				theme.TextMuted, 13.0f, theme, 136.0f);
			Wui::Checkbox(ctx, Wui::HashId(PropPath(typeName, "Primary").c_str()),
				{ primaryRow.X + 140.0f, primaryRow.Y + 1, 20, 20 }, "", primary, theme);
			const Wui::WuiRect primaryBox { primaryRow.X + 140.0f, primaryRow.Y + 1, 20, 20 };
			RegisterNode(Wui::HashId(PropPath(typeName, "Primary").c_str()), "checkbox",
				primaryBox, TermText(primaryLabel), primary ? "true" : "false", reachable(primaryBox));
			if (primary != before)
			{
				primaryField->Set(instance, Schema::Value(primary));
				changed = true;
				markField(typeName + ".Primary");
			}
		}
		else
		{
			DrawReadOnlyRow(ctx, PropPath(typeName, "Primary"), primaryRow, primaryLabel,
				std::get<bool>(primaryField->Get(instance)) ? "true" : "false", theme);
		}
		y += kRowHeight;

		const Wui::WuiRect fixedRow { rect.X, rect.Y + y, rect.W, kRowHeight };
		const Wui::LocalizedLabel fixedLabel = SchemaFieldLabel(*fixedField);
		if (writable)
		{
			bool fixed = std::get<bool>(fixedField->Get(instance));
			const bool before = fixed;
			Wui::LabelWithTerm(ctx, { fixedRow.X + 4, fixedRow.Y + 3 }, fixedLabel.Text, fixedLabel.Term,
				theme.TextMuted, 13.0f, theme, 136.0f);
			Wui::Checkbox(ctx, Wui::HashId(PropPath(typeName, "FixedAspectRatio").c_str()),
				{ fixedRow.X + 140.0f, fixedRow.Y + 1, 20, 20 }, "", fixed, theme);
			const Wui::WuiRect fixedBox { fixedRow.X + 140.0f, fixedRow.Y + 1, 20, 20 };
			RegisterNode(Wui::HashId(PropPath(typeName, "FixedAspectRatio").c_str()), "checkbox",
				fixedBox, TermText(fixedLabel), fixed ? "true" : "false", reachable(fixedBox));
			if (fixed != before)
			{
				fixedField->Set(instance, Schema::Value(fixed));
				changed = true;
				markField(typeName + ".FixedAspectRatio");
			}
		}
		else
		{
			DrawReadOnlyRow(ctx, PropPath(typeName, "FixedAspectRatio"), fixedRow, fixedLabel,
				std::get<bool>(fixedField->Get(instance)) ? "true" : "false", theme);
		}
		y += kRowHeight;

		const Schema::FieldSchema* projectionField = nullptr;
		for (const Schema::FieldSchema& field : cameraSchema->Fields)
			if (field.K == Schema::Kind::Enum)
			{
				projectionField = &field;
				break;
			}
		const std::string projectionId = PropPath(cameraType, "ProjectionType");
		const Wui::WuiRect projectionRow { rect.X, rect.Y + y, rect.W, kRowHeight };
		const Wui::LocalizedLabel projectionLabel = Wui::TrLabel("panel.properties.camera.projection", "Projection");
		CameraSettings::ProjectionType projection = camera->m_ProjectionType;
		if (projectionField && projectionField->GetEnum && projectionField->Set && writable)
		{
			const Schema::EnumSchema* enumSchema = projectionField->GetEnum();
			const int64_t raw = enumSchema ? EnumRawOf(*enumSchema, projectionField->Get(cameraInstance)) : 0;
			std::vector<std::string> names;
			int selected = 0;
			if (enumSchema)
			{
				for (size_t i = 0; i < enumSchema->Values.size(); ++i)
				{
					// 选项文案可本地化;写回仍按下标取 enumSchema->Values[i].second(raw 枚举值)。
					names.push_back(CameraProjectionLabel(enumSchema->Values[i].first));
					if (enumSchema->Values[i].second == raw)
						selected = static_cast<int>(i);
				}
			}
			Wui::LabelWithTerm(ctx, { projectionRow.X + 4, projectionRow.Y + 3 }, projectionLabel.Text,
				projectionLabel.Term, theme.TextMuted, 13.0f, theme, 136.0f);
			const Wui::WuiRect comboRect { projectionRow.X + 140.0f, projectionRow.Y + 1, projectionRow.W - 144.0f, 20 };
			if (Wui::Combo(ctx, Wui::HashId(projectionId.c_str()), comboRect, "", names, selected, theme))
			{
				projectionField->Set(cameraInstance, Schema::Value(enumSchema->Values[selected].second));
				projection = camera->m_ProjectionType;
				changed = true;
				markField(cameraType + ".ProjectionType");
			}
			RegisterNode(Wui::HashId(projectionId.c_str()), "combo", comboRect, TermText(projectionLabel),
				(selected >= 0 && selected < static_cast<int>(names.size())) ? names[selected] : std::string(),
				reachable(comboRect));
		}
		else
		{
			DrawReadOnlyRow(ctx, projectionId, projectionRow, projectionLabel,
				CameraProjectionLabel(projection == CameraSettings::ProjectionType::Perspective ? "Perspective" : "Orthographic"),
				theme);
		}
		y += kRowHeight;

		// 按当前投影类型只显示对应参数(迁移前语义),直接写 CameraSettings 字段提交;
		// 投影矩阵由 CameraSystem 按参数指纹自动重算,写入者不需要补算。
		if (projection == CameraSettings::ProjectionType::Perspective)
		{
			float fov = camera->m_PerspectiveFOV;
			const Wui::WuiRect fovRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool fovChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.FOV"),
				fovRow, Wui::TrLabel("panel.properties.camera.fov", "FOV"), fov, 1.0f, -1.0f, theme, reachable(fovRow));
			y += kRowHeight;
			float nearClip = camera->m_PerspectiveNearClip;
			const Wui::WuiRect nearRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool nearChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.NearClip"),
				nearRow, Wui::TrLabel("panel.properties.camera.near_clip", "NearClip"), nearClip, 1.0f, -1.0f, theme,
				reachable(nearRow));
			y += kRowHeight;
			float farClip = camera->m_PerspectiveFarClip;
			const Wui::WuiRect farRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool farChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.FarClip"),
				farRow, Wui::TrLabel("panel.properties.camera.far_clip", "FarClip"), farClip, 1.0f, -1.0f, theme,
				reachable(farRow));
			y += kRowHeight;
			if (fovChanged)
			{
				camera->m_PerspectiveFOV = fov;
				changed = true;
				markField(cameraType + ".Perspective.FOV");
			}
			if (nearChanged)
			{
				camera->m_PerspectiveNearClip = nearClip;
				changed = true;
				markField(cameraType + ".Perspective.NearClip");
			}
			if (farChanged)
			{
				camera->m_PerspectiveFarClip = farClip;
				changed = true;
				markField(cameraType + ".Perspective.FarClip");
			}
		}
		else
		{
			float zoom = camera->m_OrthographicZoom;
			const Wui::WuiRect zoomRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool zoomChanged = DrawFloatRow(ctx, PropPath(cameraType, "Orthographic.Zoom"),
				zoomRow, Wui::TrLabel("panel.properties.camera.zoom", "Zoom"), zoom, 1.0f, -1.0f, theme, reachable(zoomRow));
			y += kRowHeight;
			float nearClip = camera->m_OrthographicNearClip;
			const Wui::WuiRect nearRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool nearChanged = DrawFloatRow(ctx, PropPath(cameraType, "Orthographic.NearClip"),
				nearRow, Wui::TrLabel("panel.properties.camera.near_clip", "NearClip"), nearClip, 1.0f, -1.0f, theme,
				reachable(nearRow));
			y += kRowHeight;
			float farClip = camera->m_OrthographicFarClip;
			const Wui::WuiRect farRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool farChanged = DrawFloatRow(ctx, PropPath(cameraType, "Orthographic.FarClip"),
				farRow, Wui::TrLabel("panel.properties.camera.far_clip", "FarClip"), farClip, 1.0f, -1.0f, theme,
				reachable(farRow));
			y += kRowHeight;
			if (zoomChanged)
			{
				camera->m_OrthographicZoom = zoom;
				changed = true;
				markField(cameraType + ".Orthographic.Zoom");
			}
			if (nearChanged)
			{
				camera->m_OrthographicNearClip = nearClip;
				changed = true;
				markField(cameraType + ".Orthographic.NearClip");
			}
			if (farChanged)
			{
				camera->m_OrthographicFarClip = farClip;
				changed = true;
				markField(cameraType + ".Orthographic.FarClip");
			}
		}

		if (changed)
			m_Host.MarkDocumentDirty();
		return y + 2.0f;
	}


float PropertiesPanel::DrawPhysicsMaterialInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	auto* comp = static_cast<World::PhysicsMaterialComponent*>(instance);
	std::string oldPath;
	if (comp && comp->Material.HasPath())
	{
		oldPath = StringPool::Get().PathOf(comp->Material.Path);
	}

	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 4.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	if (comp)
	{
		std::string currentPath;
		if (comp->Material.HasPath())
		{
			currentPath = StringPool::Get().PathOf(comp->Material.Path);
		}

		if (!currentPath.empty() && currentPath != oldPath)
		{
			std::filesystem::path full = currentPath;
			if (!full.is_absolute())
			{
				if (currentPath.rfind("assets/", 0) == 0 || currentPath.rfind("assets\\", 0) == 0)
					full = World::Paths::AssetRoot() / currentPath.substr(7);
				else
					full = World::Paths::AssetRoot() / full;
			}
			PhysicsMaterial mat;
			if (PhysicsMaterial::LoadFromFile(full.string(), mat))
			{
				comp->Friction = mat.Friction;
				comp->Restitution = mat.Restitution;
				comp->FrictionCombine = mat.FrictionCombine;
				comp->RestitutionCombine = mat.RestitutionCombine;
				m_Host.MarkDocumentDirty();
			}
		}

		if (!currentPath.empty())
		{
			const float btnW = 160.0f;
			const Wui::WuiRect editBtnRect { rect.X + 8.0f, y, btnW, 22.0f };
			Wui::Tooltip(ctx, editBtnRect, "在物理材质编辑器中打开此资产进行微调");
			if (Wui::Button(ctx, Wui::HashId("pmat.inspector.open"), editBtnRect, "✏ 打开物理材质编辑器", theme))
			{
				m_Host.OpenPhysicsMaterialEditor(currentPath);
			}

			const Wui::WuiRect syncBtnRect { rect.X + 8.0f + btnW + 6.0f, y, 130.0f, 22.0f };
			Wui::Tooltip(ctx, syncBtnRect, "从磁盘资产文件重新载入摩擦力与弹性数值");
			if (Wui::Button(ctx, Wui::HashId("pmat.inspector.sync"), syncBtnRect, "↻ 从资产同步数值", theme))
			{
				std::filesystem::path full = currentPath;
				if (!full.is_absolute())
				{
					if (currentPath.rfind("assets/", 0) == 0 || currentPath.rfind("assets\\", 0) == 0)
						full = World::Paths::AssetRoot() / currentPath.substr(7);
					else
						full = World::Paths::AssetRoot() / full;
				}
				PhysicsMaterial mat;
			if (PhysicsMaterial::LoadFromFile(full.string(), mat))
			{
				comp->Friction = mat.Friction;
				comp->Restitution = mat.Restitution;
				comp->FrictionCombine = mat.FrictionCombine;
				comp->RestitutionCombine = mat.RestitutionCombine;
				m_Host.MarkDocumentDirty();
			}
			}
			y += 26.0f;
		}
	}

	return (y - rect.Y);
}

float PropertiesPanel::DrawAudioSourceInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* comp = static_cast<World::AudioSourceComponent*>(instance);
	std::string soundPath;
	if (comp && comp->Sound.HasPath())
	{
		soundPath = StringPool::Get().PathOf(comp->Sound.Path);
	}

	bool hasSound = !soundPath.empty();
	std::string soundEventId = soundPath;

	if (hasSound)
	{
		auto& audio = Audio::AudioSystem::Get();
		if (!audio.FindSoundEvent(soundEventId))
		{
			std::filesystem::path p(soundPath);
			std::string stem = p.stem().string();
			if (audio.FindSoundEvent(stem))
			{
				soundEventId = stem;
			}
			else
			{
				std::ifstream in(p);
				if (!in.is_open() && !p.is_absolute())
				{
					std::filesystem::path candidate = std::filesystem::current_path() / "projects" / "default" / p;
					if (std::filesystem::exists(candidate)) in.open(candidate);
				}
				if (in.is_open())
				{
					std::stringstream ss;
					ss << in.rdbuf();
					Audio::SoundEvent ev;
					if (ev.DeserializeYaml(ss.str()))
					{
						audio.RegisterSoundEvent(ev);
						soundEventId = ev.GetId();
					}
				}
			}
		}
	}

	std::string soundDisplay = hasSound ? soundEventId : "(未指定声音)";
	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "绑定声音: " + soundDisplay, hasSound ? theme.Accent : theme.TextMuted, 11.5f);
	y += 22.0f;

	const Wui::WuiRect btnPlay { rect.X + 12.0f, y, 80.0f, 24.0f };
	const Wui::WuiRect btnStop { rect.X + 98.0f, y, 80.0f, 24.0f };

	std::string playTooltip = hasSound ? ("试听声音: " + soundEventId) : "未指定声音，请先绑定声音资产！";

	Wui::Tooltip(ctx, btnPlay, playTooltip);
	if (Wui::Button(ctx, Wui::HashId("prop.audiosrc.play"), btnPlay, "▶ 试听", theme))
	{
		if (hasSound)
		{
			auto& audio = Audio::AudioSystem::Get();
			if (comp->Spatial3D)
			{
				audio.PostEvent(soundEventId, glm::vec3(0.0f), comp->Volume, comp->Pitch);
			}
			else
			{
				audio.PostEvent2D(soundEventId, comp->Volume, comp->Pitch);
			}
		}
	}

	Wui::Tooltip(ctx, btnStop, "停止试听声音");
	if (Wui::Button(ctx, Wui::HashId("prop.audiosrc.stop"), btnStop, "■ 停止", theme))
	{
		Audio::AudioSystem::Get().StopAll(0.05f);
	}

	float noticeY = y + 28.0f;
	if (hasSound)
	{
		std::string statusText = "● 状态: 已就绪" + std::string(comp->Spatial3D ? " [3D 空间声源]" : " [2D 立体声]");
		Wui::Label(ctx, { rect.X + 14.0f, noticeY }, statusText, theme.Success, 10.5f);
	}

	return (noticeY + 20.0f) - rect.Y;
}

float PropertiesPanel::DrawSpriteInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* sprite = static_cast<World::SpriteComponent*>(instance);
	if (!sprite) return height;

	// Pivot Presets
	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "锚点快捷预设 (Pivot Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 90.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnCenter { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnFoot   { rect.X + 108.0f, y, btnW, btnH };
	const Wui::WuiRect btnTopLeft{ rect.X + 204.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnCenter, "设为几何中心 (0.5, 0.5)");
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.pivot_center"), btnCenter, "中心 (0.5, 0.5)", theme))
	{
		sprite->Pivot = { 0.5f, 0.5f };
		if (changedFields) changedFields->push_back("Pivot");
	}

	Wui::Tooltip(ctx, btnFoot, "设为脚底中心 (0.5, 0.0) —— 适用于角色与地面站立");
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.pivot_foot"), btnFoot, "脚底 (0.5, 0.0)", theme))
	{
		sprite->Pivot = { 0.5f, 0.0f };
		if (changedFields) changedFields->push_back("Pivot");
	}

	Wui::Tooltip(ctx, btnTopLeft, "设为左上角 (0.0, 1.0) —— 适用于 UI 或网格瓦片");
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.pivot_topleft"), btnTopLeft, "左上 (0.0, 1.0)", theme))
	{
		sprite->Pivot = { 0.0f, 1.0f };
		if (changedFields) changedFields->push_back("Pivot");
	}
	y += 28.0f;

	// Quick Flip Toggles
	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "图形翻转 (Quick Flip):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const Wui::WuiRect btnFlipX { rect.X + 12.0f, y, 110.0f, btnH };
	const Wui::WuiRect btnFlipY { rect.X + 128.0f, y, 110.0f, btnH };

	std::string flipXText = sprite->FlipX ? "✔ 水平翻转 (X)" : "水平翻转 (X)";
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.flip_x"), btnFlipX, flipXText, theme))
	{
		sprite->FlipX = !sprite->FlipX;
		if (changedFields) changedFields->push_back("FlipX");
	}

	std::string flipYText = sprite->FlipY ? "✔ 垂直翻转 (Y)" : "垂直翻转 (Y)";
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.flip_y"), btnFlipY, flipYText, theme))
	{
		sprite->FlipY = !sprite->FlipY;
		if (changedFields) changedFields->push_back("FlipY");
	}
	y += 28.0f;

	// Quick Sorting Layer Presets
	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "排序层预设 (Sorting Layer):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const Wui::WuiRect btnLyrBg  { rect.X + 12.0f, y, 70.0f, btnH };
	const Wui::WuiRect btnLyrDef { rect.X + 88.0f, y, 70.0f, btnH };
	const Wui::WuiRect btnLyrEnt { rect.X + 164.0f, y, 70.0f, btnH };
	const Wui::WuiRect btnLyrFg  { rect.X + 240.0f, y, 70.0f, btnH };

	if (Wui::Button(ctx, Wui::HashId("prop.sprite.lyr_bg"), btnLyrBg, "背景 (-10)", theme))
	{
		sprite->SortingLayer = -10;
		if (changedFields) changedFields->push_back("SortingLayer");
	}
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.lyr_def"), btnLyrDef, "默认 (0)", theme))
	{
		sprite->SortingLayer = 0;
		if (changedFields) changedFields->push_back("SortingLayer");
	}
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.lyr_ent"), btnLyrEnt, "实体 (5)", theme))
	{
		sprite->SortingLayer = 5;
		if (changedFields) changedFields->push_back("SortingLayer");
	}
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.lyr_fg"), btnLyrFg, "前景 (20)", theme))
	{
		sprite->SortingLayer = 20;
		if (changedFields) changedFields->push_back("SortingLayer");
	}
	y += 28.0f;

	// Normal Map Selector (2D Lighting Relief)
	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "法线贴图 (Normal Map):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const std::vector<std::string>& texPaths = Editor::AssetCatalog::PathsForName("Texture2D");
	std::vector<std::string> normalMapOptions;
	normalMapOptions.reserve(texPaths.size() + 2);
	normalMapOptions.push_back("(None / 无法线贴图)");
	for (const auto& p : texPaths)
		normalMapOptions.push_back(p);

	const std::string currentNormalMap = sprite->NormalMap.IsValid()
		? World::StringPool::Get().NameOf(sprite->NormalMap)
		: std::string();
	int selectedNormalIdx = 0;
	if (!currentNormalMap.empty())
	{
		const auto found = std::find(normalMapOptions.begin() + 1, normalMapOptions.end(), currentNormalMap);
		if (found != normalMapOptions.end())
		{
			selectedNormalIdx = static_cast<int>(found - normalMapOptions.begin());
		}
		else
		{
			normalMapOptions.push_back(currentNormalMap);
			selectedNormalIdx = static_cast<int>(normalMapOptions.size()) - 1;
		}
	}

	const Wui::WuiRect normalComboRect { rect.X + 12.0f, y, std::max(140.0f, rect.W - 24.0f), 22.0f };
	Wui::Tooltip(ctx, normalComboRect, "选择用于 2D 动态光照凹凸受光的法线贴图资产 (prop.sprite.normal_map)");
	const int beforeNormalIdx = selectedNormalIdx;
	if (Wui::SearchableCombo(ctx, Wui::HashId("prop.sprite.normal_map"), normalComboRect, "",
		normalMapOptions, selectedNormalIdx, theme) && selectedNormalIdx != beforeNormalIdx)
	{
		if (selectedNormalIdx <= 0)
		{
			sprite->NormalMap = World::NameId{};
		}
		else
		{
			const std::string& selectedPath = normalMapOptions[static_cast<size_t>(selectedNormalIdx)];
			sprite->NormalMap = World::StringPool::Get().InternName(selectedPath);
		}
		m_Host.MarkDocumentDirty();
		if (changedFields) changedFields->push_back("NormalMap");
	}
	y += 28.0f;

	return (y + 10.0f) - rect.Y;
}

float PropertiesPanel::DrawCapsuleCollider2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* cap = static_cast<World::CapsuleCollider2DComponent*>(instance);
	if (!cap) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "胶囊体方向 (Direction):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 100.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnVert { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnHoriz{ rect.X + 118.0f, y, btnW, btnH };

	std::string vertText = cap->Direction == 0 ? "✔ 垂直 (Vertical)" : "垂直 (Vertical)";
	if (Wui::Button(ctx, Wui::HashId("prop.cap2d.vert"), btnVert, vertText, theme))
	{
		cap->Direction = 0;
		if (changedFields) changedFields->push_back("Direction");
	}

	std::string horizText = cap->Direction == 1 ? "✔ 水平 (Horizontal)" : "水平 (Horizontal)";
	if (Wui::Button(ctx, Wui::HashId("prop.cap2d.horiz"), btnHoriz, horizText, theme))
	{
		cap->Direction = 1;
		if (changedFields) changedFields->push_back("Direction");
	}
	y += 28.0f;

	const bool editingCollider = m_Host.IsEditingCollider();
	const Wui::WuiRect btnEditCol { rect.X + 12.0f, y, std::min(220.0f, rect.W - 24.0f), 24.0f };
	std::string editColText = editingCollider ? "✓ 退出碰撞体编辑 (Done)" : "✎ 视口编辑碰撞体 (Edit Collider)";
	Wui::Tooltip(ctx, btnEditCol, "在场景视口中直接拖动控制手柄交互式编辑碰撞体尺寸与偏移 (prop.cap2d.edit)");
	if (Wui::Button(ctx, Wui::HashId("prop.cap2d.edit"), btnEditCol, editColText, theme))
	{
		m_Host.SetEditingCollider(!editingCollider);
	}
	y += 28.0f;

	return (y + 10.0f) - rect.Y;
}

float PropertiesPanel::DrawBoxCollider2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	const bool editingCollider = m_Host.IsEditingCollider();
	const Wui::WuiRect btnEditCol { rect.X + 12.0f, y, std::min(220.0f, rect.W - 24.0f), 24.0f };
	std::string editColText = editingCollider ? "✓ 退出碰撞体编辑 (Done)" : "✎ 视口编辑碰撞体 (Edit Collider)";
	Wui::Tooltip(ctx, btnEditCol, "在场景视口中直接拖动控制手柄交互式编辑碰撞体尺寸与偏移 (prop.box2d.edit)");
	if (Wui::Button(ctx, Wui::HashId("prop.box2d.edit"), btnEditCol, editColText, theme))
	{
		m_Host.SetEditingCollider(!editingCollider);
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawCircleCollider2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	const bool editingCollider = m_Host.IsEditingCollider();
	const Wui::WuiRect btnEditCol { rect.X + 12.0f, y, std::min(220.0f, rect.W - 24.0f), 24.0f };
	std::string editColText = editingCollider ? "✓ 退出碰撞体编辑 (Done)" : "✎ 视口编辑碰撞体 (Edit Collider)";
	Wui::Tooltip(ctx, btnEditCol, "在场景视口中直接拖动控制手柄交互式编辑碰撞体尺寸与偏移 (prop.circle2d.edit)");
	if (Wui::Button(ctx, Wui::HashId("prop.circle2d.edit"), btnEditCol, editColText, theme))
	{
		m_Host.SetEditingCollider(!editingCollider);
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawSpriteAnimatorInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* anim = static_cast<World::SpriteAnimatorComponent*>(instance);
	if (!anim) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "动画播控 (Playback Controls):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 80.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnPlay { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnReset{ rect.X + 98.0f, y, btnW, btnH };

	std::string playText = anim->Playing ? "❚❚ 暂停" : "▶ 播放";
	if (Wui::Button(ctx, Wui::HashId("prop.anim.play"), btnPlay, playText, theme))
	{
		anim->Playing = !anim->Playing;
		if (anim->Playing) anim->Finished = false;
		if (changedFields) changedFields->push_back("Playing");
	}

	if (Wui::Button(ctx, Wui::HashId("prop.anim.reset"), btnReset, "■ 重置", theme))
	{
		anim->CurrentTime = 0.0f;
		anim->CurrentFrame = 0;
		anim->Finished = false;
		if (changedFields) { changedFields->push_back("CurrentTime"); changedFields->push_back("CurrentFrame"); }
	}
	y += 28.0f;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "循环模式 (Loop Mode):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const Wui::WuiRect btnLoop { rect.X + 12.0f, y, 70.0f, btnH };
	const Wui::WuiRect btnOnce { rect.X + 88.0f, y, 70.0f, btnH };
	const Wui::WuiRect btnPing { rect.X + 164.0f, y, 80.0f, btnH };

	std::string loopStr = anim->LoopMode == 0 ? "✔ 循环" : "循环";
	if (Wui::Button(ctx, Wui::HashId("prop.anim.mode_loop"), btnLoop, loopStr, theme))
	{
		anim->LoopMode = 0;
		if (changedFields) changedFields->push_back("LoopMode");
	}

	std::string onceStr = anim->LoopMode == 1 ? "✔ 单次" : "单次";
	if (Wui::Button(ctx, Wui::HashId("prop.anim.mode_once"), btnOnce, onceStr, theme))
	{
		anim->LoopMode = 1;
		if (changedFields) changedFields->push_back("LoopMode");
	}

	std::string pingStr = anim->LoopMode == 2 ? "✔ 往返" : "往返";
	if (Wui::Button(ctx, Wui::HashId("prop.anim.mode_ping"), btnPing, pingStr, theme))
	{
		anim->LoopMode = 2;
		if (changedFields) changedFields->push_back("LoopMode");
	}
	y += 28.0f;

	std::string statusMsg = anim->Playing ? ("● 播放中 | 进度: " + std::to_string(anim->CurrentTime).substr(0, 4) + "s | 帧 " + std::to_string(anim->CurrentFrame))
		: (anim->Finished ? "■ 播放已结束" : "❚❚ 已暂停");
	Wui::Label(ctx, { rect.X + 14.0f, y + 2.0f }, statusMsg, anim->Playing ? theme.Accent : theme.TextMuted, 10.5f);
	y += 20.0f;

	return y - rect.Y;
}

float PropertiesPanel::DrawFreeformLight2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* light = static_cast<World::FreeformLight2DComponent*>(instance);
	if (!light) return height;

	auto pts = light->GetLocalPolygon();
	std::string summary = "2D 自由多边形光区 (Freeform Shape · " + std::to_string(pts.size()) + " 个顶点):";
	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, summary, theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 96.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnTrapezoid { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnCone      { rect.X + 114.0f, y, btnW, btnH };
	const Wui::WuiRect btnDiamond   { rect.X + 216.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnTrapezoid, "梯形光区预设: -1,-0.7;1,-0.7;0.6,0.8;-0.6,0.8 (prop.freeform.preset_trapezoid)");
	if (Wui::Button(ctx, Wui::HashId("prop.freeform.preset_trapezoid"), btnTrapezoid, "📐 梯形光区", theme))
	{
		light->Points = StringPool::Get().InternName("-1,-0.7;1,-0.7;0.6,0.8;-0.6,0.8");
		m_Host.MarkDocumentDirty();
		if (changedFields) changedFields->push_back("Points");
	}

	Wui::Tooltip(ctx, btnCone, "射灯光锥预设: 0,-1;1.2,1;-1.2,1 (prop.freeform.preset_cone)");
	if (Wui::Button(ctx, Wui::HashId("prop.freeform.preset_cone"), btnCone, "📐 射灯光锥", theme))
	{
		light->Points = StringPool::Get().InternName("0,-1;1.2,1;-1.2,1");
		m_Host.MarkDocumentDirty();
		if (changedFields) changedFields->push_back("Points");
	}

	Wui::Tooltip(ctx, btnDiamond, "菱形天窗预设: 0,-1;1,0;0,1;-1,0 (prop.freeform.preset_diamond)");
	if (Wui::Button(ctx, Wui::HashId("prop.freeform.preset_diamond"), btnDiamond, "📐 菱形天窗", theme))
	{
		light->Points = StringPool::Get().InternName("0,-1;1,0;0,1;-1,0");
		m_Host.MarkDocumentDirty();
		if (changedFields) changedFields->push_back("Points");
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawTilemapInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* tmComp = static_cast<World::TilemapComponent*>(instance);
	if (!tmComp) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "网格拓扑形态 (Grid Topology):", theme.TextMuted, 11.0f);
	y += 20.0f;

	const float topoBtnW = 64.0f;
	const float topoBtnH = 22.0f;
	struct TopoPreset { const char* id; const char* label; uint8_t type; float cw; float ch; };
	const TopoPreset topos[4] = {
		{ "prop.tm.topo_ortho", "正交直角", 0, 1.0f, 1.0f },
		{ "prop.tm.topo_iso",   "等轴2.5D", 1, 1.5f, 0.75f },
		{ "prop.tm.topo_hexp",  "尖顶六边", 2, 1.0f, 1.0f },
		{ "prop.tm.topo_hexf",  "平顶六边", 3, 1.0f, 1.0f }
	};
	for (int i = 0; i < 4; ++i)
	{
		const Wui::WuiRect bRect { rect.X + 12.0f + i * (topoBtnW + 4.0f), y, topoBtnW, topoBtnH };
		auto btnTheme = theme;
		if (tmComp->GridType == topos[i].type)
		{
			btnTheme.ButtonBg = theme.ActiveBg;
			btnTheme.Border = theme.Accent;
		}
		if (Wui::Button(ctx, Wui::HashId(topos[i].id), bRect, topos[i].label, btnTheme))
		{
			tmComp->GridType = topos[i].type;
			tmComp->CellSize = { topos[i].cw, topos[i].ch };
			tmComp->ShowGrid = true;
			if (changedFields)
			{
				changedFields->push_back("GridType");
				changedFields->push_back("CellSize");
				changedFields->push_back("ShowGrid");
			}
		}
	}
	y += 28.0f;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "碰撞生成模式 (Collider Mode):", theme.TextMuted, 11.0f);
	y += 20.0f;

	const float colBtnW = 86.0f;
	const float colBtnH = 22.0f;
	struct ColPreset { const char* id; const char* label; const char* tip; uint8_t type; };
	const ColPreset colliders[3] = {
		{ "prop.tm.col_merged", "贪心矩形", "0: MergedBoxes 贪心合并为若干大矩形碰撞盒 (prop.tm.col_merged)", 0 },
		{ "prop.tm.col_chain",  "边缘链",   "1: CompositeChain 连续平滑多边形边缘链碰撞体 (prop.tm.col_chain)", 1 },
		{ "prop.tm.col_none",   "无碰撞",   "2: None 不为此 Tilemap 生成碰撞体 (prop.tm.col_none)", 2 }
	};
	for (int i = 0; i < 3; ++i)
	{
		const Wui::WuiRect bRect { rect.X + 12.0f + i * (colBtnW + 4.0f), y, colBtnW, colBtnH };
		auto btnTheme = theme;
		if (tmComp->ColliderType == colliders[i].type)
		{
			btnTheme.ButtonBg = theme.ActiveBg;
			btnTheme.Border = theme.Accent;
		}
		Wui::Tooltip(ctx, bRect, colliders[i].tip);
		if (Wui::Button(ctx, Wui::HashId(colliders[i].id), bRect, colliders[i].label, btnTheme))
		{
			tmComp->ColliderType = colliders[i].type;
			tmComp->GenerateCollision = (colliders[i].type != 2);
			m_Host.MarkDocumentDirty();
			if (changedFields)
			{
				changedFields->push_back("ColliderType");
				changedFields->push_back("GenerateCollision");
			}
		}
	}
	y += 28.0f;

	const std::string tmPath = tmComp->Tilemap.HasPath() ? StringPool::Get().PathOf(tmComp->Tilemap.Path) : "";

	const Wui::WuiRect btnOpen { rect.X + 12.0f, y, std::min(220.0f, rect.W - 24.0f), 24.0f };
	Wui::Tooltip(ctx, btnOpen, tmPath.empty() ? "请先绑定 .wtilemap 瓦片地图资产" : ("在瓦片地图工作台中打开: " + tmPath + " (prop.tilemap.open)"));
	if (Wui::Button(ctx, Wui::HashId("prop.tilemap.open"), btnOpen, "🗺 打开瓦片地图编辑器", theme))
	{
		if (!tmPath.empty())
			m_Host.OpenTilemapEditor(tmPath);
	}
	y += 28.0f;

	if (!tmPath.empty())
	{
		if (auto tm = Tilemap::GetOrLoad(tmPath))
		{
			const size_t mergedCount = tm->ComputeMergedColliderRects().size();
			std::string colModeStr = tmComp->ColliderType == 0
				? ("贪心矩形 (" + std::to_string(mergedCount) + " 个)")
				: (tmComp->ColliderType == 1 ? "平滑边缘链" : "无");
			std::string summary = "网格: " + std::to_string(tm->GetWidth()) + "x" + std::to_string(tm->GetHeight())
				+ " | 已绘瓦片: " + std::to_string(tm->CountPaintedTiles())
				+ " | 碰撞模式: " + colModeStr;
			Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, summary, theme.Accent, 10.5f);
			y += 20.0f;
		}
	}

	return y - rect.Y;
}




float PropertiesPanel::DrawPixelPerfectCameraInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* ppc = static_cast<World::PixelPerfectCameraComponent*>(instance);
	if (!ppc) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "常用复古像素分辨率预设:", theme.TextMuted, 11.0f);
	y += 20.0f;

	const float btnW = 64.0f;
	const float btnH = 22.0f;
	struct ResPreset { const char* id; const char* label; int w; int h; };
	const ResPreset presets[4] = {
		{ "prop.ppc.320", "320×180", 320, 180 },
		{ "prop.ppc.640", "640×360", 640, 360 },
		{ "prop.ppc.256", "256×240", 256, 240 },
		{ "prop.ppc.160", "160×144", 160, 144 }
	};
	for (int i = 0; i < 4; ++i)
	{
		const Wui::WuiRect bRect { rect.X + 12.0f + static_cast<float>(i) * (btnW + 4.0f), y, btnW, btnH };
		if (Wui::Button(ctx, Wui::HashId(presets[i].id), bRect, presets[i].label, theme))
		{
			ppc->ReferenceResolution = { presets[i].w, presets[i].h };
			if (changedFields) changedFields->push_back("ReferenceResolution");
		}
	}
	y += 28.0f;

	std::string status = "当前整数倍缩放: " + std::to_string(ppc->CurrentScale) + "x | PPU: "
		+ std::to_string(ppc->PixelsPerUnit) + " px/m | 像素吸附: " + (ppc->SnapToPixelGrid ? "开启" : "关闭");
	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, status, theme.Accent, 10.5f);
	y += 20.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawParallaxLayerInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* plx = static_cast<World::ParallaxLayerComponent*>(instance);
	if (!plx) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "视差景深快速预设 (ScrollFactor):", theme.TextMuted, 11.0f);
	y += 20.0f;

	const float btnW = 68.0f;
	const float btnH = 22.0f;
	struct PlxPreset { const char* id; const char* label; float fx; float fy; };
	const PlxPreset presets[4] = {
		{ "prop.plx.sky", "天空 (0.0)", 0.0f, 0.0f },
		{ "prop.plx.far", "远景 (0.2)", 0.2f, 0.1f },
		{ "prop.plx.mid", "中景 (0.5)", 0.5f, 0.3f },
		{ "prop.plx.fg",  "近景 (1.2)", 1.2f, 1.0f }
	};
	for (int i = 0; i < 4; ++i)
	{
		const Wui::WuiRect bRect { rect.X + 12.0f + static_cast<float>(i) * (btnW + 4.0f), y, btnW, btnH };
		if (Wui::Button(ctx, Wui::HashId(presets[i].id), bRect, presets[i].label, theme))
		{
			plx->ScrollFactor = { presets[i].fx, presets[i].fy };
			if (changedFields) changedFields->push_back("ScrollFactor");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawPolygonCollider2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* poly = static_cast<World::PolygonCollider2DComponent*>(instance);
	if (!poly) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "多边形预设 (Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 60.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnTri   { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnQuad  { rect.X + 76.0f, y, btnW, btnH };
	const Wui::WuiRect btnPenta { rect.X + 140.0f, y, btnW, btnH };
	const Wui::WuiRect btnStar  { rect.X + 204.0f, y, btnW, btnH };

	if (Wui::Button(ctx, Wui::HashId("prop.poly2d.preset_tri"), btnTri, "三角形", theme))
	{
		poly->SetPoints({ { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.0f, 0.5f } });
		if (changedFields) changedFields->push_back("Points");
	}
	if (Wui::Button(ctx, Wui::HashId("prop.poly2d.preset_quad"), btnQuad, "矩形", theme))
	{
		poly->SetPoints({ { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } });
		if (changedFields) changedFields->push_back("Points");
	}
	if (Wui::Button(ctx, Wui::HashId("prop.poly2d.preset_penta"), btnPenta, "五边形", theme))
	{
		std::vector<glm::vec2> pts;
		for (int i = 0; i < 5; ++i)
		{
			float angle = glm::radians(90.0f + i * 72.0f);
			pts.push_back({ 0.5f * std::cos(angle), 0.5f * std::sin(angle) });
		}
		poly->SetPoints(pts);
		if (changedFields) changedFields->push_back("Points");
	}
	if (Wui::Button(ctx, Wui::HashId("prop.poly2d.preset_star"), btnStar, "五角星", theme))
	{
		std::vector<glm::vec2> pts;
		for (int i = 0; i < 10; ++i)
		{
			float angle = glm::radians(90.0f + i * 36.0f);
			float r = (i % 2 == 0) ? 0.6f : 0.25f;
			pts.push_back({ r * std::cos(angle), r * std::sin(angle) });
		}
		poly->SetPoints(pts);
		if (changedFields) changedFields->push_back("Points");
	}
	y += 28.0f;

	const bool editingCollider = m_Host.IsEditingCollider();
	const Wui::WuiRect btnEditCol { rect.X + 12.0f, y, std::min(220.0f, rect.W - 24.0f), 24.0f };
	std::string editColText = editingCollider ? "✓ 退出碰撞体编辑 (Done)" : "✎ 视口编辑多边形 (Edit Polygon)";
	Wui::Tooltip(ctx, btnEditCol, "在场景视口中直接拖动控制手柄交互式编辑多边形顶点 (prop.poly2d.edit)");
	if (Wui::Button(ctx, Wui::HashId("prop.poly2d.edit"), btnEditCol, editColText, theme))
	{
		m_Host.SetEditingCollider(!editingCollider);
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawChainCollider2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* chain = static_cast<World::ChainCollider2DComponent*>(instance);
	if (!chain) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "地表轮廓预设 (Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 60.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnFlat   { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnSlope  { rect.X + 76.0f, y, btnW, btnH };
	const Wui::WuiRect btnValley { rect.X + 140.0f, y, btnW, btnH };
	const Wui::WuiRect btnWave   { rect.X + 204.0f, y, btnW, btnH };

	if (Wui::Button(ctx, Wui::HashId("prop.chain2d.preset_flat"), btnFlat, "平坦地", theme))
	{
		chain->SetPoints({ { -3.0f, 0.0f }, { -1.0f, 0.0f }, { 1.0f, 0.0f }, { 3.0f, 0.0f } });
		chain->Loop = false;
		if (changedFields) { changedFields->push_back("Points"); changedFields->push_back("Loop"); }
	}
	if (Wui::Button(ctx, Wui::HashId("prop.chain2d.preset_slope"), btnSlope, "斜坡", theme))
	{
		chain->SetPoints({ { -3.0f, -1.0f }, { -1.0f, 0.0f }, { 1.0f, 1.0f }, { 3.0f, 2.0f } });
		chain->Loop = false;
		if (changedFields) { changedFields->push_back("Points"); changedFields->push_back("Loop"); }
	}
	if (Wui::Button(ctx, Wui::HashId("prop.chain2d.preset_valley"), btnValley, "U形谷", theme))
	{
		chain->SetPoints({ { -3.0f, 2.0f }, { -1.5f, 0.0f }, { 1.5f, 0.0f }, { 3.0f, 2.0f } });
		chain->Loop = false;
		if (changedFields) { changedFields->push_back("Points"); changedFields->push_back("Loop"); }
	}
	if (Wui::Button(ctx, Wui::HashId("prop.chain2d.preset_wave"), btnWave, "波浪", theme))
	{
		chain->SetPoints({ { -3.0f, 0.0f }, { -1.5f, 1.0f }, { 0.0f, 0.0f }, { 1.5f, -1.0f }, { 3.0f, 0.0f } });
		chain->Loop = false;
		if (changedFields) { changedFields->push_back("Points"); changedFields->push_back("Loop"); }
	}
	y += 28.0f;

	const bool editingCollider = m_Host.IsEditingCollider();
	const Wui::WuiRect btnEditCol { rect.X + 12.0f, y, std::min(220.0f, rect.W - 24.0f), 24.0f };
	std::string editColText = editingCollider ? "✓ 退出碰撞体编辑 (Done)" : "✎ 视口编辑折线链 (Edit Chain)";
	Wui::Tooltip(ctx, btnEditCol, "在场景视口中直接拖动控制手柄交互式编辑折线链顶点 (prop.chain2d.edit)");
	if (Wui::Button(ctx, Wui::HashId("prop.chain2d.edit"), btnEditCol, editColText, theme))
	{
		m_Host.SetEditingCollider(!editingCollider);
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawCompositeCollider2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* comp = static_cast<World::CompositeCollider2DComponent*>(instance);
	if (!comp) return height;

	// 1. GeometryType 切换按钮
	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "几何合并模式 (Geometry Type):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 120.0f;
	const float btnH = 24.0f;
	const Wui::WuiRect btnPoly  { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnChain { rect.X + 138.0f, y, btnW, btnH };

	const std::string polyText = (comp->GeometryType == 0) ? "● 多边形 (Polygons)" : "  多边形 (Polygons)";
	const std::string chainText = (comp->GeometryType == 1) ? "● 轮廓链 (OutlineChain)" : "  轮廓链 (OutlineChain)";

	Wui::Tooltip(ctx, btnPoly, "合并为多边形实体并集 (Box2D 多边形碰撞体)");
	if (Wui::Button(ctx, Wui::HashId("prop.comp2d.geom_poly"), btnPoly, polyText, theme))
	{
		comp->GeometryType = 0;
		if (changedFields) changedFields->push_back("GeometryType");
		m_Host.MarkDocumentDirty();
	}

	Wui::Tooltip(ctx, btnChain, "合并为外部闭合轮廓链 (Box2D 链式线段碰撞体)");
	if (Wui::Button(ctx, Wui::HashId("prop.comp2d.geom_chain"), btnChain, chainText, theme))
	{
		comp->GeometryType = 1;
		if (changedFields) changedFields->push_back("GeometryType");
		m_Host.MarkDocumentDirty();
	}
	y += 30.0f;

	// 2. 统计当前实体及其子实体中收集的子碰撞体数量
	int boxCount = 0;
	int circleCount = 0;
	int capsuleCount = 0;
	int polyCount = 0;
	int totalColliders = 0;

	if (entity.IsValid() && entity.GetScene())
	{
		const entt::registry& reg = static_cast<const Scene*>(entity.GetScene())->GetRegistry();
		std::unordered_set<entt::entity> visited;
		std::vector<entt::entity> stack;
		stack.push_back(static_cast<entt::entity>(entity));

		while (!stack.empty())
		{
			entt::entity curr = stack.back();
			stack.pop_back();

			if (!reg.valid(curr) || visited.count(curr))
				continue;
			visited.insert(curr);

			if (reg.all_of<BoxCollider2DComponent>(curr)) { ++boxCount; ++totalColliders; }
			if (reg.all_of<CircleCollider2DComponent>(curr)) { ++circleCount; ++totalColliders; }
			if (reg.all_of<CapsuleCollider2DComponent>(curr)) { ++capsuleCount; ++totalColliders; }
			if (reg.all_of<PolygonCollider2DComponent>(curr)) { ++polyCount; ++totalColliders; }

			const auto& children = Hierarchy::ChildrenOf(reg, curr);
			for (entt::entity child : children)
			{
				if (reg.valid(child))
					stack.push_back(child);
			}
		}
	}

	// 3. 绘制统计提示卡片
	const float cardW = std::max(200.0f, rect.W - 24.0f);
	const float cardH = (totalColliders > 0) ? 52.0f : 44.0f;
	const Wui::WuiRect cardRect { rect.X + 12.0f, y, cardW, cardH };
	Wui::PanelBackground(ctx, cardRect, theme.PanelBg, 4.0f);
	Wui::HighlightOutline(ctx, cardRect, (totalColliders > 0) ? theme.Success : theme.Border, 4.0f, 1.0f);

	if (totalColliders > 0)
	{
		std::string summary = "✓ 已聚合 " + std::to_string(totalColliders) + " 个子碰撞体 (Box: " +
			std::to_string(boxCount) + ", Poly: " + std::to_string(polyCount) +
			", Circle: " + std::to_string(circleCount) + ", Cap: " + std::to_string(capsuleCount) + ")";
		Wui::Label(ctx, { cardRect.X + 10.0f, cardRect.Y + 6.0f }, summary, theme.Success, 11.5f);

		std::string geomNote = (comp->GeometryType == 0)
			? "当前合成目标: 多边形实体 (Polygons 模式)"
			: "当前合成目标: 闭合外轮廓 (OutlineChain 模式)";
		Wui::Label(ctx, { cardRect.X + 10.0f, cardRect.Y + 28.0f }, geomNote, theme.TextMuted, 10.5f);
	}
	else
	{
		Wui::Label(ctx, { cardRect.X + 10.0f, cardRect.Y + 6.0f }, "ℹ 暂未聚合到子碰撞体", theme.Warning, 11.5f);
		Wui::Label(ctx, { cardRect.X + 10.0f, cardRect.Y + 24.0f }, "提示: 在此实体或其子级添加 Box/Polygon 碰撞体以自动合并", theme.TextMuted, 10.5f);
	}
	y += cardH + 8.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawPlatformEffector2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* effector = static_cast<World::PlatformEffector2DComponent*>(instance);
	if (!effector) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "单向跳板预设 (Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 96.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btn180 { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btn90  { rect.X + 114.0f, y, btnW, btnH };

	if (Wui::Button(ctx, Wui::HashId("prop.plat2d.preset_180"), btn180, "顶面半球 180°", theme))
	{
		effector->SurfaceArc = 180.0f;
		effector->RotationalOffset = 0.0f;
		effector->OneWay = true;
		if (changedFields) { changedFields->push_back("SurfaceArc"); changedFields->push_back("RotationalOffset"); changedFields->push_back("OneWay"); }
	}
	if (Wui::Button(ctx, Wui::HashId("prop.plat2d.preset_90"), btn90, "垂直顶面 90°", theme))
	{
		effector->SurfaceArc = 90.0f;
		effector->RotationalOffset = 0.0f;
		effector->OneWay = true;
		if (changedFields) { changedFields->push_back("SurfaceArc"); changedFields->push_back("RotationalOffset"); changedFields->push_back("OneWay"); }
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawPointLight2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* light = static_cast<World::PointLight2DComponent*>(instance);
	if (!light) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "2D 点光源快捷预设 (Light Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 84.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnTorch   { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnCrystal { rect.X + 102.0f, y, btnW, btnH };
	const Wui::WuiRect btnSpot    { rect.X + 192.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnTorch, "暖橙色全向火把光 (Intensity=1.8, OuterRadius=5.5m, 360°)");
	if (Wui::Button(ctx, Wui::HashId("prop.light2d.preset_torch"), btnTorch, "火把暖光", theme))
	{
		light->Color = { 1.0f, 0.62f, 0.22f };
		light->Intensity = 1.8f;
		light->InnerRadius = 0.4f;
		light->OuterRadius = 5.5f;
		light->InnerAngle = 360.0f;
		light->OuterAngle = 360.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Color");
			changedFields->push_back("Intensity");
			changedFields->push_back("InnerRadius");
			changedFields->push_back("OuterRadius");
			changedFields->push_back("InnerAngle");
			changedFields->push_back("OuterAngle");
		}
	}

	Wui::Tooltip(ctx, btnCrystal, "冰蓝色全向魔法晶石光 (Intensity=1.5, OuterRadius=4.5m, 360°)");
	if (Wui::Button(ctx, Wui::HashId("prop.light2d.preset_crystal"), btnCrystal, "魔法晶石", theme))
	{
		light->Color = { 0.25f, 0.82f, 1.0f };
		light->Intensity = 1.5f;
		light->InnerRadius = 0.3f;
		light->OuterRadius = 4.5f;
		light->InnerAngle = 360.0f;
		light->OuterAngle = 360.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Color");
			changedFields->push_back("Intensity");
			changedFields->push_back("InnerRadius");
			changedFields->push_back("OuterRadius");
			changedFields->push_back("InnerAngle");
			changedFields->push_back("OuterAngle");
		}
	}

	Wui::Tooltip(ctx, btnSpot, "高亮度聚光锥形手电筒 (Intensity=2.0, OuterRadius=8.0m, 32°~65°)");
	if (Wui::Button(ctx, Wui::HashId("prop.light2d.preset_spot"), btnSpot, "锥形手电", theme))
	{
		light->Color = { 0.95f, 0.96f, 0.85f };
		light->Intensity = 2.0f;
		light->InnerRadius = 0.2f;
		light->OuterRadius = 8.0f;
		light->InnerAngle = 32.0f;
		light->OuterAngle = 65.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Color");
			changedFields->push_back("Intensity");
			changedFields->push_back("InnerRadius");
			changedFields->push_back("OuterRadius");
			changedFields->push_back("InnerAngle");
			changedFields->push_back("OuterAngle");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawGlobalLight2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* globalLight = static_cast<World::GlobalLight2DComponent*>(instance);
	if (!globalLight) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "全局氛围光预设 (Atmosphere Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 64.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnDay     { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnDusk    { rect.X + 80.0f, y, btnW, btnH };
	const Wui::WuiRect btnDungeon { rect.X + 148.0f, y, btnW, btnH };
	const Wui::WuiRect btnNight   { rect.X + 216.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnDay, "晴朗白昼环境光 (Intensity=1.0, DirectionIntensity=0.15)");
	if (Wui::Button(ctx, Wui::HashId("prop.glight2d.preset_day"), btnDay, "晴朗白昼", theme))
	{
		globalLight->Color = { 1.0f, 1.0f, 1.0f };
		globalLight->Intensity = 1.0f;
		globalLight->DirectionIntensity = 0.15f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Color");
			changedFields->push_back("Intensity");
			changedFields->push_back("DirectionIntensity");
		}
	}

	Wui::Tooltip(ctx, btnDusk, "温暖黄昏斜射夕照 (Intensity=0.8, Direction=(-0.6, -0.8), DirectionIntensity=0.45)");
	if (Wui::Button(ctx, Wui::HashId("prop.glight2d.preset_dusk"), btnDusk, "黄昏夕照", theme))
	{
		globalLight->Color = { 1.0f, 0.72f, 0.48f };
		globalLight->Intensity = 0.8f;
		globalLight->Direction = { -0.6f, -0.8f };
		globalLight->DirectionIntensity = 0.45f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Color");
			changedFields->push_back("Intensity");
			changedFields->push_back("Direction");
			changedFields->push_back("DirectionIntensity");
		}
	}

	Wui::Tooltip(ctx, btnDungeon, "冷暗地牢氛围底光 (Intensity=0.45, DirectionIntensity=0.35)");
	if (Wui::Button(ctx, Wui::HashId("prop.glight2d.preset_dungeon"), btnDungeon, "幽暗地牢", theme))
	{
		globalLight->Color = { 0.22f, 0.25f, 0.35f };
		globalLight->Intensity = 0.45f;
		globalLight->DirectionIntensity = 0.35f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Color");
			changedFields->push_back("Intensity");
			changedFields->push_back("DirectionIntensity");
		}
	}

	Wui::Tooltip(ctx, btnNight, "静谧蓝调月夜环境光 (Intensity=0.55, Direction=(0.3, -0.95), DirectionIntensity=0.4)");
	if (Wui::Button(ctx, Wui::HashId("prop.glight2d.preset_night"), btnNight, "静谧月夜", theme))
	{
		globalLight->Color = { 0.35f, 0.48f, 0.75f };
		globalLight->Intensity = 0.55f;
		globalLight->Direction = { 0.3f, -0.95f };
		globalLight->DirectionIntensity = 0.4f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Color");
			changedFields->push_back("Intensity");
			changedFields->push_back("Direction");
			changedFields->push_back("DirectionIntensity");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawShadowCaster2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* caster = static_cast<World::ShadowCaster2DComponent*>(instance);
	if (!caster) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "遮光轮廓形态切换 (Hull Shape):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnH = 22.0f;
	const Wui::WuiRect btnShapeBox  { rect.X + 12.0f, y, 108.0f, btnH };
	const Wui::WuiRect btnShapePoly { rect.X + 126.0f, y, 124.0f, btnH };

	const std::string boxText = (caster->ShapeType == 0) ? "✔ 矩形 Box" : "矩形 Box";
	Wui::Tooltip(ctx, btnShapeBox, "使用 Size 宽高定义的矩形遮光轮廓 (ShapeType=0)");
	if (Wui::Button(ctx, Wui::HashId("prop.shadow2d.shape_box"), btnShapeBox, boxText, theme))
	{
		caster->ShapeType = 0;
		m_Host.MarkDocumentDirty();
		if (changedFields) changedFields->push_back("ShapeType");
	}

	const std::string polyText = (caster->ShapeType == 1) ? "✔ 多边形 Polygon" : "多边形 Polygon";
	Wui::Tooltip(ctx, btnShapePoly, "使用 Points 顶点列表定义的多边形遮光轮廓 (ShapeType=1)");
	if (Wui::Button(ctx, Wui::HashId("prop.shadow2d.shape_poly"), btnShapePoly, polyText, theme))
	{
		caster->ShapeType = 1;
		if (!caster->Points.IsValid())
		{
			caster->SetPoints({ { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } });
			if (changedFields) changedFields->push_back("Points");
		}
		m_Host.MarkDocumentDirty();
		if (changedFields) changedFields->push_back("ShapeType");
	}
	y += 28.0f;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "多边形轮廓预设 (Polygon Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float presetW = 76.0f;
	const Wui::WuiRect btnSquare  { rect.X + 12.0f, y, presetW, btnH };
	const Wui::WuiRect btnOctagon { rect.X + 94.0f, y, presetW, btnH };
	const Wui::WuiRect btnLWall   { rect.X + 176.0f, y, presetW, btnH };

	Wui::Tooltip(ctx, btnSquare, "生成 1×1 标准正方形遮光轮廓并切换为多边形模式");
	if (Wui::Button(ctx, Wui::HashId("prop.shadow2d.preset_square"), btnSquare, "正方形", theme))
	{
		caster->ShapeType = 1;
		caster->SetPoints({ { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } });
		m_Host.MarkDocumentDirty();
		if (changedFields) { changedFields->push_back("ShapeType"); changedFields->push_back("Points"); }
	}

	Wui::Tooltip(ctx, btnOctagon, "生成 8 顶点八角柱遮光轮廓并切换为多边形模式");
	if (Wui::Button(ctx, Wui::HashId("prop.shadow2d.preset_octagon"), btnOctagon, "八角柱", theme))
	{
		std::vector<glm::vec2> pts;
		pts.reserve(8);
		for (int i = 0; i < 8; ++i)
		{
			float angle = glm::radians(22.5f + static_cast<float>(i) * 45.0f);
			pts.push_back({ 0.5f * std::cos(angle), 0.5f * std::sin(angle) });
		}
		caster->ShapeType = 1;
		caster->SetPoints(pts);
		m_Host.MarkDocumentDirty();
		if (changedFields) { changedFields->push_back("ShapeType"); changedFields->push_back("Points"); }
	}

	Wui::Tooltip(ctx, btnLWall, "生成 6 顶点 L 形转角墙体遮光轮廓并切换为多边形模式");
	if (Wui::Button(ctx, Wui::HashId("prop.shadow2d.preset_lwall"), btnLWall, "L形墙体", theme))
	{
		caster->ShapeType = 1;
		caster->SetPoints({
			{ -0.5f, -0.5f },
			{  0.5f, -0.5f },
			{  0.5f, -0.15f },
			{ -0.15f, -0.15f },
			{ -0.15f,  0.5f },
			{ -0.5f,  0.5f }
		});
		m_Host.MarkDocumentDirty();
		if (changedFields) { changedFields->push_back("ShapeType"); changedFields->push_back("Points"); }
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawDistanceJoint2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* joint = static_cast<World::DistanceJoint2DComponent*>(instance);
	if (!joint) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "距离关节预设 (Distance Joint Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float gap = 6.0f;
	const float btnH = 22.0f;
	const float availW = std::max(270.0f, rect.W - 24.0f);
	const float btnW = (availW - gap * 2.0f) / 3.0f;
	const Wui::WuiRect btnRigid  { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnSpring { rect.X + 12.0f + btnW + gap, y, btnW, btnH };
	const Wui::WuiRect btnRope   { rect.X + 12.0f + (btnW + gap) * 2.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnRigid, "刚性硬杆: 刚性连接，无弹性 (EnableSpring=false, Hertz=0.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.dist2d.preset_rigid"), btnRigid, "钢制硬杆 (Rigid)", theme))
	{
		joint->EnableSpring = false;
		joint->Hertz = 0.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableSpring");
			changedFields->push_back("Hertz");
		}
	}

	Wui::Tooltip(ctx, btnSpring, "弹性弹簧: 5Hz 频率弹性阻尼连接 (EnableSpring=true, Hertz=5.0, DampingRatio=0.7)");
	if (Wui::Button(ctx, Wui::HashId("prop.dist2d.preset_spring"), btnSpring, "弹性弹簧 (Spring 5Hz)", theme))
	{
		joint->EnableSpring = true;
		joint->Hertz = 5.0f;
		joint->DampingRatio = 0.7f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableSpring");
			changedFields->push_back("Hertz");
			changedFields->push_back("DampingRatio");
		}
	}

	Wui::Tooltip(ctx, btnRope, "柔性绳索: 允许在 0~3m 范围内活动 (EnableLimit=true, MinDistance=0.0, MaxDistance=3.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.dist2d.preset_rope"), btnRope, "柔性绳索 (Limit)", theme))
	{
		joint->EnableLimit = true;
		joint->MinDistance = 0.0f;
		joint->MaxDistance = 3.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableLimit");
			changedFields->push_back("MinDistance");
			changedFields->push_back("MaxDistance");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawRevoluteJoint2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* joint = static_cast<World::RevoluteJoint2DComponent*>(instance);
	if (!joint) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "销轴铰链预设 (Revolute Joint Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float gap = 6.0f;
	const float btnH = 22.0f;
	const float availW = std::max(270.0f, rect.W - 24.0f);
	const float btnW = (availW - gap * 2.0f) / 3.0f;
	const Wui::WuiRect btnFree   { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnPend   { rect.X + 12.0f + btnW + gap, y, btnW, btnH };
	const Wui::WuiRect btnMotor  { rect.X + 12.0f + (btnW + gap) * 2.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnFree, "自由销轴: 360° 无约束自由旋转 (EnableLimit=false, EnableMotor=false)");
	if (Wui::Button(ctx, Wui::HashId("prop.revolute2d.preset_free"), btnFree, "自由销轴 360°", theme))
	{
		joint->EnableLimit = false;
		joint->EnableMotor = false;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableLimit");
			changedFields->push_back("EnableMotor");
		}
	}

	Wui::Tooltip(ctx, btnPend, "钟摆限位: 限制在 -45°~+45° 摆动 (EnableLimit=true, LowerAngle=-45.0, UpperAngle=45.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.revolute2d.preset_pendulum"), btnPend, "钟摆限位 ±45°", theme))
	{
		joint->EnableLimit = true;
		joint->LowerAngle = -45.0f;
		joint->UpperAngle = 45.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableLimit");
			changedFields->push_back("LowerAngle");
			changedFields->push_back("UpperAngle");
		}
	}

	Wui::Tooltip(ctx, btnMotor, "旋转电机: 180°/s 驱动电机 (EnableMotor=true, MotorSpeed=180.0, MaxMotorTorque=25.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.revolute2d.preset_motor"), btnMotor, "旋转电机 (180°/s)", theme))
	{
		joint->EnableMotor = true;
		joint->MotorSpeed = 180.0f;
		joint->MaxMotorTorque = 25.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableMotor");
			changedFields->push_back("MotorSpeed");
			changedFields->push_back("MaxMotorTorque");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawPrismaticJoint2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* joint = static_cast<World::PrismaticJoint2DComponent*>(instance);
	if (!joint) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "滑轨关节预设 (Prismatic Joint Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float gap = 6.0f;
	const float btnH = 22.0f;
	const float availW = std::max(270.0f, rect.W - 24.0f);
	const float btnW = (availW - gap * 2.0f) / 3.0f;
	const Wui::WuiRect btnRailX  { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnLiftY  { rect.X + 12.0f + btnW + gap, y, btnW, btnH };
	const Wui::WuiRect btnMotor  { rect.X + 12.0f + (btnW + gap) * 2.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnRailX, "水平滑轨: 沿 X 轴滑动 ±2.0m (Axis={1, 0}, LowerTranslation=-2.0, UpperTranslation=2.0, EnableLimit=true)");
	if (Wui::Button(ctx, Wui::HashId("prop.prismatic2d.preset_rail_x"), btnRailX, "水平滑轨 X", theme))
	{
		joint->Axis = { 1.0f, 0.0f };
		joint->LowerTranslation = -2.0f;
		joint->UpperTranslation = 2.0f;
		joint->EnableLimit = true;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Axis");
			changedFields->push_back("LowerTranslation");
			changedFields->push_back("UpperTranslation");
			changedFields->push_back("EnableLimit");
		}
	}

	Wui::Tooltip(ctx, btnLiftY, "垂直升降: 沿 Y 轴升降 -1.5m~3.0m (Axis={0, 1}, LowerTranslation=-1.5, UpperTranslation=3.0, EnableLimit=true)");
	if (Wui::Button(ctx, Wui::HashId("prop.prismatic2d.preset_lift_y"), btnLiftY, "垂直升降 Y", theme))
	{
		joint->Axis = { 0.0f, 1.0f };
		joint->LowerTranslation = -1.5f;
		joint->UpperTranslation = 3.0f;
		joint->EnableLimit = true;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Axis");
			changedFields->push_back("LowerTranslation");
			changedFields->push_back("UpperTranslation");
			changedFields->push_back("EnableLimit");
		}
	}

	Wui::Tooltip(ctx, btnMotor, "往复驱动电机: 2m/s 驱动电机 (EnableMotor=true, MotorSpeed=2.0, MaxMotorForce=100.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.prismatic2d.preset_motor"), btnMotor, "往复驱动电机", theme))
	{
		joint->EnableMotor = true;
		joint->MotorSpeed = 2.0f;
		joint->MaxMotorForce = 100.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableMotor");
			changedFields->push_back("MotorSpeed");
			changedFields->push_back("MaxMotorForce");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawWheelJoint2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* joint = static_cast<World::WheelJoint2DComponent*>(instance);
	if (!joint) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "车轮悬挂关节预设 (Wheel Joint Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float gap = 6.0f;
	const float btnH = 22.0f;
	const float availW = std::max(270.0f, rect.W - 24.0f);
	const float btnW = (availW - gap * 2.0f) / 3.0f;
	const Wui::WuiRect btnOffroad { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnSport   { rect.X + 12.0f + btnW + gap, y, btnW, btnH };
	const Wui::WuiRect btnMotor   { rect.X + 12.0f + (btnW + gap) * 2.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnOffroad, "越野车长行程悬挂 (Hertz=4.0, DampingRatio=0.7, LowerTranslation=-0.3, UpperTranslation=0.3)");
	if (Wui::Button(ctx, Wui::HashId("prop.wheel2d.preset_offroad"), btnOffroad, "越野车悬挂 (4Hz)", theme))
	{
		joint->Hertz = 4.0f;
		joint->DampingRatio = 0.7f;
		joint->LowerTranslation = -0.3f;
		joint->UpperTranslation = 0.3f;
		joint->EnableLimit = true;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Hertz");
			changedFields->push_back("DampingRatio");
			changedFields->push_back("LowerTranslation");
			changedFields->push_back("UpperTranslation");
			changedFields->push_back("EnableLimit");
		}
	}

	Wui::Tooltip(ctx, btnSport, "硬派跑车短行程硬悬挂 (Hertz=8.0, DampingRatio=0.85, LowerTranslation=-0.1, UpperTranslation=0.1)");
	if (Wui::Button(ctx, Wui::HashId("prop.wheel2d.preset_sport"), btnSport, "硬派跑车 (8Hz)", theme))
	{
		joint->Hertz = 8.0f;
		joint->DampingRatio = 0.85f;
		joint->LowerTranslation = -0.1f;
		joint->UpperTranslation = 0.1f;
		joint->EnableLimit = true;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Hertz");
			changedFields->push_back("DampingRatio");
			changedFields->push_back("LowerTranslation");
			changedFields->push_back("UpperTranslation");
			changedFields->push_back("EnableLimit");
		}
	}

	Wui::Tooltip(ctx, btnMotor, "车轮驱动电机: 720°/s 匀速旋转 (EnableMotor=true, MotorSpeed=720.0, MaxMotorTorque=30.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.wheel2d.preset_motor"), btnMotor, "车轮驱动电机", theme))
	{
		joint->EnableMotor = true;
		joint->MotorSpeed = 720.0f;
		joint->MaxMotorTorque = 30.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableMotor");
			changedFields->push_back("MotorSpeed");
			changedFields->push_back("MaxMotorTorque");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}


float PropertiesPanel::DrawAreaEffector2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* effector = static_cast<World::AreaEffector2DComponent*>(instance);
	if (!effector) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "区域效应器预设 (Area Effector Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float gap = 6.0f;
	const float btnH = 22.0f;
	const float availW = std::max(270.0f, rect.W - 24.0f);
	const float btnW = (availW - gap) / 2.0f;

	const Wui::WuiRect btnUpdraft   { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnCrosswind { rect.X + 12.0f + btnW + gap, y, btnW, btnH };

	Wui::Tooltip(ctx, btnUpdraft, "上升气流: 垂直向上吹拂升力 (ForceDirection={0, 1}, ForceMagnitude=18.0, ForceMode=0 [方向风场])");
	if (Wui::Button(ctx, Wui::HashId("prop.area2d.preset_updraft"), btnUpdraft, "上升气流 (Updraft)", theme))
	{
		effector->ForceDirection = { 0.0f, 1.0f };
		effector->ForceMagnitude = 18.0f;
		effector->ForceMode = 0;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("ForceDirection");
			changedFields->push_back("ForceMagnitude");
			changedFields->push_back("ForceMode");
		}
	}

	Wui::Tooltip(ctx, btnCrosswind, "横向强风: 水平向右吹拂 (ForceDirection={1, 0}, ForceMagnitude=15.0, ForceMode=0 [方向风场])");
	if (Wui::Button(ctx, Wui::HashId("prop.area2d.preset_crosswind"), btnCrosswind, "横向强风 (Crosswind)", theme))
	{
		effector->ForceDirection = { 1.0f, 0.0f };
		effector->ForceMagnitude = 15.0f;
		effector->ForceMode = 0;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("ForceDirection");
			changedFields->push_back("ForceMagnitude");
			changedFields->push_back("ForceMode");
		}
	}
	y += 28.0f;

	const Wui::WuiRect btnAttractor { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnRepulsor  { rect.X + 12.0f + btnW + gap, y, btnW, btnH };

	Wui::Tooltip(ctx, btnAttractor, "引力黑洞: 向区域中心聚集吸引 (ForceMode=1 [点引力], ForceMagnitude=25.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.area2d.preset_attractor"), btnAttractor, "引力黑洞 (Attractor)", theme))
	{
		effector->ForceMode = 1;
		effector->ForceMagnitude = 25.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("ForceMode");
			changedFields->push_back("ForceMagnitude");
		}
	}

	Wui::Tooltip(ctx, btnRepulsor, "冲击排斥: 向外围爆发排斥 (ForceMode=2 [点斥力], ForceMagnitude=30.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.area2d.preset_repulsor"), btnRepulsor, "冲击排斥 (Repulsor)", theme))
	{
		effector->ForceMode = 2;
		effector->ForceMagnitude = 30.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("ForceMode");
			changedFields->push_back("ForceMagnitude");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawBuoyancyEffector2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* effector = static_cast<World::BuoyancyEffector2DComponent*>(instance);
	if (!effector) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "浮力水体预设 (Buoyancy Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float gap = 6.0f;
	const float btnH = 22.0f;
	const float availW = std::max(270.0f, rect.W - 24.0f);
	const float btnW = (availW - gap * 2.0f) / 3.0f;

	const Wui::WuiRect btnWater { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnRiver { rect.X + 12.0f + btnW + gap, y, btnW, btnH };
	const Wui::WuiRect btnMud   { rect.X + 12.0f + (btnW + gap) * 2.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnWater, "静止深水池: 常规清水平静水域 (Density=2.0, LinearDrag=2.5, AngularDrag=1.5, FlowVelocity={0, 0})");
	if (Wui::Button(ctx, Wui::HashId("prop.buoy2d.preset_water"), btnWater, "静止深水池", theme))
	{
		effector->Density = 2.0f;
		effector->LinearDrag = 2.5f;
		effector->AngularDrag = 1.5f;
		effector->FlowVelocity = { 0.0f, 0.0f };
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Density");
			changedFields->push_back("LinearDrag");
			changedFields->push_back("AngularDrag");
			changedFields->push_back("FlowVelocity");
		}
	}

	Wui::Tooltip(ctx, btnRiver, "向右急流河: 沿 X 轴向右流动的水流 (Density=1.8, LinearDrag=2.0, FlowVelocity={4.0, 0})");
	if (Wui::Button(ctx, Wui::HashId("prop.buoy2d.preset_river"), btnRiver, "向右急流河", theme))
	{
		effector->Density = 1.8f;
		effector->LinearDrag = 2.0f;
		effector->FlowVelocity = { 4.0f, 0.0f };
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Density");
			changedFields->push_back("LinearDrag");
			changedFields->push_back("FlowVelocity");
		}
	}

	Wui::Tooltip(ctx, btnMud, "高粘度泥浆: 高密度高粘滞阻尼泥泞环境 (Density=3.0, LinearDrag=6.0, AngularDrag=4.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.buoy2d.preset_mud"), btnMud, "高粘度泥浆", theme))
	{
		effector->Density = 3.0f;
		effector->LinearDrag = 6.0f;
		effector->AngularDrag = 4.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Density");
			changedFields->push_back("LinearDrag");
			changedFields->push_back("AngularDrag");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawSurfaceEffector2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* effector = static_cast<World::SurfaceEffector2DComponent*>(instance);
	if (!effector) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "传送带预设 (Conveyor Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float gap = 6.0f;
	const float btnH = 22.0f;
	const float availW = std::max(270.0f, rect.W - 24.0f);
	const float btnW = (availW - gap * 2.0f) / 3.0f;

	const Wui::WuiRect btnNormal  { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnFast    { rect.X + 12.0f + btnW + gap, y, btnW, btnH };
	const Wui::WuiRect btnReverse { rect.X + 12.0f + (btnW + gap) * 2.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnNormal, "常规传送带: 3.0m/s 标准传送 (Speed=3.0, ForceScale=1.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.surf2d.preset_normal"), btnNormal, "常规传送带 (3m/s)", theme))
	{
		effector->Speed = 3.0f;
		effector->ForceScale = 1.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Speed");
			changedFields->push_back("ForceScale");
		}
	}

	Wui::Tooltip(ctx, btnFast, "高速加速带: 8.0m/s 快速推进强力驱动 (Speed=8.0, ForceScale=2.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.surf2d.preset_fast"), btnFast, "高速加速带 (8m/s)", theme))
	{
		effector->Speed = 8.0f;
		effector->ForceScale = 2.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Speed");
			changedFields->push_back("ForceScale");
		}
	}

	Wui::Tooltip(ctx, btnReverse, "反向传送带: -3.0m/s 逆向传输 (Speed=-3.0, ForceScale=1.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.surf2d.preset_reverse"), btnReverse, "反向传送带 (-3m/s)", theme))
	{
		effector->Speed = -3.0f;
		effector->ForceScale = 1.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Speed");
			changedFields->push_back("ForceScale");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawVfxInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	auto* comp = static_cast<World::VfxComponent*>(instance);
	const Wui::WuiId base = Wui::HashId(schema.DisplayName.c_str());
	float height = DrawSchemaFields(ctx, base, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	if (comp)
	{
		// 1. 资产状态提示与跳转 VFX 工作台
		const Wui::WuiRect openBtnRect { rect.X + 8.0f, y, 220.0f, 24.0f };
		Wui::Tooltip(ctx, openBtnRect, "在独立的 WorldVFX 工作台中可视化编排多发射器、拖尾、3D模型与光照");
		if (Wui::Button(ctx, Wui::HashId("vfx.comp.open_studio"), openBtnRect, "[打开] 打开 VFX 特效工作台", theme))
		{
			const std::string logical = comp->Effect.HasPath() ? StringPool::Get().PathOf(comp->Effect.Path) : "";
			m_Host.OpenParticleEditor(logical);
		}
		y += 28.0f;

		// 2. 走带控制 (Transport Controls)
		Wui::Label(ctx, { rect.X + 8.0f, y + 3.0f }, "走带控制 (Transport):", theme.Accent, 11.0f);
		float bx = rect.X + 140.0f;
		const float bw = 46.0f;

		const Wui::WuiRect playBtn { bx, y, bw, 22.0f };
		Wui::Tooltip(ctx, playBtn, "开始/继续播放特效");
		if (Wui::Button(ctx, Wui::HashId("vfx.comp.play"), playBtn, "[播]", theme))
		{
			comp->PlaybackState = VfxPlaybackState::Playing;
			if (changedFields) changedFields->push_back("PlaybackState");
		}
		bx += bw + 4.0f;

		const Wui::WuiRect pauseBtn { bx, y, bw, 22.0f };
		Wui::Tooltip(ctx, pauseBtn, "暂停特效模拟");
		if (Wui::Button(ctx, Wui::HashId("vfx.comp.pause"), pauseBtn, "[停]", theme))
		{
			comp->PlaybackState = VfxPlaybackState::Paused;
			if (changedFields) changedFields->push_back("PlaybackState");
		}
		bx += bw + 4.0f;

		const Wui::WuiRect resetBtn { bx, y, bw, 22.0f };
		Wui::Tooltip(ctx, resetBtn, "重置特效模拟回溯到起点");
		if (Wui::Button(ctx, Wui::HashId("vfx.comp.reset"), resetBtn, "[复位]", theme))
		{
			uint32_t entId = static_cast<uint32_t>(static_cast<entt::entity>(entity));
			for (size_t i = 0; i < 8; ++i)
			{
				uint32_t poolId = (entId << 8) | static_cast<uint32_t>(i);
				if (GetGlobalParticleSystem().HasPoolForEntity(poolId))
					GetGlobalParticleSystem().GetPoolForEntity(poolId).Reset();
			}
			comp->PlaybackState = VfxPlaybackState::Playing;
			if (changedFields) changedFields->push_back("PlaybackState");
		}
		y += 28.0f;

		// 提示：预设与内容资产已统一迁入内容浏览器【引擎内容库 (Engine)】与 Effect 下拉选择器
		Wui::Label(ctx, { rect.X + 8.0f, y + 3.0f }, "提示: 官方特效资产请在上方资产选择器或内容浏览器 [引擎内容库] 中选取", theme.TextMuted, 10.0f);
		y += 18.0f;
	}

	return (y - rect.Y);
}


float PropertiesPanel::DrawBehaviorTreeInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect, std::vector<std::string>* changedFields)
{
	(void)schema;
	(void)visibleRect;
	const Wui::WuiTheme& theme = m_Host.Theme();
	auto* bt = static_cast<BehaviorTreeComponent*>(instance);
	if (!bt)
		return 0.0f;

	const float pad = theme.Pad;
	const float startX = rect.X + pad;
	const float contentW = rect.W - pad * 2.0f;
	float curY = rect.Y + 4.0f;
	const float paramLabelW = 120.0f;
	const float paramFieldW = contentW - paramLabelW;

	// =========================================================================
	// 1. 行为树资产槽位与绑定 (Behavior Tree Asset Slot)
	// =========================================================================
	Wui::Label(ctx, { startX, curY + 2.0f }, "[资产与绑定] 行为树资产槽位 (Behavior Tree Asset)", theme.Accent, theme.FontSizeSmall);
	curY += 22.0f;

	// 启用开关 Enabled
	bool enabled = (bt->Enabled != 0);
	if (Wui::Checkbox(ctx, Wui::HashId("inspect.bt.enabled"),
		{ startX, curY, contentW, 22.0f }, "启用行为控制器 (Enabled)", enabled, theme))
	{
		bt->Enabled = enabled ? 1 : 0;
		if (changedFields) changedFields->push_back("Enabled");
		m_Host.MarkDocumentDirty();
	}
	curY += 26.0f;

	// 资产扫描与下拉选项
	const std::vector<std::string>& btAssets = Editor::AssetCatalog::PathsFor(EditorAssetKind::BehaviorTree);
	std::vector<std::string> assetOptions = btAssets;
	if (assetOptions.empty())
	{
		assetOptions.push_back("assets/ai/SmartObjectDemo.wbt");
	}

	std::string currentPath = assetOptions[0];
	int selectedAssetIdx = -1;
	for (size_t i = 0; i < assetOptions.size(); ++i)
	{
		uint32_t h = static_cast<uint32_t>(std::hash<std::string>{}(assetOptions[i]));
		if (h == bt->TreeAssetId)
		{
			currentPath = assetOptions[i];
			selectedAssetIdx = static_cast<int>(i);
			break;
		}
	}
	if (selectedAssetIdx < 0)
	{
		if (bt->TreeAssetId != 0)
		{
			std::string customEntry = "ID: 0x" + std::to_string(bt->TreeAssetId);
			assetOptions.insert(assetOptions.begin(), customEntry);
			selectedAssetIdx = 0;
			currentPath = customEntry;
		}
		else
		{
			selectedAssetIdx = 0;
			currentPath = assetOptions[0];
		}
	}

	Wui::Label(ctx, { startX, curY + 2.0f }, "行为树资产 (.wbt):", theme.Text, theme.FontSizeSmall);
	int chosenIdx = selectedAssetIdx;
	if (Wui::Combo(ctx, Wui::HashId("inspect.bt.asset_combo"),
		{ startX + paramLabelW, curY, paramFieldW, 22.0f }, "行为树资产", assetOptions, chosenIdx, theme))
	{
		if (chosenIdx >= 0 && chosenIdx < static_cast<int>(assetOptions.size()))
		{
			currentPath = assetOptions[chosenIdx];
			bt->TreeAssetId = static_cast<uint32_t>(std::hash<std::string>{}(currentPath));
			if (changedFields) changedFields->push_back("TreeAssetId");
			m_Host.MarkDocumentDirty();
		}
	}
	curY += 28.0f;

	Wui::Label(ctx, { startX, curY + 2.0f }, "TreeAssetId 哈希:", theme.TextMuted, theme.FontSizeSmall);
	int64_t treeId = static_cast<int64_t>(bt->TreeAssetId);
	if (Wui::DragInt(ctx, Wui::HashId("inspect.bt.tree_asset_id"),
		{ startX + paramLabelW, curY, paramFieldW, 20.0f }, treeId, 0, 0xFFFFFFFF, theme))
	{
		bt->TreeAssetId = static_cast<uint32_t>(treeId);
		if (changedFields) changedFields->push_back("TreeAssetId");
		m_Host.MarkDocumentDirty();
	}
	curY += 30.0f;

	// =========================================================================
	// 2. 醒目大按钮: 【在专属编辑器中打开 (Open in BT Editor)】
	// =========================================================================
	const Wui::WuiRect openBtnRect { startX, curY, contentW, 30.0f };
	Wui::WuiTheme openTheme = theme;
	openTheme.ButtonBg = Wui::WuiColor { 0.18f, 0.32f, 0.65f, 0.95f };
	openTheme.Border = Wui::WuiColor { 0.35f, 0.65f, 0.95f, 1.0f };
	if (Wui::Button(ctx, Wui::HashId("inspect.bt.btn_open_editor"), openBtnRect, "在专属编辑器中打开 (Open in BT Editor)", openTheme))
	{
		std::string targetPath = currentPath;
		if (targetPath.find(".wbt") == std::string::npos && !btAssets.empty())
		{
			targetPath = btAssets[0];
		}
		BehaviorTreeEditorPanel::RequestOpenPath(targetPath);
		m_Host.ActivatePanel("behaviortree");
		m_Host.Notify("已在专属行为树编辑器中载入: " + targetPath);
	}
	Wui::Tooltip(ctx, openBtnRect, "打开专属行为树工作台面板，可视化编排树结构大纲、属性参数与黑板变量");
	curY += 38.0f;

	// =========================================================================
	// 3. 运行期只读监控卡片 (Runtime Read-Only Monitor)
	// =========================================================================
	const float cardH = 148.0f;
	const Wui::WuiRect cardRect { startX, curY, contentW, cardH };
	Wui::PanelBackground(ctx, cardRect, Wui::WuiColor { 0.12f, 0.14f, 0.18f, 0.95f }, theme.Radius);
	Wui::HighlightOutline(ctx, cardRect, theme.Border, theme.Radius, 1.0f);
	float cardY = curY + 8.0f;

	Wui::Label(ctx, { startX + 8.0f, cardY }, "[运行期监控] AI 状态与活跃节点 (Runtime Monitor)", theme.Warning, theme.FontSizeSmall);
	cardY += 22.0f;

	const char* statusText = "[Inactive] 行为树休眠待机 (Inactive)";
	Wui::WuiColor statusColor = theme.TextMuted;
	switch (bt->Status)
	{
	case BehaviorStatus::Running:
		statusText = "[Running] 行为树正在运行推进 (Running)";
		statusColor = theme.Warning;
		break;
	case BehaviorStatus::Success:
		statusText = "[Success] 行为执行成功完成 (Success)";
		statusColor = theme.Success;
		break;
	case BehaviorStatus::Failure:
		statusText = "[Failure] 行为执行失败终止 (Failure)";
		statusColor = theme.Danger;
		break;
	case BehaviorStatus::Inactive:
	default:
		statusText = "[Inactive] 行为树休眠待机 (Inactive)";
		statusColor = theme.TextMuted;
		break;
	}

	const Wui::WuiRect badgeRect { startX + 8.0f, cardY, contentW - 16.0f, 22.0f };
	Wui::PanelBackground(ctx, badgeRect, Wui::WuiColor { statusColor.R * 0.20f, statusColor.G * 0.20f, statusColor.B * 0.20f, 0.9f }, theme.Radius);
	Wui::HighlightOutline(ctx, badgeRect, statusColor, theme.Radius, 1.0f);
	Wui::Label(ctx, { startX + 16.0f, cardY + 3.0f }, statusText, statusColor, theme.FontSizeSmall);
	cardY += 28.0f;

	std::string activeNodeStr = "活跃节点 ID: #" + std::to_string(bt->CurrentNodeIndex);
	Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, activeNodeStr.c_str(), theme.Text, theme.FontSizeSmall);
	cardY += 24.0f;

	const char* fsmNames[] = {
		"0 - Patrolling (日常巡逻状态)",
		"1 - SeekingSmartObject (急救求援：寻找并前往医疗站)",
		"2 - Interacting (正在使用智能对象交互充能)",
		"3 - CoolingDown (交互完毕：冷却与状态回退)"
	};
	const char* fsmDesc = (bt->StateMachineState < 4) ? fsmNames[bt->StateMachineState] : "未知状态 (Unknown)";
	std::string fsmText = "状态机 FSM: " + std::string(fsmDesc);
	Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, fsmText.c_str(), theme.Accent, theme.FontSizeSmall);
	cardY += 24.0f;

	std::ostringstream ss;
	ss << "黑板快照: Pos(" << static_cast<int>(bt->BlackboardTargetPos.x) << "," << static_cast<int>(bt->BlackboardTargetPos.y) << "," << static_cast<int>(bt->BlackboardTargetPos.z) << ") "
	   << "| Ent#" << bt->TargetEntity << " | Timer:" << bt->ExecutionTimer << "s | Val:" << bt->BlackboardValue;
	Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, ss.str().c_str(), theme.TextMuted, theme.FontSizeSmall);

	curY += cardH + 12.0f;
	return (curY - rect.Y);
}

float PropertiesPanel::DrawSmartObjectInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect, std::vector<std::string>* changedFields)
{
	(void)schema;
	(void)visibleRect;
	const Wui::WuiTheme& theme = m_Host.Theme();
	auto* so = static_cast<SmartObjectComponent*>(instance);
	if (!so)
		return 0.0f;

	const float pad = theme.Pad;
	const float startX = rect.X + pad;
	const float contentW = rect.W - pad * 2.0f;
	float curY = rect.Y + 4.0f;
	const float paramLabelW = 120.0f;
	const float paramFieldW = contentW - paramLabelW;

	// =========================================================================
	// 1. 配置与对齐参数 (Authoring & Alignment Properties)
	// =========================================================================
	Wui::Label(ctx, { startX, curY + 2.0f }, "[配置] 智能对象参数 (Smart Object Properties)", theme.Accent, theme.FontSizeSmall);
	curY += 22.0f;

	// 启用开关 Enabled
	bool enabled = so->Enabled;
	if (Wui::Checkbox(ctx, Wui::HashId("inspect.so.enabled"),
		{ startX, curY, contentW, 22.0f }, "启用智能对象 (Enabled)", enabled, theme))
	{
		so->Enabled = enabled;
		if (changedFields) changedFields->push_back("Enabled");
		m_Host.MarkDocumentDirty();
	}
	curY += 26.0f;

	// 交互距离 InteractionDistance
	Wui::Label(ctx, { startX, curY + 2.0f }, "交互距离 (Distance m):", theme.Text, theme.FontSizeSmall);
	if (Wui::DragFloat(ctx, Wui::HashId("inspect.so.interact_dist"),
		{ startX + paramLabelW, curY, paramFieldW, 22.0f }, so->InteractionDistance, 0.1f, 0.1f, 30.0f, theme))
	{
		if (changedFields) changedFields->push_back("InteractionDistance");
		m_Host.MarkDocumentDirty();
	}
	curY += 26.0f;

	// 冷却周期 CooldownDuration
	Wui::Label(ctx, { startX, curY + 2.0f }, "冷却周期 (Cooldown s):", theme.Text, theme.FontSizeSmall);
	if (Wui::DragFloat(ctx, Wui::HashId("inspect.so.cooldown_dur"),
		{ startX + paramLabelW, curY, paramFieldW, 22.0f }, so->CooldownDuration, 0.1f, 0.0f, 60.0f, theme))
	{
		if (changedFields) changedFields->push_back("CooldownDuration");
		m_Host.MarkDocumentDirty();
	}
	curY += 26.0f;

	// 朝向角度 FacingAngleDeg
	Wui::Label(ctx, { startX, curY + 2.0f }, "交互朝向 (Facing Deg):", theme.Text, theme.FontSizeSmall);
	if (Wui::DragFloat(ctx, Wui::HashId("inspect.so.facing_angle"),
		{ startX + paramLabelW, curY, paramFieldW, 22.0f }, so->FacingAngleDeg, 1.0f, 0.0f, 360.0f, theme))
	{
		if (changedFields) changedFields->push_back("FacingAngleDeg");
		m_Host.MarkDocumentDirty();
	}
	curY += 26.0f;

	// 相对偏移 SlotOffset (X, Y, Z)
	Wui::Label(ctx, { startX, curY + 2.0f }, "插槽相对偏移 (SlotOffset):", theme.Text, theme.FontSizeSmall);
	curY += 22.0f;
	const float coordLabelW = 16.0f;
	const float coordFieldW = (contentW - coordLabelW * 3.0f - 12.0f) / 3.0f;

	Wui::Label(ctx, { startX, curY + 2.0f }, "X", theme.AxisX, theme.FontSizeSmall);
	if (Wui::DragFloat(ctx, Wui::HashId("inspect.so.offset_x"),
		{ startX + coordLabelW, curY, coordFieldW, 20.0f }, so->SlotOffset.x, 0.05f, -100.0f, 100.0f, theme))
	{
		if (changedFields) changedFields->push_back("SlotOffset");
		m_Host.MarkDocumentDirty();
	}
	Wui::Label(ctx, { startX + coordLabelW + coordFieldW + 6.0f, curY + 2.0f }, "Y", theme.AxisY, theme.FontSizeSmall);
	if (Wui::DragFloat(ctx, Wui::HashId("inspect.so.offset_y"),
		{ startX + coordLabelW * 2.0f + coordFieldW + 6.0f, curY, coordFieldW, 20.0f }, so->SlotOffset.y, 0.05f, -100.0f, 100.0f, theme))
	{
		if (changedFields) changedFields->push_back("SlotOffset");
		m_Host.MarkDocumentDirty();
	}
	Wui::Label(ctx, { startX + (coordLabelW + coordFieldW + 6.0f) * 2.0f, curY + 2.0f }, "Z", theme.AxisZ, theme.FontSizeSmall);
	if (Wui::DragFloat(ctx, Wui::HashId("inspect.so.offset_z"),
		{ startX + (coordLabelW + coordFieldW + 6.0f) * 2.0f + coordLabelW, curY, coordFieldW, 20.0f }, so->SlotOffset.z, 0.05f, -100.0f, 100.0f, theme))
	{
		if (changedFields) changedFields->push_back("SlotOffset");
		m_Host.MarkDocumentDirty();
	}
	curY += 26.0f;

	// 活动标签 ActivityTag
	Wui::Label(ctx, { startX, curY + 2.0f }, "活动标签 (ActivityTag):", theme.Text, theme.FontSizeSmall);
	int64_t actTag = static_cast<int64_t>(so->ActivityTag);
	if (Wui::DragInt(ctx, Wui::HashId("inspect.so.activity_tag"),
		{ startX + paramLabelW, curY, paramFieldW, 22.0f }, actTag, 0, 0xFFFFFFFF, theme))
	{
		so->ActivityTag = static_cast<uint32_t>(actTag);
		if (changedFields) changedFields->push_back("ActivityTag");
		m_Host.MarkDocumentDirty();
	}
	curY += 32.0f;

	// =========================================================================
	// 2. 运行时状态与插槽仲裁卡片 (Runtime State & Slot Arbitration Card)
	// =========================================================================
	const float cardH = 196.0f;
	const Wui::WuiRect cardRect { startX, curY, contentW, cardH };
	Wui::PanelBackground(ctx, cardRect, theme.CardBg, theme.Radius);
	Wui::HighlightOutline(ctx, cardRect, theme.Border, theme.Radius, 1.0f);
	float cardY = curY + 8.0f;

	Wui::Label(ctx, { startX + 8.0f, cardY }, "[运行时] 插槽实时状态与占用者 (Slot State)", theme.Warning, theme.FontSizeSmall);
	cardY += 22.0f;

	// 插槽状态实时徽章 (0=Available [绿], 1=Claimed [黄], 2=InUse [蓝], 3=Cooldown [橙])
	const char* slotStateText = "[Available] 空闲可用 (0)";
	Wui::WuiColor slotStateColor { 0.2f, 0.85f, 0.35f, 1.0f }; // Green
	switch (so->SlotState)
	{
	case SmartObjectSlotState::Available:
		slotStateText = "[Available] 空闲可用 (0=可预约/可交互)";
		slotStateColor = Wui::WuiColor { 0.2f, 0.85f, 0.35f, 1.0f }; // Green
		break;
	case SmartObjectSlotState::Claimed:
		slotStateText = "[Claimed] 已被预约锁定 (1=前往中)";
		slotStateColor = Wui::WuiColor { 0.95f, 0.85f, 0.2f, 1.0f }; // Yellow
		break;
	case SmartObjectSlotState::InUse:
		slotStateText = "[InUse] 正在交互使用中 (2=占用)";
		slotStateColor = Wui::WuiColor { 0.25f, 0.65f, 0.95f, 1.0f }; // Blue
		break;
	case SmartObjectSlotState::Cooldown:
		slotStateText = "[Cooldown] 冷却倒计时中 (3=等待可用)";
		slotStateColor = Wui::WuiColor { 0.95f, 0.55f, 0.15f, 1.0f }; // Orange
		break;
	}

	const Wui::WuiRect badgeRect { startX + 8.0f, cardY, contentW - 16.0f, 24.0f };
	Wui::PanelBackground(ctx, badgeRect, Wui::WuiColor { slotStateColor.R * 0.18f, slotStateColor.G * 0.18f, slotStateColor.B * 0.18f, 0.85f }, theme.Radius);
	Wui::HighlightOutline(ctx, badgeRect, slotStateColor, theme.Radius, 1.0f);
	Wui::Label(ctx, { startX + 16.0f, cardY + 4.0f }, slotStateText, slotStateColor, theme.FontSizeSmall);
	cardY += 30.0f;

	// 冷却倒计时 (CooldownTimer / CooldownDuration)
	char cdBuf[64];
	std::snprintf(cdBuf, sizeof(cdBuf), "冷却倒计时: %.2f s / %.2f s", so->CooldownTimer, so->CooldownDuration);
	Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, cdBuf, theme.Text, theme.FontSizeSmall);
	cardY += 20.0f;

	const Wui::WuiRect pbRect { startX + 8.0f, cardY, contentW - 16.0f, 10.0f };
	Wui::PanelBackground(ctx, pbRect, theme.PanelBg, 3.0f);
	const float ratio = (so->CooldownDuration > 0.0f) ? std::clamp(so->CooldownTimer / so->CooldownDuration, 0.0f, 1.0f) : 0.0f;
	if (ratio > 0.0f)
	{
		Wui::WuiRect fillRect = pbRect;
		fillRect.W = pbRect.W * ratio;
		Wui::PanelBackground(ctx, fillRect, slotStateColor, 3.0f);
	}
	Wui::HighlightOutline(ctx, pbRect, theme.Border, 3.0f, 1.0f);
	cardY += 18.0f;

	// 占用者实体 ID
	char claimantBuf[64];
	if (so->ClaimedByEntity != 0)
		std::snprintf(claimantBuf, sizeof(claimantBuf), "当前占用者实体 ID: %u", so->ClaimedByEntity);
	else
		std::snprintf(claimantBuf, sizeof(claimantBuf), "当前占用者实体 ID: 无 (None / 0)");
	Wui::Label(ctx, { startX + 8.0f, cardY + 2.0f }, claimantBuf, (so->ClaimedByEntity != 0) ? theme.Warning : theme.TextMuted, theme.FontSizeSmall);
	cardY += 26.0f;

	// 调试按钮: 【一键重置插槽 (Release Slot)】、【一键占用测试 (Force Claim)】
	const float btnW = (contentW - 24.0f) * 0.5f;
	const Wui::WuiRect releaseBtnRect { startX + 8.0f, cardY, btnW, 24.0f };
	if (Wui::Button(ctx, Wui::HashId("inspect.so.btn_release"), releaseBtnRect, "[释放] 一键重置插槽", theme))
	{
		so->SlotState = SmartObjectSlotState::Available;
		so->ClaimedByEntity = 0;
		so->CooldownTimer = 0.0f;
		if (changedFields)
		{
			changedFields->push_back("SlotState");
			changedFields->push_back("ClaimedByEntity");
			changedFields->push_back("CooldownTimer");
		}
		m_Host.Notify("已强制重置智能对象插槽为 Available 空闲可用");
		m_Host.MarkDocumentDirty();
	}
	Wui::Tooltip(ctx, releaseBtnRect, "一键解除任何实体占用与冷却，恢复 Available 空闲状态");

	const Wui::WuiRect claimBtnRect { startX + 8.0f + btnW + 8.0f, cardY, btnW, 24.0f };
	if (Wui::Button(ctx, Wui::HashId("inspect.so.btn_force_claim"), claimBtnRect, "[占用] 一键占用测试", theme))
	{
		so->SlotState = SmartObjectSlotState::Claimed;
		so->ClaimedByEntity = static_cast<uint32_t>(static_cast<entt::entity>(entity));
		if (changedFields)
		{
			changedFields->push_back("SlotState");
			changedFields->push_back("ClaimedByEntity");
		}
		m_Host.Notify("已模拟占用插槽 (Claimed)");
		m_Host.MarkDocumentDirty();
	}
	Wui::Tooltip(ctx, claimBtnRect, "一键将插槽标记为 Claimed 预约占用状态以测试防抢占仲裁逻辑");
	cardY += 32.0f;

	curY += cardH + 12.0f;
	return (curY - rect.Y);
}

}








