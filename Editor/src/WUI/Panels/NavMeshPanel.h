#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/NavMesh/NavMesh.h"
#include "World/Scene/Entity.h"

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <future>

namespace World
{
	class NavMeshPanel final : public EditorPanel
	{
	public:
		NavMeshPanel();
		explicit NavMeshPanel(PanelHost& host);
		~NavMeshPanel() override = default;

		const char* Id() const override { return "navmesh"; }
		const char* Title() const override { return "3D 导航网格 (NavMesh & AI Navigation)"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt) override;

		// 视口可视化全局控制标志
		static bool ShowPolygons;
		static bool FillPolygons;
		static bool ShowPaths;
		static bool ShowObstacles;
		static bool ShowAreaCosts;
		static bool ShowOffMeshLinks;
		static bool ShowLoSRaycast;

		// 探针数据结构与访问接口
		struct RaycastProbeData
		{
			glm::vec3 Start { 0.0f, 1.0f, 0.0f };
			glm::vec3 End { 10.0f, 1.0f, 10.0f };
			bool HasTested { false };
			bool IsVisible { false };
			glm::vec3 HitPoint { 0.0f };
		};
		static const RaycastProbeData& GetRaycastProbe() { return s_RaycastProbe; }

		struct WanderProbeData
		{
			glm::vec3 Center { 0.0f, 0.5f, 0.0f };
			float Radius { 15.0f };
			bool HasSampled { false };
			glm::vec3 SampledPoint { 0.0f };
		};
		static const WanderProbeData& GetWanderProbe() { return s_WanderProbe; }

		static const std::vector<glm::vec3>& GetTestPath() { return s_TestPath; }

		// 烘焙配置项访问器
		float GetAgentRadius() const { return m_AgentRadius; }
		void SetAgentRadius(float r) { m_AgentRadius = r; }
		float GetAgentHeight() const { return m_AgentHeight; }
		void SetAgentHeight(float h) { m_AgentHeight = h; }
		float GetMaxSlope() const { return m_MaxSlope; }
		void SetMaxSlope(float s) { m_MaxSlope = s; }
		float GetStepHeight() const { return m_StepHeight; }
		void SetStepHeight(float s) { m_StepHeight = s; }
		float GetVoxelSize() const { return m_VoxelSize; }
		void SetVoxelSize(float v) { m_VoxelSize = v; }

	private:
		void RenderMetricsCard(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, NavMesh* navMesh);
		void RenderBakeTab(Wui::WuiContext& ctx, float startX, float curY, float contentW, float cardH, const Wui::WuiTheme& theme, PanelHost& host, NavMesh* navMesh);
		void RenderVisibilityTab(Wui::WuiContext& ctx, float startX, float curY, float contentW, float cardH, const Wui::WuiTheme& theme);
		void RenderProbesTab(Wui::WuiContext& ctx, float startX, float curY, float contentW, float cardH, const Wui::WuiTheme& theme, PanelHost& host, NavMesh* navMesh);

		void ApplyAgentProfile(int profileIdx);

		// 选项卡索引: 0=烘焙配置, 1=视口显示, 2=空间探针
		int m_ActiveTabIdx { 0 };
		float m_ScrollY { 0.0f };

		// 代理规格预设: 0=人形单位, 1=小型爬虫, 2=大型Boss/载具, 3=自定义
		int m_AgentProfileIdx { 0 };

		// 烘焙参数
		float m_AgentRadius { 0.5f };
		float m_AgentHeight { 2.0f };
		float m_MaxSlope { 45.0f };
		float m_StepHeight { 0.45f };
		float m_VoxelSize { 1.0f };

		// 寻路探针测试参数
		glm::vec3 m_StartPos { 0.0f, 0.0f, 0.0f };
		glm::vec3 m_EndPos { 10.0f, 0.0f, 10.0f };
		bool m_LastPathFound { false };
		float m_LastPathDistance { 0.0f };
		std::string m_PathQueryStatus;

		// 视线通视探针参数
		glm::vec3 m_LoSStart { 0.0f, 1.5f, 0.0f };
		glm::vec3 m_LoSEnd { 12.0f, 1.5f, 12.0f };
		std::string m_LoSStatus;

		// 随机漫游采样探针参数
		glm::vec3 m_WanderCenter { 0.0f, 0.5f, 0.0f };
		float m_WanderRadius { 15.0f };
		std::string m_WanderStatus;

		static inline RaycastProbeData s_RaycastProbe;
		static inline WanderProbeData s_WanderProbe;
		static inline std::vector<glm::vec3> s_TestPath;

		struct AsyncBakeTask
		{
			std::future<std::shared_ptr<NavMesh>> Future;
			std::chrono::high_resolution_clock::time_point StartTime;
			bool IsRunning { false };
		};
		AsyncBakeTask m_AsyncBake;
		int m_BakeScopeIdx { 0 }; // 0=全场景, 1=视口关注区 (64m)

		std::string m_StatusFeedback;
		PanelHost* m_ShellHost { nullptr };
	};
}
