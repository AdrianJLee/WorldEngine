#include "WUI/Panels/ViewportPanel_ToolGizmos.h"
#include "World/Command/CommandHistory.h"
#include "World/Scene/SelectionFilter.h"
#include "wldpch.h"
#include "WUI/Panels/ViewportPanel.h"

#include "World/WUI/WuiGizmo.h"
#include "World/Renderer/Tilemap.h"
#include "World/Renderer/Renderer3D.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/Core/KeyCodes.h"
#include "World/Physics/Physics3D.h"
#include "World/Physics/Physics2DUtils.h"
#include "World/Audio/AudioComponents.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/WuiWidgets.h"
#include "Core/EditorPreferences.h"
#include "WUI/Panels/TerrainPanel.h"
#include "WUI/Panels/FoliagePanel.h"
#include "World/Foliage/Foliage.h"
#include "World/Foliage/FoliageRenderer.h"
#include "World/Terrain/Terrain.h"
#include "World/Terrain/TerrainRenderer.h"
#include "World/Core/Input.h"
#include "WUI/Panels/NavMeshPanel.h"
#include "WUI/Panels/PhysicsPainterPanel.h"
#include "World/Level/Spline3D.h"
#include "World/Level/WhiteboxBuilder.h"
#include "World/Scene/Hierarchy.h"
#include "World/NavMesh/NavMesh.h"
#include "World/Renderer/MeshLOD.h"
#include "World/Renderer/MeshletBuilder.h"
#include <glm/gtc/matrix_inverse.hpp>
#include "World/Scene/SceneOcclusion.h"
#include "World/Scene/SceneOctree.h"
#include "World/Scene/Scene.h"
#include "World/Asset/WModelIO.h"
#include "World/Utils/Paths.h"
#include <unordered_map>
#include "World/Environment/PCGBiome.h"
#include "World/Level/AI/EQSCoverSystem.h"
#include "World/Foliage/FoliageInteraction.h"
#include "WUI/Common/EQSPreviewState.h"


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

	struct FoliagePreviewState
	{
		bool Active = false;
		entt::entity TargetEntity = entt::null;
		glm::vec3 Displacement { 0.0f };
		glm::vec3 PushDirection { 1.0f, 0.0f, 0.0f };
		float PushMagnitude { 0.0f };
		float LastImpulseTime { 0.0f };
	};

	extern PCGPreviewState g_PCGPreviewState;
	extern FoliagePreviewState g_FoliagePreviewState;

	namespace
	{
		struct CachedConvexHullEdges
		{
			std::vector<std::pair<glm::vec3, glm::vec3>> Edges;
		};

		struct CachedModelConvex
		{
			std::string Path;
			std::filesystem::file_time_type LastWriteTime {};
			glm::vec3 BoundsMin { -0.5f, -0.5f, -0.5f };
			glm::vec3 BoundsMax {  0.5f,  0.5f,  0.5f };
			bool HasBounds = false;
			bool HasHulls = false;
			std::vector<CachedConvexHullEdges> Hulls;
		};

		static const CachedModelConvex* GetCachedModelConvex(const std::string& modelPath)
		{
			if (modelPath.empty())
				return nullptr;

			static std::unordered_map<std::string, CachedModelConvex> s_ConvexWireframeCache;

			std::filesystem::path physicalPath;
			if (modelPath.rfind("engine/", 0) == 0)
				physicalPath = Paths::EngineContentRoot() / modelPath.substr(7);
			else
				physicalPath = Paths::AssetRoot() / modelPath;

			std::error_code ec;
			const auto writeTime = std::filesystem::last_write_time(physicalPath, ec);

			auto it = s_ConvexWireframeCache.find(modelPath);
			if (it != s_ConvexWireframeCache.end() && !ec && it->second.LastWriteTime == writeTime)
			{
				return &it->second;
			}

			CachedModelConvex entry;
			entry.Path = modelPath;
			entry.LastWriteTime = ec ? std::filesystem::file_time_type{} : writeTime;

			Asset::WModelData model;
			std::string err;
			if (Asset::WModelIO::ReadFile(modelPath, model, &err))
			{
				entry.BoundsMin = model.Bounds.Min;
				entry.BoundsMax = model.Bounds.Max;
				entry.HasBounds = true;

				if (model.Collision.CollisionType != 0 && !model.Collision.ConvexHulls.empty())
				{
					entry.HasHulls = true;
					entry.Hulls.reserve(model.Collision.ConvexHulls.size());
					for (const auto& hull : model.Collision.ConvexHulls)
					{
						CachedConvexHullEdges hullEdges;
						if (hull.Indices.size() >= 3)
						{
							std::vector<std::pair<uint32_t, uint32_t>> edgePairs;
							edgePairs.reserve(hull.Indices.size());
							for (size_t i = 0; i + 2 < hull.Indices.size(); i += 3)
							{
								uint32_t a = hull.Indices[i];
								uint32_t b = hull.Indices[i + 1];
								uint32_t c = hull.Indices[i + 2];
								auto addEdge = [&](uint32_t u, uint32_t v)
								{
									if (u > v) std::swap(u, v);
									edgePairs.push_back({ u, v });
								};
								addEdge(a, b);
								addEdge(b, c);
								addEdge(c, a);
							}
							std::sort(edgePairs.begin(), edgePairs.end());
							edgePairs.erase(std::unique(edgePairs.begin(), edgePairs.end()), edgePairs.end());

							hullEdges.Edges.reserve(edgePairs.size());
							for (const auto& ep : edgePairs)
							{
								if (ep.first < hull.Vertices.size() && ep.second < hull.Vertices.size())
								{
									hullEdges.Edges.push_back({ hull.Vertices[ep.first], hull.Vertices[ep.second] });
								}
							}
						}
						else if (hull.Vertices.size() >= 2)
						{
							for (size_t i = 0; i + 1 < hull.Vertices.size(); ++i)
							{
								hullEdges.Edges.push_back({ hull.Vertices[i], hull.Vertices[i + 1] });
							}
						}
						entry.Hulls.push_back(std::move(hullEdges));
					}
				}
			}

			auto [insertedIt, _] = s_ConvexWireframeCache.insert_or_assign(modelPath, std::move(entry));
			return &insertedIt->second;
		}

		static bool s_ShowAllAudioGizmos = false;
		static bool s_ShowNavMesh = true;
		static bool s_ShowOctreeBounds = false;
		static bool s_ShowLODHud = true;
		static bool s_ShowMeshletClusters = false;
		static bool s_AudioGizmoActive = false;
		static float s_AudioGizmoBeforeMin = 1.0f;
		static float s_AudioGizmoBeforeMax = 25.0f;
		class TerrainModifyCommand final : public IEditorCommand
		{
		public:
			TerrainModifyCommand(std::shared_ptr<TerrainData> terrain,
				std::vector<float> beforeHeights,
				std::vector<uint32_t> beforeSplat,
				std::vector<uint8_t> beforeHoles,
				std::vector<float> afterHeights,
				std::vector<uint32_t> afterSplat,
				std::vector<uint8_t> afterHoles,
				std::string_view name = "Terrain Edit")
				: m_Terrain(std::move(terrain)),
				  m_BeforeHeights(std::move(beforeHeights)),
				  m_BeforeSplat(std::move(beforeSplat)),
				  m_BeforeHoles(std::move(beforeHoles)),
				  m_AfterHeights(std::move(afterHeights)),
				  m_AfterSplat(std::move(afterSplat)),
				  m_AfterHoles(std::move(afterHoles)),
				  m_Name(name)
			{}

			void Execute() override
			{
				if (m_Terrain)
					Apply(m_AfterHeights, m_AfterSplat, m_AfterHoles);
			}

			void Undo() override
			{
				if (m_Terrain)
					Apply(m_BeforeHeights, m_BeforeSplat, m_BeforeHoles);
			}

			std::string_view GetName() const override { return m_Name; }
			size_t GetMemorySize() const override
			{
				return sizeof(*this) + (m_BeforeHeights.size() + m_AfterHeights.size()) * sizeof(float)
					+ (m_BeforeSplat.size() + m_AfterSplat.size()) * sizeof(uint32_t)
					+ (m_BeforeHoles.size() + m_AfterHoles.size()) * sizeof(uint8_t);
			}

		private:
			void Apply(const std::vector<float>& h, const std::vector<uint32_t>& s, const std::vector<uint8_t>& holes)
			{
				const uint32_t w = m_Terrain->GetWidth();
				const uint32_t height = m_Terrain->GetHeight();
				if (h.size() == m_Terrain->GetHeightBuffer().size())
				{
					for (uint32_t z = 0; z < height; ++z)
						for (uint32_t x = 0; x < w; ++x)
							m_Terrain->SetHeight(x, z, h[z * w + x]);
				}
				if (s.size() == m_Terrain->GetSplatmapBuffer().size())
				{
					for (uint32_t z = 0; z < height; ++z)
						for (uint32_t x = 0; x < w; ++x)
						{
							uint32_t packed = s[z * w + x];
							glm::vec4 weights(
								(packed & 0xFF) / 255.0f,
								((packed >> 8) & 0xFF) / 255.0f,
								((packed >> 16) & 0xFF) / 255.0f,
								((packed >> 24) & 0xFF) / 255.0f
							);
							m_Terrain->SetSplatWeights(x, z, weights);
						}
				}
				if (holes.size() == m_Terrain->GetHoleBuffer().size())
				{
					for (uint32_t z = 0; z < height; ++z)
						for (uint32_t x = 0; x < w; ++x)
							m_Terrain->SetHole(x, z, holes[z * w + x] != 0);
				}
				m_Terrain->MarkDirty(0.0f, 0.0f, m_Terrain->GetWorldWidth(), m_Terrain->GetWorldHeight());
			}

			std::shared_ptr<TerrainData> m_Terrain;
			std::vector<float> m_BeforeHeights;
			std::vector<uint32_t> m_BeforeSplat;
			std::vector<uint8_t> m_BeforeHoles;
			std::vector<float> m_AfterHeights;
			std::vector<uint32_t> m_AfterSplat;
			std::vector<uint8_t> m_AfterHoles;
			std::string m_Name;
		};

		static const char* OrthoAxisShortName(World::OrthographicAxis axis)
		{
			switch (axis)
			{
			case World::OrthographicAxis::Top:    return "Top";
			case World::OrthographicAxis::Bottom: return "Bottom";
			case World::OrthographicAxis::Front:  return "Front";
			case World::OrthographicAxis::Back:   return "Back";
			case World::OrthographicAxis::Right:  return "Right";
			case World::OrthographicAxis::Left:   return "Left";
			}
			return "Front";
		}

		// Liang-Barsky 严格二维线段裁剪: 确保裁剪后线段严格保持原始斜率与方向, 绝不因轴向暴力 clamp 产生几何畸变
		static bool ClipLineSegment2D(glm::vec2& p0, glm::vec2& p1, float minX, float maxX, float minY, float maxY)
		{
			float t0 = 0.0f, t1 = 1.0f;
			const float dx = p1.x - p0.x;
			const float dy = p1.y - p0.y;

			const auto clipTest = [&](float p, float q) -> bool
			{
				if (p < 0.0f)
				{
					const float r = q / p;
					if (r > t1) return false;
					if (r > t0) t0 = r;
				}
				else if (p > 0.0f)
				{
					const float r = q / p;
					if (r < t0) return false;
					if (r < t1) t1 = r;
				}
				else if (q < 0.0f)
				{
					return false;
				}
				return true;
			};

			if (clipTest(-dx, p0.x - minX) &&
				clipTest(dx, maxX - p0.x) &&
				clipTest(-dy, p0.y - minY) &&
				clipTest(dy, maxY - p0.y))
			{
				const glm::vec2 origP0 = p0;
				p0 = origP0 + t0 * glm::vec2(dx, dy);
				p1 = origP0 + t1 * glm::vec2(dx, dy);
				return true;
			}
			return false;
		}

		// 投影 + 近平面裁剪 + 线段四边形:相机视锥等覆盖层线段共用。
		void PushProjectedSegment(Wui::WuiContext& ctx, const glm::mat4& viewProjection,
			const Wui::WuiRect& viewport, const glm::vec4& clipA, const glm::vec4& clipB,
			const Wui::WuiColor& color, float thickness,
			glm::vec2* boundsMin = nullptr, glm::vec2* boundsMax = nullptr)
		{
			constexpr float kMinW = 0.15f;
			glm::vec4 a = clipA;
			glm::vec4 b = clipB;
			if (a.w < kMinW && b.w < kMinW)
				return;
			if (a.w < kMinW)
			{
				const float t = (kMinW - a.w) / (b.w - a.w);
				a = glm::mix(a, b, t);
			}
			else if (b.w < kMinW)
			{
				const float t = (kMinW - b.w) / (a.w - b.w);
				b = glm::mix(b, a, t);
			}
			const auto project = [&viewport](const glm::vec4& clip)
			{
				const glm::vec3 ndc = glm::vec3(clip) / clip.w;
				return glm::vec2 { viewport.X + (ndc.x + 1.0f) * 0.5f * viewport.W,
					viewport.Y + (1.0f - ndc.y) * 0.5f * viewport.H };
			};
			glm::vec2 from = project(a);
			glm::vec2 to = project(b);

			if (std::isnan(from.x) || std::isnan(from.y) || std::isnan(to.x) || std::isnan(to.y))
				return;

			// 使用 Liang-Barsky 严格保持几何斜率地裁剪至视口矩形
			const float pad = 4.0f;
			if (!ClipLineSegment2D(from, to, viewport.X - pad, viewport.X + viewport.W + pad,
				viewport.Y - pad, viewport.Y + viewport.H + pad))
			{
				return;
			}

			if (boundsMin)
				*boundsMin = glm::min(*boundsMin, glm::min(from, to));
			if (boundsMax)
				*boundsMax = glm::max(*boundsMax, glm::max(from, to));
			const glm::vec2 delta = to - from;
			const float length = glm::length(delta);
			if (length < 0.5f)
				return;
			Wui::LineSegment(ctx, from, to, color, thickness);
		}

		// 相机预览小窗位置:场景图左下角,宽取视口的 28%,16:9。
		Wui::WuiRect CameraPreviewRect(const Wui::WuiRect& sceneRect)
		{
			const float width = std::max(160.0f, sceneRect.W * 0.28f);
			const float height = width * 9.0f / 16.0f;
			const float margin = 10.0f;
			return { sceneRect.X + margin, sceneRect.Y + sceneRect.H - height - margin, width, height };
		}

		// 只读节点(status):AI 能读到,ui.invoke 不会去点它(与脚本面板同一约定)。
		// ---- P4-U6:范围可视化辅助(光源范围 / 碰撞体轮廓共用) ----

		// 世界变换:优先 WorldTransformComponent(带父级链),否则退回本地矩阵。
		glm::mat4 WorldMatrixOf(const entt::registry& registry, entt::entity handle)
		{
			if (const auto* world = registry.try_get<WorldTransformComponent>(handle))
				return world->Matrix;
			if (const auto* transform = registry.try_get<TransformComponent>(handle))
				return transform->GetLocalMatrix();
			return glm::mat4(1.0f);
		}

		// 世界空间线段(两端点)→ 投影成屏幕线段。
		void PushWorldSegment(Wui::WuiContext& ctx, const glm::mat4& viewProjection, const Wui::WuiRect& viewport,
			const glm::mat4& world, const glm::vec3& from, const glm::vec3& to,
			const Wui::WuiColor& color, float thickness, glm::vec2* boundsMin, glm::vec2* boundsMax)
		{
			PushProjectedSegment(ctx, viewProjection, viewport,
				viewProjection * (world * glm::vec4 { from, 1.0f }),
				viewProjection * (world * glm::vec4 { to, 1.0f }),
				color, thickness, boundsMin, boundsMax);
		}

		// 世界空间圆(axis:0=XY 平面 1=XZ 2=YZ),按 40 段折线画成"圆环"。
		// 点光范围 / 球形碰撞体 / 胶囊端面都用它 —— 一套几何代码,不各写一遍。
		void PushWorldCircle(Wui::WuiContext& ctx, const glm::mat4& viewProjection, const Wui::WuiRect& viewport,
			const glm::mat4& world, const glm::vec3& center, float radius, int axis,
			const Wui::WuiColor& color, float thickness, glm::vec2* boundsMin = nullptr, glm::vec2* boundsMax = nullptr)
		{
			constexpr int kSegments = 16;
			constexpr float kTwoPi = 6.28318530718f;
			glm::vec4 previous {};
			for (int i = 0; i <= kSegments; ++i)
			{
				const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(kSegments);
				glm::vec3 offset(0.0f);
				if (axis == 0)
					offset = { std::cos(angle) * radius, std::sin(angle) * radius, 0.0f };
				else if (axis == 1)
					offset = { std::cos(angle) * radius, 0.0f, std::sin(angle) * radius };
				else
					offset = { 0.0f, std::cos(angle) * radius, std::sin(angle) * radius };
				const glm::vec4 clip = viewProjection * (world * glm::vec4 { center + offset, 1.0f });
				if (i > 0)
					PushProjectedSegment(ctx, viewProjection, viewport, previous, clip, color, thickness,
						boundsMin, boundsMax);
				previous = clip;
			}
		}

		// P4-U8a:视口"悬浮层"布局 —— 运行控制 / 视图菜单 / 状态徽标全部**浮在画面上**,
		// 不占布局(否则场景图被挤扁,用户 2026-09-21:「play 三个按钮使视口变形了」)。
		constexpr float kChromePad = 8.0f;     // 悬浮层与画面边缘的距离
		constexpr float kIslandH = 30.0f;      // 悬浮岛屿底板高度
		constexpr float kBtnH = 22.0f;         // 悬浮岛内按钮与控件高度
		constexpr float kPillH = 36.0f;        // 居中药丸(运行控制)的高度
		// 运行控制按钮的稳定无障碍 id:脚本按 id 点,不靠坐标(与菜单/其它按钮同一条口径)。
		const char* const kViewportToolIds[3] = {
			"viewport.tool.play", "viewport.tool.simulate", "viewport.tool.pause" };

		void RegisterReadonlyNode(Wui::WuiId id, const char* kind, const std::string& label,
			const std::string& value, const Wui::WuiRect& rect, const std::string& tooltip = std::string())
		{
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = kind;
			node.Label = label;
			node.Value = value;
			node.Tooltip = tooltip;
			node.Rect = rect;
			node.Interactive = false;
			Wui::WuiAccessibility::Get().Register(node);
		}
	}

	void ViewportPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost&)
	{
		const Wui::WuiTheme& theme = m_Host.Theme();
		m_Host.SetViewportRect(rect);

		if (!m_Root)
		{
			m_Root = std::make_shared<Wui::WuiBox>();
			m_Root->Direction = Wui::WuiDirection::Column;
			m_SceneImage = std::make_shared<Wui::WuiImage>();
			m_SceneImage->Uv = { 0, 1, 1, -1 };
			// 场景图**占满整块面板**:视口尺寸 = 面板尺寸,渲染目标不再被任何工具条挤掉一块。
			m_Root->Add(m_SceneImage, { 0, 1e30f, 0, 1e30f, 1 });

			// 悬浮的运行控制(居中药丸):独立的小布局树,按**绝对矩形**摆放(见 OnRender),
			// 不进场景图布局 —— 悬浮层可以有,但不能改变画面大小。
			m_ToolPill = std::make_shared<Wui::WuiBox>();
			m_ToolPill->Direction = Wui::WuiDirection::Row;
			m_ToolPill->Gap = 4;
			m_ToolPill->AlignMain = Wui::WuiAlign::Center;
			m_ToolPill->AlignCross = Wui::WuiAlign::Center;
			for (int i = 0; i < 3; ++i)
			{
				auto button = std::make_shared<Wui::WuiImageButton>();
				button->Uv = { 0, 1, 1, -1 };
				button->SetId(Wui::HashId(kViewportToolIds[i]));
				m_ToolPill->Add(button, { 28, 28, 28, 28, 0 });
				m_Tools.push_back(button);
			}
		}

		const bool play = m_Host.IsPlaying();
		const bool simulate = m_Host.IsSimulating();
		const bool paused = m_Host.IsPaused();
		const int icons[3] = {
			play ? 1 : 0,
			simulate ? 5 : 4,
			paused ? (simulate ? 7 : 3) : (simulate ? 6 : 2),
		};
		const bool dim[3] = { simulate, play, !play && !simulate };
		// 文案随状态变化:同一个按钮既是"开始"也是"停止",无障碍/提示必须说清当前能做什么。
		const std::string labels[3] = {
			play ? Wui::Tr("viewport.tool.stop_play", "Stop") : Wui::Tr("viewport.tool.play", "Play"),
			simulate ? Wui::Tr("viewport.tool.stop_simulate", "Stop Simulate")
				: Wui::Tr("viewport.tool.simulate", "Simulate"),
			paused ? Wui::Tr("viewport.tool.resume", "Resume") : Wui::Tr("viewport.tool.pause", "Pause"),
		};
		const std::string tooltips[3] = {
			Wui::Tr("viewport.tool.play.tooltip", "Play the scene (F5). Play again to stop."),
			Wui::Tr("viewport.tool.simulate.tooltip",
				"Simulate: run physics/animation without game scripts. Simulate again to stop."),
			Wui::Tr("viewport.tool.pause.tooltip", "Pause / resume the running scene."),
		};
		m_Tools[0]->OnClick = [this] { m_Host.TogglePlay(); };
		m_Tools[1]->OnClick = [this] { m_Host.ToggleSimulate(); };
		m_Tools[2]->OnClick = [this] { m_Host.TogglePause(); };
		for (int i = 0; i < 3; ++i)
		{
			m_Tools[i]->TextureId = m_Host.GetIconId(icons[i]);
			m_Tools[i]->Dim = dim[i];
			m_Tools[i]->Label = labels[i];
			m_Tools[i]->Tooltip = tooltips[i];
		}
		m_SceneImage->TextureId = m_Host.GetSceneTextureId();

		// 面板底色:纯绘制,不进布局树。
		Wui::PanelBackground(ctx, rect, theme.WindowBg);
		Wui::LayoutWidgetTree(m_Root, rect);
		Wui::WuiPaintContext paint(ctx);
		m_Root->Paint(paint);

		const Wui::WuiRect sceneRect = m_SceneImage->Rect();
		// 画面区进无障碍树:脚本/验收可以断言"视口 = 整块面板"(悬浮层不许改变画面尺寸),
		// 也能拿它算视口内坐标给拾取/gizmo 自动化用。
		RegisterReadonlyNode(Wui::HashId("viewport.surface"), "viewport",
			Wui::Tr("viewport.surface", "Viewport"),
			std::to_string(static_cast<int>(sceneRect.W)) + "x" + std::to_string(static_cast<int>(sceneRect.H)),
			sceneRect, Wui::Tr("viewport.surface.tooltip",
				"The rendered surface fills the whole panel; run controls and the view menu float above it."));

		// ---- 悬浮层(chrome):全部画在画面之上,不占布局 ----
		// P4-U7 的覆盖层机制在这里同样适用:整层进 overlay 通道 + 登记覆盖层矩形 →
		// 悬浮控件自己照常命中,而**它下面的视口拾取/gizmo 不会同时吃到这一下点击**。
		ctx.PushOverlay();

				// ② 自适应视口顶栏布局 (3阶弹性自适应):
		// 1) 宽屏 (W >= 880px): 单行豪华布局 [左岛] --- [中岛运行控制药丸] --- [右岛]
		// 2) 中屏 (650px <= W < 880px): 顶行放两端岛屿 [左岛] --- [右岛]，运行药丸优雅浮动于第二行居中展示
		// 3) 窄屏 (W < 650px): 顶行独享 [左岛]；第二行容纳 [运行药丸] 与 [右岛四大功能块]，全要素100%可见防挤压防遮挡
		const bool isWideViewport = (sceneRect.W >= 880.0f);
		const bool isNarrowViewport = (sceneRect.W < 650.0f);

		const float pillW = 3 * 28.0f + 2 * 4.0f + 20.0f; // 112px
		const float pillY = isWideViewport
			? (sceneRect.Y + kChromePad)
			: (sceneRect.Y + kChromePad + kIslandH + 5.0f);
		const float pillX = isNarrowViewport
			? (sceneRect.X + kChromePad)
			: (sceneRect.X + (sceneRect.W - pillW) * 0.5f);
		const Wui::WuiRect pillRect { pillX, pillY, pillW, kPillH };

		const bool pillHovered = ctx.IsHovered(pillRect);
		Wui::Toolbar(ctx, pillRect, theme, kPillH * 0.5f,
			pillHovered ? 0.95f : (play || simulate ? 0.85f : 0.65f));
		Wui::LayoutWidgetTree(m_ToolPill, pillRect);
		m_ToolPill->Paint(paint);
		ctx.RegisterOverlayRect(pillRect);

		// 药丸安全边界防护 (仅在药丸处于顶栏同一行时对两端施加截断)
		const float pillLeftBound = isWideViewport ? (pillRect.X - 10.0f) : (sceneRect.X + sceneRect.W);
		const float pillRightBound = isWideViewport ? (pillRect.X + pillRect.W + 10.0f) : (sceneRect.X);

		// ② 左岛 (Left Island - 四大折叠归纳标准工具组):
		// [相机/方位下拉] + [辅助显示下拉] + [分割线] + [模式切换下拉] + [分割线] + [变换工具下拉]
		const World::ViewportProjection currentProj = m_Host.GetViewportProjection();
		const World::ViewportViewpoint currentVp = m_Host.GetViewportViewpoint();
		const bool isOrtho = (currentProj == World::ViewportProjection::Orthographic);
		const bool camera3D = !isOrtho;
		const char* projName = isOrtho ? "正交" : "透视";
		const char* vpShort = "自由";
		switch (currentVp)
		{
		case World::ViewportViewpoint::Free:   vpShort = "自由"; break;
		case World::ViewportViewpoint::Top:    vpShort = "Top"; break;
		case World::ViewportViewpoint::Bottom: vpShort = "Bottom"; break;
		case World::ViewportViewpoint::Front:  vpShort = "Front"; break;
		case World::ViewportViewpoint::Back:   vpShort = "Back"; break;
		case World::ViewportViewpoint::Right:  vpShort = "Right"; break;
		case World::ViewportViewpoint::Left:   vpShort = "Left"; break;
		}
		const std::string cameraText = std::string(projName) + " · " + vpShort;
		const std::string viewLabel = std::string(projName) + " · " + vpShort + " ▼";
		const float viewButtonW = std::max(78.0f, ctx.MeasureTextWidth(viewLabel, 12.0f) + 14.0f);

		// 显示菜单按钮规格 (纯辅助显示与图元专席)
		const std::string showLabel = Wui::Tr("viewport.show.button", "显示") + " ▼";
		const float showBtnW = std::max(56.0f, ctx.MeasureTextWidth(showLabel, 12.0f) + 16.0f);

		// 视口工作模式折叠下拉规格 (选择 / 地貌 / 植被)
		const EditorToolMode currentToolMode = m_Host.GetEditorToolMode();
		std::string modeShort = Wui::Tr("viewport.mode.select_short", "选择");
		if (currentToolMode == EditorToolMode::Landscape)
			modeShort = Wui::Tr("viewport.mode.landscape_short", "地貌");
		else if (currentToolMode == EditorToolMode::Foliage)
			modeShort = Wui::Tr("viewport.mode.foliage_short", "植被");
		else if (currentToolMode == EditorToolMode::PhysicsPainter)
			modeShort = Wui::Tr("viewport.mode.painter_short", "画笔");
		const std::string modeLabel = (sceneRect.W >= 760.0f) ? (Wui::Tr("viewport.mode.dropdown", "模式") + ": " + modeShort + " ▼") : (modeShort + " ▼");
		const float modeBtnW = std::max(54.0f, ctx.MeasureTextWidth(modeLabel, 12.0f) + 16.0f);

		// Gizmo 变换工具折叠下拉规格 (Q: 选 / W: 移 / E: 转 / R: 缩 / T: 2D矩形)
		const Wui::GizmoOperation activeGizmo = m_Host.GetGizmoOperation();
		std::string gizmoShort = Wui::Tr("viewport.gizmo.q_short", "选择 (Q)");
		switch (activeGizmo)
		{
		case Wui::GizmoOperation::None:      gizmoShort = Wui::Tr("viewport.gizmo.q_short", "选择 (Q)"); break;
		case Wui::GizmoOperation::Translate: gizmoShort = Wui::Tr("viewport.gizmo.w_short", "平移 (W)"); break;
		case Wui::GizmoOperation::Rotate:    gizmoShort = Wui::Tr("viewport.gizmo.e_short", "旋转 (E)"); break;
		case Wui::GizmoOperation::Scale:     gizmoShort = Wui::Tr("viewport.gizmo.r_short", "缩放 (R)"); break;
		case Wui::GizmoOperation::Rect:      gizmoShort = Wui::Tr("viewport.gizmo.t_short", "矩形 (T)"); break;
		}
		const std::string gizmoLabel = (sceneRect.W >= 760.0f) ? (Wui::Tr("viewport.gizmo.dropdown", "工具") + ": " + gizmoShort + " ▼") : (gizmoShort + " ▼");
		const float gizmoBtnW = std::max(64.0f, ctx.MeasureTextWidth(gizmoLabel, 12.0f) + 16.0f);

		// 左岛全宽计算
		Wui::WuiRect viewButton { sceneRect.X + kChromePad, sceneRect.Y + kChromePad + 4.0f, 0.0f, 0.0f };
		Wui::WuiRect showButton { sceneRect.X + kChromePad, sceneRect.Y + kChromePad + 4.0f, 0.0f, 0.0f };
		Wui::WuiRect modeButton { sceneRect.X + kChromePad, sceneRect.Y + kChromePad + 4.0f, 0.0f, 0.0f };
		Wui::WuiRect gizmoButton { sceneRect.X + kChromePad, sceneRect.Y + kChromePad + 4.0f, 0.0f, 0.0f };
		Wui::WuiRect leftIslandRect { sceneRect.X + kChromePad, sceneRect.Y + kChromePad, 0.0f, 0.0f };
		if (!play)
		{
			const float fullLeftIslandW = 4.0f + viewButtonW + 3.0f + showBtnW + 3.0f + 8.0f + 3.0f + modeBtnW + 3.0f + 8.0f + 3.0f + gizmoBtnW + 4.0f;
			const float leftIslandX = sceneRect.X + kChromePad;
			const float leftIslandY = sceneRect.Y + kChromePad;
			const float minGuaranteedW = viewButtonW + 14.0f + modeBtnW + 12.0f;
			const float maxLeftIslandW = isWideViewport ? std::max(viewButtonW + 14.0f + modeBtnW + 12.0f, pillLeftBound - leftIslandX) : fullLeftIslandW;
			const float leftIslandW = std::min(fullLeftIslandW, maxLeftIslandW);

			leftIslandRect = { leftIslandX, leftIslandY, leftIslandW, kIslandH };
			Wui::Toolbar(ctx, leftIslandRect, theme, 6.0f, 0.85f);
			ctx.RegisterOverlayRect(leftIslandRect);

			const float leftBtnY = leftIslandRect.Y + 4.0f;
			viewButton = { leftIslandRect.X + 4.0f, leftBtnY, viewButtonW, kBtnH };
			const std::string viewHint = Wui::Tr("viewport.view.hint",
				"相机视图设置: 投影方式(透视/正交)、方位、旋转锁定与视角复位。");
			Wui::Tooltip(ctx, viewButton, viewHint);
			auto viewBtnTheme = theme;
			if (ctx.IsPopupOpen(Wui::HashId("viewport.view.popup")))
			{
				viewBtnTheme.ButtonBg = theme.ActiveBg;
				viewBtnTheme.Border = theme.Accent;
			}
			if (Wui::Button(ctx, Wui::HashId("viewport.view"), viewButton, viewLabel, viewBtnTheme))
			{
				if (ctx.IsPopupOpen(Wui::HashId("viewport.view.popup")))
				{
					ctx.ClosePopup(Wui::HashId("viewport.view.popup"));
				}
				else
				{
					ctx.ClosePopup(Wui::HashId("viewport.show.popup"));
					ctx.ClosePopup(Wui::HashId("viewport.mode.popup"));
					ctx.ClosePopup(Wui::HashId("viewport.gizmo.popup"));
					ctx.OpenPopup(Wui::HashId("viewport.view.popup"));
				}
			}
			RegisterReadonlyNode(Wui::HashId("viewport.camera.mode"), "text", cameraText,
				camera3D ? "3d" : "2d", viewButton,
				Wui::Tr("viewport.camera.hint", "Viewport view mode — switch in the View menu (top-left)."));
			ctx.RegisterOverlayRect(viewButton);

			float leftCursor = viewButton.X + viewButton.W + 3.0f;

			// 显示菜单按钮 (Show: 纯辅助显示与图元专席，对标 UE5 / Unity)
			if (leftCursor + showBtnW <= leftIslandRect.X + leftIslandRect.W - 4.0f)
			{
				showButton = { leftCursor, leftBtnY, showBtnW, kBtnH };
				const std::string showHint = Wui::Tr("viewport.show.hint",
					"辅助显示与图元选项: 基准网格、空间分区、碰撞体、音源、CSM 阴影、3D 导航网格。");
				Wui::Tooltip(ctx, showButton, showHint);
				auto showBtnTheme = theme;
				if (ctx.IsPopupOpen(Wui::HashId("viewport.show.popup")))
				{
					showBtnTheme.ButtonBg = theme.ActiveBg;
					showBtnTheme.Border = theme.Accent;
				}
				if (Wui::Button(ctx, Wui::HashId("viewport.show"), showButton, showLabel, showBtnTheme))
				{
					if (ctx.IsPopupOpen(Wui::HashId("viewport.show.popup")))
					{
						ctx.ClosePopup(Wui::HashId("viewport.show.popup"));
						m_ActiveViewSubmenu.clear();
					}
					else
					{
						ctx.ClosePopup(Wui::HashId("viewport.view.popup"));
						ctx.ClosePopup(Wui::HashId("viewport.mode.popup"));
						ctx.ClosePopup(Wui::HashId("viewport.gizmo.popup"));
						ctx.OpenPopup(Wui::HashId("viewport.show.popup"));
					}
				}
				ctx.RegisterOverlayRect(showButton);
				leftCursor += showBtnW + 3.0f;
			}

			// 分割线
			if (leftCursor + 8.0f + modeBtnW <= leftIslandRect.X + leftIslandRect.W - 4.0f)
			{
				const Wui::WuiRect sepRect { leftCursor, leftIslandRect.Y, 8.0f, kIslandH };
				Wui::ToolbarSeparator(ctx, sepRect, theme);
				leftCursor += 8.0f + 3.0f;
			}

			// 模式切换折叠下拉按钮 (模式: 选择 / 地貌 / 植被)
			if (leftCursor + modeBtnW <= leftIslandRect.X + leftIslandRect.W - 4.0f)
			{
				modeButton = { leftCursor, leftBtnY, modeBtnW, kBtnH };
				auto modeTheme = theme;
				if (ctx.IsPopupOpen(Wui::HashId("viewport.mode.popup")) || currentToolMode != EditorToolMode::Select)
				{
					modeTheme.ButtonBg = theme.ActiveBg;
					modeTheme.Border = theme.Accent;
				}
				Wui::Tooltip(ctx, modeButton, Wui::Tr("viewport.mode.hint",
					"视口工作模式切换 (选择与通用变换 [Shift+1] / 地貌雕刻 [Shift+2] / 植被散布 [Shift+3])"));
				if (Wui::Button(ctx, Wui::HashId("viewport.mode.dropdown_btn"), modeButton, modeLabel, modeTheme))
				{
					if (ctx.IsPopupOpen(Wui::HashId("viewport.mode.popup")))
					{
						ctx.ClosePopup(Wui::HashId("viewport.mode.popup"));
					}
					else
					{
						ctx.ClosePopup(Wui::HashId("viewport.view.popup"));
						ctx.ClosePopup(Wui::HashId("viewport.show.popup"));
						ctx.ClosePopup(Wui::HashId("viewport.gizmo.popup"));
						ctx.OpenPopup(Wui::HashId("viewport.mode.popup"));
					}
				}
				ctx.RegisterOverlayRect(modeButton);
				leftCursor += modeBtnW + 3.0f;
			}

			// 分割线
			if (leftCursor + 8.0f + gizmoBtnW <= leftIslandRect.X + leftIslandRect.W - 4.0f)
			{
				const Wui::WuiRect sepRect { leftCursor, leftIslandRect.Y, 8.0f, kIslandH };
				Wui::ToolbarSeparator(ctx, sepRect, theme);
				leftCursor += 8.0f + 3.0f;
			}

			// 变换工具折叠下拉按钮 (工具: 移动 (W) 等)
			if (leftCursor + gizmoBtnW <= leftIslandRect.X + leftIslandRect.W - 4.0f)
			{
				gizmoButton = { leftCursor, leftBtnY, gizmoBtnW, kBtnH };
				auto gTheme = theme;
				if (ctx.IsPopupOpen(Wui::HashId("viewport.gizmo.popup")) || activeGizmo != Wui::GizmoOperation::None)
				{
					gTheme.ButtonBg = theme.ActiveBg;
					gTheme.Border = theme.Accent;
				}
				Wui::Tooltip(ctx, gizmoButton, Wui::Tr("viewport.gizmo.hint",
					"变换工具切换 (选择 [Q] / 平移 [W] / 旋转 [E] / 缩放 [R] / 2D矩形 [T])"));
				if (Wui::Button(ctx, Wui::HashId("viewport.gizmo.dropdown_btn"), gizmoButton, gizmoLabel, gTheme))
				{
					if (ctx.IsPopupOpen(Wui::HashId("viewport.gizmo.popup")))
					{
						ctx.ClosePopup(Wui::HashId("viewport.gizmo.popup"));
					}
					else
					{
						ctx.ClosePopup(Wui::HashId("viewport.view.popup"));
						ctx.ClosePopup(Wui::HashId("viewport.show.popup"));
						ctx.ClosePopup(Wui::HashId("viewport.mode.popup"));
						ctx.OpenPopup(Wui::HashId("viewport.gizmo.popup"));
					}
				}
				ctx.RegisterOverlayRect(gizmoButton);
				leftCursor += gizmoBtnW + 3.0f;
			}
		}

		// ② 右岛 (Right Island - 四大归纳标准功能块):
		// [块1: 过滤] + [分割线] + [块2: 图层] + [分割线] + [块3: 吸附与对齐] + [分割线] + [块4: 运行状态]
		SelectionFilter& selFilter = m_Host.GetSelectionFilter();
		size_t selectableLayersCount = 0;
		size_t lockedLayersCount = 0;
		const uint32_t allFilterLayers[] = {
			Layer_Actor, Layer_Light, Layer_Trigger, Layer_Collider,
			Layer_Decor, Layer_Background, Layer_UI
		};
		constexpr size_t kTotalFilterLayers = 7;
		for (uint32_t mask : allFilterLayers)
		{
			if (selFilter.IsLayerSelectable(mask) && !selFilter.IsLayerLocked(mask))
				selectableLayersCount++;
			if (selFilter.IsLayerLocked(mask))
				lockedLayersCount++;
		}

		// 块1: 过滤下拉规格
		char filterBtnLabel[64] = {};
		std::snprintf(filterBtnLabel, sizeof(filterBtnLabel), "%s: %zu/%zu ▼",
			Wui::Tr("viewport.filter.dropdown", "过滤").c_str(), selectableLayersCount, kTotalFilterLayers);
		const float filterBtnW = std::max(68.0f, ctx.MeasureTextWidth(filterBtnLabel, 12.0f) + 16.0f);

		// 块2: 图层下拉规格
		const std::vector<Level::DataLayerDefinition>& dataLayers = m_Host.GetLevelDataLayers();
		const uint32_t activeLayers = m_Host.GetActiveDataLayers();
		const bool hasDataLayers = !dataLayers.empty();
		size_t activeCount = 0;
		if (hasDataLayers)
		{
			for (const auto& layer : dataLayers)
			{
				if (!layer.Name.empty() && (layer.LayerMask & activeLayers) != 0)
					activeCount++;
			}
		}
		char layersLabel[64] = {};
		if (hasDataLayers)
		{
			std::snprintf(layersLabel, sizeof(layersLabel), "%s: %zu/%zu ▼",
				Wui::Tr("viewport.layers.dropdown", "图层").c_str(), activeCount, dataLayers.size());
		}
		else
		{
			std::snprintf(layersLabel, sizeof(layersLabel), "%s ▼",
				Wui::Tr("viewport.layers.dropdown", "图层").c_str());
		}
		const float layersBtnW = std::max(56.0f, ctx.MeasureTextWidth(layersLabel, 12.0f) + 16.0f);

		// 块3: 吸附与对齐归纳下拉规格 (合并 Grid, Part, Snap, CSM, Lock)
		const std::string snapLabel = (sceneRect.W >= 760.0f) ? (Wui::Tr("viewport.snap.dropdown", "吸附与对齐") + " ▼") : (Wui::Tr("viewport.snap.short", "吸附") + " ▼");
		const float snapBtnW = std::max(56.0f, ctx.MeasureTextWidth(snapLabel, 12.0f) + 16.0f);

		// 块4: 运行状态徽章规格
		const char* const modeKey = simulate ? (paused ? "paused" : "simulate")
			: (play ? (paused ? "paused" : "play") : "edit");
		const std::string modeText = paused ? Wui::Tr("viewport.mode.paused", "Paused")
			: (simulate ? Wui::Tr("viewport.mode.simulate", "Simulating")
				: (play ? Wui::Tr("viewport.mode.play", "Playing")
					: Wui::Tr("viewport.mode.edit", "Edit Mode")));
		const Wui::WuiColor modeColor = paused ? theme.Warning
			: (simulate ? theme.Accent : (play ? theme.Success : theme.TextDisabled));
		const float modeWidth = ctx.MeasureTextWidth(modeText, 11.5f) + 18.0f;

		// 计算右岛理想总宽度 (4个功能块 + 3根分割线 + 边距)
		float naturalRightIslandW = 4.0f;
		naturalRightIslandW += filterBtnW + 3.0f + 8.0f + 3.0f; // 块1: 过滤 + 分割线
		naturalRightIslandW += layersBtnW + 3.0f + 8.0f + 3.0f; // 块2: 图层 + 分割线
		naturalRightIslandW += snapBtnW + 3.0f + 8.0f + 3.0f;   // 块3: 吸附与对齐 + 分割线
		naturalRightIslandW += modeWidth + 4.0f;                 // 块4: 运行状态徽章

		float rightIslandW = naturalRightIslandW;
		float rightIslandX = sceneRect.X + sceneRect.W - kChromePad - rightIslandW;

		if (isWideViewport && rightIslandX < pillRightBound)
		{
			rightIslandX = pillRightBound;
			rightIslandW = std::max(0.0f, (sceneRect.X + sceneRect.W - kChromePad) - rightIslandX);
		}
		else if (isNarrowViewport && rightIslandX < pillRect.X + pillRect.W + 8.0f)
		{
			rightIslandX = pillRect.X + pillRect.W + 8.0f;
			rightIslandW = std::max(0.0f, (sceneRect.X + sceneRect.W - kChromePad) - rightIslandX);
		}

		const float rightIslandY = isNarrowViewport
			? (sceneRect.Y + kChromePad + kIslandH + 5.0f)
			: (sceneRect.Y + kChromePad);
		Wui::WuiRect rightIslandRect { rightIslandX, rightIslandY, rightIslandW, kIslandH };
		if (!play)
		{
			Wui::Toolbar(ctx, rightIslandRect, theme, 6.0f, 0.85f);
			ctx.RegisterOverlayRect(rightIslandRect);
		}

		float rightCursor = rightIslandRect.X + 4.0f;
		const float rightBtnY = rightIslandRect.Y + 4.0f;
		const float rightLimit = rightIslandRect.X + rightIslandRect.W - 4.0f;

		// 块1: 实体选择过滤器下拉 (Selection Filter)
		Wui::WuiRect filterBtnRect {};
		if (!play && rightCursor + filterBtnW <= rightLimit)
		{
			filterBtnRect = { rightCursor, rightBtnY, filterBtnW, kBtnH };
			Wui::Tooltip(ctx, filterBtnRect, Wui::Tr("viewport.filter.hint",
				"选择过滤器: 控制视口内哪些图层的实体可以被鼠标点选。点击展开配置。"));
			auto filterTheme = theme;
			if (selectableLayersCount < kTotalFilterLayers || lockedLayersCount > 0)
			{
				filterTheme.ButtonBg = theme.ActiveBg;
				filterTheme.Border = theme.Warning;
			}
			if (Wui::Button(ctx, Wui::HashId("viewport.filter_dropdown"), filterBtnRect, filterBtnLabel, filterTheme))
			{
				if (ctx.IsPopupOpen(Wui::HashId("viewport.filter.popup")))
					ctx.ClosePopup(Wui::HashId("viewport.filter.popup"));
				else
				{
					ctx.ClosePopup(Wui::HashId("viewport.layers.popup"));
					ctx.ClosePopup(Wui::HashId("viewport.snap.popup"));
					ctx.OpenPopup(Wui::HashId("viewport.filter.popup"));
				}
			}
			ctx.RegisterOverlayRect(filterBtnRect);
			rightCursor += filterBtnW + 3.0f;

			if (rightCursor + 8.0f <= rightLimit)
			{
				const Wui::WuiRect sepRect { rightCursor, rightIslandRect.Y, 8.0f, kIslandH };
				Wui::ToolbarSeparator(ctx, sepRect, theme);
				rightCursor += 8.0f + 3.0f;
			}
		}

		// 块2: 关卡数据层下拉 (Data Layers)
		Wui::WuiRect layersBtnRect {};
		if (!play && rightCursor + layersBtnW <= rightLimit)
		{
			layersBtnRect = { rightCursor, rightBtnY, layersBtnW, kBtnH };
			Wui::Tooltip(ctx, layersBtnRect, Wui::Tr("viewport.layers.hint", "管理关卡数据层流送与可见性。"));
			auto layersTheme = theme;
			if (ctx.IsPopupOpen(Wui::HashId("viewport.layers.popup")))
			{
				layersTheme.ButtonBg = theme.ActiveBg;
				layersTheme.Border = theme.Accent;
			}
			if (Wui::Button(ctx, Wui::HashId("viewport.layers_dropdown"), layersBtnRect, layersLabel, layersTheme))
			{
				if (ctx.IsPopupOpen(Wui::HashId("viewport.layers.popup")))
					ctx.ClosePopup(Wui::HashId("viewport.layers.popup"));
				else
				{
					ctx.ClosePopup(Wui::HashId("viewport.filter.popup"));
					ctx.ClosePopup(Wui::HashId("viewport.snap.popup"));
					ctx.OpenPopup(Wui::HashId("viewport.layers.popup"));
				}
			}
			ctx.RegisterOverlayRect(layersBtnRect);
			rightCursor += layersBtnW + 3.0f;

			if (rightCursor + 8.0f <= rightLimit)
			{
				const Wui::WuiRect sepRect { rightCursor, rightIslandRect.Y, 8.0f, kIslandH };
				Wui::ToolbarSeparator(ctx, sepRect, theme);
				rightCursor += 8.0f + 3.0f;
			}
		}

		// 块3: 吸附与对齐归纳下拉 (Snapping & Reference Alignment)
		Wui::WuiRect snapBtnRect {};
		if (!play && rightCursor + snapBtnW <= rightLimit)
		{
			snapBtnRect = { rightCursor, rightBtnY, snapBtnW, kBtnH };
			Wui::Tooltip(ctx, snapBtnRect, Wui::Tr("viewport.snap.hint",
				"空间吸附与参考对齐设置: 包含地面网格、世界分区网格、2D像素吸附、CSM级联调试与旋转锁定。"));
			auto snapTheme = theme;
			const bool isSnapActive = m_Host.IsPixelSnapEnabled() || m_ShowWorldPartition ||
				m_Host.Is2DMode() || World::Renderer3D::IsCascadeDebugVisualization() ||
				IsMeshletClustersVisible() ||
				ctx.IsPopupOpen(Wui::HashId("viewport.snap.popup"));
			if (isSnapActive)
			{
				snapTheme.ButtonBg = theme.ActiveBg;
				snapTheme.Border = theme.Accent;
			}
			if (Wui::Button(ctx, Wui::HashId("viewport.snap.dropdown_btn"), snapBtnRect, snapLabel, snapTheme))
			{
				if (ctx.IsPopupOpen(Wui::HashId("viewport.snap.popup")))
					ctx.ClosePopup(Wui::HashId("viewport.snap.popup"));
				else
				{
					ctx.ClosePopup(Wui::HashId("viewport.filter.popup"));
					ctx.ClosePopup(Wui::HashId("viewport.layers.popup"));
					ctx.OpenPopup(Wui::HashId("viewport.snap.popup"));
				}
			}
			ctx.RegisterOverlayRect(snapBtnRect);
			rightCursor += snapBtnW + 3.0f;

			if (rightCursor + 8.0f <= rightLimit)
			{
				const Wui::WuiRect sepRect { rightCursor, rightIslandRect.Y, 8.0f, kIslandH };
				Wui::ToolbarSeparator(ctx, sepRect, theme);
				rightCursor += 8.0f + 3.0f;
			}
		}

		// 块4: 运行状态徽章 (viewport.mode)
		if (!play && rightCursor + modeWidth <= rightIslandRect.X + rightIslandRect.W + 2.0f)
		{
			const Wui::WuiRect modeRect { rightCursor, rightBtnY, modeWidth, kBtnH };
			Wui::WuiColor modeFill = modeColor;
			modeFill.A = modeKey[0] == 'e' ? 0.12f : 0.22f;
			Wui::Badge(ctx, modeRect, modeText, modeFill, modeColor, theme, 11.5f, true, 4.0f);
			RegisterReadonlyNode(Wui::HashId("viewport.mode"), "text", modeText, modeKey, modeRect);
			ctx.RegisterOverlayRect(modeRect);
		}

		// ③ 纯相机视角弹层 (View Popup: 仅相机投影、方位、旋转锁定与视角复位，零冗余)
		const Wui::WuiId viewPopup = Wui::HashId("viewport.view.popup");
		if (ctx.IsPopupOpen(viewPopup))
		{
			const float rowH = 22.0f;
			const float comboH = 24.0f;
			const float panelH = 6.0f + (comboH * 2.0f + 14.0f) + (rowH + 4.0f) + 7.0f + rowH + 6.0f;
			const Wui::WuiRect parentPanel { viewButton.X, leftIslandRect.Y + leftIslandRect.H + 3.0f, 240.0f, panelH };
			Wui::DrawPanelSurface(ctx, parentPanel, theme);
			ctx.RegisterOverlayRect(parentPanel);
			float rowY = parentPanel.Y + 6.0f;

			// ① 投影方式下拉选项 (Projection)
			{
				const Wui::WuiRect comboRect { parentPanel.X + 8.0f, rowY, parentPanel.W - 16.0f, comboH };
				rowY += comboH + 6.0f;
				const std::vector<std::string> projOptions = {
					Wui::Tr("viewport.view.proj_persp", "透视 (Perspective)"),
					Wui::Tr("viewport.view.proj_ortho", "正交 (Orthographic)")
				};
				int selectedProj = (currentProj == World::ViewportProjection::Orthographic) ? 1 : 0;
				if (Wui::Combo(ctx, Wui::HashId("viewport.combo.proj"), comboRect,
					Wui::Tr("viewport.view.proj_label", "投影"), projOptions, selectedProj, theme))
				{
					m_Host.SetViewportProjection(selectedProj == 1 ? World::ViewportProjection::Orthographic : World::ViewportProjection::Perspective);
				}
				ctx.RegisterOverlayRect(comboRect);
			}

			// ② 观察方位下拉选项 (Viewpoint)
			{
				const Wui::WuiRect comboRect { parentPanel.X + 8.0f, rowY, parentPanel.W - 16.0f, comboH };
				rowY += comboH + 8.0f;
				const std::vector<std::string> vpOptions = {
					Wui::Tr("viewport.vp.free", "自由视角 (Free Orbit)"),
					Wui::Tr("viewport.vp.top", "顶视图 Top (XZ [俯视])"),
					Wui::Tr("viewport.vp.front", "前视图 Front (XY [立面])"),
					Wui::Tr("viewport.vp.right", "右视图 Right (ZY [侧面])"),
					Wui::Tr("viewport.vp.bottom", "底视图 Bottom (XZ [仰视])"),
					Wui::Tr("viewport.vp.back", "后视图 Back (XY [背面])"),
					Wui::Tr("viewport.vp.left", "左视图 Left (ZY [侧面])")
				};
				int selectedVp = 0;
				switch (currentVp)
				{
				case World::ViewportViewpoint::Free:   selectedVp = 0; break;
				case World::ViewportViewpoint::Top:    selectedVp = 1; break;
				case World::ViewportViewpoint::Front:  selectedVp = 2; break;
				case World::ViewportViewpoint::Right:  selectedVp = 3; break;
				case World::ViewportViewpoint::Bottom: selectedVp = 4; break;
				case World::ViewportViewpoint::Back:   selectedVp = 5; break;
				case World::ViewportViewpoint::Left:   selectedVp = 6; break;
				}
				if (Wui::Combo(ctx, Wui::HashId("viewport.combo.vp"), comboRect,
					Wui::Tr("viewport.view.vp_label", "方位"), vpOptions, selectedVp, theme))
				{
					World::ViewportViewpoint newVp = World::ViewportViewpoint::Free;
					switch (selectedVp)
					{
					case 0: newVp = World::ViewportViewpoint::Free; break;
					case 1: newVp = World::ViewportViewpoint::Top; break;
					case 2: newVp = World::ViewportViewpoint::Front; break;
					case 3: newVp = World::ViewportViewpoint::Right; break;
					case 4: newVp = World::ViewportViewpoint::Bottom; break;
					case 5: newVp = World::ViewportViewpoint::Back; break;
					case 6: newVp = World::ViewportViewpoint::Left; break;
					}
					m_Host.SetViewportViewpoint(newVp);
				}
				ctx.RegisterOverlayRect(comboRect);
			}

			// ③ 视角鼠标旋转锁定开关
			{
				const Wui::WuiRect lockRow { parentPanel.X + 4.0f, rowY, parentPanel.W - 8.0f, rowH };
				rowY += rowH + 4.0f;
				const bool locked = m_Host.Is2DMode();
				const std::string lockTip = Wui::Tr("viewport.view.lock_rotation.tooltip",
					"锁定鼠标旋转视角 (右键拖拽改为平移画布); 左上角『方位』下拉菜单仍可正常切换方向。");
				Wui::Tooltip(ctx, lockRow, lockTip);
				if (Wui::MenuItem(ctx, Wui::HashId("viewport.view.lock_rotation"), lockRow,
					Wui::Tr("viewport.view.lock_rotation", "锁定鼠标旋转 (Lock Rotation)"), locked, true, theme))
				{
					m_Host.Set2DMode(!locked);
				}
				ctx.RegisterOverlayRect(lockRow);
			}

			// 分割线
			Wui::PanelBackground(ctx, { parentPanel.X + 8.0f, rowY, parentPanel.W - 16.0f, 1.0f }, theme.Border, 0.0f);
			rowY += 6.0f;

			// ④ 视角复位项
			{
				const Wui::WuiRect resetRow { parentPanel.X + 4.0f, rowY, parentPanel.W - 8.0f, rowH };
				rowY += rowH;
				Wui::Tooltip(ctx, resetRow, Wui::Tr("viewport.view.reset.tooltip", "复位视角到选中目标或世界中心"));
				if (Wui::MenuItem(ctx, Wui::HashId("viewport.view.reset"), resetRow,
					Wui::Tr("viewport.view.reset", "复位视角到原点"), false, true, theme))
				{
					m_Host.ResetViewportCamera();
					ctx.ClosePopup(viewPopup);
				}
				ctx.RegisterOverlayRect(resetRow);
			}

			const bool insideParent = parentPanel.W > 0.0f && ctx.HitTestRaw(parentPanel, ctx.Input().MousePos);
			const bool insideButton = ctx.HitTestRaw(viewButton, ctx.Input().MousePos);
			const bool comboOpen = ctx.IsPopupOpen(Wui::HashId("viewport.combo.proj")) ||
			                       ctx.IsPopupOpen(Wui::HashId("viewport.combo.vp"));
			if (!comboOpen && (ctx.Input().MouseClicked[0] || ctx.Input().MouseClicked[1]) && !insideParent && !insideButton)
			{
				ctx.ClosePopup(viewPopup);
			}
			if (ctx.IsKeyPressed(KeyCodes::Escape))
			{
				ctx.ClosePopup(viewPopup);
			}
		}

		// ④ 纯辅助显示与图元专属弹层 (Show Popup: 基准网格、辅助图元、3D导航三大二级子菜单)
		const Wui::WuiId showPopup = Wui::HashId("viewport.show.popup");
		if (ctx.IsPopupOpen(showPopup))
		{
			const float rowH = 22.0f;
			const float panelH = 6.0f + rowH + (4.0f * rowH) + 7.0f + (4.0f * rowH) + 6.0f;
			const float px = showButton.W > 0.0f ? showButton.X : (viewButton.X + viewButton.W + 3.0f);
			const Wui::WuiRect parentPanel { px, leftIslandRect.Y + leftIslandRect.H + 3.0f, 246.0f, panelH };
			Wui::DrawPanelSurface(ctx, parentPanel, theme);
			ctx.RegisterOverlayRect(parentPanel);
			float rowY = parentPanel.Y + 6.0f;

			Wui::Label(ctx, { parentPanel.X + 8.0f, rowY + 3.0f },
				Wui::Tr("viewport.view.group.overlays", "辅助显示与图元"), theme.TextMuted, 11.5f);
			rowY += rowH;

			Wui::WuiRect activeSubmenuRow {};
			const auto catRow = [&](const char* catId, const std::string& label, const std::string& subKey)
			{
				const Wui::WuiRect row { parentPanel.X + 4.0f, rowY, parentPanel.W - 8.0f, rowH };
				rowY += rowH;
				const bool active = (m_ActiveViewSubmenu == subKey);
				const bool hov = ctx.IsHovered(row);
				if (hov || active)
				{
					Wui::PanelBackground(ctx, row, theme.ButtonHover, 2.0f);
				}
				const bool clicked = Wui::MenuItem(ctx, Wui::HashId(catId), row, label, true, theme);
				if (hov || clicked)
				{
					m_ActiveViewSubmenu = subKey;
				}
				const float arrowW = ctx.MeasureTextWidth("▶", 11.0f);
				Wui::Label(ctx, { row.X + row.W - arrowW - 6.0f, row.Y + (rowH - 11.0f) * 0.5f }, "▶",
					(active || hov) ? theme.Accent : theme.TextMuted, 11.0f);
				ctx.RegisterOverlayRect(row);
				if (m_ActiveViewSubmenu == subKey)
				{
					activeSubmenuRow = row;
				}
			};

			catRow("viewport.show.cat.grids", Wui::Tr("viewport.menu.grids", "空间基准与网格"), "grids");
			catRow("viewport.show.cat.gizmos", Wui::Tr("viewport.menu.gizmos", "辅助图元与范围"), "gizmos");
			catRow("viewport.show.cat.lod", Wui::Tr("viewport.menu.lod_octree", "网格 LOD 与 Meshlet 簇"), "lod");
			catRow("viewport.show.cat.navmesh", Wui::Tr("viewport.menu.navigation", "3D 导航与寻路"), "navmesh");

			// 分割线
			Wui::PanelBackground(ctx, { parentPanel.X + 8.0f, rowY, parentPanel.W - 16.0f, 1.0f }, theme.Border, 0.0f);
			rowY += 6.0f;

			// 常用快速开关项
			const auto quickItem = [&](const char* id, const std::string& label, bool checked, const std::function<void()>& action)
			{
				const Wui::WuiRect row { parentPanel.X + 4.0f, rowY, parentPanel.W - 8.0f, rowH };
				rowY += rowH;
				if (Wui::MenuItem(ctx, Wui::HashId(id), row, label, checked, true, theme))
					action();
				ctx.RegisterOverlayRect(row);
			};
			quickItem("viewport.show.quick.grid", Wui::Tr("viewport.view.reference_grid", "地面参考网格 (Ground Grid)"),
				m_Host.IsReferenceGridEnabled(), [this] { m_Host.SetReferenceGridEnabled(!m_Host.IsReferenceGridEnabled()); });
			quickItem("viewport.show.quick.navmesh", Wui::Tr("viewport.view.navmesh", "3D 导航网格 (NavMesh) [P]"),
				s_ShowNavMesh, [] { SetNavMeshVisible(!s_ShowNavMesh); });
			quickItem("viewport.show.quick.octree", Wui::Tr("viewport.view.octree_bounds", "空间八叉树包围盒 (Octree)"),
				s_ShowOctreeBounds, [] { s_ShowOctreeBounds = !s_ShowOctreeBounds; });
			quickItem("viewport.show.quick.lod_color", Wui::Tr("viewport.view.lod_coloration", "LOD 阶梯染色 (LOD Color)"),
				MeshLODManager::Get().IsLODColorationEnabled(), [] {
					MeshLODManager::Get().SetLODColorationEnabled(!MeshLODManager::Get().IsLODColorationEnabled());
				});
			quickItem("viewport.show.quick.meshlet_clusters", Wui::Tr("viewport.view.meshlet_clusters", "Meshlet 集群视图 (Meshlet Clusters)"),
				IsMeshletClustersVisible(), [] { SetMeshletClustersVisible(!IsMeshletClustersVisible()); });

			// 二级子菜单渲染体 (Submenus)
			Wui::WuiRect subPanel {};
			if (!m_ActiveViewSubmenu.empty() && activeSubmenuRow.W > 0.0f)
			{
				float subW = 246.0f;
				float subH = 80.0f;
				if (m_ActiveViewSubmenu == "grids")
					subH = 6.0f + 3.0f * rowH + 6.0f;
				else if (m_ActiveViewSubmenu == "gizmos")
					subH = 6.0f + 5.0f * rowH + 6.0f;
				else if (m_ActiveViewSubmenu == "lod")
					subH = 6.0f + 4.0f * rowH + 6.0f;
				else if (m_ActiveViewSubmenu == "navmesh")
					subH = 6.0f + 6.0f * rowH + 6.0f;

				const glm::vec2 winVp = ctx.ViewportSize();
				float subX = parentPanel.X + parentPanel.W - 2.0f;
				if (subX + subW > winVp.x - 6.0f)
					subX = parentPanel.X - subW + 2.0f;
				subX = std::max(6.0f, std::min(subX, winVp.x - subW - 6.0f));
				float subY = std::max(6.0f, std::min(activeSubmenuRow.Y - 2.0f, winVp.y - subH - 6.0f));
				subPanel = { subX, subY, subW, subH };

				Wui::DrawPanelSurface(ctx, subPanel, theme);
				ctx.RegisterOverlayRect(subPanel);
				float subRowY = subPanel.Y + 6.0f;

				const auto subItem = [&](const char* id, const std::string& label, bool checked,
					const std::string& tooltip, const std::function<void()>& action)
				{
					const Wui::WuiRect row { subPanel.X + 4.0f, subRowY, subPanel.W - 8.0f, rowH };
					subRowY += rowH;
					if (!tooltip.empty())
						Wui::Tooltip(ctx, row, tooltip);
					if (Wui::MenuItem(ctx, Wui::HashId(id), row, label, checked, true, theme))
						action();
					if (!tooltip.empty())
					{
						Wui::WuiAccessNode node;
						node.Id = Wui::HashId(id);
						node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
						node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
						node.Kind = "menu-item";
						node.Label = label;
						node.Value = checked ? "checked" : "unchecked";
						node.Tooltip = tooltip;
						node.Rect = row;
						Wui::WuiAccessibility::Get().Register(node);
					}
					ctx.RegisterOverlayRect(row);
				};

				if (m_ActiveViewSubmenu == "grids")
				{
					subItem("viewport.view.reference_grid", Wui::Tr("viewport.view.reference_grid", "地面参考网格 (1m/10m)"),
						m_Host.IsReferenceGridEnabled(), Wui::Tr("viewport.grid.hint", "开关地面参考基准网格 (1m/10m)。"),
						[this] { m_Host.SetReferenceGridEnabled(!m_Host.IsReferenceGridEnabled()); });
					subItem("viewport.view.world_partition", Wui::Tr("viewport.view.world_partition", "世界分区流送网格"),
						m_ShowWorldPartition, Wui::Tr("viewport.partition.hint", "开关大世界空间分区与流送区块辅助网格。"),
						[this] { m_ShowWorldPartition = !m_ShowWorldPartition; });
					subItem("viewport.view.pixel_snap", Wui::Tr("viewport.snap.pixel_snap", "2D 像素网格吸附"),
						m_Host.IsPixelSnapEnabled(), Wui::Tr("viewport.snap.hint", "按 1/PPU 步长吸附坐标，杜绝亚像素插值撕裂与接缝抖动。"),
						[this] { m_Host.SetPixelSnapEnabled(!m_Host.IsPixelSnapEnabled()); });
				}
				else if (m_ActiveViewSubmenu == "gizmos")
				{
					subItem("viewport.view.camera_preview", Wui::Tr("viewport.view.camera_preview", "相机视锥与画中画"),
						m_Host.IsCameraPreviewEnabled(), Wui::Tr("viewport.view.camera_preview.tooltip", "在视口角落显示场景活动主相机的 PiP 实时预览画面。"),
						[this] { m_Host.ToggleCameraPreview(); });

					const bool rangesAll = Editor::EditorPreferences::Get().Data().ViewportLightRangesAll;
					subItem("viewport.view.light_ranges", Wui::Tr("viewport.view.light_ranges", "光源范围: 全部"),
						rangesAll, Wui::Tr("viewport.overlays.light_ranges.tooltip", "画光源范围: 点光 = Range 球; 平行光 = Direction 箭头。关 = 仅选中实体。"),
						[] {
							Editor::EditorPreferences& prefs = Editor::EditorPreferences::Get();
							prefs.SetViewportLightRangesAll(!prefs.Data().ViewportLightRangesAll);
						});

					const bool collidersEnabled = Editor::EditorPreferences::Get().Data().ViewportColliderOutlinesEnabled;
					subItem("viewport.view.collider_outlines_enabled", Wui::Tr("viewport.view.collider_outlines_enabled", "碰撞体轮廓显示 (Show Colliders)"),
						collidersEnabled, Wui::Tr("viewport.overlays.collider_outlines_enabled.tooltip", "视口中是否绘制物理碰撞体线框。关闭后哪怕选中实体也彻底隐藏。"),
						[] {
							Editor::EditorPreferences& prefs = Editor::EditorPreferences::Get();
							prefs.SetViewportColliderOutlinesEnabled(!prefs.Data().ViewportColliderOutlinesEnabled);
						});

					const bool collidersAll = Editor::EditorPreferences::Get().Data().ViewportColliderOutlinesAll;
					subItem("viewport.view.collider_outlines", Wui::Tr("viewport.view.collider_outlines", "碰撞体轮廓: 全部 (All Colliders)"),
						collidersAll, Wui::Tr("viewport.overlays.collider_outlines.tooltip", "编辑期画碰撞体形状 (2D 矩形/圆、3D 盒/球/胶囊)。关 = 仅选中实体。"),
						[] {
							Editor::EditorPreferences& prefs = Editor::EditorPreferences::Get();
							prefs.SetViewportColliderOutlinesAll(!prefs.Data().ViewportColliderOutlinesAll);
						});

					subItem("viewport.view.audio_gizmos", Wui::Tr("viewport.view.audio_gizmos", "音频衰减球: 全部"),
						s_ShowAllAudioGizmos, Wui::Tr("viewport.overlays.audio_gizmos.tooltip", "画 3D 音频衰减球与声学混响空间线框。关 = 仅选中实体。"),
						[] { s_ShowAllAudioGizmos = !s_ShowAllAudioGizmos; });

					const bool csmOn = World::Renderer3D::IsCascadeDebugVisualization();
					subItem("viewport.view.csm_debug", Wui::Tr("viewport.csm.short", "CSM 阴影级联染色"),
						csmOn, Wui::Tr("viewport.csm.hint", "CSM 4级级联阴影色彩调试 (红/绿/蓝/黄)"),
						[csmOn] { World::Renderer3D::SetCascadeDebugVisualization(!csmOn); });
				}
				else if (m_ActiveViewSubmenu == "lod")
				{
					subItem("viewport.view.octree_bounds", Wui::Tr("viewport.view.octree_bounds", "空间八叉树包围盒 (Octree Bounds)"),
						s_ShowOctreeBounds, "视口绘制当前场景 SceneOctree 的所有节点 AABB 线框",
						[] { s_ShowOctreeBounds = !s_ShowOctreeBounds; });
					subItem("viewport.view.lod_coloration", Wui::Tr("viewport.view.lod_coloration", "LOD 阶梯染色 (LOD Coloration)"),
						MeshLODManager::Get().IsLODColorationEnabled(), "全场景 LOD 物体实时按绿/黄/橙/红着色",
						[] { MeshLODManager::Get().SetLODColorationEnabled(!MeshLODManager::Get().IsLODColorationEnabled()); });
					subItem("viewport.view.meshlet_clusters", Wui::Tr("viewport.view.meshlet_clusters", "Meshlet 集群视图 (Meshlet Clusters)"),
						IsMeshletClustersVisible(), "按 ≤64 顶点/126 三角形划分网格微型簇，使用独立色彩可视化 GPU-Driven Meshlet 结构",
						[] { SetMeshletClustersVisible(!IsMeshletClustersVisible()); });
					subItem("viewport.view.lod_hud", Wui::Tr("viewport.view.lod_hud", "LOD & 八叉树遥测浮层 (LOD & Octree HUD)"),
						s_ShowLODHud, "视口浮动小药丸面板，显示当前 Octree 节点数/元素数与 LOD 档位分布",
						[] { s_ShowLODHud = !s_ShowLODHud; });
				}
				else if (m_ActiveViewSubmenu == "navmesh")
				{
					subItem("viewport.view.navmesh", Wui::Tr("viewport.view.navmesh", "3D 导航网格 (NavMesh) [P]"),
						s_ShowNavMesh, Wui::Tr("viewport.overlays.navmesh.tooltip", "开启/关闭 3D 导航网格叠加层 (快捷键 P)"),
						[] { SetNavMeshVisible(!s_ShowNavMesh); });
					subItem("viewport.view.navmesh_fill", Wui::Tr("viewport.view.navmesh_fill", "半透明全涂色填充"),
						NavMeshPanel::FillPolygons, "UE5 风格半透明色彩涂色铺满可行走表面，配合宏观大区域与近处自适应网格",
						[] { NavMeshPanel::FillPolygons = !NavMeshPanel::FillPolygons; });
					subItem("viewport.view.navmesh_obstacles", Wui::Tr("viewport.view.navmesh_obstacles", "动态障碍物盒体"),
						NavMeshPanel::ShowObstacles, "在视口中绘制动态障碍物的橙色 3D 盒体包围线框",
						[] { NavMeshPanel::ShowObstacles = !NavMeshPanel::ShowObstacles; });
					subItem("viewport.view.navmesh_area_costs", Wui::Tr("viewport.view.navmesh_area_costs", "区域代价色彩"),
						NavMeshPanel::ShowAreaCosts, "按区域着色: 普通(绿)、道路(亮绿)、泥潭(蓝紫)、阻挡(红)",
						[] { NavMeshPanel::ShowAreaCosts = !NavMeshPanel::ShowAreaCosts; });
					subItem("viewport.view.navmesh_links", Wui::Tr("viewport.view.navmesh_links", "跳跃点弧线指示"),
						NavMeshPanel::ShowOffMeshLinks, "在视口中绘制洋红色抛物线跳跃弧线与箭头指示",
						[] { NavMeshPanel::ShowOffMeshLinks = !NavMeshPanel::ShowOffMeshLinks; });
					subItem("viewport.view.navmesh_paths", Wui::Tr("viewport.view.navmesh_paths", "寻路路径折线"),
						NavMeshPanel::ShowPaths, "在视口中绘制金黄色平滑折线走廊与航点圆环",
						[] { NavMeshPanel::ShowPaths = !NavMeshPanel::ShowPaths; });
				}
			}

			const bool insideParent = parentPanel.W > 0.0f && ctx.HitTestRaw(parentPanel, ctx.Input().MousePos);
			const bool insideSub = !m_ActiveViewSubmenu.empty() && subPanel.W > 0.0f && ctx.HitTestRaw(subPanel, ctx.Input().MousePos);
			const bool insideButton = ctx.HitTestRaw(showButton, ctx.Input().MousePos);
			if ((ctx.Input().MouseClicked[0] || ctx.Input().MouseClicked[1]) && !insideParent && !insideSub && !insideButton)
			{
				ctx.ClosePopup(showPopup);
				m_ActiveViewSubmenu.clear();
			}
			if (ctx.IsKeyPressed(KeyCodes::Escape))
			{
				ctx.ClosePopup(showPopup);
				m_ActiveViewSubmenu.clear();
			}
		}

		// ⑤ 视口工作模式弹层 (Mode Popup: 选择 / 地貌 / 植被)
		const Wui::WuiId modePopup = Wui::HashId("viewport.mode.popup");
		if (ctx.IsPopupOpen(modePopup))
		{
			const float rowH = 24.0f;
			const float popupW = 265.0f;
			constexpr int kWorkflowModeCount = 4;
			const float popupH = 6.0f + 20.0f + 4.0f + static_cast<float>(kWorkflowModeCount) * rowH + 6.0f;
			const float px = modeButton.W > 0.0f ? modeButton.X : (leftIslandRect.X + 100.0f);
			const Wui::WuiRect panel { px, leftIslandRect.Y + leftIslandRect.H + 3.0f, popupW, popupH };
			Wui::DrawPanelSurface(ctx, panel, theme);
			ctx.RegisterOverlayRect(panel);
			float rowY = panel.Y + 6.0f;

			Wui::Label(ctx, { panel.X + 8.0f, rowY + 3.0f },
				Wui::Tr("viewport.mode.title", "视口工作模式 (Workflow Modes)"), theme.TextMuted, 11.5f);
			rowY += 20.0f;
			Wui::PanelBackground(ctx, { panel.X + 6.0f, rowY, panel.W - 12.0f, 1.0f }, theme.Border, 0.0f);
			rowY += 4.0f;

			const auto modeItem = [&](const char* id, const std::string& label, bool checked, const std::string& tip, const std::function<void()>& action)
			{
				const Wui::WuiRect row { panel.X + 4.0f, rowY, panel.W - 8.0f, rowH };
				rowY += rowH;
				Wui::Tooltip(ctx, row, tip);
				if (Wui::MenuItem(ctx, Wui::HashId(id), row, label, checked, true, theme))
				{
					action();
					ctx.ClosePopup(modePopup);
				}
				ctx.RegisterOverlayRect(row);
			};

			modeItem("viewport.mode.select", Wui::Tr("viewport.mode.select_name", "选择与通用变换") + " [Shift+1]",
				currentToolMode == EditorToolMode::Select, "选择与通用变换模式 (Select & Transform) [快捷键 Shift+1]",
				[this] { m_Host.SetEditorToolMode(EditorToolMode::Select); });

			modeItem("viewport.mode.landscape", Wui::Tr("viewport.mode.landscape_name", "3D 地貌雕刻与混绘") + " [Shift+2]",
				currentToolMode == EditorToolMode::Landscape, "3D 地貌雕刻与混绘模式 (Landscape Sculpt & Paint) [快捷键 Shift+2]",
				[this] {
					m_Host.SetEditorToolMode(EditorToolMode::Landscape);
					m_Host.ActivatePanel("terrain_editor");
				});

			modeItem("viewport.mode.foliage", Wui::Tr("viewport.mode.foliage_name", "3D 植被散布模式") + " [Shift+3]",
				currentToolMode == EditorToolMode::Foliage, "3D 植被散布模式 (Foliage Scatter) [快捷键 Shift+3]",
				[this] {
					m_Host.SetEditorToolMode(EditorToolMode::Foliage);
					m_Host.ActivatePanel("foliage_editor");
				});

			modeItem("viewport.mode.painter", Wui::Tr("viewport.mode.painter_name", "3D 物理画笔模式") + " [Shift+4]",
				currentToolMode == EditorToolMode::PhysicsPainter, "3D 物理画笔与地表散布模式 (Physics Painter) [快捷键 Shift+4]",
				[this] {
					m_Host.SetEditorToolMode(EditorToolMode::PhysicsPainter);
					m_Host.ActivatePanel("physicspainter");
				});

			modeItem("viewport.mode.pcg", Wui::Tr("viewport.mode.pcg_name", "PCG 生态生成器工作台"),
				false, "打开 PCG 生态生成器工作台 (PCG Graph Workbench)",
				[this] {
					m_Host.OpenPcgEditor();
				});

			ctx.ClosePopupsOnOutsideClick({ modePopup }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(modePopup);
		}

		// ⑥ 变换工具弹层 (Gizmo Popup: Q 选 / W 移 / E 转 / R 缩 / T 矩形)
		const Wui::WuiId gizmoPopup = Wui::HashId("viewport.gizmo.popup");
		if (ctx.IsPopupOpen(gizmoPopup))
		{
			const float rowH = 24.0f;
			const float popupW = 230.0f;
			const float popupH = 6.0f + 20.0f + 4.0f + 5.0f * rowH + 6.0f;
			const float px = gizmoButton.W > 0.0f ? gizmoButton.X : (leftIslandRect.X + 180.0f);
			const Wui::WuiRect panel { px, leftIslandRect.Y + leftIslandRect.H + 3.0f, popupW, popupH };
			Wui::DrawPanelSurface(ctx, panel, theme);
			ctx.RegisterOverlayRect(panel);
			float rowY = panel.Y + 6.0f;

			Wui::Label(ctx, { panel.X + 8.0f, rowY + 3.0f },
				Wui::Tr("viewport.gizmo.title", "变换工具 (Transform Tools)"), theme.TextMuted, 11.5f);
			rowY += 20.0f;
			Wui::PanelBackground(ctx, { panel.X + 6.0f, rowY, panel.W - 12.0f, 1.0f }, theme.Border, 0.0f);
			rowY += 4.0f;

			const auto gizmoItem = [&](const char* id, const std::string& label, Wui::GizmoOperation op, const std::string& tip)
			{
				const Wui::WuiRect row { panel.X + 4.0f, rowY, panel.W - 8.0f, rowH };
				rowY += rowH;
				Wui::Tooltip(ctx, row, tip);
				if (Wui::MenuItem(ctx, Wui::HashId(id), row, label, activeGizmo == op, true, theme))
				{
					m_Host.SetGizmoOperation(op);
					ctx.ClosePopup(gizmoPopup);
				}
				ctx.RegisterOverlayRect(row);
			};

			gizmoItem("viewport.btn.gizmo_q", Wui::Tr("viewport.gizmo.none", "选择模式 (None) [Q]"),
				Wui::GizmoOperation::None, "选择模式 (None) [快捷键 Q]");
			gizmoItem("viewport.btn.gizmo_w", Wui::Tr("viewport.gizmo.translate", "平移工具 (Move) [W]"),
				Wui::GizmoOperation::Translate, "平移工具 (Move) [快捷键 W]");
			gizmoItem("viewport.btn.gizmo_e", Wui::Tr("viewport.gizmo.rotate", "旋转工具 (Rotate) [E]"),
				Wui::GizmoOperation::Rotate, "旋转工具 (Rotate) [快捷键 E]");
			gizmoItem("viewport.btn.gizmo_r", Wui::Tr("viewport.gizmo.scale", "缩放工具 (Scale) [R]"),
				Wui::GizmoOperation::Scale, "缩放工具 (Scale) [快捷键 R]");
			gizmoItem("viewport.btn.gizmo_t", Wui::Tr("viewport.gizmo.rect", "2D 矩形手柄 (Rect) [T]"),
				Wui::GizmoOperation::Rect, "2D 矩形变换手柄 (Rect Transform) [快捷键 T]");

			ctx.ClosePopupsOnOutsideClick({ gizmoPopup }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(gizmoPopup);
		}

		// ⑦ 空间吸附与参考对齐弹层 (Snap Popup: 合并 Grid, Part, Snap, CSM, Lock)
		const Wui::WuiId snapPopup = Wui::HashId("viewport.snap.popup");
		if (ctx.IsPopupOpen(snapPopup))
		{
			const float rowH = 24.0f;
			const float popupW = 250.0f;
			const float popupH = 6.0f + 20.0f + 4.0f + 5.0f * rowH + 6.0f;
			const float px = snapBtnRect.W > 0.0f
				? std::min(snapBtnRect.X, sceneRect.X + sceneRect.W - popupW - kChromePad)
				: (sceneRect.X + sceneRect.W - popupW - kChromePad);
			const float popupY = rightIslandRect.Y + rightIslandRect.H + 3.0f;
			const Wui::WuiRect panel { px, popupY, popupW, popupH };
			Wui::DrawPanelSurface(ctx, panel, theme);
			ctx.RegisterOverlayRect(panel);
			float rowY = panel.Y + 6.0f;

			Wui::Label(ctx, { panel.X + 8.0f, rowY + 3.0f },
				Wui::Tr("viewport.snap.title", "空间吸附与参考对齐"), theme.TextMuted, 11.5f);
			rowY += 20.0f;
			Wui::PanelBackground(ctx, { panel.X + 6.0f, rowY, panel.W - 12.0f, 1.0f }, theme.Border, 0.0f);
			rowY += 4.0f;

			const auto snapItem = [&](const char* id, const std::string& label, bool checked, const std::string& tip, const std::function<void()>& action)
			{
				const Wui::WuiRect row { panel.X + 4.0f, rowY, panel.W - 8.0f, rowH };
				rowY += rowH;
				Wui::Tooltip(ctx, row, tip);
				if (Wui::MenuItem(ctx, Wui::HashId(id), row, label, checked, true, theme))
					action();
				ctx.RegisterOverlayRect(row);
			};

			snapItem("viewport.stream_grid", Wui::Tr("viewport.snap.ground_grid", "地面参考网格 (1m/10m)"),
				m_Host.IsReferenceGridEnabled(), Wui::Tr("viewport.grid.hint", "开关地面参考基准网格 (1m/10m)。"),
				[this] { m_Host.SetReferenceGridEnabled(!m_Host.IsReferenceGridEnabled()); });

			snapItem("viewport.partition_grid", Wui::Tr("viewport.snap.partition", "世界分区流送网格"),
				m_ShowWorldPartition, Wui::Tr("viewport.partition.hint", "开关大世界空间分区与流送区块辅助网格。"),
				[this] { m_ShowWorldPartition = !m_ShowWorldPartition; });

			snapItem("viewport.btn.toggle_snap", Wui::Tr("viewport.snap.pixel_snap", "2D 像素网格吸附"),
				m_Host.IsPixelSnapEnabled(), Wui::Tr("viewport.snap.hint", "2D 像素网格吸附开关 (按 1/PPU 步长吸附坐标，杜绝亚像素插值撕裂与接缝抖动)"),
				[this] { m_Host.SetPixelSnapEnabled(!m_Host.IsPixelSnapEnabled()); });

			const bool csmOn = World::Renderer3D::IsCascadeDebugVisualization();
			snapItem("viewport.csm_debug", Wui::Tr("viewport.snap.csm_debug", "CSM 阴影级联调试染色"),
				csmOn, Wui::Tr("viewport.csm.hint", "CSM 4级级联阴影色彩调试: 红(C0)、绿(C1)、蓝(C2)、黄(C3)。"),
				[csmOn] { World::Renderer3D::SetCascadeDebugVisualization(!csmOn); });

			snapItem("viewport.meshlet_clusters", Wui::Tr("viewport.snap.meshlet_clusters", "Meshlet 簇化调试 (Meshlets)"),
				IsMeshletClustersVisible(), Wui::Tr("viewport.meshlet.hint", "按 ≤64 顶点/126 三角形划分微型簇，在几何体表面进行真彩簇染色检视。"),
				[] { SetMeshletClustersVisible(!IsMeshletClustersVisible()); });

			const bool is2D = m_Host.Is2DMode();
			snapItem("viewport.btn.toggle_2d", Wui::Tr("viewport.snap.lock_rotation", "锁定鼠标旋转"),
				is2D, Wui::Tr("viewport.2d.hint", "锁定鼠标旋转视角 (锁定后禁止鼠标旋转、右键拖拽变为平移；左上角方位菜单仍可正常切换方向)"),
				[this, is2D] { m_Host.Set2DMode(!is2D); });

			ctx.ClosePopupsOnOutsideClick({ snapPopup }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(snapPopup);
		}

		// ⑧ 数据层多选弹层 (Layers Popup: 管理关卡数据层流送与可见性)
		const Wui::WuiId layersPopup = Wui::HashId("viewport.layers.popup");
		if (ctx.IsPopupOpen(layersPopup))
		{
			const float rowH = 22.0f;
			const float popupW = 220.0f;
			const float popupH = dataLayers.empty() ? 64.0f : (30.0f + static_cast<float>(dataLayers.size()) * rowH + 34.0f);
			const float popupX = layersBtnRect.W > 0.0f
				? std::min(layersBtnRect.X, sceneRect.X + sceneRect.W - popupW - kChromePad)
				: (sceneRect.X + sceneRect.W - popupW - kChromePad);
			const float popupY = rightIslandRect.Y + rightIslandRect.H + 3.0f;
			const Wui::WuiRect panel { popupX, popupY, popupW, popupH };
			Wui::DrawPanelSurface(ctx, panel, theme);
			ctx.RegisterOverlayRect(panel);

			float rowY = panel.Y + 6.0f;
			Wui::Label(ctx, { panel.X + 8.0f, rowY + 3.0f }, Wui::Tr("viewport.layers.title", "关卡数据层 (Data Layers)"), theme.TextMuted, 12.0f);
			rowY += 20.0f;
			Wui::PanelBackground(ctx, { panel.X + 6.0f, rowY, panel.W - 12.0f, 1.0f }, theme.Border, 0.0f);
			rowY += 4.0f;

			if (dataLayers.empty())
			{
				Wui::Label(ctx, { panel.X + 12.0f, rowY + 3.0f }, Wui::Tr("viewport.layers.empty", "当前关卡未定义数据层"), theme.TextDisabled, 11.5f);
			}
			else
			{
				for (const Level::DataLayerDefinition& layer : dataLayers)
				{
					if (layer.Name.empty())
						continue;
					const bool isOn = (layer.LayerMask & activeLayers) != 0;
					const Wui::WuiRect row { panel.X + 4.0f, rowY, panel.W - 8.0f, rowH };
					rowY += rowH;

					const std::string layerHint = Wui::TrFormat("viewport.datalayer.hint",
						"Toggle data layer '{0}' (mask {1}).", { { "name", layer.Name },
							{ "mask", std::to_string(layer.LayerMask) } });
					Wui::Tooltip(ctx, row, layerHint);

					if (Wui::MenuItem(ctx, Wui::HashId(("viewport.layer." + layer.Name).c_str()), row, layer.Name, isOn, true, theme))
					{
						const uint32_t next = isOn ? (activeLayers & ~layer.LayerMask) : (activeLayers | layer.LayerMask);
						m_Host.SetActiveDataLayers(next);
					}
				}

				rowY += 2.0f;
				Wui::PanelBackground(ctx, { panel.X + 6.0f, rowY, panel.W - 12.0f, 1.0f }, theme.Border, 0.0f);
				rowY += 4.0f;
				const float btnHalfW = (panel.W - 16.0f) * 0.5f;
				const Wui::WuiRect allBtn { panel.X + 6.0f, rowY, btnHalfW, 20.0f };
				const Wui::WuiRect baseBtn { panel.X + 10.0f + btnHalfW, rowY, btnHalfW, 20.0f };
				if (Wui::Button(ctx, Wui::HashId("viewport.layers.all"), allBtn, "全部显示", theme))
				{
					uint32_t allMask = 0;
					for (const auto& l : dataLayers) allMask |= l.LayerMask;
					m_Host.SetActiveDataLayers(allMask);
				}
				if (Wui::Button(ctx, Wui::HashId("viewport.layers.base_only"), baseBtn, "仅Base", theme))
				{
					m_Host.SetActiveDataLayers(1u);
				}
			}

			ctx.ClosePopupsOnOutsideClick({ layersPopup }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(layersPopup);
		}

		// ⑨ 选择过滤器弹层 (Selection Filter Popup: 视口防误选快速配置与图层锁定)
		const Wui::WuiId filterPopup = Wui::HashId("viewport.filter.popup");
		if (ctx.IsPopupOpen(filterPopup))
		{
			SelectionFilter& curFilter = m_Host.GetSelectionFilter();
			const float popupW = 260.0f;
			const float popupRowH = 22.0f;
			const float popupH = 28.0f + 26.0f + 4.0f + 20.0f + 7 * popupRowH + 8.0f;
			const float popupX = filterBtnRect.W > 0.0f
				? std::min(filterBtnRect.X, sceneRect.X + sceneRect.W - popupW - kChromePad)
				: (sceneRect.X + sceneRect.W - popupW - kChromePad);
			const float popupY = rightIslandRect.Y + rightIslandRect.H + 3.0f;
			const Wui::WuiRect panel { popupX, popupY, popupW, popupH };
			Wui::DrawPanelSurface(ctx, panel, theme);
			ctx.RegisterOverlayRect(panel);

			float rowY = panel.Y + 6.0f;
			Wui::Label(ctx, { panel.X + 8.0f, rowY + 2.0f },
				Wui::Tr("viewport.filter.title", "选择过滤器 (Selection Filter)"), theme.TextMuted, 12.0f);
			rowY += 22.0f;

			// 快捷预设按钮组: [全选] [仅角色] [锁BG] [重置]
			const float presetBtnW = (panel.W - 16.0f - 9.0f) / 4.0f;
			float pBtnX = panel.X + 8.0f;
			if (Wui::Button(ctx, Wui::HashId("viewport.filter.preset_all"), { pBtnX, rowY, presetBtnW, 22.0f }, "全选", theme))
			{
				curFilter.Reset();
			}
			pBtnX += presetBtnW + 3.0f;
			if (Wui::Button(ctx, Wui::HashId("viewport.filter.preset_actors"), { pBtnX, rowY, presetBtnW, 22.0f }, "仅角色", theme))
			{
				for (uint32_t mask : allFilterLayers)
					curFilter.SetLayerSelectable(mask, mask == Layer_Actor);
			}
			pBtnX += presetBtnW + 3.0f;
			if (Wui::Button(ctx, Wui::HashId("viewport.filter.preset_lock_bg"), { pBtnX, rowY, presetBtnW, 22.0f }, "锁BG", theme))
			{
				curFilter.SetLayerLocked(Layer_Background, true);
				curFilter.SetLayerLocked(Layer_Decor, true);
			}
			pBtnX += presetBtnW + 3.0f;
			if (Wui::Button(ctx, Wui::HashId("viewport.filter.preset_reset"), { pBtnX, rowY, presetBtnW, 22.0f }, "重置", theme))
			{
				curFilter.Reset();
			}
			rowY += 26.0f;

			// 分割线
			Wui::PanelBackground(ctx, { panel.X + 6.0f, rowY, panel.W - 12.0f, 1.0f }, theme.Border, 0.0f);
			rowY += 4.0f;

			// 表头: 图层 / 可选 / 锁定
			Wui::Label(ctx, { panel.X + 12.0f, rowY + 2.0f }, "图层", theme.TextMuted, 11.0f);
			Wui::Label(ctx, { panel.X + 144.0f, rowY + 2.0f }, "可选", theme.TextMuted, 11.0f);
			Wui::Label(ctx, { panel.X + 204.0f, rowY + 2.0f }, "锁定", theme.TextMuted, 11.0f);
			rowY += 18.0f;

			struct LayerDef {
				const char* Name;
				uint32_t Mask;
				const char* Tip;
			};
			const LayerDef layerDefs[] = {
				{ "Actor (角色/逻辑)",     Layer_Actor,      "动态刚体、角色控制器、玩家与怪物" },
				{ "Light (光源)",          Layer_Light,      "方向光、点光源、2D 多边形光" },
				{ "Trigger (触发器)",      Layer_Trigger,    "碰撞传感器 (IsSensor)、区域触发盒" },
				{ "Collider (碰撞体)",     Layer_Collider,   "静态碰撞盒、刚体外壳" },
				{ "Decor (地块/装饰)",     Layer_Decor,      "Tilemap 瓦片地图、静态装饰物" },
				{ "Background (背景/视差)", Layer_Background, "2D 视差层、远景贴图、天空盒" },
				{ "UI (画布/HUD)",         Layer_UI,         "世界空间 UI、HUD 元素" }
			};

			Wui::WuiId baseId = Wui::HashId("viewport.filter.layer_items");
			for (const auto& item : layerDefs)
			{
				const Wui::WuiRect rowRect { panel.X + 6.0f, rowY, panel.W - 12.0f, popupRowH };
				Wui::Tooltip(ctx, rowRect, item.Tip);

				// 图层名称
				Wui::Label(ctx, { panel.X + 10.0f, rowY + 3.0f }, item.Name, theme.Text, 12.0f);

				// 可选复选框
				bool selectable = curFilter.IsLayerSelectable(item.Mask);
				const bool prevSelectable = selectable;
				Wui::WuiRect selRect { panel.X + 140.0f, rowY + 1.0f, 50.0f, 20.0f };
				if (Wui::Checkbox(ctx, baseId++, selRect, "选", selectable, theme))
				{
					if (selectable != prevSelectable)
						curFilter.SetLayerSelectable(item.Mask, selectable);
				}

				// 锁定复选框
				bool locked = curFilter.IsLayerLocked(item.Mask);
				const bool prevLocked = locked;
				Wui::WuiRect lockRect { panel.X + 200.0f, rowY + 1.0f, 50.0f, 20.0f };
				if (Wui::Checkbox(ctx, baseId++, lockRect, "锁", locked, theme))
				{
					if (locked != prevLocked)
						curFilter.SetLayerLocked(item.Mask, locked);
				}

				rowY += popupRowH;
			}

			ctx.ClosePopupsOnOutsideClick({ filterPopup }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(filterPopup);
		}

		// U22(用户 §5.5「所有视口类窗口都要有坐标系来表明方向」):主视口右下角
		// 半透明三轴指示(X 红 / Y 绿 / Z 蓝),朝向取**当前画面用的相机基** ——
		// Play 下是场景主相机、2D 下是正交相机(此时 Z 投影为点,自然只剩 X/Y)。
		{
		if (!play)
		{
			const Wui::GizmoCamera axisCamera = m_Host.GetGizmoCamera();
			const Wui::WuiRect axisRect = ViewChrome::DrawAxisIndicator(ctx, sceneRect,
				axisCamera.Right, axisCamera.Up);
			float axisYaw = 0.0f;
			float axisPitch = 0.0f;
			ViewChrome::BasisYawPitch(axisCamera.Forward, &axisYaw, &axisPitch);
			ViewChrome::RegisterAxisNode(axisRect, "viewport.axis",
				Wui::Tr("viewport.axis", "Viewport axes"),
				ViewChrome::AxisReadout(axisYaw, axisPitch, !m_Host.IsViewportCamera3D()),
				Wui::Tr("viewport.axis.tooltip",
					"World axes drawn in the corner of the viewport: X red, Y green, Z blue. "
					"They follow the viewport camera; in 2D orthographic views only X and Y are visible."));
			// 与其它悬浮层同一条口径:指示器下面的拾取/gizmo 不吃这一下的点击。
			ctx.RegisterOverlayRect(axisRect);
		}
		}
		ctx.PopOverlay();

		const bool hovered = ctx.IsHovered(rect);
		if (ctx.IsClicked(rect))
			ctx.SetFocus(Wui::HashId("viewport"));
		const bool focused = ctx.Focus() == Wui::HashId("viewport");
		// 视口尺寸/边界取**场景图像区域**(sceneRect)= 整块面板(悬浮层不占布局)。
		// 拾取与 gizmo 的屏幕↔世界映射都按 sceneRect 算,所以它必须等于真正显示画面的矩形。
		glm::vec2 bounds[2] = { { sceneRect.X, sceneRect.Y },
			{ sceneRect.X + sceneRect.W, sceneRect.Y + sceneRect.H } };
		m_Host.SetViewportState(focused, hovered, { sceneRect.W, sceneRect.H }, bounds);

		// W5-L1:磁盘场景改动提示条(顶部居中)。先算矩形:它要参与"点提示条不吃实体拾取"的判定;
		// 真正绘制在 OnRender 末尾,保证盖在选中框/视锥覆盖层之上。
		const bool bannerVisible = m_Host.ExternalSceneChanged() && !m_Host.IsPlaying() && !m_Host.IsSimulating();
		const float bannerWidth = std::min(sceneRect.W - 24.0f, 470.0f);
		const Wui::WuiRect bannerRect { sceneRect.X + (sceneRect.W - bannerWidth) * 0.5f, sceneRect.Y + 10.0f,
			bannerWidth, 26.0f };
		const bool overBanner = bannerVisible && ctx.IsHovered(bannerRect);

		// 先跑 gizmo:它有"鼠标占用"语义(拖拽中/悬停在手柄上),必须优先于实体拾取 ——
		// 否则点箭头会被"点空白清空选择"吃掉,表现为"拖不动箭头"。
		bool gizmoEngaged = false;
		if (m_Host.GetEditorToolMode() != EditorToolMode::Select)
		{
			m_GizmoActive = false;
		}

		Entity selected = m_Host.GetSelectedEntity();
		if (!play && m_Host.GetEditorToolMode() == EditorToolMode::Select && selected.IsValid() && selected.GetScene() == m_Host.GetActiveScene().get() &&
			selected.HasComponent<TransformComponent>() && m_Host.HasRenderedScene())
		{
			Wui::ClipScope viewportClip(ctx, sceneRect);

			auto& transform = selected.GetComponent<TransformComponent>();
			const TransformComponent before = transform;
			bool nowUsing = false;

			const uint32_t entLayer = SelectionFilter::ClassifyEntityLayer(selected, *selected.GetScene());
			const bool isLocked = m_Host.GetSelectionFilter().IsLayerLocked(entLayer);
			const bool allowManipulation = !m_Host.IsReadOnlyMode() && !isLocked;

			if (m_Host.IsEditingCollider())
			{
				if (selected.HasComponent<BoxCollider2DComponent>())
				{
					auto& box = selected.GetComponent<BoxCollider2DComponent>();
					nowUsing = Wui::ManipulateBoxCollider2D(m_Host.GetGizmoCamera(), transform, box.Offset, box.Size, sceneRect, ctx, allowManipulation);
				}
				else if (selected.HasComponent<CircleCollider2DComponent>())
				{
					auto& circle = selected.GetComponent<CircleCollider2DComponent>();
					nowUsing = Wui::ManipulateCircleCollider2D(m_Host.GetGizmoCamera(), transform, circle.Offset, circle.Radius, sceneRect, ctx, allowManipulation);
				}
				else if (selected.HasComponent<CapsuleCollider2DComponent>())
				{
					auto& cap = selected.GetComponent<CapsuleCollider2DComponent>();
					nowUsing = Wui::ManipulateCapsuleCollider2D(m_Host.GetGizmoCamera(), transform, cap.Offset, cap.Radius, cap.Length, cap.Direction, sceneRect, ctx, allowManipulation);
				}
				else if (selected.HasComponent<PolygonCollider2DComponent>())
				{
					auto& poly = selected.GetComponent<PolygonCollider2DComponent>();
					nowUsing = ManipulatePolygonCollider2D(m_Host.GetGizmoCamera(), transform, poly, sceneRect, ctx, allowManipulation);
				}
				else if (selected.HasComponent<ChainCollider2DComponent>())
				{
					auto& chain = selected.GetComponent<ChainCollider2DComponent>();
					nowUsing = ManipulateChainCollider2D(m_Host.GetGizmoCamera(), transform, chain, sceneRect, ctx, allowManipulation);
				}
			}
			else if (m_Host.GetGizmoOperation() == Wui::GizmoOperation::Rect)
			{
				glm::vec2 pivot { 0.5f, 0.5f };
				glm::vec2 baseSize { 1.0f, 1.0f };
				if (selected.HasComponent<SpriteComponent>())
				{
					auto& spr = selected.GetComponent<SpriteComponent>();
					nowUsing = Wui::ManipulateRect2D(m_Host.GetGizmoCamera(), transform, spr.Pivot, baseSize, sceneRect, ctx, allowManipulation);
				}
				else if (selected.HasComponent<TilemapComponent>())
				{
					auto& tmComp = selected.GetComponent<TilemapComponent>();
					float mapW = 16.0f, mapH = 16.0f;
					if (tmComp.Tilemap.HasPath())
					{
						const std::string tmP = StringPool::Get().PathOf(tmComp.Tilemap.Path);
						if (auto tm = Tilemap::GetOrLoad(tmP))
						{
							mapW = static_cast<float>(tm->GetWidth()) * (tmComp.CellSize.x > 0.0f ? tmComp.CellSize.x : tm->GetCellSize().x);
							mapH = static_cast<float>(tm->GetHeight()) * (tmComp.CellSize.y > 0.0f ? tmComp.CellSize.y : tm->GetCellSize().y);
						}
					}
					nowUsing = Wui::ManipulateRect2D(m_Host.GetGizmoCamera(), transform, pivot, { mapW, mapH }, sceneRect, ctx, allowManipulation);
				}
				else
				{
					nowUsing = Wui::ManipulateRect2D(m_Host.GetGizmoCamera(), transform, pivot, baseSize, sceneRect, ctx, allowManipulation);
				}
			}
			else
			{
				nowUsing = Wui::ManipulateGizmo(m_Host.GetGizmoCamera(),
					m_Host.GetGizmoOperation(), transform, sceneRect, ctx,
					/*allowManipulation=*/allowManipulation);
			}
			if (nowUsing && m_Host.IsPixelSnapEnabled())
			{
				const float ppu = m_Host.GetPixelSnapPpu();
				transform.Location.x = std::round(transform.Location.x * ppu) / ppu;
				transform.Location.y = std::round(transform.Location.y * ppu) / ppu;
			}
			if (selected.HasComponent<FreeformLight2DComponent>())
			{
				auto& light = selected.GetComponent<FreeformLight2DComponent>();
				const bool lightUsing = ManipulateFreeformLight2D(m_Host.GetGizmoCamera(), transform, light, sceneRect, ctx, allowManipulation);
				if (lightUsing) nowUsing = true;
			}
			if (selected.HasComponent<AudioSourceComponent>())
			{
				auto& audioSrc = selected.GetComponent<AudioSourceComponent>();
				if (audioSrc.Spatial3D)
				{
					const float beforeMin = audioSrc.MinDistance;
					const float beforeMax = audioSrc.MaxDistance;
					const bool audioUsing = Wui::ManipulateAudioSource3D(m_Host.GetGizmoCamera(), transform,
						audioSrc.MinDistance, audioSrc.MaxDistance, sceneRect, ctx, allowManipulation);
					if (audioUsing)
					{
						nowUsing = true;
						if (!s_AudioGizmoActive)
						{
							s_AudioGizmoActive = true;
							s_AudioGizmoBeforeMin = beforeMin;
							s_AudioGizmoBeforeMax = beforeMax;
						}
						if (audioSrc.MinDistance != beforeMin || audioSrc.MaxDistance != beforeMax)
						{
							m_Host.MarkDocumentDirty();
						}
					}
					else if (s_AudioGizmoActive)
					{
						s_AudioGizmoActive = false;
						if (audioSrc.MinDistance != s_AudioGizmoBeforeMin || audioSrc.MaxDistance != s_AudioGizmoBeforeMax)
						{
							m_Host.MarkDocumentDirty();
						}
					}
				}
			}
			gizmoEngaged = nowUsing;
			if (!m_GizmoActive && nowUsing)
			{
				m_GizmoActive = true;
				m_GizmoBefore = before;
			}
			else if (m_GizmoActive && !nowUsing)
			{
				m_GizmoActive = false;
				const auto& after = selected.GetComponent<TransformComponent>();
				if (after.Location != m_GizmoBefore.Location || after.Rotation != m_GizmoBefore.Rotation || after.Scale != m_GizmoBefore.Scale)
				{
					m_Host.PushCommand(std::make_unique<EntityTransformCommand>(selected, m_GizmoBefore, after, "Gizmo Transform"));
					m_Host.MarkDocumentDirty();
				}
			}
		}
		const Wui::WuiRect previewRect = CameraPreviewRect(sceneRect);
		// 预览小窗只在**选中相机**时才有内容(CameraPreviewLabel 为空 = 没有相机预览)。
		const std::string previewLabel = m_Host.CameraPreviewLabel();
		const bool previewVisible = m_Host.IsCameraPreviewEnabled() &&
			m_Host.GetCameraPreviewTextureId() != 0 && !previewLabel.empty();
		// 相机预览小窗挡住的位置不参与拾取(避免"点预览把背后物体选走")。
		const bool overPreview = previewVisible && ctx.IsHovered(previewRect);
		const bool isPainterMode = (m_Host.GetEditorToolMode() == EditorToolMode::PhysicsPainter);
		if (!play && ctx.IsClicked(sceneRect) && !m_GizmoActive && !gizmoEngaged && !overPreview && !overBanner && isPainterMode && PhysicsPainterPanel::IsActive())
		{
			const glm::vec2 mousePos = ctx.Input().MousePos;
			const Wui::GizmoCamera pickCam = m_Host.GetGizmoCamera();
			const float ndcX = ((mousePos.x - sceneRect.X) / sceneRect.W) * 2.0f - 1.0f;
			const float ndcY = 1.0f - ((mousePos.y - sceneRect.Y) / sceneRect.H) * 2.0f;
			const glm::mat4 invVP = glm::inverse(pickCam.ViewProjection);
			glm::vec4 nearPointClip(ndcX, ndcY, -1.0f, 1.0f);
			glm::vec4 farPointClip(ndcX, ndcY, 1.0f, 1.0f);
			glm::vec4 nearPointWorld = invVP * nearPointClip;
			glm::vec4 farPointWorld = invVP * farPointClip;
			if (std::abs(nearPointWorld.w) > 1e-6f) nearPointWorld /= nearPointWorld.w;
			if (std::abs(farPointWorld.w) > 1e-6f) farPointWorld /= farPointWorld.w;
			glm::vec3 rayOrigin = pickCam.Position;
			glm::vec3 rayDir = glm::normalize(glm::vec3(farPointWorld) - rayOrigin);

			glm::vec3 hitPoint { 0.0f };
			glm::vec3 hitNormal { 0.0f, 1.0f, 0.0f };
			bool hasHit = false;

			Ref<Scene> activeScene = m_Host.GetActiveScene();
			if (activeScene)
			{
				std::shared_ptr<TerrainData> terrain = nullptr;
				glm::mat4 terrainWorld { 1.0f };
				for (auto [h, tc, xf] : activeScene->GetRegistry().view<TerrainComponent, TransformComponent>().each())
				{
					terrain = TerrainRenderer::GetOrCreateTerrainData(tc.TerrainAsset, tc.CellSize, tc.HeightScale);
					terrainWorld = xf.GetLocalMatrix();
					break;
				}
				if (terrain)
				{
					const glm::mat4 invTW = glm::inverse(terrainWorld);
					const glm::vec3 localOrigin = glm::vec3(invTW * glm::vec4(rayOrigin, 1.0f));
					const glm::vec3 localDir = glm::normalize(glm::vec3(invTW * glm::vec4(rayDir, 0.0f)));
					TerrainRaycastHit thit;
					if (terrain->Raycast(localOrigin, localDir, 2000.0f, thit))
					{
						hitPoint = glm::vec3(terrainWorld * glm::vec4(thit.Point, 1.0f));
						hitNormal = glm::normalize(glm::vec3(terrainWorld * glm::vec4(thit.Normal, 0.0f)));
						hasHit = true;
					}
				}
				if (!hasHit && std::abs(rayDir.y) > 1e-4f)
				{
					const float t = -rayOrigin.y / rayDir.y;
					if (t > 0.0f && t < 2000.0f)
					{
						hitPoint = rayOrigin + rayDir * t;
						hitNormal = glm::vec3(0.0f, 1.0f, 0.0f);
						hasHit = true;
					}
				}
				if (hasHit)
				{
					PhysicsPainterPanel::PaintAt(*activeScene, hitPoint, hitNormal, m_Host);
				}
			}
		}
		else if (!play && ctx.IsClicked(sceneRect) && !m_GizmoActive && !gizmoEngaged && !overPreview && !overBanner && m_Host.GetEditorToolMode() == EditorToolMode::Select)
		{
			const glm::vec2 local = ctx.Input().MousePos - glm::vec2 { sceneRect.X, sceneRect.Y };
			Entity picked = m_Host.PickEntityAt(local);

			// 视口代价区域中心标志与 3D 盒体直接拾取增强:
			// 若 picked 为空或点击位置在代价区域中心标志半径 (约 18 屏幕像素) 内，或者光标射线命中了代价修饰区的 3D 盒体，
			// 优先将 picked 判定为该代价区域实体，使用户能够轻松点击选中代价区域实体并在 Inspector 属性面板中编辑其属性。
			const glm::vec2 mousePos = ctx.Input().MousePos;
			const Wui::GizmoCamera pickCamera = m_Host.GetGizmoCamera();
			const float ndcX = ((mousePos.x - sceneRect.X) / sceneRect.W) * 2.0f - 1.0f;
			const float ndcY = 1.0f - ((mousePos.y - sceneRect.Y) / sceneRect.H) * 2.0f;
			const glm::mat4 invVP = glm::inverse(pickCamera.ViewProjection);
			glm::vec4 nearPointClip(ndcX, ndcY, -1.0f, 1.0f);
			glm::vec4 farPointClip(ndcX, ndcY, 1.0f, 1.0f);
			glm::vec4 nearPointWorld = invVP * nearPointClip;
			glm::vec4 farPointWorld = invVP * farPointClip;
			if (std::abs(nearPointWorld.w) > 1e-6f) nearPointWorld /= nearPointWorld.w;
			if (std::abs(farPointWorld.w) > 1e-6f) farPointWorld /= farPointWorld.w;
			glm::vec3 rayOrigin = pickCamera.Position;
			glm::vec3 rayDir = glm::normalize(glm::vec3(farPointWorld) - rayOrigin);
			if (pickCamera.IsOrthographic)
			{
				rayOrigin = glm::vec3(nearPointWorld);
				rayDir = pickCamera.Forward;
			}

			Entity bestAreaEntity;
			float bestDist = 1e9f;
			bool hitBadge = false;

			const Ref<Scene> activeScene = m_Host.GetActiveScene();
			if (activeScene)
			{
				const auto& reg = std::as_const(*activeScene).GetRegistry();
				for (auto [h, areaComp] : reg.view<NavMeshAreaModifierComponent>().each())
				{
					const auto* xform = reg.try_get<TransformComponent>(h);
					if (!xform) continue;

					const glm::vec3 modCenter = xform->Location;
					const glm::vec4 clipPos = pickCamera.ViewProjection * glm::vec4(modCenter, 1.0f);
					if (clipPos.w > 0.05f)
					{
						const glm::vec2 ndc { clipPos.x / clipPos.w, clipPos.y / clipPos.w };
						const glm::vec2 sp {
							sceneRect.X + (ndc.x + 1.0f) * 0.5f * sceneRect.W,
							sceneRect.Y + (1.0f - ndc.y) * 0.5f * sceneRect.H
						};
						const float screenDist = glm::length(mousePos - sp);
						if (screenDist <= 18.0f && screenDist < bestDist)
						{
							bestDist = screenDist;
							bestAreaEntity = Entity(activeScene.get(), h);
							hitBadge = true;
						}
					}

					if (!hitBadge)
					{
						const glm::mat4 localMat = xform->GetLocalMatrix();
					const glm::mat4 invWorld = glm::inverse(localMat);
						const glm::vec3 localOrig = glm::vec3(invWorld * glm::vec4(rayOrigin, 1.0f));
						const glm::vec3 localDir = glm::normalize(glm::vec3(invWorld * glm::vec4(rayDir, 0.0f)));
						const glm::vec3 bMin = -areaComp.Extents;
						const glm::vec3 bMax = areaComp.Extents;
						float tmin = 0.0f;
						float tmax = 2000.0f;
						bool hitBox = true;
						for (int i = 0; i < 3; ++i)
						{
							if (std::abs(localDir[i]) < 1e-6f)
							{
								if (localOrig[i] < bMin[i] || localOrig[i] > bMax[i]) { hitBox = false; break; }
							}
							else
							{
								const float invD = 1.0f / localDir[i];
								float t1 = (bMin[i] - localOrig[i]) * invD;
								float t2 = (bMax[i] - localOrig[i]) * invD;
								if (t1 > t2) std::swap(t1, t2);
								tmin = std::max(tmin, t1);
								tmax = std::min(tmax, t2);
								if (tmin > tmax) { hitBox = false; break; }
							}
						}
						if (hitBox && tmax > 0.0f)
						{
							const float hitT = (tmin > 0.0f) ? tmin : tmax;
							if (hitT < bestDist)
							{
								bestDist = hitT;
								bestAreaEntity = Entity(activeScene.get(), h);
							}
						}
					}
				}
			}

			if (bestAreaEntity.IsValid())
			{
				picked = bestAreaEntity;
			}

			// 空间八叉树快速相交剪枝拾取 (SceneOctree Ray Query Acceleration)
			if (!picked.IsValid() && activeScene)
			{
				std::vector<uint32_t> octreeCandidates;
				activeScene->GetOctree().QueryRay(rayOrigin, rayDir, 2000.0f, octreeCandidates);
				float bestOctreeDist = 1e9f;
				Entity bestOctreeEntity;
				const auto& reg = std::as_const(*activeScene).GetRegistry();
				for (uint32_t entId : octreeCandidates)
				{
					entt::entity h = static_cast<entt::entity>(entId);
					if (!reg.valid(h)) continue;
					const auto* xform = reg.try_get<TransformComponent>(h);
					if (!xform) continue;

					glm::vec3 bMin(-0.5f);
					glm::vec3 bMax(0.5f);
					if (const auto* boxCol = reg.try_get<BoxCollider3DComponent>(h))
					{
						bMin = boxCol->Offset - boxCol->HalfExtents;
						bMax = boxCol->Offset + boxCol->HalfExtents;
					}

					const glm::mat4 localMat = xform->GetLocalMatrix();
					const glm::mat4 invWorld = glm::inverse(localMat);
					const glm::vec3 localOrig = glm::vec3(invWorld * glm::vec4(rayOrigin, 1.0f));
					const glm::vec3 localDir = glm::normalize(glm::vec3(invWorld * glm::vec4(rayDir, 0.0f)));
					float tmin = 0.0f;
					float tmax = 2000.0f;
					bool hitBox = true;
					for (int i = 0; i < 3; ++i)
					{
						if (std::abs(localDir[i]) < 1e-6f)
						{
							if (localOrig[i] < bMin[i] || localOrig[i] > bMax[i]) { hitBox = false; break; }
						}
						else
						{
							const float invD = 1.0f / localDir[i];
							float t1 = (bMin[i] - localOrig[i]) * invD;
							float t2 = (bMax[i] - localOrig[i]) * invD;
							if (t1 > t2) std::swap(t1, t2);
							tmin = std::max(tmin, t1);
							tmax = std::min(tmax, t2);
							if (tmin > tmax) { hitBox = false; break; }
						}
					}
					if (hitBox && tmax > 0.0f)
					{
						const float hitT = (tmin > 0.0f) ? tmin : tmax;
						if (hitT < bestOctreeDist)
						{
							bestOctreeDist = hitT;
							bestOctreeEntity = Entity(activeScene.get(), h);
						}
					}
				}
				if (bestOctreeEntity.IsValid() && bestOctreeDist < bestDist)
				{
					picked = bestOctreeEntity;
				}
			}

			if (std::getenv("WLD_TRACE_UI"))
				WLD_CORE_INFO("[ui] viewport click local=({0},{1}) sceneRect=({2},{3},{4},{5}) -> handle={6} valid={7}",
					local.x, local.y, sceneRect.X, sceneRect.Y, sceneRect.W, sceneRect.H,
					picked.IsValid() ? static_cast<uint32_t>(static_cast<entt::entity>(picked)) : 0u,
					picked.IsValid() ? 1 : 0);
			m_Host.SetSelectedEntity(picked);
		}

		// ---- 3D 地貌模式快捷键与视口直接交互 (Landscape Direct Interaction) ----
		const bool isLandscapeMode = (m_Host.GetEditorToolMode() == EditorToolMode::Landscape);
		if (!play && isLandscapeMode && (focused || hovered))
		{
			TerrainPanel* tp = m_Host.GetTerrainPanel();
			if (tp)
			{
				const bool shift = Input::IsKeyPressed(KeyCodes::LeftShift) || Input::IsKeyPressed(KeyCodes::RightShift);
				if (ctx.IsKeyPressed(KeyCodes::LeftBracket))
				{
					if (shift)
						tp->SetBrushStrength(tp->GetBrushStrength() - 0.05f);
					else
						tp->SetBrushRadius(tp->GetBrushRadius() - 1.0f);
				}
				if (ctx.IsKeyPressed(KeyCodes::RightBracket))
				{
					if (shift)
						tp->SetBrushStrength(tp->GetBrushStrength() + 0.05f);
					else
						tp->SetBrushRadius(tp->GetBrushRadius() + 1.0f);
				}
			}
		}

		// Shift+1/2/3 模式快速切换
		{
			const bool shift = Input::IsKeyPressed(KeyCodes::LeftShift) || Input::IsKeyPressed(KeyCodes::RightShift);
			if (shift)
			{
				if (ctx.IsKeyPressed(KeyCodes::D1))
					m_Host.SetEditorToolMode(EditorToolMode::Select);
				else if (ctx.IsKeyPressed(KeyCodes::D2))
				{
					m_Host.SetEditorToolMode(EditorToolMode::Landscape);
					m_Host.ActivatePanel("terrain_editor");
				}
				else if (ctx.IsKeyPressed(KeyCodes::D3))
				{
					m_Host.SetEditorToolMode(EditorToolMode::Foliage);
					m_Host.ActivatePanel("foliage_editor");
				}
				else if (ctx.IsKeyPressed(KeyCodes::D4))
				{
					m_Host.SetEditorToolMode(EditorToolMode::PhysicsPainter);
					m_Host.ActivatePanel("physicspainter");
				}
			}
		}

		if (!play && isLandscapeMode && m_Host.HasRenderedScene())
		{
			Entity terrainEntity;
			Ref<Scene> activeScene = m_Host.GetActiveScene();
			if (activeScene)
			{
				Entity selectedEnt = m_Host.GetSelectedEntity();
				if (selectedEnt.IsValid() && selectedEnt.GetScene() == activeScene.get() && selectedEnt.HasComponent<TerrainComponent>())
				{
					terrainEntity = selectedEnt;
				}
				else
				{
					const auto& registry = std::as_const(*activeScene).GetRegistry();
					for (auto [handle, comp] : registry.view<TerrainComponent>().each())
					{
						terrainEntity = Entity(activeScene.get(), handle);
						break;
					}
				}
			}

			if (terrainEntity.IsValid())
			{
				glm::mat4 terrainWorld = glm::mat4(1.0f);
				if (terrainEntity.HasComponent<TransformComponent>())
				{
					terrainWorld = terrainEntity.GetComponent<TransformComponent>().GetLocalMatrix();
					if (terrainEntity.HasComponent<WorldTransformComponent>())
						terrainWorld = terrainEntity.GetComponent<WorldTransformComponent>().Matrix;
				}

				auto& comp = terrainEntity.GetComponent<TerrainComponent>();
				std::shared_ptr<TerrainData> terrain = TerrainRenderer::GetOrCreateTerrainData(comp.TerrainAsset, comp.CellSize, comp.HeightScale);

				TerrainPanel* terrainPanel = m_Host.GetTerrainPanel();

				const glm::vec2 mousePos = ctx.Input().MousePos;
				const bool mouseInViewport = (mousePos.x >= sceneRect.X && mousePos.x <= sceneRect.X + sceneRect.W &&
					mousePos.y >= sceneRect.Y && mousePos.y <= sceneRect.Y + sceneRect.H);

				if (terrain && mouseInViewport && !overBanner && !overPreview && !m_Host.IsFlyNavigating())
				{
					const Wui::GizmoCamera gizmoCamera = m_Host.GetGizmoCamera();
					const float ndcX = ((mousePos.x - sceneRect.X) / sceneRect.W) * 2.0f - 1.0f;
					const float ndcY = 1.0f - ((mousePos.y - sceneRect.Y) / sceneRect.H) * 2.0f;

					const glm::mat4 invVP = glm::inverse(gizmoCamera.ViewProjection);
					glm::vec4 nearPointClip(ndcX, ndcY, -1.0f, 1.0f);
					glm::vec4 farPointClip(ndcX, ndcY, 1.0f, 1.0f);
					glm::vec4 nearPointWorld = invVP * nearPointClip;
					glm::vec4 farPointWorld = invVP * farPointClip;
					if (std::abs(nearPointWorld.w) > 1e-6f) nearPointWorld /= nearPointWorld.w;
					if (std::abs(farPointWorld.w) > 1e-6f) farPointWorld /= farPointWorld.w;

					glm::vec3 rayOrigin = gizmoCamera.Position;
					glm::vec3 rayDir = glm::normalize(glm::vec3(farPointWorld) - rayOrigin);
					if (gizmoCamera.IsOrthographic)
					{
						rayOrigin = glm::vec3(nearPointWorld);
						rayDir = gizmoCamera.Forward;
					}

					const glm::mat4 invTerrainWorld = glm::inverse(terrainWorld);
					const glm::vec3 localOrigin = glm::vec3(invTerrainWorld * glm::vec4(rayOrigin, 1.0f));
					const glm::vec3 localDir = glm::normalize(glm::vec3(invTerrainWorld * glm::vec4(rayDir, 0.0f)));

					TerrainRaycastHit hit;
					const bool hasHit = terrain->Raycast(localOrigin, localDir, 2000.0f, hit);

					if (hasHit)
					{
						const float R = terrainPanel ? terrainPanel->GetBrushRadius() : 15.0f;
						const TerrainToolTab toolTab = terrainPanel ? terrainPanel->GetToolTab() : TerrainToolTab::Sculpt;
						const TerrainSculptMode sculptMode = terrainPanel ? terrainPanel->GetSculptMode() : TerrainSculptMode::Raise;
						const bool shiftPressed = Input::IsKeyPressed(KeyCodes::LeftShift) || Input::IsKeyPressed(KeyCodes::RightShift);

						// 1. 动态 3D 地表贴合外圆环 (32 段)
						constexpr int kSegments = 32;
						glm::vec4 outerClips[kSegments];
						for (int i = 0; i < kSegments; ++i)
						{
							const float angle = static_cast<float>(i) / static_cast<float>(kSegments) * 6.2831853f;
							const float px = hit.Point.x + R * std::cos(angle);
							const float pz = hit.Point.z + R * std::sin(angle);
							const float py = terrain->SampleHeight(px, pz) + 0.05f;
							const glm::vec4 localPos(px, py, pz, 1.0f);
							outerClips[i] = gizmoCamera.ViewProjection * (terrainWorld * localPos);
						}

						// 外环高对比高光色
						Wui::WuiColor outerColor = theme.Info; // default Raise
						if (toolTab == TerrainToolTab::Sculpt)
						{
							TerrainSculptMode eff = sculptMode;
							if (shiftPressed)
							{
								if (eff == TerrainSculptMode::Raise) eff = TerrainSculptMode::Lower;
								else if (eff == TerrainSculptMode::Lower) eff = TerrainSculptMode::Raise;
							}
							switch (eff)
							{
							case TerrainSculptMode::Raise:   outerColor = theme.Info; break;
							case TerrainSculptMode::Lower:   outerColor = theme.Warning; break;
							case TerrainSculptMode::Smooth:  outerColor = theme.Accent; break;
							case TerrainSculptMode::Flatten: outerColor = theme.Warning; break;
							case TerrainSculptMode::Erode:   outerColor = theme.Accent; break;
							case TerrainSculptMode::Hole:    outerColor = theme.Danger; break;
							default: break;
							}
						}
						else if (toolTab == TerrainToolTab::Paint)
						{
							outerColor = shiftPressed ? theme.Accent : theme.Success;
						}

						for (int i = 0; i < kSegments; ++i)
						{
							PushProjectedSegment(ctx, gizmoCamera.ViewProjection, sceneRect,
								outerClips[i], outerClips[(i + 1) % kSegments], outerColor, 2.4f);
						}

						// 2. 内衰减细线圆环
						const float falloffRatio = terrainPanel ? terrainPanel->GetBrushFalloffFactor() : 0.5f;
						const float innerR = R * (1.0f - falloffRatio);
						if (innerR > 0.05f)
						{
							glm::vec4 innerClips[kSegments];
							for (int i = 0; i < kSegments; ++i)
							{
								const float angle = static_cast<float>(i) / static_cast<float>(kSegments) * 6.2831853f;
								const float px = hit.Point.x + innerR * std::cos(angle);
								const float pz = hit.Point.z + innerR * std::sin(angle);
								const float py = terrain->SampleHeight(px, pz) + 0.05f;
								const glm::vec4 localPos(px, py, pz, 1.0f);
								innerClips[i] = gizmoCamera.ViewProjection * (terrainWorld * localPos);
							}
							const Wui::WuiColor innerColor { outerColor.R, outerColor.G, outerColor.B, 0.55f };
							for (int i = 0; i < kSegments; ++i)
							{
								PushProjectedSegment(ctx, gizmoCamera.ViewProjection, sceneRect,
									innerClips[i], innerClips[(i + 1) % kSegments], innerColor, 1.2f);
							}
						}

						// 3. 浮动 HUD 胶囊卡片
						const glm::vec3 worldHit = glm::vec3(terrainWorld * glm::vec4(hit.Point, 1.0f));
						const glm::vec4 clipHit = gizmoCamera.ViewProjection * glm::vec4(worldHit, 1.0f);
						if (clipHit.w > 0.15f)
						{
							const glm::vec3 ndcHit = glm::vec3(clipHit) / clipHit.w;
							const glm::vec2 screenHit {
								sceneRect.X + (ndcHit.x + 1.0f) * 0.5f * sceneRect.W,
								sceneRect.Y + (1.0f - ndcHit.y) * 0.5f * sceneRect.H
							};
							char hudText[128] = {};
							const float strength = terrainPanel ? terrainPanel->GetBrushStrength() : 0.5f;
							const int strPct = static_cast<int>(strength * 100.0f);
							if (toolTab == TerrainToolTab::Sculpt)
							{
								const char* modeName = "Raise";
								TerrainSculptMode eff = sculptMode;
								if (shiftPressed)
								{
									if (eff == TerrainSculptMode::Raise) eff = TerrainSculptMode::Lower;
									else if (eff == TerrainSculptMode::Lower) eff = TerrainSculptMode::Raise;
								}
								switch (eff)
								{
								case TerrainSculptMode::Raise: modeName = "Raise"; break;
								case TerrainSculptMode::Lower: modeName = "Lower"; break;
								case TerrainSculptMode::Smooth: modeName = "Smooth"; break;
								case TerrainSculptMode::Flatten: modeName = "Flatten"; break;
								case TerrainSculptMode::Erode: modeName = "Erode"; break;
								case TerrainSculptMode::Hole: modeName = "Hole"; break;
								default: break;
								}
								std::snprintf(hudText, sizeof(hudText), "Sculpt: %s | R: %.1fm | S: %d%% | Shift: 反向",
									modeName, R, strPct);
							}
							else if (toolTab == TerrainToolTab::Paint)
							{
								const uint32_t layerIdx = terrainPanel ? terrainPanel->GetActivePaintLayer() : 0;
								const std::string layerName = (terrain && layerIdx < 4) ? terrain->GetLayer(layerIdx).Name : ("Layer " + std::to_string(layerIdx));
								if (shiftPressed)
								{
									std::snprintf(hudText, sizeof(hudText), "Paint: [Smooth 平滑] | R: %.1fm | S: %d%%",
										R, strPct);
								}
								else
								{
									std::snprintf(hudText, sizeof(hudText), "Paint: %s | R: %.1fm | S: %d%%",
										layerName.c_str(), R, strPct);
								}
							}
							else
							{
								std::snprintf(hudText, sizeof(hudText), "Terrain | R: %.1fm | S: %d%%", R, strPct);
							}

							const float fontSz = theme.FontSizeSmall;
							const float textW = ctx.MeasureTextWidth(hudText, fontSz);
							const float cardW = textW + 16.0f;
							const float cardH = 22.0f;
							float cardX = screenHit.x + 20.0f;
							float cardY = screenHit.y - 28.0f;
							if (cardX + cardW > sceneRect.X + sceneRect.W - 10.0f)
								cardX = screenHit.x - cardW - 10.0f;
							if (cardY < sceneRect.Y + 10.0f)
								cardY = sceneRect.Y + 10.0f;

							const Wui::WuiRect hudRect { cardX, cardY, cardW, cardH };
							Wui::Toolbar(ctx, hudRect, theme, 4.0f, 0.88f);
							Wui::Label(ctx, { cardX + 8.0f, cardY + 3.0f }, hudText, theme.Text, fontSz);
						}

						// 4. 视口直接拖拽雕刻与混绘
						const bool isMouseDown = ctx.Input().MouseDown[0];
						if (isMouseDown)
						{
							if (!m_TerrainStrokeActive)
							{
								m_TerrainStrokeActive = true;
								m_StrokeTerrain = terrain;
								m_StrokeTab = static_cast<uint8_t>(toolTab);
								m_BeforeHeights = terrain->GetHeightBuffer();
								m_BeforeSplatmap = terrain->GetSplatmapBuffer();
								m_BeforeHoles = terrain->GetHoleBuffer();
							}

							const float dt = 0.016f;
							if (toolTab == TerrainToolTab::Sculpt)
							{
								TerrainSculptMode eff = sculptMode;
								if (shiftPressed)
								{
									if (eff == TerrainSculptMode::Raise) eff = TerrainSculptMode::Lower;
									else if (eff == TerrainSculptMode::Lower) eff = TerrainSculptMode::Raise;
								}
								const TerrainBrushFalloff falloffType = terrainPanel ? terrainPanel->GetBrushFalloff() : TerrainBrushFalloff::Smooth;
								const float strength = terrainPanel ? terrainPanel->GetBrushStrength() : 0.5f;
								switch (eff)
								{
								case TerrainSculptMode::Raise:
									terrain->SculptRaiseLower(hit.Point.x, hit.Point.z, R, strength * 2.0f * dt * 60.0f, falloffType);
									break;
								case TerrainSculptMode::Lower:
									terrain->SculptRaiseLower(hit.Point.x, hit.Point.z, R, -strength * 2.0f * dt * 60.0f, falloffType);
									break;
								case TerrainSculptMode::Smooth:
									terrain->SculptSmooth(hit.Point.x, hit.Point.z, R, strength * 0.5f, falloffType);
									break;
								case TerrainSculptMode::Flatten:
								{
									const float targetH = terrainPanel ? terrainPanel->GetTargetHeight() : 0.0f;
									terrain->SculptFlatten(hit.Point.x, hit.Point.z, R, targetH, strength * 0.5f, falloffType);
									break;
								}
								case TerrainSculptMode::Erode:
									terrain->SculptErode(hit.Point.x, hit.Point.z, R, strength * 0.2f, 1);
									break;
								case TerrainSculptMode::Hole:
									terrain->PaintHole(hit.Point.x, hit.Point.z, R, !shiftPressed);
									break;
								default: break;
								}
							}
							else if (toolTab == TerrainToolTab::Paint)
							{
								const TerrainBrushFalloff falloffType = terrainPanel ? terrainPanel->GetBrushFalloff() : TerrainBrushFalloff::Smooth;
								const float strength = terrainPanel ? terrainPanel->GetBrushStrength() : 0.5f;
								if (shiftPressed)
								{
									terrain->SculptSmooth(hit.Point.x, hit.Point.z, R, strength * 0.5f, falloffType);
								}
								else
								{
									const uint32_t activeLayer = terrainPanel ? terrainPanel->GetActivePaintLayer() : 0;
									terrain->PaintLayer(hit.Point.x, hit.Point.z, R, activeLayer, strength * 0.5f, falloffType);
								}
							}
							m_Host.MarkDocumentDirty();
						}
					}
				}

				// 鼠标释放那一帧：压入撤销命令
				if (m_TerrainStrokeActive && !ctx.Input().MouseDown[0])
				{
					m_TerrainStrokeActive = false;
					if (m_StrokeTerrain)
					{
						auto afterHeights = m_StrokeTerrain->GetHeightBuffer();
						auto afterSplat = m_StrokeTerrain->GetSplatmapBuffer();
						auto afterHoles = m_StrokeTerrain->GetHoleBuffer();
						if (afterHeights != m_BeforeHeights || afterSplat != m_BeforeSplatmap || afterHoles != m_BeforeHoles)
						{
							m_Host.PushCommand(std::make_unique<TerrainModifyCommand>(
								m_StrokeTerrain,
								std::move(m_BeforeHeights),
								std::move(m_BeforeSplatmap),
								std::move(m_BeforeHoles),
								std::move(afterHeights),
								std::move(afterSplat),
								std::move(afterHoles),
								m_StrokeTab == static_cast<uint8_t>(TerrainToolTab::Paint) ? "Terrain Paint" : "Terrain Sculpt"
							));
							m_Host.MarkDocumentDirty();
						}
					}
					m_StrokeTerrain.reset();
				}
			}
		}

		// ---- 3D 植被模式 (Foliage Mode) 视口直接操纵与笔刷渲染 ----
		const bool isFoliageMode = (m_Host.GetEditorToolMode() == EditorToolMode::Foliage);
		// Play/Simulate 下这是**运行时画面**：笔刷圈、HUD 与散布/擦除一律不参与。
		// 旧实现没有这个 gate，运行中左键会直接往播放副本里写植被（并经由共享
		// FoliageData 改到编辑器资产），表现为“人物一走动植被就在变”。
		if (!play && !m_Host.IsReadOnlyMode() && isFoliageMode && (focused || hovered))
		{
			FoliagePanel* foliagePanel = m_Host.GetFoliagePanel();
			FoliageData* foliageData = foliagePanel ? foliagePanel->GetActiveFoliage() : nullptr;
			if (foliagePanel && foliageData)
			{
				const bool shift = Input::IsKeyPressed(KeyCodes::LeftShift) || Input::IsKeyPressed(KeyCodes::RightShift);
				if (ctx.IsKeyPressed(KeyCodes::LeftBracket))
				{
					if (shift)
						foliagePanel->SetBrushDensity(std::clamp(foliagePanel->GetBrushDensity() - 0.2f, 0.1f, 10.0f));
					else
						foliagePanel->SetBrushRadius(std::clamp(foliagePanel->GetBrushRadius() - 1.0f, 1.0f, 100.0f));
				}
				else if (ctx.IsKeyPressed(KeyCodes::RightBracket))
				{
					if (shift)
						foliagePanel->SetBrushDensity(std::clamp(foliagePanel->GetBrushDensity() + 0.2f, 0.1f, 10.0f));
					else
						foliagePanel->SetBrushRadius(std::clamp(foliagePanel->GetBrushRadius() + 1.0f, 1.0f, 100.0f));
				}

				Ref<Scene> activeScene = m_Host.GetActiveScene();
				std::shared_ptr<TerrainData> terrain = nullptr;
				glm::mat4 terrainWorld { 1.0f };
				if (activeScene)
				{
					for (auto [handle, comp] : std::as_const(*activeScene).GetRegistry().view<TerrainComponent>().each())
					{
						Entity e(activeScene.get(), handle);
						if (e.HasComponent<TransformComponent>())
						{
							terrainWorld = e.GetComponent<TransformComponent>().GetLocalMatrix();
							if (e.HasComponent<WorldTransformComponent>())
								terrainWorld = e.GetComponent<WorldTransformComponent>().Matrix;
						}
						terrain = TerrainRenderer::GetOrCreateTerrainData(comp.TerrainAsset, comp.CellSize, comp.HeightScale);
						break;
					}
				}

				const glm::vec2 mousePos = ctx.Input().MousePos;
				const bool mouseInViewport = (mousePos.x >= sceneRect.X && mousePos.x <= sceneRect.X + sceneRect.W &&
					mousePos.y >= sceneRect.Y && mousePos.y <= sceneRect.Y + sceneRect.H);

				if (terrain && mouseInViewport && !m_Host.IsFlyNavigating())
				{
					const Wui::GizmoCamera gizmoCamera = m_Host.GetGizmoCamera();
					const float ndcX = ((mousePos.x - sceneRect.X) / sceneRect.W) * 2.0f - 1.0f;
					const float ndcY = 1.0f - ((mousePos.y - sceneRect.Y) / sceneRect.H) * 2.0f;

					const glm::mat4 invVP = glm::inverse(gizmoCamera.ViewProjection);
					glm::vec4 nearPointClip(ndcX, ndcY, -1.0f, 1.0f);
					glm::vec4 farPointClip(ndcX, ndcY, 1.0f, 1.0f);
					glm::vec4 nearPointWorld = invVP * nearPointClip;
					glm::vec4 farPointWorld = invVP * farPointClip;
					if (std::abs(nearPointWorld.w) > 1e-6f) nearPointWorld /= nearPointWorld.w;
					if (std::abs(farPointWorld.w) > 1e-6f) farPointWorld /= farPointWorld.w;

					glm::vec3 rayOrigin = gizmoCamera.Position;
					glm::vec3 rayDir = glm::normalize(glm::vec3(farPointWorld) - rayOrigin);
					if (gizmoCamera.IsOrthographic)
					{
						rayOrigin = glm::vec3(nearPointWorld);
						rayDir = gizmoCamera.Forward;
					}

					const glm::mat4 invTerrainWorld = glm::inverse(terrainWorld);
					const glm::vec3 localOrigin = glm::vec3(invTerrainWorld * glm::vec4(rayOrigin, 1.0f));
					const glm::vec3 localDir = glm::normalize(glm::vec3(invTerrainWorld * glm::vec4(rayDir, 0.0f)));

					TerrainRaycastHit hit;
					const bool hasHit = terrain->Raycast(localOrigin, localDir, 2000.0f, hit);
					if (hasHit)
					{
						const bool interactMode = (foliagePanel->GetTool() == FoliageBrushTool::Interact);
						const bool eraseMode = !interactMode && (shift || (foliagePanel->GetTool() == FoliageBrushTool::Erase));
						const float R = interactMode ? foliagePanel->GetInteractRadius() : foliagePanel->GetBrushRadius();

						// 32 段贴地光圈
						constexpr int kSegments = 32;
						glm::vec4 clips[kSegments];
						for (int i = 0; i < kSegments; ++i)
						{
							const float angle = static_cast<float>(i) / static_cast<float>(kSegments) * 6.2831853f;
							const float px = hit.Point.x + R * std::cos(angle);
							const float pz = hit.Point.z + R * std::sin(angle);
							const float py = terrain->SampleHeight(px, pz) + 0.05f;
							clips[i] = gizmoCamera.ViewProjection * (terrainWorld * glm::vec4(px, py, pz, 1.0f));
						}

						const Wui::WuiColor ringColor = interactMode ? Wui::WuiColor{ 0.15f, 0.85f, 0.95f, 0.95f } : (eraseMode ? theme.Danger : theme.Success);

						for (int i = 0; i < kSegments; ++i)
						{
							PushProjectedSegment(ctx, gizmoCamera.ViewProjection, sceneRect,
								clips[i], clips[(i + 1) % kSegments], ringColor, 2.4f);
						}

						const std::vector<size_t> activeTypes = foliagePanel->GetActivePaintingTypes();
						const bool multiSpecies = (activeTypes.size() > 1);

						// 浮动 HUD 提示
						char hudText[160] = {};
						if (interactMode)
						{
							std::snprintf(hudText, sizeof(hudText), "Foliage: [受力推挤 Interact] 拖拽测试倒伏 | 半径: %.1fm | 强度: %.1f",
								R, foliagePanel->GetInteractStrength());
						}
						else if (eraseMode)
						{
							const bool eraseAll = (foliagePanel->GetEraseTarget() == FoliageEraseTarget::AllSpecies) || ctx.Input().Alt;
							std::snprintf(hudText, sizeof(hudText), "Foliage: [擦除 Erase] %s | R: %.1fm | Shift: 擦除",
								eraseAll ? "全部物种" : "选中物种", R);
						}
						else if (multiSpecies)
						{
							std::snprintf(hudText, sizeof(hudText), "Foliage: [混刷 Multi (%zu种)] | R: %.1fm | D: %.1f | Shift: 擦除",
								activeTypes.size(), R, foliagePanel->GetBrushDensity());
						}
						else
						{
							const size_t typeIdx = !activeTypes.empty() ? activeTypes[0] : foliagePanel->GetActiveTypeIndex();
							std::string typeName = (typeIdx < foliageData->GetTypeCount())
								? foliageData->GetType(typeIdx).Name : "物种";
							std::snprintf(hudText, sizeof(hudText), "Foliage: [散布 Paint] %s | R: %.1fm | D: %.1f | Shift: 擦除",
								typeName.c_str(), R, foliagePanel->GetBrushDensity());
						}

						const float fontSz = theme.FontSizeSmall;
						const float textW = ctx.MeasureTextWidth(hudText, fontSz);
						const float cardW = textW + 16.0f;
						const float cardH = 22.0f;
						float cardX = mousePos.x + 20.0f;
						float cardY = mousePos.y - 28.0f;
						if (cardX + cardW > sceneRect.X + sceneRect.W - 10.0f) cardX = mousePos.x - cardW - 10.0f;
						if (cardY < sceneRect.Y + 10.0f) cardY = sceneRect.Y + 10.0f;

						Wui::Toolbar(ctx, { cardX, cardY, cardW, cardH }, theme, 4.0f, 0.88f);
						Wui::Label(ctx, { cardX + 8.0f, cardY + 3.0f }, hudText, theme.Text, fontSz);

						// 视口直接拖拽种草与擦除（带笔刷间距与冷却节流，杜绝原地无限重叠）
						static glm::vec3 s_LastFoliageHit(1e9f);
						static float s_FoliagePaintCooldown = 0.0f;
						s_FoliagePaintCooldown += 0.016f;

						if (ctx.Input().MouseDown[0])
						{
							const glm::vec3 worldHitPoint = glm::vec3(terrainWorld * glm::vec4(hit.Point, 1.0f));
							const float distMoved = glm::distance(glm::vec2(worldHitPoint.x, worldHitPoint.z),
								glm::vec2(s_LastFoliageHit.x, s_LastFoliageHit.z));

							if (interactMode)
							{
								FoliageInteractionSystem::TriggerImpulse(worldHitPoint, R, foliagePanel->GetInteractStrength(), 3.5f);
								FoliageRenderer::AddLocalWind(worldHitPoint, R, foliagePanel->GetInteractStrength() * 2.5f,
									4.0f, 1.5f, LocalWindType::Radial, glm::vec3(0.0f, 1.0f, 0.0f));
							}
							else if (distMoved >= R * 0.25f || s_FoliagePaintCooldown >= 0.10f)
							{
								if (eraseMode)
								{
									const bool eraseAll = (foliagePanel->GetEraseTarget() == FoliageEraseTarget::AllSpecies) || ctx.Input().Alt;
									const int32_t eraseIdx = eraseAll ? -1 : static_cast<int32_t>(foliagePanel->GetActiveTypeIndex());
									foliageData->EraseFoliage(worldHitPoint, R, eraseIdx);
								}
								else
								{
									if (activeTypes.size() > 1)
									{
										foliageData->PaintFoliageMulti(worldHitPoint, R, foliagePanel->GetBrushDensity(),
											activeTypes, terrain.get(), terrainWorld);
									}
									else if (!activeTypes.empty())
									{
										foliageData->PaintFoliage(worldHitPoint, R, foliagePanel->GetBrushDensity(),
											activeTypes[0], terrain.get(), terrainWorld);
									}
									else
									{
										foliageData->PaintFoliage(worldHitPoint, R, foliagePanel->GetBrushDensity(),
											foliagePanel->GetActiveTypeIndex(), terrain.get(), terrainWorld);
									}
								}
								s_LastFoliageHit = worldHitPoint;
								s_FoliagePaintCooldown = 0.0f;
								m_Host.MarkDocumentDirty();
							}
						}
						if (ctx.Input().MouseReleased[0])
						{
							s_LastFoliageHit = glm::vec3(1e9f);
							foliagePanel->SaveCurrentFoliage();
						}
					}
				}
			}
		}

		// ---- 3D 物理画笔模式 (Physics Painter Mode) 视口直接拾取、笔刷圈与实时散布 ----
		if (!play && !m_Host.IsReadOnlyMode() && isPainterMode && (focused || hovered))
		{
			// 快捷键调整笔刷半径: [ 缩小 / ] 放大
			if (ctx.IsKeyPressed(KeyCodes::LeftBracket))
			{
				PhysicsPainterPanel::SetBrushRadius(std::clamp(PhysicsPainterPanel::GetBrushRadius() - 0.5f, 0.5f, 50.0f));
			}
			else if (ctx.IsKeyPressed(KeyCodes::RightBracket))
			{
				PhysicsPainterPanel::SetBrushRadius(std::clamp(PhysicsPainterPanel::GetBrushRadius() + 0.5f, 0.5f, 50.0f));
			}

			const glm::vec2 mousePos = ctx.Input().MousePos;
			const bool mouseInViewport = (mousePos.x >= sceneRect.X && mousePos.x <= sceneRect.X + sceneRect.W &&
				mousePos.y >= sceneRect.Y && mousePos.y <= sceneRect.Y + sceneRect.H);

			if (mouseInViewport && !m_Host.IsFlyNavigating())
			{
				const Wui::GizmoCamera gizmoCamera = m_Host.GetGizmoCamera();
				const float ndcX = ((mousePos.x - sceneRect.X) / sceneRect.W) * 2.0f - 1.0f;
				const float ndcY = 1.0f - ((mousePos.y - sceneRect.Y) / sceneRect.H) * 2.0f;

				const glm::mat4 invVP = glm::inverse(gizmoCamera.ViewProjection);
				glm::vec4 nearPointClip(ndcX, ndcY, -1.0f, 1.0f);
				glm::vec4 farPointClip(ndcX, ndcY, 1.0f, 1.0f);
				glm::vec4 nearPointWorld = invVP * nearPointClip;
				glm::vec4 farPointWorld = invVP * farPointClip;
				if (std::abs(nearPointWorld.w) > 1e-6f) nearPointWorld /= nearPointWorld.w;
				if (std::abs(farPointWorld.w) > 1e-6f) farPointWorld /= farPointWorld.w;

				glm::vec3 rayOrigin = gizmoCamera.Position;
				glm::vec3 rayDir = glm::normalize(glm::vec3(farPointWorld) - rayOrigin);
				if (gizmoCamera.IsOrthographic)
				{
					rayOrigin = glm::vec3(nearPointWorld);
					rayDir = gizmoCamera.Forward;
				}

				Ref<Scene> activeScene = m_Host.GetActiveScene();
				glm::vec3 worldHitPoint { 0.0f };
				glm::vec3 worldHitNormal { 0.0f, 1.0f, 0.0f };
				bool hasHit = false;

				if (activeScene)
				{
					for (auto [handle, comp] : std::as_const(*activeScene).GetRegistry().view<TerrainComponent>().each())
					{
						Entity e(activeScene.get(), handle);
						glm::mat4 tWorld { 1.0f };
						if (e.HasComponent<TransformComponent>())
						{
							tWorld = e.GetComponent<TransformComponent>().GetLocalMatrix();
							if (e.HasComponent<WorldTransformComponent>())
								tWorld = e.GetComponent<WorldTransformComponent>().Matrix;
						}
						std::shared_ptr<TerrainData> tData = TerrainRenderer::GetOrCreateTerrainData(comp.TerrainAsset, comp.CellSize, comp.HeightScale);
						if (tData)
						{
							const glm::mat4 invTW = glm::inverse(tWorld);
							const glm::vec3 localOrig = glm::vec3(invTW * glm::vec4(rayOrigin, 1.0f));
							const glm::vec3 localD = glm::normalize(glm::vec3(invTW * glm::vec4(rayDir, 0.0f)));
							TerrainRaycastHit tHit;
							if (tData->Raycast(localOrig, localD, 2000.0f, tHit))
							{
								worldHitPoint = glm::vec3(tWorld * glm::vec4(tHit.Point, 1.0f));
								worldHitNormal = glm::normalize(glm::vec3(glm::transpose(invTW) * glm::vec4(tHit.Normal, 0.0f)));
								hasHit = true;
								break;
							}
						}
					}
				}

				if (!hasHit && std::abs(rayDir.y) > 1e-4f)
				{
					const float t = (0.0f - rayOrigin.y) / rayDir.y;
					if (t > 0.0f)
					{
						worldHitPoint = rayOrigin + rayDir * t;
						worldHitNormal = glm::vec3(0.0f, 1.0f, 0.0f);
						hasHit = true;
					}
				}

				if (hasHit)
				{
					const float R = PhysicsPainterPanel::GetBrushRadius();
					constexpr int kSegments = 32;
					glm::vec4 clips[kSegments];
					for (int i = 0; i < kSegments; ++i)
					{
						const float angle = static_cast<float>(i) / static_cast<float>(kSegments) * 6.2831853f;
						const glm::vec3 offset(R * std::cos(angle), 0.0f, R * std::sin(angle));
						const glm::vec3 pt = worldHitPoint + offset + worldHitNormal * 0.03f;
						clips[i] = gizmoCamera.ViewProjection * glm::vec4(pt, 1.0f);
					}

					const Wui::WuiColor ringColor { 1.0f, 0.65f, 0.15f, 0.95f };
					for (int i = 0; i < kSegments; ++i)
					{
						PushProjectedSegment(ctx, gizmoCamera.ViewProjection, sceneRect,
							clips[i], clips[(i + 1) % kSegments], ringColor, 2.4f);
					}

					// 绘制中心十字准星
					const glm::vec4 cX0 = gizmoCamera.ViewProjection * glm::vec4(worldHitPoint + glm::vec3(-0.3f, 0.03f, 0.0f), 1.0f);
					const glm::vec4 cX1 = gizmoCamera.ViewProjection * glm::vec4(worldHitPoint + glm::vec3( 0.3f, 0.03f, 0.0f), 1.0f);
					const glm::vec4 cZ0 = gizmoCamera.ViewProjection * glm::vec4(worldHitPoint + glm::vec3(0.0f, 0.03f, -0.3f), 1.0f);
					const glm::vec4 cZ1 = gizmoCamera.ViewProjection * glm::vec4(worldHitPoint + glm::vec3(0.0f, 0.03f,  0.3f), 1.0f);
					PushProjectedSegment(ctx, gizmoCamera.ViewProjection, sceneRect, cX0, cX1, ringColor, 1.8f);
					PushProjectedSegment(ctx, gizmoCamera.ViewProjection, sceneRect, cZ0, cZ1, ringColor, 1.8f);

					// 浮动 HUD 提示
					char hudText[160] = {};
					std::snprintf(hudText, sizeof(hudText), "Physics Painter: [物理画笔] R: %.1fm | D: %d/笔 | [ / ] 调半径",
						R, PhysicsPainterPanel::GetBrushDensity());
					const float hudW = ctx.MeasureTextWidth(hudText, 12.0f);
					const Wui::WuiRect hudRect { sceneRect.X + (sceneRect.W - hudW) * 0.5f - 8.0f, sceneRect.Y + sceneRect.H - 32.0f, hudW + 16.0f, 22.0f };
					Wui::PanelBackground(ctx, hudRect, Wui::WuiColor{ 0.08f, 0.08f, 0.10f, 0.88f }, 4.0f);
					Wui::HighlightOutline(ctx, hudRect, ringColor, 4.0f, 1.0f);
					Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 4.0f }, hudText, ringColor, 12.0f);

					// 点击或按住拖拽散布物体
					if (ctx.Input().MouseDown[0] && !m_GizmoActive)
					{
						static uint64_t s_LastPaintFrame = 0;
						if (ctx.Frame() - s_LastPaintFrame >= 6)
						{
							s_LastPaintFrame = ctx.Frame();
							if (activeScene)
							{
								PhysicsPainterPanel::PaintAt(*activeScene, worldHitPoint, worldHitNormal, m_Host);
								m_Host.MarkDocumentDirty();
							}
						}
					}
				}
			}
		}

				// 3D 网格实体的选中框:投影单位网格包围盒的 12 条棱(真 3D 线框盒),画在场景图之上。
		// 近的棱更实、远的棱更淡,按相机深度排序(远的先画);整体用 ClipPush 夹在场景图区域内。
		// 旧实现是 SceneRenderer 里用 Renderer2D 按实体 Transform 画的一个 2D 方块 ——
		// 在 3D 视口里看着就是"一个跟方块无关的方形,位置还不对"(用户 2026-09-16 反馈)。

		// ---- 相机可视化: 已移入下方 P4-U6 范围覆盖层统一执行逐段深度遮挡 ----

		// ---- P4-U6:范围可视化(光源范围 + 碰撞体轮廓) ----
		//
		// 用户 2026-09-21:「像光照这种需要标明范围的目前没有范围显示」+
		// 「应该在 view 中有个选项可以选是否显示范围;如果这个选项没开应该只画选中」。
		// 口径:开关(编辑器偏好 ▸ Viewport / 视口工具栏 Overlays)= 画全部还是只画选中;
		// 选中实体的范围**永远**画(与相机视锥一致:选中就看得见自己的影响范围)。
		{
			const Editor::EditorPreferencesData& prefs = Editor::EditorPreferences::Get().Data();
			const bool showAllLights = prefs.ViewportLightRangesAll;
			const bool showAllColliders = prefs.ViewportColliderOutlinesAll;
			const Ref<Scene> overlayScene = m_Host.GetActiveScene();
			if (!play && overlayScene && m_Host.HasRenderedScene())
			{
				const entt::registry& registry = static_cast<const Scene*>(overlayScene.get())->GetRegistry();
				const Wui::GizmoCamera overlayCamera = m_Host.GetGizmoCamera();
				const entt::entity selectedHandle = selected.IsValid()
					&& selected.GetScene() == overlayScene.get()
					? static_cast<entt::entity>(selected) : entt::null;
				const glm::vec3 cameraPos = overlayCamera.Position;

				// 获取地形高程数据与静态碰撞体包围盒（用于视口元素未选中时的遮挡剔除）
				std::shared_ptr<TerrainData> terrainData = nullptr;
				glm::vec3 terrainOrigin { 0.0f };
				glm::vec3 terrainScale { 1.0f };
				for (auto [h, tc, xf] : registry.view<TerrainComponent, TransformComponent>().each())
				{
					terrainData = TerrainRenderer::GetOrCreateTerrainData(tc.TerrainAsset, tc.CellSize, tc.HeightScale);
					if (terrainData)
					{
						terrainOrigin = xf.Location;
						terrainScale = glm::max(glm::abs(xf.Scale), glm::vec3(0.001f));
						break;
					}
				}
				if (!terrainData)
				{
					for (auto [h, tc] : registry.view<TerrainComponent>().each())
					{
						terrainData = TerrainRenderer::GetOrCreateTerrainData(tc.TerrainAsset, tc.CellSize, tc.HeightScale);
						if (terrainData)
							break;
					}
				}

				auto sampleTerrainWorldHeight = [&](float worldX, float worldZ, float& outWorldH) -> bool {
					if (!terrainData) return false;
					const float localX = (worldX - terrainOrigin.x) / terrainScale.x;
					const float localZ = (worldZ - terrainOrigin.z) / terrainScale.z;
					const float maxLocalX = static_cast<float>(terrainData->GetWidth() > 0 ? terrainData->GetWidth() - 1 : 0) * terrainData->GetCellSize();
					const float maxLocalZ = static_cast<float>(terrainData->GetHeight() > 0 ? terrainData->GetHeight() - 1 : 0) * terrainData->GetCellSize();
					if (localX < 0.0f || localZ < 0.0f || localX > maxLocalX || localZ > maxLocalZ)
						return false;
					outWorldH = terrainOrigin.y + terrainData->SampleHeight(localX, localZ) * terrainScale.y;
					return true;
				};

				const std::vector<SceneOccluder> occluderBoxes = CollectSceneOccluders(registry, selectedHandle);

				auto isOccluded = [&](const glm::vec3& targetPt, bool isSurface = false, float extraThreshold = 0.0f, entt::entity ignoreEntity = entt::null) -> bool
				{
					const glm::vec3 toTarget = targetPt - cameraPos;
					const float dist = glm::length(toTarget);
					if (dist < 1.0f)
						return false;

					if (terrainData)
					{
						float targetGndH = 0.0f;
						if (!isSurface && sampleTerrainWorldHeight(targetPt.x, targetPt.z, targetGndH))
						{
							if (targetPt.y < targetGndH - 0.04f)
								return true;
						}

						const int numSteps = std::clamp(static_cast<int>(std::ceil(dist / 2.0f)), isSurface ? 4 : 6, 16);
						const float normDist = dist;
						for (int s = 1; s <= numSteps; ++s)
						{
							const float t = static_cast<float>(s) / static_cast<float>(numSteps);
							const float distFromTarget = (1.0f - t) * normDist;
							if (isSurface && distFromTarget < 2.5f)
								break;
							if (!isSurface && distFromTarget < 0.25f)
								break;

							const glm::vec3 rayPt = glm::mix(cameraPos, targetPt, t);
							float gndH = 0.0f;
							if (sampleTerrainWorldHeight(rayPt.x, rayPt.z, gndH))
							{
								const float heightMargin = isSurface ? (0.25f + extraThreshold) : (0.06f + extraThreshold);
								if (gndH > rayPt.y + heightMargin)
									return true;
							}
						}
					}

					if (dist > 0.5f)
					{
						const glm::vec3 normDir = toTarget / dist;
						if (RayHitsSceneOccluders(occluderBoxes, cameraPos, normDir, dist, targetPt, ignoreEntity))
							return true;
					}
					return false;
				};

				auto PushOccludedWorldSegment = [&](const glm::vec3& p0, const glm::vec3& p1,
					const Wui::WuiColor& color, float lineWidth, entt::entity ignoreEntity = entt::null)
				{
					const float totalDist = glm::distance(p0, p1);
					if (totalDist < 1e-4f)
						return;

					const int numSteps = std::clamp(static_cast<int>(std::ceil(totalDist / 0.40f)), 3, 24);

					std::vector<bool> vis(numSteps + 1);
					for (int i = 0; i <= numSteps; ++i)
					{
						const float t = static_cast<float>(i) / static_cast<float>(numSteps);
						const glm::vec3 pt = glm::mix(p0, p1, t);
						vis[i] = !isOccluded(pt, false, 0.0f, ignoreEntity);
					}

					float curStart = -1.0f;
					for (int i = 0; i < numSteps; ++i)
					{
						const float t0 = static_cast<float>(i) / static_cast<float>(numSteps);
						const float t1 = static_cast<float>(i + 1) / static_cast<float>(numSteps);
						const bool v0 = vis[i];
						const bool v1 = vis[i + 1];

						if (v0 && v1)
						{
							if (curStart < 0.0f)
								curStart = t0;
						}
						else if (v0 && !v1)
						{
							if (curStart < 0.0f)
								curStart = t0;
							float tVis = t0, tOcc = t1;
							for (int b = 0; b < 3; ++b)
							{
								const float tMid = (tVis + tOcc) * 0.5f;
								if (!isOccluded(glm::mix(p0, p1, tMid), false, 0.0f, ignoreEntity))
									tVis = tMid;
								else
									tOcc = tMid;
							}
							const glm::vec3 startPt = glm::mix(p0, p1, curStart);
							const glm::vec3 endPt = glm::mix(p0, p1, tVis);
							PushProjectedSegment(ctx, overlayCamera.ViewProjection, sceneRect,
								overlayCamera.ViewProjection * glm::vec4(startPt, 1.0f),
								overlayCamera.ViewProjection * glm::vec4(endPt, 1.0f),
								color, lineWidth);
							curStart = -1.0f;
						}
						else if (!v0 && v1)
						{
							float tOcc = t0, tVis = t1;
							for (int b = 0; b < 3; ++b)
							{
								const float tMid = (tVis + tOcc) * 0.5f;
								if (!isOccluded(glm::mix(p0, p1, tMid), false, 0.0f, ignoreEntity))
									tVis = tMid;
								else
									tOcc = tMid;
							}
							curStart = tVis;
						}
						else
						{
							if (curStart >= 0.0f)
							{
								const glm::vec3 startPt = glm::mix(p0, p1, curStart);
								const glm::vec3 endPt = glm::mix(p0, p1, t0);
								PushProjectedSegment(ctx, overlayCamera.ViewProjection, sceneRect,
									overlayCamera.ViewProjection * glm::vec4(startPt, 1.0f),
									overlayCamera.ViewProjection * glm::vec4(endPt, 1.0f),
									color, lineWidth);
								curStart = -1.0f;
							}
						}
					}

					if (curStart >= 0.0f)
					{
						const glm::vec3 startPt = glm::mix(p0, p1, curStart);
						const glm::vec3 endPt = p1;
						PushProjectedSegment(ctx, overlayCamera.ViewProjection, sceneRect,
							overlayCamera.ViewProjection * glm::vec4(startPt, 1.0f),
							overlayCamera.ViewProjection * glm::vec4(endPt, 1.0f),
							color, lineWidth);
					}
				};
				auto PushOccludedWorldSegmentLocal = [&](const glm::mat4& worldMatrix, const glm::vec3& from, const glm::vec3& to,
					const Wui::WuiColor& color, float lineWidth, entt::entity ignoreEntity = entt::null)
				{
					const glm::vec3 p0 = glm::vec3(worldMatrix * glm::vec4(from, 1.0f));
					const glm::vec3 p1 = glm::vec3(worldMatrix * glm::vec4(to, 1.0f));
					PushOccludedWorldSegment(p0, p1, color, lineWidth, ignoreEntity);
				};

				auto PushOccludedWorldCircle = [&](const glm::mat4& worldMatrix, const glm::vec3& center, float radius,
					int axis, const Wui::WuiColor& color, float lineWidth, entt::entity ignoreEntity = entt::null, int segments = 24)
				{
					if (radius <= 0.0f)
						return;
					constexpr float kTwoPi = 6.28318530718f;
					const int numSegs = std::clamp(segments, 8, 36);
					glm::vec3 prevWorldPt(0.0f);
					for (int i = 0; i <= numSegs; ++i)
					{
						const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(numSegs);
						glm::vec3 offset(0.0f);
						if (axis == 0)
							offset = { std::cos(angle) * radius, std::sin(angle) * radius, 0.0f };
						else if (axis == 1)
							offset = { std::cos(angle) * radius, 0.0f, std::sin(angle) * radius };
						else
							offset = { 0.0f, std::cos(angle) * radius, std::sin(angle) * radius };

						const glm::vec3 curWorldPt = glm::vec3(worldMatrix * glm::vec4(center + offset, 1.0f));
						if (i > 0)
						{
							PushOccludedWorldSegment(prevWorldPt, curWorldPt, color, lineWidth, ignoreEntity);
						}
						prevWorldPt = curWorldPt;
					}
				};


				// 平行光箭头长度:按场景实体位置的包围盒对角线取 25%(下限 1.0,上限 50),
				// 这样小场景大场景都看得清;没有实体时退回 2.0。
				float sceneDiagonal = 0.0f;
				{
					glm::vec3 minPos(1e30f), maxPos(-1e30f);
					int transformCount = 0;
					for (const entt::entity handle : registry.view<TransformComponent>())
					{
						const glm::vec3 position = glm::vec3(WorldMatrixOf(registry, handle)[3]);
						minPos = glm::min(minPos, position);
						maxPos = glm::max(maxPos, position);
						++transformCount;
					}
					if (transformCount > 0)
						sceneDiagonal = glm::length(maxPos - minPos);
				}
				const float arrowLength = glm::clamp(sceneDiagonal * 0.25f, 1.0f, 50.0f);

				int lightCount = 0;
				int colliderCount = 0;
				int audioCount = 0;
				bool showAllAudio = false;
				glm::vec2 overlayMin { 1e30f, 1e30f };
				glm::vec2 overlayMax { -1e30f, -1e30f };
				{
					// P1c-W3.4:范围覆盖层(光源/碰撞体)的裁剪走库件 Wui::ClipScope(同相机视锥那条)。
					Wui::ClipScope overlayClip(ctx, sceneRect);

					// ---- 相机可视化: 选中相机的视锥线框 (带方向指示与逐段深度遮挡) ----
					if (selected.IsValid() && selected.HasComponent<CameraComponent>() &&
						selected.HasComponent<TransformComponent>())
					{
						const entt::entity camHandle = static_cast<entt::entity>(selected);
						const CameraSettings& camera = registry.get<CameraComponent>(camHandle).Camera;
						const auto* cameraView = registry.try_get<CameraViewComponent>(camHandle);
						const float aspectRatio = (cameraView && cameraView->Valid)
							? cameraView->LastAspectRatio : camera.m_AspectRatio;
						glm::mat4 world = registry.get<TransformComponent>(camHandle).GetLocalMatrix();
						if (const auto* worldTransform = registry.try_get<WorldTransformComponent>(camHandle))
							world = worldTransform->Matrix;

						float hNear = 0.0f, wNear = 0.0f, hFar = 0.0f, wFar = 0.0f, zNear = 0.0f, zFar = 0.0f;
						if (camera.m_ProjectionType == CameraSettings::ProjectionType::Perspective)
						{
							const float tanHalf = std::tan(glm::radians(camera.m_PerspectiveFOV) * 0.5f);
							zNear = -camera.m_PerspectiveNearClip;
							zFar = -camera.m_PerspectiveFarClip;
							hNear = tanHalf * std::abs(zNear);
							wNear = hNear * aspectRatio;
							hFar = tanHalf * std::abs(zFar);
							wFar = hFar * aspectRatio;
						}
						else
						{
							zNear = -camera.m_OrthographicNearClip;
							zFar = -camera.m_OrthographicFarClip;
							hNear = hFar = camera.m_OrthographicZoom;
							wNear = wFar = camera.m_OrthographicZoom * aspectRatio;
						}
						const glm::vec3 localCorners[8] = {
							{ -wNear, -hNear, zNear }, { wNear, -hNear, zNear }, { wNear, hNear, zNear }, { -wNear, hNear, zNear },
							{ -wFar, -hFar, zFar }, { wFar, -hFar, zFar }, { wFar, hFar, zFar }, { -wFar, hFar, zFar },
						};
						glm::vec3 worldCorners[8];
						for (int i = 0; i < 8; ++i)
							worldCorners[i] = glm::vec3(world * glm::vec4 { localCorners[i], 1.0f });

						const Wui::WuiColor nearColor { theme.Warning.R, theme.Warning.G, theme.Warning.B, 0.95f };
						const Wui::WuiColor farColor { theme.Warning.R, theme.Warning.G, theme.Warning.B, 0.38f };
						const Wui::WuiColor axisColor { theme.Accent.R, theme.Accent.G, theme.Accent.B, 0.85f };
						for (int i = 0; i < 4; ++i)
						{
							PushOccludedWorldSegment(worldCorners[i], worldCorners[(i + 1) % 4], nearColor, 2.4f, camHandle);
							PushOccludedWorldSegment(worldCorners[4 + i], worldCorners[4 + (i + 1) % 4], farColor, 1.2f, camHandle);
							PushOccludedWorldSegment(worldCorners[i], worldCorners[4 + i], nearColor, 1.6f, camHandle);
						}
						const glm::vec3 eye = glm::vec3(world * glm::vec4 { 0.0f, 0.0f, 0.0f, 1.0f });
						const glm::vec3 farCenter = glm::vec3(world * glm::vec4 { 0.0f, 0.0f, zFar, 1.0f });
						PushOccludedWorldSegment(eye, farCenter, axisColor, 2.2f, camHandle);

						const float crossSize = std::max(0.08f * std::max(wFar, hFar), 0.02f);
						PushOccludedWorldSegment(glm::vec3(world * glm::vec4 { crossSize, 0.0f, zFar, 1.0f }),
							glm::vec3(world * glm::vec4 { -crossSize, 0.0f, zFar, 1.0f }), axisColor, 1.8f, camHandle);
						PushOccludedWorldSegment(glm::vec3(world * glm::vec4 { 0.0f, crossSize, zFar, 1.0f }),
							glm::vec3(world * glm::vec4 { 0.0f, -crossSize, zFar, 1.0f }), axisColor, 1.8f, camHandle);
						PushOccludedWorldSegment(eye, glm::vec3(world * glm::vec4 { 0.0f, 0.25f, 0.0f, 1.0f }), nearColor, 2.8f, camHandle);
					}

					// ---- 光源 ----
					for (const entt::entity handle : registry.view<TransformComponent>())
					{
						const bool isSelected = handle == selectedHandle;
						if (!isSelected && !showAllLights)
							continue;
						const glm::mat4 world = WorldMatrixOf(registry, handle);
						const float alpha = isSelected ? 0.95f : 0.45f;
						bool drew = false;
						if (const auto* point = registry.try_get<PointLightComponent>(handle))
						{
							// 点光:半径 = Range 的三个大圆(2D 视口只画 XY 那一个,避免看起来像乱线)。
							const Wui::WuiColor color { glm::clamp(point->Color.r + point->Intensity * 0.05f, 0.0f, 1.0f),
								glm::clamp(point->Color.g + point->Intensity * 0.05f, 0.0f, 1.0f),
								glm::clamp(point->Color.b + point->Intensity * 0.05f, 0.0f, 1.0f), alpha };
							if (point->Range > 0.0f)
							{
								// 用户 2026-09-21:「绘制的光源范围怎么是 2d 的,要 3d 的」→
								// 三个大圆**始终**画(2D 正交视口下它同样投影成一个球(圆环),不再降级成单圈)。
								PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, glm::vec3(0.0f),
									point->Range, 0, color, isSelected ? 1.6f : 1.1f, &overlayMin, &overlayMax);
								PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, glm::vec3(0.0f),
									point->Range, 1, color, isSelected ? 1.3f : 0.9f, &overlayMin, &overlayMax);
								PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, glm::vec3(0.0f),
									point->Range, 2, color, isSelected ? 1.3f : 0.9f, &overlayMin, &overlayMax);
								drew = true;
							}
							// 位置十字(即使 Range=0 也能看到光源在哪)。
							const float cross = glm::clamp(point->Range * 0.1f, 0.05f, 0.5f);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								{ -cross, 0.0f, 0.0f }, { cross, 0.0f, 0.0f }, color, 2.0f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								{ 0.0f, -cross, 0.0f }, { 0.0f, cross, 0.0f }, color, 2.0f, &overlayMin, &overlayMax);
							drew = true;
						}
						if (const auto* directional = registry.try_get<DirectionalLightComponent>(handle))
						{
							// 平行光:沿 Direction 的箭头 + 末端四根短射线(太阳标记)。方向是世界空间,不受实体缩放影响。
							const Wui::WuiColor color { glm::clamp(directional->Color.r + directional->Intensity * 0.05f, 0.0f, 1.0f),
								glm::clamp(directional->Color.g + directional->Intensity * 0.05f, 0.0f, 1.0f),
								glm::clamp(directional->Color.b + directional->Intensity * 0.05f, 0.0f, 1.0f), alpha };
							glm::vec3 direction = directional->Direction;
							if (glm::length(direction) < 1e-4f)
								direction = { 0.0f, -1.0f, 0.0f };
							direction = glm::normalize(direction);
							const float length = arrowLength;
							const glm::vec3 tip = direction * length;
							const glm::vec3 side = glm::normalize(glm::cross(direction, glm::vec3(0.0f, 1.0f, 0.0f))
								+ glm::vec3(1e-4f, 0.0f, 0.0f));
							const glm::vec3 side2 = glm::normalize(glm::cross(direction, side));
							const float wing = length * 0.12f;
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world, glm::vec3(0.0f), tip,
								color, isSelected ? 2.2f : 1.4f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								tip, tip - direction * wing + side * wing, color, 2.0f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								tip, tip - direction * wing - side * wing, color, 2.0f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								tip, tip - direction * wing + side2 * wing, color, 2.0f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								tip, tip - direction * wing - side2 * wing, color, 2.0f, &overlayMin, &overlayMax);
							drew = true;
						}
						if (const auto* spot = registry.try_get<SpotLight3DComponent>(handle))
						{
							const Wui::WuiColor color {
								glm::clamp(spot->Color.r + spot->Intensity * 0.05f, 0.0f, 1.0f),
								glm::clamp(spot->Color.g + spot->Intensity * 0.05f, 0.0f, 1.0f),
								glm::clamp(spot->Color.b + spot->Intensity * 0.05f, 0.0f, 1.0f),
								alpha
							};
							const float range = std::max(0.1f, spot->Range);
							const float outerAngle = std::clamp(spot->OuterConeAngle, 0.1f, 179.0f);
							const float innerAngle = std::clamp(spot->InnerConeAngle, 0.1f, outerAngle);
							const float outerRadius = range * std::tan(glm::radians(outerAngle * 0.5f));
							const float innerRadius = range * std::tan(glm::radians(innerAngle * 0.5f));
							const float thickness = isSelected ? 1.8f : 1.1f;
							const glm::vec3 baseCenter { 0.0f, 0.0f, -range };

							// 1. 原点到端面中心的主光轴线
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								glm::vec3(0.0f), baseCenter, color, thickness * 0.8f, &overlayMin, &overlayMax);

							// 2. 端面外锥圆环与内锥圆环 (位于 Z = -range 截面)
							PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world,
								baseCenter, outerRadius, 0, color, thickness, &overlayMin, &overlayMax);
							if (innerRadius > 0.0f && innerRadius < outerRadius)
							{
								PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world,
									baseCenter, innerRadius, 0,
									Wui::WuiColor { color.R, color.G, color.B, color.A * 0.6f }, 1.0f,
									&overlayMin, &overlayMax);
							}

							// 3. 4 条从光源原点连接到外圆环四极点(上、下、左、右)的母线
							const glm::vec3 cardinalPoints[4] = {
								{  0.0f,         outerRadius, -range },
								{  0.0f,        -outerRadius, -range },
								{ -outerRadius,  0.0f,        -range },
								{  outerRadius,  0.0f,        -range }
							};
							for (const auto& cp : cardinalPoints)
							{
								PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
									glm::vec3(0.0f), cp, color, thickness, &overlayMin, &overlayMax);
							}

							// 4. 光源位置十字定位标
							const float cross = glm::clamp(range * 0.05f, 0.05f, 0.5f);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								{ -cross, 0.0f, 0.0f }, { cross, 0.0f, 0.0f }, color, 2.0f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								{ 0.0f, -cross, 0.0f }, { 0.0f, cross, 0.0f }, color, 2.0f, &overlayMin, &overlayMax);
							drew = true;
						}
						if (const auto* point2d = registry.try_get<PointLight2DComponent>(handle))
						{
							const Wui::WuiColor color {
								glm::clamp(point2d->Color.r + point2d->Intensity * 0.05f, 0.0f, 1.0f),
								glm::clamp(point2d->Color.g + point2d->Intensity * 0.05f, 0.0f, 1.0f),
								glm::clamp(point2d->Color.b + point2d->Intensity * 0.05f, 0.0f, 1.0f),
								alpha
							};
							const float thickness = isSelected ? 1.6f : 1.1f;
							if (point2d->OuterRadius > 0.0f)
							{
								PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world,
									glm::vec3(0.0f), point2d->OuterRadius, 0, color, thickness, &overlayMin, &overlayMax);
								if (point2d->InnerRadius > 0.0f && point2d->InnerRadius < point2d->OuterRadius)
								{
									PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world,
										glm::vec3(0.0f), point2d->InnerRadius, 0,
										Wui::WuiColor { color.R, color.G, color.B, color.A * 0.6f }, 1.0f,
										&overlayMin, &overlayMax);
								}
								drew = true;
							}
							const float cross = glm::clamp(point2d->OuterRadius * 0.08f, 0.05f, 0.5f);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								{ -cross, 0.0f, 0.0f }, { cross, 0.0f, 0.0f }, color, 2.0f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								{ 0.0f, -cross, 0.0f }, { 0.0f, cross, 0.0f }, color, 2.0f, &overlayMin, &overlayMax);
							drew = true;
						}
						// AmbientLightComponent 是全局语义(没有空间范围)→ 不画,属性行 tooltip 已写明。
						if (drew)
							++lightCount;
					}

					// ---- 碰撞体轮廓(编辑期;MeshCollider3D 无解析形状,按设计不画) ----
					const bool collidersEnabled = Editor::EditorPreferences::Get().Data().ViewportColliderOutlinesEnabled;
					const Wui::WuiColor colliderColor { theme.Success.R, theme.Success.G, theme.Success.B, 0.55f };
					const Wui::WuiColor colliderSelected { theme.Success.R, theme.Success.G, theme.Success.B, 0.95f };
					for (const entt::entity handle : registry.view<TransformComponent>())
					{
						if (!collidersEnabled)
							continue;
						const bool isSelected = handle == selectedHandle;
						if (!isSelected && !showAllColliders)
							continue;
						// 口径:与物理**同源** —— 2D(Box2D)与 3D(Jolt)都是用实体的**本地** Transform
						// 建刚体/形状(见 Scene.cpp 的 b2MakeOffsetBox 与 Physics3D.cpp:415),所以轮廓也走本地矩阵;
						// 用世界矩阵在"有父级"的实体上会和真实碰撞体错位。
						const glm::mat4 world = registry.try_get<TransformComponent>(handle)
							? registry.get<TransformComponent>(handle).GetLocalMatrix() : glm::mat4(1.0f);
						const Wui::WuiColor color = isSelected ? colliderSelected : colliderColor;
						const float thickness = isSelected ? 1.8f : 1.1f;
						bool drew = false;
						if (const auto* box2d = registry.try_get<BoxCollider2DComponent>(handle))
						{
							// 注意:`BoxCollider2DComponent::Size` 是**半尺寸**(运行时直接喂
							// b2MakeOffsetBox(halfWidth, halfHeight, …)),不要再乘 0.5 ——
							// 乘了会把轮廓画成真实碰撞体的一半(we_engine 在 U6 复核时抓到的口径矛盾)。
							const float hx = std::max(0.0f, box2d->Size.x);
							const float hy = std::max(0.0f, box2d->Size.y);
							const glm::vec3 c { box2d->Offset.x, box2d->Offset.y, 0.0f };
							const glm::vec3 corners[4] = {
								c + glm::vec3 { -hx, -hy, 0.0f }, c + glm::vec3 { hx, -hy, 0.0f },
								c + glm::vec3 { hx, hy, 0.0f }, c + glm::vec3 { -hx, hy, 0.0f } };
							for (int i = 0; i < 4; ++i)
								PushOccludedWorldSegmentLocal(world, corners[i], corners[(i + 1) % 4], color, thickness, handle);
							drew = true;
						}
						if (const auto* circle2d = registry.try_get<CircleCollider2DComponent>(handle))
						{
							if (circle2d->Radius > 0.0f)
								PushOccludedWorldCircle(world, { circle2d->Offset.x, circle2d->Offset.y, 0.0f }, circle2d->Radius, 0, color, thickness, handle);
							drew = true;
						}
						if (const auto* box3d = registry.try_get<BoxCollider3DComponent>(handle))
						{
							if (!isSelected && s_ShowNavMesh)
							{
								// 当导航网格开启时，未选中实体的静态 BoxCollider3D 绿色线框隐藏，避免与导航网格重合杂乱
							}
							else
							{
								// 12 条棱:结合面法线朝向与静态遮挡剔除，彻底杜绝穿透
								glm::vec3 corners[8];
								glm::vec3 worldCorners[8];
								for (int i = 0; i < 8; ++i)
								{
									corners[i] = box3d->Offset + glm::vec3 {
										(i & 1) ? box3d->HalfExtents.x : -box3d->HalfExtents.x,
										(i & 2) ? box3d->HalfExtents.y : -box3d->HalfExtents.y,
										(i & 4) ? box3d->HalfExtents.z : -box3d->HalfExtents.z };
									worldCorners[i] = glm::vec3(world * glm::vec4(corners[i], 1.0f));
								}
								const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
								const glm::vec3 faceNormals[6] = {
									{  1,  0,  0 }, { -1,  0,  0 },
									{  0,  1,  0 }, {  0, -1,  0 },
									{  0,  0,  1 }, {  0,  0, -1 }
								};
								const glm::vec3 faceCenters[6] = {
									box3d->Offset + glm::vec3( box3d->HalfExtents.x, 0, 0),
									box3d->Offset + glm::vec3(-box3d->HalfExtents.x, 0, 0),
									box3d->Offset + glm::vec3(0,  box3d->HalfExtents.y, 0),
									box3d->Offset + glm::vec3(0, -box3d->HalfExtents.y, 0),
									box3d->Offset + glm::vec3(0, 0,  box3d->HalfExtents.z),
									box3d->Offset + glm::vec3(0, 0, -box3d->HalfExtents.z)
								};
								bool faceVisible[6] = {};
								for (int f = 0; f < 6; ++f)
								{
									const glm::vec3 centerWorld = glm::vec3(world * glm::vec4(faceCenters[f], 1.0f));
									const glm::vec3 normalWorld = glm::normalize(normalMatrix * faceNormals[f]);
									faceVisible[f] = glm::dot(normalWorld, overlayCamera.Position - centerWorld) > 0.0f;
								}
								const int cubeEdges[12][4] = {
									{ 0, 1, 3, 5 }, { 2, 3, 2, 5 }, { 4, 5, 3, 4 }, { 6, 7, 2, 4 },
									{ 0, 2, 1, 5 }, { 1, 3, 0, 5 }, { 4, 6, 1, 4 }, { 5, 7, 0, 4 },
									{ 0, 4, 1, 3 }, { 1, 5, 0, 3 }, { 2, 6, 1, 2 }, { 3, 7, 0, 2 },
								};
								for (const auto& edge : cubeEdges)
								{
									if (faceVisible[edge[2]] || faceVisible[edge[3]])
									{
										PushOccludedWorldSegment(worldCorners[edge[0]], worldCorners[edge[1]],
											color, thickness, handle);
									}
								}
								drew = true;
							}
						}
						if (const auto* sphere = registry.try_get<SphereCollider3DComponent>(handle))
						{
							if (sphere->Radius > 0.0f)
								for (int axis = 0; axis < 3; ++axis)
									PushOccludedWorldCircle(world, sphere->Offset, sphere->Radius, axis, color, thickness, handle);
							drew = true;
						}
						if (const auto* capsule = registry.try_get<CapsuleCollider3DComponent>(handle))
						{
							// 近似:上下两个端面圆 + 两条母线(沿本地 Y 轴)。
							const float radius = std::max(0.0f, capsule->Radius);
							const float half = std::max(0.0f, capsule->HalfHeight);
							if (radius > 0.0f)
							{
								const glm::vec3 base = capsule->Offset;
								for (int axis = 0; axis < 3; ++axis)
								{
									PushOccludedWorldCircle(world, base + glm::vec3 { 0.0f, half, 0.0f },
										radius, axis, color, thickness, handle);
									PushOccludedWorldCircle(world, base - glm::vec3 { 0.0f, half, 0.0f },
										radius, axis, color, thickness, handle);
								}
								PushOccludedWorldSegmentLocal(world,
									base + glm::vec3 { radius, -half, 0.0f }, base + glm::vec3 { radius, half, 0.0f },
									color, thickness, handle);
								PushOccludedWorldSegmentLocal(world,
									base + glm::vec3 { -radius, -half, 0.0f }, base + glm::vec3 { -radius, half, 0.0f },
									color, thickness, handle);
								PushOccludedWorldSegmentLocal(world,
									base + glm::vec3 { 0.0f, -half, radius }, base + glm::vec3 { 0.0f, half, radius },
									color, thickness, handle);
								PushOccludedWorldSegmentLocal(world,
									base + glm::vec3 { 0.0f, -half, -radius }, base + glm::vec3 { 0.0f, half, -radius },
									color, thickness, handle);
							}
							drew = true;
						}
						if (const auto* cap2d = registry.try_get<CapsuleCollider2DComponent>(handle))
						{
							const float r = std::max(0.01f, cap2d->Radius);
							const float halfLen = std::max(0.0f, cap2d->Length * 0.5f);
							const glm::vec3 center { cap2d->Offset.x, cap2d->Offset.y, 0.0f };
							constexpr int kHalfSegments = 16;
							constexpr float kPi = 3.141592653589793f;

							if (cap2d->Direction == 0) // 垂直胶囊体 (Vertical)
							{
								const glm::vec3 cTop = center + glm::vec3(0.0f, halfLen, 0.0f);
								const glm::vec3 cBottom = center - glm::vec3(0.0f, halfLen, 0.0f);
								PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
									cBottom + glm::vec3(-r, 0.0f, 0.0f), cTop + glm::vec3(-r, 0.0f, 0.0f),
									color, thickness, &overlayMin, &overlayMax);
								PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
									cBottom + glm::vec3(r, 0.0f, 0.0f), cTop + glm::vec3(r, 0.0f, 0.0f),
									color, thickness, &overlayMin, &overlayMax);
								for (int i = 0; i < kHalfSegments; ++i)
								{
									const float a0 = kPi * static_cast<float>(i) / static_cast<float>(kHalfSegments);
									const float a1 = kPi * static_cast<float>(i + 1) / static_cast<float>(kHalfSegments);
									const glm::vec3 p0 = cTop + glm::vec3(std::cos(a0) * r, std::sin(a0) * r, 0.0f);
									const glm::vec3 p1 = cTop + glm::vec3(std::cos(a1) * r, std::sin(a1) * r, 0.0f);
									PushOccludedWorldSegmentLocal(world, p0, p1, color, thickness, handle);
								}
								for (int i = 0; i < kHalfSegments; ++i)
								{
									const float a0 = kPi * (1.0f + static_cast<float>(i) / static_cast<float>(kHalfSegments));
									const float a1 = kPi * (1.0f + static_cast<float>(i + 1) / static_cast<float>(kHalfSegments));
									const glm::vec3 p0 = cBottom + glm::vec3(std::cos(a0) * r, std::sin(a0) * r, 0.0f);
									const glm::vec3 p1 = cBottom + glm::vec3(std::cos(a1) * r, std::sin(a1) * r, 0.0f);
									PushOccludedWorldSegmentLocal(world, p0, p1, color, thickness, handle);
								}
							}
							else // 水平胶囊体 (Horizontal)
							{
								const glm::vec3 cRight = center + glm::vec3(halfLen, 0.0f, 0.0f);
								const glm::vec3 cLeft = center - glm::vec3(halfLen, 0.0f, 0.0f);
								PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
									cLeft + glm::vec3(0.0f, r, 0.0f), cRight + glm::vec3(0.0f, r, 0.0f),
									color, thickness, &overlayMin, &overlayMax);
								PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
									cLeft + glm::vec3(0.0f, -r, 0.0f), cRight + glm::vec3(0.0f, -r, 0.0f),
									color, thickness, &overlayMin, &overlayMax);
								for (int i = 0; i < kHalfSegments; ++i)
								{
									const float a0 = -kPi * 0.5f + kPi * static_cast<float>(i) / static_cast<float>(kHalfSegments);
									const float a1 = -kPi * 0.5f + kPi * static_cast<float>(i + 1) / static_cast<float>(kHalfSegments);
									const glm::vec3 p0 = cRight + glm::vec3(std::cos(a0) * r, std::sin(a0) * r, 0.0f);
									const glm::vec3 p1 = cRight + glm::vec3(std::cos(a1) * r, std::sin(a1) * r, 0.0f);
									PushOccludedWorldSegmentLocal(world, p0, p1, color, thickness, handle);
								}
								for (int i = 0; i < kHalfSegments; ++i)
								{
									const float a0 = kPi * 0.5f + kPi * static_cast<float>(i) / static_cast<float>(kHalfSegments);
									const float a1 = kPi * 0.5f + kPi * static_cast<float>(i + 1) / static_cast<float>(kHalfSegments);
									const glm::vec3 p0 = cLeft + glm::vec3(std::cos(a0) * r, std::sin(a0) * r, 0.0f);
									const glm::vec3 p1 = cLeft + glm::vec3(std::cos(a1) * r, std::sin(a1) * r, 0.0f);
									PushOccludedWorldSegmentLocal(world, p0, p1, color, thickness, handle);
								}
							}
							drew = true;
						}
						if (const auto* poly2d = registry.try_get<PolygonCollider2DComponent>(handle))
						{
							const auto pts = poly2d->GetPoints();
							const glm::vec3 offset { poly2d->Offset.x, poly2d->Offset.y, 0.0f };
							const size_t ptCount = pts.size();
							if (ptCount >= 3)
							{
								for (size_t i = 0; i < ptCount; ++i)
								{
									const glm::vec3 p0 = offset + glm::vec3(pts[i].x, pts[i].y, 0.0f);
									const glm::vec3 p1 = offset + glm::vec3(pts[(i + 1) % ptCount].x, pts[(i + 1) % ptCount].y, 0.0f);
									PushOccludedWorldSegmentLocal(world, p0, p1, color, thickness, handle);
								}
								drew = true;
							}
						}
						if (const auto* chain2d = registry.try_get<ChainCollider2DComponent>(handle))
						{
							const auto pts = chain2d->GetPoints();
							const glm::vec3 offset { chain2d->Offset.x, chain2d->Offset.y, 0.0f };
							const size_t ptCount = pts.size();
							if (ptCount >= 2)
							{
								const size_t segCount = chain2d->Loop ? ptCount : (ptCount - 1);
								for (size_t i = 0; i < segCount; ++i)
								{
									const glm::vec3 p0 = offset + glm::vec3(pts[i].x, pts[i].y, 0.0f);
									const glm::vec3 p1 = offset + glm::vec3(pts[(i + 1) % ptCount].x, pts[(i + 1) % ptCount].y, 0.0f);
									PushOccludedWorldSegmentLocal(world, p0, p1, color, thickness, handle);
								}
								drew = true;
							}
						}
						if (const auto* convex = registry.try_get<ConvexCollider3DComponent>(handle))
						{
							std::string modelPath;
							if (convex->MeshSource.HasPath())
							{
								modelPath = std::string(StringPool::Get().PathOf(convex->MeshSource.Path));
							}
							else if (const auto* mr = registry.try_get<MeshRendererComponent>(handle))
							{
								if (mr->Mesh.HasPath())
								{
									modelPath = std::string(StringPool::Get().PathOf(mr->Mesh.Path));
								}
							}

							const CachedModelConvex* cached = GetCachedModelConvex(modelPath);
							if (cached && cached->HasHulls && !cached->Hulls.empty())
							{
								// 预烘焙复合凸包: 直接绘制子凸包真实顶点与线段，与实机模型 100% 几何对齐吻合
								for (const auto& hull : cached->Hulls)
								{
									for (const auto& edge : hull.Edges)
									{
										PushOccludedWorldSegmentLocal(world, edge.first, edge.second, color, thickness, handle);
									}
								}
								drew = true;
							}
							else
							{
								// 未预烘焙: 采用关联网格真实几何包围盒 (MeshBounds)，杜绝手写 [-0.5, 0.5] 假长方体错位
								glm::vec3 localMin = (cached && cached->HasBounds) ? cached->BoundsMin : glm::vec3(-0.5f, -0.5f, -0.5f);
								glm::vec3 localMax = (cached && cached->HasBounds) ? cached->BoundsMax : glm::vec3( 0.5f,  0.5f,  0.5f);
								if (const auto* mr = registry.try_get<MeshRendererComponent>(handle))
								{
									if (mr->Primitive == MeshRendererComponent::PrimitiveShape::Plane)
									{
										localMin.y = 0.0f;
										localMax.y = 0.0f;
									}
								}
								const uint32_t hullCount = std::clamp(convex->HullCount, 1u, 16u);
								if (hullCount <= 1)
								{
									glm::vec3 corners[8];
									for (int i = 0; i < 8; ++i)
									{
										corners[i] = glm::vec3 {
											(i & 1) ? localMax.x : localMin.x,
											(i & 2) ? localMax.y : localMin.y,
											(i & 4) ? localMax.z : localMin.z
										};
									}
									for (int i = 0; i < 8; ++i)
									{
										for (int axis = 0; axis < 3; ++axis)
										{
											const int other = i ^ (1 << axis);
											if (other > i)
												PushOccludedWorldSegmentLocal(world, corners[i], corners[other], color, thickness, handle);
										}
									}
									PushOccludedWorldSegmentLocal(world, corners[0], corners[7], color, thickness * 0.7f, handle);
									PushOccludedWorldSegmentLocal(world, corners[1], corners[6], color, thickness * 0.7f, handle);
								}
								else
								{
									const glm::vec3 ext = localMax - localMin;
									int splitAxis = 0;
									if (ext.y >= ext.x && ext.y >= ext.z) splitAxis = 1;
									else if (ext.z >= ext.x && ext.z >= ext.y) splitAxis = 2;

									const float step = ext[splitAxis] / static_cast<float>(hullCount);
									for (uint32_t k = 0; k < hullCount; ++k)
									{
										glm::vec3 subMin = localMin;
										glm::vec3 subMax = localMax;
										subMin[splitAxis] = localMin[splitAxis] + step * static_cast<float>(k);
										subMax[splitAxis] = localMin[splitAxis] + step * static_cast<float>(k + 1);

										glm::vec3 corners[8];
										for (int i = 0; i < 8; ++i)
										{
											corners[i] = glm::vec3 {
												(i & 1) ? subMax.x : subMin.x,
												(i & 2) ? subMax.y : subMin.y,
												(i & 4) ? subMax.z : subMin.z
											};
										}
										for (int i = 0; i < 8; ++i)
										{
											for (int axis = 0; axis < 3; ++axis)
											{
												const int other = i ^ (1 << axis);
												if (other > i)
													PushOccludedWorldSegmentLocal(world, corners[i], corners[other], color, thickness, handle);
											}
										}
									}
								}
								drew = true;
							}
						}
						if (const auto* weatherVol = registry.try_get<WeatherVolumeComponent>(handle))
						{
							const Wui::WuiColor innerColor { 0.20f, 0.70f, 1.0f, 0.95f };
							const Wui::WuiColor outerColor { 0.35f, 0.85f, 1.0f, 0.35f };
							const float innerThick = thickness * 1.5f;

							auto drawBoxEdges = [&](const glm::vec3& extents, const Wui::WuiColor& c, float th)
							{
								glm::vec3 corners[8];
								glm::vec3 worldCorners[8];
								for (int i = 0; i < 8; ++i)
								{
									corners[i] = glm::vec3 {
										(i & 1) ? extents.x : -extents.x,
										(i & 2) ? extents.y : -extents.y,
										(i & 4) ? extents.z : -extents.z
									};
									worldCorners[i] = glm::vec3(world * glm::vec4(corners[i], 1.0f));
								}
								const int cubeEdges[12][2] = {
									{ 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
									{ 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
									{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
								};
								for (const auto& edge : cubeEdges)
								{
									PushOccludedWorldSegment(worldCorners[edge[0]], worldCorners[edge[1]],
										c, th, handle);
								}
							};

							// 1. 核心触发盒体 (Inner Box: 严格逐段遮挡，杜绝穿透山体与建筑)
							drawBoxEdges(weatherVol->Extents, innerColor, innerThick);

							// 2. 外部平滑过渡缓冲范围 (Outer Blend Margin)
							if (weatherVol->BlendDistance > 0.1f)
							{
								drawBoxEdges(weatherVol->Extents + glm::vec3(weatherVol->BlendDistance), outerColor, thickness);
							}
							drew = true;
						}
						if (drew)
							++colliderCount;
					}

					// ---- 声学覆盖层 (Audio Gizmos: AudioSourceComponent & AudioZoneComponent) ----
					showAllAudio = (std::getenv("WLD_AUDIO_DEBUG") != nullptr);
					for (const entt::entity handle : registry.view<TransformComponent>())
					{
						const bool isSelected = (handle == selectedHandle);
						if (!isSelected && !showAllAudio)
							continue;

						const glm::mat4 world = registry.try_get<TransformComponent>(handle)
							? registry.get<TransformComponent>(handle).GetLocalMatrix() : glm::mat4(1.0f);

						bool drewAudio = false;
						if (const auto* audioSrc = registry.try_get<AudioSourceComponent>(handle))
						{
							if (audioSrc->Spatial3D)
							{
								constexpr int kCircleSegments = 24;
								constexpr float kTwoPi = 6.28318530718f;

								auto DrawOccludedCircle = [&](float radius, int axis, const Wui::WuiColor& col, float thickness)
								{
									for (int i = 0; i < kCircleSegments; ++i)
									{
										const float a0 = kTwoPi * static_cast<float>(i) / static_cast<float>(kCircleSegments);
										const float a1 = kTwoPi * static_cast<float>(i + 1) / static_cast<float>(kCircleSegments);
										glm::vec3 off0(0.0f), off1(0.0f);
										if (axis == 0)
										{
											off0 = { std::cos(a0) * radius, std::sin(a0) * radius, 0.0f };
											off1 = { std::cos(a1) * radius, std::sin(a1) * radius, 0.0f };
										}
										else if (axis == 1)
										{
											off0 = { std::cos(a0) * radius, 0.0f, std::sin(a0) * radius };
											off1 = { std::cos(a1) * radius, 0.0f, std::sin(a1) * radius };
										}
										else
										{
											off0 = { 0.0f, std::cos(a0) * radius, std::sin(a0) * radius };
											off1 = { 0.0f, std::cos(a1) * radius, std::sin(a1) * radius };
										}
										const glm::vec3 w0 = glm::vec3(world * glm::vec4(off0, 1.0f));
										const glm::vec3 w1 = glm::vec3(world * glm::vec4(off1, 1.0f));
										PushOccludedWorldSegment(w0, w1, col, thickness, handle);
									}
								};

								// 1. MinDistance (全响近场区): 亮青色三轴立体大圆线框 (带深度遮挡)
								if (audioSrc->MinDistance > 0.0f)
								{
									const Wui::WuiColor minColor { theme.Info.R, theme.Info.G, theme.Info.B, isSelected ? 0.95f : 0.5f };
									const float thickness = isSelected ? 1.6f : 1.1f;
									for (int axis = 0; axis < 3; ++axis)
									{
										DrawOccludedCircle(audioSrc->MinDistance, axis, minColor, thickness);
									}
								}

								// 2. MaxDistance (衰减截止区): 深蓝色三轴立体大圆线框 (带深度遮挡)
								if (audioSrc->MaxDistance > audioSrc->MinDistance)
								{
									const Wui::WuiColor maxColor { theme.Accent.R, theme.Accent.G, theme.Accent.B, isSelected ? 0.75f : 0.35f };
									const float thickness = isSelected ? 1.3f : 0.9f;
									for (int axis = 0; axis < 3; ++axis)
									{
										DrawOccludedCircle(audioSrc->MaxDistance, axis, maxColor, thickness);
									}
								}

								// 3. 中心扬声器 3 轴十字标: 0.3m 定位十字 (带深度遮挡)
								const float cross = 0.15f; // 0.3m 总跨度
								const Wui::WuiColor crossColor { theme.Info.R, theme.Info.G, theme.Info.B, isSelected ? 0.95f : 0.5f };
								const float crossThick = isSelected ? 2.0f : 1.2f;

								PushOccludedWorldSegment(glm::vec3(world * glm::vec4(-cross, 0.0f, 0.0f, 1.0f)),
									glm::vec3(world * glm::vec4(cross, 0.0f, 0.0f, 1.0f)), crossColor, crossThick, handle);
								PushOccludedWorldSegment(glm::vec3(world * glm::vec4(0.0f, -cross, 0.0f, 1.0f)),
									glm::vec3(world * glm::vec4(0.0f, cross, 0.0f, 1.0f)), crossColor, crossThick, handle);
								PushOccludedWorldSegment(glm::vec3(world * glm::vec4(0.0f, 0.0f, -cross, 1.0f)),
									glm::vec3(world * glm::vec4(0.0f, 0.0f, cross, 1.0f)), crossColor, crossThick, handle);

								drewAudio = true;
							}
						}

						if (const auto* audioZone = registry.try_get<AudioZoneComponent>(handle))
						{
							// 4. AudioZoneComponent 空间混响区域包围盒: 暖金黄色 12 条棱边 AABB 线框
							const Wui::WuiColor zoneColor { theme.Warning.R, theme.Warning.G, theme.Warning.B, isSelected ? 0.9f : 0.45f };
							const float hx = std::max(0.05f, audioZone->HalfExtents[0]);
							const float hy = std::max(0.05f, audioZone->HalfExtents[1]);
							const float hz = std::max(0.05f, audioZone->HalfExtents[2]);

							const glm::vec3 corners[8] = {
								{ -hx, -hy, -hz }, // 0
								{  hx, -hy, -hz }, // 1
								{  hx,  hy, -hz }, // 2
								{ -hx,  hy, -hz }, // 3
								{ -hx, -hy,  hz }, // 4
								{  hx, -hy,  hz }, // 5
								{  hx,  hy,  hz }, // 6
								{ -hx,  hy,  hz }  // 7
							};

							const float thickness = isSelected ? 1.8f : 1.1f;
							glm::vec3 worldCorners[8];
							for (int i = 0; i < 8; ++i)
								worldCorners[i] = glm::vec3(world * glm::vec4(corners[i], 1.0f));

							// 底面 4 条棱边 (逐段遮挡)
							PushOccludedWorldSegment(worldCorners[0], worldCorners[1], zoneColor, thickness, handle);
							PushOccludedWorldSegment(worldCorners[1], worldCorners[2], zoneColor, thickness, handle);
							PushOccludedWorldSegment(worldCorners[2], worldCorners[3], zoneColor, thickness, handle);
							PushOccludedWorldSegment(worldCorners[3], worldCorners[0], zoneColor, thickness, handle);

							// 顶面 4 条棱边
							PushOccludedWorldSegment(worldCorners[4], worldCorners[5], zoneColor, thickness, handle);
							PushOccludedWorldSegment(worldCorners[5], worldCorners[6], zoneColor, thickness, handle);
							PushOccludedWorldSegment(worldCorners[6], worldCorners[7], zoneColor, thickness, handle);
							PushOccludedWorldSegment(worldCorners[7], worldCorners[4], zoneColor, thickness, handle);

							// 4 条垂直柱棱边
							PushOccludedWorldSegment(worldCorners[0], worldCorners[4], zoneColor, thickness, handle);
							PushOccludedWorldSegment(worldCorners[1], worldCorners[5], zoneColor, thickness, handle);
							PushOccludedWorldSegment(worldCorners[2], worldCorners[6], zoneColor, thickness, handle);
							PushOccludedWorldSegment(worldCorners[3], worldCorners[7], zoneColor, thickness, handle);

							drewAudio = true;
						}

						if (drewAudio)
							++audioCount;
					}

					// ---- 局部风场覆盖层 (Local Wind Gizmos: LocalWindComponent) ----
					int windCount = 0;
					const bool isFoliageToolMode = (m_Host.GetEditorToolMode() == EditorToolMode::Foliage);
					for (const entt::entity handle : registry.view<TransformComponent, LocalWindComponent>())
					{
						const bool isSelected = (handle == selectedHandle);
						if (!isSelected && !isFoliageToolMode)
							continue;

						const auto& [xform, wind] = registry.get<TransformComponent, LocalWindComponent>(handle);
						const glm::mat4 world = xform.GetLocalMatrix();
						const float r = std::max(0.1f, wind.Radius);

						// 翡翠青绿高对比度风场主题色 (Emerald Wind Color)
						const Wui::WuiColor windColor { 0.15f, 0.85f, 0.65f, isSelected ? 0.95f : 0.45f };
						const Wui::WuiColor innerColor { 0.25f, 0.95f, 0.75f, isSelected ? 0.65f : 0.25f };
						const float thickness = isSelected ? 1.8f : 1.1f;

						if (wind.Type == LocalWindType::Radial)
						{
							// 1. 球形径向冲击波: 3 轴正交立体大圆环 (XY, YZ, ZX)
							for (int axis = 0; axis < 3; ++axis)
							{
								PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, glm::vec3(0.0f),
									r, axis, windColor, thickness, &overlayMin, &overlayMax);
								PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, glm::vec3(0.0f),
									r * 0.5f, axis, innerColor, 1.0f, &overlayMin, &overlayMax);
							}
							const float arrowLen = r * 0.35f;
							const glm::vec3 dirs[6] = {
								{  1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f,  0.0f },
								{  0.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f },
								{  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f, -1.0f }
							};
							for (const auto& d : dirs)
							{
								PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
									d * (r * 0.15f), d * (r * 0.15f + arrowLen), windColor, thickness, &overlayMin, &overlayMax);
							}
						}
						else if (wind.Type == LocalWindType::Vortex)
						{
							// 2. 切向旋转气旋: 顶、中、底三层水平圆环 + 垂直旋转中轴
							const glm::vec3 axisDir = (glm::length(wind.Direction) > 1e-4f) ? glm::normalize(wind.Direction) : glm::vec3(0.0f, 1.0f, 0.0f);
							const float h = r * 0.8f;
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								-axisDir * h, axisDir * h, windColor, thickness * 1.4f, &overlayMin, &overlayMax);

							PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, glm::vec3(0.0f),
								r, 1, windColor, thickness, &overlayMin, &overlayMax);
							PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, axisDir * (h * 0.6f),
								r * 0.7f, 1, innerColor, 1.0f, &overlayMin, &overlayMax);
							PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, -axisDir * (h * 0.6f),
								r * 0.7f, 1, innerColor, 1.0f, &overlayMin, &overlayMax);

							const glm::vec3 tanDirs[4] = {
								{ r * 0.95f, 0.0f, 0.0f },
								{ -r * 0.95f, 0.0f, 0.0f },
								{ 0.0f, 0.0f, r * 0.95f },
								{ 0.0f, 0.0f, -r * 0.95f }
							};
							for (int ti = 0; ti < 4; ++ti)
							{
								const glm::vec3 p = tanDirs[ti];
								const glm::vec3 tan = glm::normalize(glm::cross(axisDir, glm::normalize(p))) * (r * 0.25f);
								PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
									p - tan * 0.5f, p + tan * 0.5f, windColor, thickness * 1.5f, &overlayMin, &overlayMax);
							}
						}
						else
						{
							// 3. 局域定向风道: 沿风向的圆柱风道与中轴箭头
							const glm::vec3 dir = (glm::length(wind.Direction) > 1e-4f) ? glm::normalize(wind.Direction) : glm::vec3(1.0f, 0.0f, 0.0f);
							const float len = r * 1.2f;
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								-dir * (len * 0.5f), dir * (len * 0.5f), windColor, thickness * 1.5f, &overlayMin, &overlayMax);
							PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, -dir * (len * 0.4f),
								r * 0.5f, 0, innerColor, 1.0f, &overlayMin, &overlayMax);
							PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, dir * (len * 0.4f),
								r * 0.5f, 0, windColor, thickness, &overlayMin, &overlayMax);
						}

						++windCount;
					}

					// ---- 空间几何范围组件覆盖层 (DecalComponent, PartitionVolumeComponent, NavMeshObstacleComponent, SmartObjectComponent) ----
					for (const entt::entity handle : registry.view<TransformComponent>())
					{
						const bool isSelected = (handle == selectedHandle);
						const auto* decal = registry.try_get<DecalComponent>(handle);
						const auto* partVol = registry.try_get<PartitionVolumeComponent>(handle);
						const auto* obstacle = registry.try_get<NavMeshObstacleComponent>(handle);
						const auto* areaMod = registry.try_get<NavMeshAreaModifierComponent>(handle);
						const auto* smartObj = registry.try_get<SmartObjectComponent>(handle);
						const auto* water = registry.try_get<WaterBodyComponent>(handle);

						if (!decal && !partVol && !obstacle && !areaMod && !smartObj && !water)
							continue;

						// 选中时始终绘制；或者在全局功能开关开启时绘制
						if (!isSelected && !(partVol && m_ShowWorldPartition) && !(obstacle && showAllColliders))
							continue;

						const glm::mat4 world = WorldMatrixOf(registry, handle);
						const glm::vec3 worldPos = glm::vec3(world[3]);
						if (!obstacle && !isSelected && isOccluded(worldPos, false, 0.0f, handle))
							continue;
						const float alpha = isSelected ? 0.95f : 0.50f;
						const float thickness = isSelected ? 1.8f : 1.1f;

						// 1. 贴花投影立体线框盒 (DecalComponent)
						if (decal)
						{
							const Wui::WuiColor decalColor = isSelected
								? Wui::WuiColor { 1.0f, 0.55f, 0.05f, 0.98f }
								: Wui::WuiColor { 1.0f, 0.65f, 0.15f, alpha };
							const float decalThickness = isSelected ? 2.2f : 1.2f;
							const glm::vec3 h = decal->Extents;
							glm::vec3 corners[8];
							for (int i = 0; i < 8; ++i)
							{
								corners[i] = glm::vec3 {
									(i & 1) ? h.x : -h.x,
									(i & 2) ? h.y : -h.y,
									(i & 4) ? h.z : -h.z
								};
							}
							for (int i = 0; i < 8; ++i)
							{
								for (int axis = 0; axis < 3; ++axis)
								{
									const int other = i ^ (1 << axis);
									if (other > i)
										PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
											corners[i], corners[other], decalColor, decalThickness, &overlayMin, &overlayMax);
								}
							}
							// 沿投影轴 -Y（垂直指向投影面/地表法向）绘制中心朝向线与箭头
							const float forwardLen = std::max(0.15f, h.y * 1.35f);
							const glm::vec3 arrowBase { 0.0f, h.y, 0.0f };
							const glm::vec3 arrowTip { 0.0f, -forwardLen, 0.0f };
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								arrowBase, arrowTip, decalColor, decalThickness * 1.5f, &overlayMin, &overlayMax);
							const float headSize = std::clamp(forwardLen * 0.15f, 0.04f, 0.4f);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								arrowTip, arrowTip + glm::vec3(headSize, headSize, 0.0f), decalColor, decalThickness * 1.4f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								arrowTip, arrowTip + glm::vec3(-headSize, headSize, 0.0f), decalColor, decalThickness * 1.4f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								arrowTip, arrowTip + glm::vec3(0.0f, headSize, headSize), decalColor, decalThickness * 1.4f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								arrowTip, arrowTip + glm::vec3(0.0f, headSize, -headSize), decalColor, decalThickness * 1.4f, &overlayMin, &overlayMax);

							// 投影盒正中心投影范围指示四边形 (X-Z 平面，高对比度高亮，带十字分划与选中高亮)
							const Wui::WuiColor quadColor = isSelected
								? Wui::WuiColor { 1.0f, 0.90f, 0.20f, 0.95f }
								: Wui::WuiColor { 1.0f, 0.70f, 0.15f, 0.75f };
							const glm::vec3 q0 { -h.x, 0.0f, -h.z };
							const glm::vec3 q1 {  h.x, 0.0f, -h.z };
							const glm::vec3 q2 {  h.x, 0.0f,  h.z };
							const glm::vec3 q3 { -h.x, 0.0f,  h.z };
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world, q0, q1, quadColor, decalThickness * 1.3f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world, q1, q2, quadColor, decalThickness * 1.3f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world, q2, q3, quadColor, decalThickness * 1.3f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world, q3, q0, quadColor, decalThickness * 1.3f, &overlayMin, &overlayMax);

							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								glm::vec3(-h.x, 0.0f, 0.0f), glm::vec3(h.x, 0.0f, 0.0f), quadColor, decalThickness * 0.9f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								glm::vec3(0.0f, 0.0f, -h.z), glm::vec3(0.0f, 0.0f, h.z), quadColor, decalThickness * 0.9f, &overlayMin, &overlayMax);
						}

						// 2. 大世界流送分区盒 (PartitionVolumeComponent)
						if (partVol && partVol->Active)
						{
							const Wui::WuiColor partColor { 0.2f, 0.7f, 1.0f, alpha };
							const glm::vec3 h = partVol->HalfExtents;
							glm::vec3 corners[8];
							for (int i = 0; i < 8; ++i)
							{
								corners[i] = glm::vec3 {
									(i & 1) ? h.x : -h.x,
									(i & 2) ? h.y : -h.y,
									(i & 4) ? h.z : -h.z
								};
							}
							for (int i = 0; i < 8; ++i)
							{
								for (int axis = 0; axis < 3; ++axis)
								{
									const int other = i ^ (1 << axis);
									if (other > i)
										PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
											corners[i], corners[other], partColor, thickness, &overlayMin, &overlayMax);
								}
							}
						}

						// 3. 动态导航障碍盒 (NavMeshObstacleComponent)
						// 若已在导航网格通道中开启障碍物显示 (NavMeshPanel::ShowObstacles)，此处跳过绘制，避免重叠双画
						if (obstacle && !NavMeshPanel::ShowObstacles)
						{
							const Wui::WuiColor obsColor { 0.95f, 0.85f, 0.15f, alpha };
							const glm::vec3 h = obstacle->Extents;
							const glm::vec3 localCorners[8] = {
								glm::vec3(-h.x, -h.y, -h.z),
								glm::vec3( h.x, -h.y, -h.z),
								glm::vec3( h.x, -h.y,  h.z),
								glm::vec3(-h.x, -h.y,  h.z),
								glm::vec3(-h.x,  h.y, -h.z),
								glm::vec3( h.x,  h.y, -h.z),
								glm::vec3( h.x,  h.y,  h.z),
								glm::vec3(-h.x,  h.y,  h.z)
							};
							glm::vec3 worldCorners[8];
							for (int i = 0; i < 8; ++i)
								worldCorners[i] = glm::vec3(world * glm::vec4(localCorners[i], 1.0f));

							constexpr int edges[12][2] = {
								{0,1}, {1,2}, {2,3}, {3,0},
								{4,5}, {5,6}, {6,7}, {7,4},
								{0,4}, {1,5}, {2,6}, {3,7}
							};
							for (int e = 0; e < 12; ++e)
							{
								PushOccludedWorldSegment(worldCorners[edges[e][0]], worldCorners[edges[e][1]],
									obsColor, thickness, handle);
							}
						}

						// 3.1 导航代价修饰盒 (NavMeshAreaModifierComponent)
						// 若已在导航网格通道中开启代价修饰显示 (NavMeshPanel::ShowAreaCosts)，此处跳过绘制，避免重叠双画
						if (areaMod && !NavMeshPanel::ShowAreaCosts)
						{
							Wui::WuiColor areaColor { 0.35f, 0.95f, 0.35f, alpha };
							if (areaMod->AreaType == 2)
								areaColor = Wui::WuiColor{ 0.70f, 0.35f, 0.95f, alpha };
							else if (areaMod->AreaType == 3)
								areaColor = Wui::WuiColor{ 0.95f, 0.45f, 0.15f, alpha };
							else if (areaMod->AreaType == 4)
								areaColor = Wui::WuiColor{ 0.95f, 0.20f, 0.20f, alpha };
							else if (areaMod->AreaType == 5)
								areaColor = Wui::WuiColor{ 0.15f, 0.85f, 0.95f, alpha };

							const glm::vec3 h = areaMod->Extents;
							const glm::vec3 localCorners[8] = {
								glm::vec3(-h.x, -h.y, -h.z),
								glm::vec3( h.x, -h.y, -h.z),
								glm::vec3( h.x, -h.y,  h.z),
								glm::vec3(-h.x, -h.y,  h.z),
								glm::vec3(-h.x,  h.y, -h.z),
								glm::vec3( h.x,  h.y, -h.z),
								glm::vec3( h.x,  h.y,  h.z),
								glm::vec3(-h.x,  h.y,  h.z)
							};
							glm::vec3 worldCorners[8];
							for (int i = 0; i < 8; ++i)
								worldCorners[i] = glm::vec3(world * glm::vec4(localCorners[i], 1.0f));

							constexpr int edges[12][2] = {
								{0,1}, {1,2}, {2,3}, {3,0},
								{4,5}, {5,6}, {6,7}, {7,4},
								{0,4}, {1,5}, {2,6}, {3,7}
							};
							for (int e = 0; e < 12; ++e)
							{
								PushOccludedWorldSegment(worldCorners[edges[e][0]], worldCorners[edges[e][1]],
									areaColor, thickness, handle);
							}
						}

						// 4. 智能对象交互距离与槽位范围 (SmartObjectComponent)
						if (smartObj && (isSelected || smartObj->Enabled))
						{
							const Wui::WuiColor smartColor { 0.25f, 0.95f, 0.55f, alpha };
							const float r = std::max(0.1f, smartObj->InteractionDistance);

							// 实体原点水平面交互大圆环 (XZ 平面)
							PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world,
								glm::vec3(0.0f), r, 1, smartColor, thickness * 1.5f, &overlayMin, &overlayMax);

							// 3 轴辅助立体圆环 (半透明)
							const Wui::WuiColor dimSmartColor { smartColor.R, smartColor.G, smartColor.B, smartColor.A * 0.4f };
							PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world,
								glm::vec3(0.0f), r, 0, dimSmartColor, 1.0f, &overlayMin, &overlayMax);
							PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world,
								glm::vec3(0.0f), r, 2, dimSmartColor, 1.0f, &overlayMin, &overlayMax);

							// 从原点到槽位偏移的引导线
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								glm::vec3(0.0f), smartObj->SlotOffset, dimSmartColor, thickness, &overlayMin, &overlayMax);

							// 槽位点朝向箭头 (FacingAngleDeg)
							const float rad = glm::radians(smartObj->FacingAngleDeg);
							const glm::vec3 facingDir { std::sin(rad), 0.0f, -std::cos(rad) };
							const glm::vec3 sideDir { std::cos(rad), 0.0f, std::sin(rad) };
							const float arrowLen = std::clamp(r * 0.4f, 0.3f, 1.2f);
							const glm::vec3 slotTip = smartObj->SlotOffset + facingDir * arrowLen;
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								smartObj->SlotOffset, slotTip, smartColor, thickness * 1.6f, &overlayMin, &overlayMax);

							const float headLen = arrowLen * 0.25f;
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								slotTip, slotTip - facingDir * headLen + sideDir * (headLen * 0.6f),
								smartColor, thickness * 1.4f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								slotTip, slotTip - facingDir * headLen - sideDir * (headLen * 0.6f),
								smartColor, thickness * 1.4f, &overlayMin, &overlayMax);

							// 槽位世界坐标十字标记 (基于 SlotOffset 绘制 3D 完整十字)
							const float slotCross = 0.2f;
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								smartObj->SlotOffset - sideDir * slotCross, smartObj->SlotOffset + sideDir * slotCross,
								smartColor, 1.8f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								smartObj->SlotOffset - facingDir * slotCross, smartObj->SlotOffset + facingDir * slotCross,
								smartColor, 1.8f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								smartObj->SlotOffset - glm::vec3(0.0f, slotCross, 0.0f), smartObj->SlotOffset + glm::vec3(0.0f, slotCross, 0.0f),
								smartColor, 1.8f, &overlayMin, &overlayMax);
						}

						// 5. 物理水体表面覆盖范围与波浪传播向 Gizmo (WaterBodyComponent)
						if (water && isSelected)
						{
							// 金青色（Cyan/Azure）水体边框与波浪指引色
							const Wui::WuiColor waterColor { 0.05f, 0.85f, 0.95f, 0.95f };
							const Wui::WuiColor gridColor { 0.10f, 0.70f, 0.90f, 0.45f };
							const Wui::WuiColor waveArrowColor { 0.95f, 0.85f, 0.20f, 1.0f }; // 金黄与青色高对比度
							const float waterThickness = 2.0f;

							// ① 水面覆盖 3D 边界盒 (遵循 TransformComponent.Scale 局部 [-0.5, 0.5])
							const glm::vec3 corners[8] = {
								{ -0.5f, -0.05f, -0.5f },
								{  0.5f, -0.05f, -0.5f },
								{  0.5f,  0.05f, -0.5f },
								{ -0.5f,  0.05f, -0.5f },
								{ -0.5f, -0.05f,  0.5f },
								{  0.5f, -0.05f,  0.5f },
								{  0.5f,  0.05f,  0.5f },
								{ -0.5f,  0.05f,  0.5f }
							};
							glm::vec3 worldCorners[8];
							for (int i = 0; i < 8; ++i)
								worldCorners[i] = glm::vec3(world * glm::vec4(corners[i], 1.0f));

							// 底面 4 条边 (逐段遮挡)
							PushOccludedWorldSegment(worldCorners[0], worldCorners[1], waterColor, waterThickness, handle);
							PushOccludedWorldSegment(worldCorners[1], worldCorners[2], waterColor, waterThickness, handle);
							PushOccludedWorldSegment(worldCorners[2], worldCorners[3], waterColor, waterThickness, handle);
							PushOccludedWorldSegment(worldCorners[3], worldCorners[0], waterColor, waterThickness, handle);
							// 顶面 4 条边
							PushOccludedWorldSegment(worldCorners[4], worldCorners[5], waterColor, waterThickness, handle);
							PushOccludedWorldSegment(worldCorners[5], worldCorners[6], waterColor, waterThickness, handle);
							PushOccludedWorldSegment(worldCorners[6], worldCorners[7], waterColor, waterThickness, handle);
							PushOccludedWorldSegment(worldCorners[7], worldCorners[4], waterColor, waterThickness, handle);
							// 4 条垂直边
							PushOccludedWorldSegment(worldCorners[0], worldCorners[4], waterColor, waterThickness, handle);
							PushOccludedWorldSegment(worldCorners[1], worldCorners[5], waterColor, waterThickness, handle);
							PushOccludedWorldSegment(worldCorners[2], worldCorners[6], waterColor, waterThickness, handle);
							PushOccludedWorldSegment(worldCorners[3], worldCorners[7], waterColor, waterThickness, handle);

							// 水面顶层 XZ 分割十字线
							PushOccludedWorldSegment(glm::vec3(world * glm::vec4(-0.5f, 0.05f, 0.0f, 1.0f)),
								glm::vec3(world * glm::vec4(0.5f, 0.05f, 0.0f, 1.0f)), gridColor, 1.2f, handle);
							PushOccludedWorldSegment(glm::vec3(world * glm::vec4(0.0f, 0.05f, -0.5f, 1.0f)),
								glm::vec3(world * glm::vec4(0.0f, 0.05f, 0.5f, 1.0f)), gridColor, 1.2f, handle);

							// ② 波浪主传播向矢量箭头 (Primary Wave Propagation Vector Arrow)
							const glm::vec3 waveDir = glm::normalize(glm::vec3(1.0f, 0.0f, 0.2f));
							const glm::vec3 waveSide = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), waveDir));
							const glm::vec3 arrowStart = glm::vec3(0.0f, 0.08f, 0.0f) - waveDir * 0.22f;
							const glm::vec3 arrowEnd   = glm::vec3(0.0f, 0.08f, 0.0f) + waveDir * 0.22f;

							// 箭头主轴
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								arrowStart, arrowEnd, waveArrowColor, 2.6f, &overlayMin, &overlayMax);
							// 箭头双翼
							const glm::vec3 wing0 = arrowEnd - waveDir * 0.06f + waveSide * 0.035f;
							const glm::vec3 wing1 = arrowEnd - waveDir * 0.06f - waveSide * 0.035f;
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								arrowEnd, wing0, waveArrowColor, 2.6f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								arrowEnd, wing1, waveArrowColor, 2.6f, &overlayMin, &overlayMax);

							// 波峰波前波纹线条 (Wavefront Crest Ripples)
							for (float offset : { -0.12f, 0.0f, 0.10f })
							{
								const glm::vec3 rippleCenter = glm::vec3(0.0f, 0.08f, 0.0f) + waveDir * offset;
								const float crestSpan = 0.08f * (1.0f - std::abs(offset) * 2.0f);
								PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
									rippleCenter - waveSide * crestSpan, rippleCenter + waveSide * crestSpan,
									waveArrowColor, 1.6f, &overlayMin, &overlayMax);
							}
						}
					}
				}
					// ---- 选中网格模型立体线框盒 (MeshRendererComponent): 逐段遮挡绘制，杜绝穿透建筑物/山体/地表 ----
					auto drawWireframeBox = [&](const glm::mat4& world, const glm::vec3& bMin, const glm::vec3& bMax, bool isPlane, const Wui::WuiColor& color, entt::entity handle)
					{
						const glm::vec3 corners[8] = {
							{ bMin.x, isPlane ? 0.0f : bMin.y, bMin.z },
							{ bMax.x, isPlane ? 0.0f : bMin.y, bMin.z },
							{ bMax.x, isPlane ? 0.0f : bMin.y, bMin.z },
							{ bMin.x, isPlane ? 0.0f : bMax.y, bMin.z },
							{ bMin.x, isPlane ? 0.0f : bMin.y, bMax.z },
							{ bMax.x, isPlane ? 0.0f : bMin.y, bMax.z },
							{ bMax.x, isPlane ? 0.0f : bMax.y, bMax.z },
							{ bMin.x, isPlane ? 0.0f : bMax.y, bMax.z },
						};
						glm::vec3 worldCorners[8];
						for (int i = 0; i < 8; ++i)
							worldCorners[i] = glm::vec3(world * glm::vec4(corners[i], 1.0f));

						if (isPlane)
						{
							const int planeEdges[4][2] = { { 0, 1 }, { 1, 5 }, { 5, 4 }, { 4, 0 } };
							for (const auto& edge : planeEdges)
							{
								PushOccludedWorldSegment(worldCorners[edge[0]], worldCorners[edge[1]], color, 1.8f, handle);
							}
						}
						else
						{
							const glm::mat3 normalMatrix = glm::mat3(world);
							const glm::vec3 faceNormals[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
								{ 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
							const glm::vec3 faceCenters[6] = {
								{ bMax.x, (bMin.y + bMax.y) * 0.5f, (bMin.z + bMax.z) * 0.5f },
								{ bMin.x, (bMin.y + bMax.y) * 0.5f, (bMin.z + bMax.z) * 0.5f },
								{ (bMin.x + bMax.x) * 0.5f, bMax.y, (bMin.z + bMax.z) * 0.5f },
								{ (bMin.x + bMax.x) * 0.5f, bMin.y, (bMin.z + bMax.z) * 0.5f },
								{ (bMin.x + bMax.x) * 0.5f, (bMin.y + bMax.y) * 0.5f, bMax.z },
								{ (bMin.x + bMax.x) * 0.5f, (bMin.y + bMax.y) * 0.5f, bMin.z }
							};
							bool faceVisible[6] = {};
							for (int i = 0; i < 6; ++i)
							{
								const glm::vec3 centerWorld = glm::vec3(world * glm::vec4 { faceCenters[i], 1.0f });
								const glm::vec3 normalWorld = glm::normalize(normalMatrix * faceNormals[i]);
								faceVisible[i] = glm::dot(normalWorld, overlayCamera.Position - centerWorld) > 0.0f;
							}

							const int cubeEdges[12][4] = {
								{ 0, 1, 3, 5 }, { 2, 3, 2, 5 }, { 4, 5, 3, 4 }, { 6, 7, 2, 4 },
								{ 0, 2, 1, 5 }, { 1, 3, 0, 5 }, { 4, 6, 1, 4 }, { 5, 7, 0, 4 },
								{ 0, 4, 1, 3 }, { 1, 5, 0, 3 }, { 2, 6, 1, 2 }, { 3, 7, 0, 2 },
							};
							for (const auto& edge : cubeEdges)
							{
								if (faceVisible[edge[2]] || faceVisible[edge[3]])
								{
									PushOccludedWorldSegment(worldCorners[edge[0]], worldCorners[edge[1]], color, 1.8f, handle);
								}
							}
						}
					};

					if (selected.IsValid() && selected.HasComponent<MeshRendererComponent>() &&
						selected.HasComponent<TransformComponent>())
					{
						glm::mat4 world = selected.GetComponent<TransformComponent>().GetLocalMatrix();
						if (selected.HasComponent<WorldTransformComponent>())
							world = selected.GetComponent<WorldTransformComponent>().Matrix;
						const auto& mr = selected.GetComponent<MeshRendererComponent>();
						const bool plane = mr.Primitive == MeshRendererComponent::PrimitiveShape::Plane && !mr.Mesh.HasPath() && !mr.Mesh.Identity.IsValid();

						glm::vec3 bMin { -0.5f, -0.5f, -0.5f };
						glm::vec3 bMax {  0.5f,  0.5f,  0.5f };
						if (mr.Mesh.HasPath() || mr.Mesh.Identity.IsValid())
						{
							std::string err;
							if (auto mesh = Mesh::LoadWModel(mr.Mesh.Path, &err))
							{
								bMin = mesh->GetBounds().Min;
								bMax = mesh->GetBounds().Max;
							}
						}
						if (bMin.x > bMax.x || bMin.y > bMax.y || bMin.z > bMax.z)
						{
							bMin = glm::vec3(-0.5f);
							bMax = glm::vec3(0.5f);
						}
						bMax = glm::max(bMax, bMin + glm::vec3(0.05f));

						drawWireframeBox(world, bMin, bMax, plane, theme.Warning, static_cast<entt::entity>(selected));
					}

					// ---- 选中动画骨骼模型立体线框盒 (SkinnedMeshRendererComponent) ----
					if (selected.IsValid() && selected.HasComponent<SkinnedMeshRendererComponent>() &&
						selected.HasComponent<TransformComponent>())
					{
						glm::mat4 world = selected.GetComponent<TransformComponent>().GetLocalMatrix();
						if (selected.HasComponent<WorldTransformComponent>())
							world = selected.GetComponent<WorldTransformComponent>().Matrix;
						const auto& smr = selected.GetComponent<SkinnedMeshRendererComponent>();

						glm::vec3 bMin { -0.5f, -0.5f, -0.5f };
						glm::vec3 bMax {  0.5f,  0.5f,  0.5f };
						if (smr.Mesh.HasPath() || smr.Mesh.Identity.IsValid())
						{
							std::string err;
							if (auto mesh = Mesh::LoadWModel(smr.Mesh.Path, &err))
							{
								bMin = mesh->GetBounds().Min;
								bMax = mesh->GetBounds().Max;
							}
						}
						if (bMin.x > bMax.x || bMin.y > bMax.y || bMin.z > bMax.z)
						{
							bMin = glm::vec3(-0.5f);
							bMax = glm::vec3(0.5f);
						}
						bMax = glm::max(bMax, bMin + glm::vec3(0.05f));

						drawWireframeBox(world, bMin, bMax, false, theme.Warning, static_cast<entt::entity>(selected));
					}

					// ---- 选中 2D 精灵图元线框 (SpriteComponent) ----
					if (selected.IsValid() && selected.HasComponent<SpriteComponent>() &&
						selected.HasComponent<TransformComponent>())
					{
						const auto& spr = selected.GetComponent<SpriteComponent>();
						glm::mat4 world = selected.GetComponent<TransformComponent>().GetLocalMatrix();
						if (selected.HasComponent<WorldTransformComponent>())
							world = selected.GetComponent<WorldTransformComponent>().Matrix;
						if (spr.Pivot.x != 0.5f || spr.Pivot.y != 0.5f)
							world = glm::translate(world, glm::vec3(0.5f - spr.Pivot.x, 0.5f - spr.Pivot.y, 0.0f));

						const glm::vec3 corners[4] = {
							glm::vec3(world * glm::vec4(-0.5f, -0.5f, 0.0f, 1.0f)),
							glm::vec3(world * glm::vec4( 0.5f, -0.5f, 0.0f, 1.0f)),
							glm::vec3(world * glm::vec4( 0.5f,  0.5f, 0.0f, 1.0f)),
							glm::vec3(world * glm::vec4(-0.5f,  0.5f, 0.0f, 1.0f)),
						};
						const Wui::WuiColor selectOutlineColor = theme.Warning;
						const entt::entity selHandle = static_cast<entt::entity>(selected);
						for (int i = 0; i < 4; ++i)
						{
							PushOccludedWorldSegment(corners[i], corners[(i + 1) % 4], selectOutlineColor, 1.8f, selHandle);
						}
					}

					// ---- 选中 2D 瓦片地图线框 (TilemapComponent) ----
					if (selected.IsValid() && selected.HasComponent<TilemapComponent>() &&
						selected.HasComponent<TransformComponent>())
					{
						const auto& tmComp = selected.GetComponent<TilemapComponent>();
						glm::mat4 world = selected.GetComponent<TransformComponent>().GetLocalMatrix();
						if (selected.HasComponent<WorldTransformComponent>())
							world = selected.GetComponent<WorldTransformComponent>().Matrix;

						float mapW = 16.0f, mapH = 16.0f;
						if (tmComp.Tilemap.HasPath())
						{
							const std::string tmP = StringPool::Get().PathOf(tmComp.Tilemap.Path);
							if (auto tm = Tilemap::GetOrLoad(tmP))
							{
								mapW = static_cast<float>(tm->GetWidth()) * (tmComp.CellSize.x > 0.0f ? tmComp.CellSize.x : tm->GetCellSize().x);
								mapH = static_cast<float>(tm->GetHeight()) * (tmComp.CellSize.y > 0.0f ? tmComp.CellSize.y : tm->GetCellSize().y);
							}
						}

						const glm::vec3 corners[4] = {
							glm::vec3(world * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)),
							glm::vec3(world * glm::vec4(mapW, 0.0f, 0.0f, 1.0f)),
							glm::vec3(world * glm::vec4(mapW, mapH, 0.0f, 1.0f)),
							glm::vec3(world * glm::vec4(0.0f, mapH, 0.0f, 1.0f)),
						};
						const Wui::WuiColor selectOutlineColor = theme.Warning;
						const entt::entity selHandle = static_cast<entt::entity>(selected);
						for (int i = 0; i < 4; ++i)
						{
							PushOccludedWorldSegment(corners[i], corners[(i + 1) % 4], selectOutlineColor, 1.8f, selHandle);
						}
					}

					// 选中实体 LOD 同心圆环与实时标签
					if (selected.IsValid() && selected.HasComponent<MeshLODComponent>() && selected.HasComponent<TransformComponent>())
					{
						const auto& lod = selected.GetComponent<MeshLODComponent>();
						const auto& xf = selected.GetComponent<TransformComponent>();
						const glm::vec3 entCenter = xf.Location;
						const float distToCam = glm::length(overlayCamera.Position - entCenter);

						// 1. 三圈平滑同心距离圆环 (水平 XZ 平面)
						if (lod.LOD1Distance > 0.0f)
						{
							PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f),
								entCenter, lod.LOD1Distance, 1, Wui::WuiColor{ 0.2f, 0.9f, 0.2f, 0.85f }, 1.8f, &overlayMin, &overlayMax);
						}
						if (lod.LOD2Distance > 0.0f)
						{
							PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f),
								entCenter, lod.LOD2Distance, 1, Wui::WuiColor{ 0.95f, 0.85f, 0.1f, 0.85f }, 1.8f, &overlayMin, &overlayMax);
						}
						if (lod.CullDistance > 0.0f)
						{
							PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f),
								entCenter, lod.CullDistance, 1, Wui::WuiColor{ 0.85f, 0.2f, 0.2f, 0.85f }, 1.8f, &overlayMin, &overlayMax);
						}

						// 2. 实体上方实时标签
						const glm::vec4 clipPos = overlayCamera.ViewProjection * glm::vec4(entCenter + glm::vec3(0.0f, xf.Scale.y * 0.6f + 0.5f, 0.0f), 1.0f);
						if (clipPos.w > 0.05f)
						{
							const glm::vec2 ndc { clipPos.x / clipPos.w, clipPos.y / clipPos.w };
							const glm::vec2 badgePos {
								sceneRect.X + (ndc.x + 1.0f) * 0.5f * sceneRect.W,
								sceneRect.Y + (1.0f - ndc.y) * 0.5f * sceneRect.H
							};
							if (badgePos.x >= sceneRect.X && badgePos.x <= sceneRect.X + sceneRect.W &&
								badgePos.y >= sceneRect.Y && badgePos.y <= sceneRect.Y + sceneRect.H)
							{
								char lodTagText[64];
								const glm::vec4 glmCol = MeshLODManager::Get().GetLODColor(lod.CurrentLOD);
								Wui::WuiColor tagCol { glmCol.r, glmCol.g, glmCol.b, 1.0f };
								if (lod.CurrentLOD == 255)
								{
									std::snprintf(lodTagText, sizeof(lodTagText), "CULLED | Dist: %.1f m", distToCam);
								}
								else if (lod.CurrentLOD < 8)
								{
									std::snprintf(lodTagText, sizeof(lodTagText), "LOD %u | Dist: %.1f m",
										static_cast<unsigned>(lod.CurrentLOD), distToCam);
								}
								else
								{
									std::snprintf(lodTagText, sizeof(lodTagText), "LOD ? | Dist: %.1f m", distToCam);
								}

								const float fontSz = 11.5f;
								const float textW = ctx.MeasureTextWidth(lodTagText, fontSz);
								const Wui::WuiRect tagRect { badgePos.x - textW * 0.5f - 8.0f, badgePos.y - 20.0f, textW + 16.0f, 20.0f };
								Wui::PanelBackground(ctx, tagRect, Wui::WuiColor{ 0.05f, 0.10f, 0.15f, 0.90f }, 4.0f);
								Wui::HighlightOutline(ctx, tagRect, tagCol, 4.0f, 1.2f);
								Wui::Label(ctx, { tagRect.X + 8.0f, tagRect.Y + 3.0f }, lodTagText, tagCol, fontSz);
							}
						}
					}

					// ---- GPU Meshlet 集群真彩染色与硬件簇调试覆盖层 (MeshletComponent) ----
					// 严格遵循实体组件对等律 (Entity-Component Parity)，杜绝名称字符串或数据层隐式特化
					const bool showAllMeshlets = IsMeshletClustersVisible();
					std::unordered_set<entt::entity> renderedMeshletEntities;
					for (const entt::entity handle : registry.view<TransformComponent, MeshletComponent>())
					{
						auto& meshletComp = registry.get<MeshletComponent>(handle);
						if (!meshletComp.Enabled)
							continue;

						const bool isSelected = (handle == selectedHandle);
						if (!showAllMeshlets && !isSelected && !meshletComp.ShowClusters)
							continue;

						const auto& xf = registry.get<TransformComponent>(handle);
						const glm::mat4 world = WorldMatrixOf(registry, handle);

						std::vector<glm::vec3> vertices;
						std::vector<uint32_t> indices;

						if (const auto* wb = registry.try_get<WhiteboxComponent>(handle))
						{
							WhiteboxMesh wbMesh;
							WhiteboxBuilder::BuildMesh(*wb, wbMesh);
							vertices = std::move(wbMesh.Vertices);
							indices = std::move(wbMesh.Indices);
						}
						else if (const auto* mr = registry.try_get<MeshRendererComponent>(handle))
						{
							WhiteboxComponent dummyWb;
							dummyWb.Dimensions = glm::vec3(1.0f);
							dummyWb.Subdivisions = 4;
						if (mr->Primitive == MeshRendererComponent::PrimitiveShape::Sphere)
							dummyWb.Shape = static_cast<uint8_t>(WhiteboxComponent::ShapeType::Sphere);
						else
							dummyWb.Shape = static_cast<uint8_t>(WhiteboxComponent::ShapeType::Box);
							WhiteboxMesh wbMesh;
							WhiteboxBuilder::BuildMesh(dummyWb, wbMesh);
							vertices = std::move(wbMesh.Vertices);
							indices = std::move(wbMesh.Indices);
						}

						if (vertices.empty() || indices.empty())
							continue;

						std::vector<Meshlet> meshlets;
						std::vector<uint32_t> meshletVerts;
						std::vector<uint8_t> meshletTris;
						const uint32_t maxV = std::clamp(static_cast<uint32_t>(meshletComp.MaxVertices), 8u, 64u);
						const uint32_t maxT = std::clamp(static_cast<uint32_t>(meshletComp.MaxTriangles), 4u, 126u);
						MeshletBuilder::BuildMeshlets(vertices, indices, meshlets, meshletVerts, meshletTris, maxV, maxT);

						if (meshlets.empty())
							continue;


						renderedMeshletEntities.insert(handle);
						const glm::mat3 normalMatrix = glm::mat3(world);

						// 逐集群着色绘制微型簇三角形与包络轴锥
						for (size_t mIdx = 0; mIdx < meshlets.size(); ++mIdx)
						{
							const auto& m = meshlets[mIdx];
							const glm::vec4 debugCol = MeshletBuilder::GetMeshletDebugColor(static_cast<uint32_t>(mIdx));
							const Wui::WuiColor clusterCol { debugCol.r, debugCol.g, debugCol.b, 0.95f };
							const Wui::WuiColor coneCol { debugCol.r, debugCol.g, debugCol.b, 0.65f };

							// 1. 簇内三角形线框 (以独立色彩呈现簇划分边缘与拓扑)
							const uint32_t triBase = m.TriangleOffset;
							for (uint32_t t = 0; t < m.TriangleCount; ++t)
							{
								const uint32_t i0 = triBase + t * 3;
								const uint32_t i1 = triBase + t * 3 + 1;
								const uint32_t i2 = triBase + t * 3 + 2;
								if (i2 >= meshletTris.size()) break;

								const uint32_t v0Idx = m.VertexOffset + meshletTris[i0];
								const uint32_t v1Idx = m.VertexOffset + meshletTris[i1];
								const uint32_t v2Idx = m.VertexOffset + meshletTris[i2];
								if (v2Idx >= meshletVerts.size()) break;

								const uint32_t g0 = meshletVerts[v0Idx];
								const uint32_t g1 = meshletVerts[v1Idx];
								const uint32_t g2 = meshletVerts[v2Idx];
								if (g0 >= vertices.size() || g1 >= vertices.size() || g2 >= vertices.size()) break;

								const glm::vec3 w0 = glm::vec3(world * glm::vec4(vertices[g0], 1.0f));
								const glm::vec3 w1 = glm::vec3(world * glm::vec4(vertices[g1], 1.0f));
								const glm::vec3 w2 = glm::vec3(world * glm::vec4(vertices[g2], 1.0f));

								PushOccludedWorldSegment(w0, w1, clusterCol, 1.6f, handle);
								PushOccludedWorldSegment(w1, w2, clusterCol, 1.6f, handle);
								PushOccludedWorldSegment(w2, w0, clusterCol, 1.6f, handle);
							}

							// 2. 集群背面剔除法线轴锥 (Normal Cone Axis)
							const glm::vec3 sphereCenterWorld = glm::vec3(world * glm::vec4(m.BoundingCenter, 1.0f));
							const glm::vec3 coneDirWorld = glm::normalize(normalMatrix * m.ConeAxis);
							const float coneLen = std::max(0.15f, m.BoundingRadius * 0.40f);
							PushOccludedWorldSegment(sphereCenterWorld, sphereCenterWorld + coneDirWorld * coneLen, coneCol, 2.0f, handle);
						}

						// 3. 实体头顶 3D 悬浮标牌 (Meshlet Telemetry Badge)
						const glm::vec3 entCenter = glm::vec3(world[3]);
						const glm::vec4 clipPos = overlayCamera.ViewProjection * glm::vec4(entCenter + glm::vec3(0.0f, xf.Scale.y * 0.55f + 0.35f, 0.0f), 1.0f);
						if (clipPos.w > 0.05f)
						{
							const glm::vec2 ndc { clipPos.x / clipPos.w, clipPos.y / clipPos.w };
							const glm::vec2 badgePos {
								sceneRect.X + (ndc.x + 1.0f) * 0.5f * sceneRect.W,
								sceneRect.Y + (1.0f - ndc.y) * 0.5f * sceneRect.H
							};
							if (badgePos.x >= sceneRect.X && badgePos.x <= sceneRect.X + sceneRect.W &&
								badgePos.y >= sceneRect.Y && badgePos.y <= sceneRect.Y + sceneRect.H)
							{
								char badgeText[128];
								std::snprintf(badgeText, sizeof(badgeText), "Meshlets: %zu 簇 (≤64V/126T)", meshlets.size());
								const float fontSz = 11.5f;
								const float textW = ctx.MeasureTextWidth(badgeText, fontSz);
								const Wui::WuiRect badgeRect { badgePos.x - textW * 0.5f - 8.0f, badgePos.y - 20.0f, textW + 16.0f, 20.0f };
								const Wui::WuiColor badgeBg { 0.06f, 0.10f, 0.18f, 0.90f };
								const Wui::WuiColor badgeBorder { 0.35f, 0.85f, 1.0f, 0.95f };
								const Wui::WuiColor textCol { 0.85f, 0.95f, 1.0f, 1.0f };
								Wui::PanelBackground(ctx, badgeRect, badgeBg, 4.0f);
								Wui::HighlightOutline(ctx, badgeRect, badgeBorder, 4.0f, 1.2f);
								Wui::Label(ctx, { badgeRect.X + 8.0f, badgeRect.Y + 3.0f }, badgeText, textCol, fontSz);
							}
						}
					}

					// ---- 选中白盒原型线框 (WhiteboxComponent) ----
					// 严格遵循面法线朝向与静态遮挡剔除，彻底杜绝线条穿透遮挡物与自身
					if (selected.IsValid() && selected.HasComponent<WhiteboxComponent>() &&
						selected.HasComponent<TransformComponent>())
					{
						const auto& wb = selected.GetComponent<WhiteboxComponent>();
						const bool alreadyDrewMeshlets = (renderedMeshletEntities.count(static_cast<entt::entity>(selected)) > 0);
						if (!alreadyDrewMeshlets)
						{
							glm::mat4 world = selected.GetComponent<TransformComponent>().GetLocalMatrix();
							if (selected.HasComponent<WorldTransformComponent>())
								world = selected.GetComponent<WorldTransformComponent>().Matrix;

							const Wui::WuiColor wbOutlineColor { theme.Accent.R, theme.Accent.G, theme.Accent.B, 0.95f };
							const float wbThickness = 1.8f;
							const entt::entity selHandle = static_cast<entt::entity>(selected);

						if (wb.GetShapeType() == WhiteboxComponent::ShapeType::Box)
						{
							const glm::vec3 h = wb.Dimensions * 0.5f;
							const glm::vec3 corners[8] = {
								{ -h.x, -h.y, -h.z }, {  h.x, -h.y, -h.z },
								{  h.x,  h.y, -h.z }, { -h.x,  h.y, -h.z },
								{ -h.x, -h.y,  h.z }, {  h.x, -h.y,  h.z },
								{  h.x,  h.y,  h.z }, { -h.x,  h.y,  h.z },
							};
							glm::vec3 worldCorners[8];
							for (int i = 0; i < 8; ++i)
								worldCorners[i] = glm::vec3(world * glm::vec4(corners[i], 1.0f));

							const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
							const glm::vec3 faceNormals[6] = {
								{  1,  0,  0 }, { -1,  0,  0 },
								{  0,  1,  0 }, {  0, -1,  0 },
								{  0,  0,  1 }, {  0,  0, -1 }
							};
							const glm::vec3 faceCenters[6] = {
								{  h.x, 0, 0 }, { -h.x, 0, 0 },
								{ 0,  h.y, 0 }, { 0, -h.y, 0 },
								{ 0, 0,  h.z }, { 0, 0, -h.z }
							};
							bool faceVisible[6] = {};
							for (int f = 0; f < 6; ++f)
							{
								const glm::vec3 centerWorld = glm::vec3(world * glm::vec4(faceCenters[f], 1.0f));
								const glm::vec3 normalWorld = glm::normalize(normalMatrix * faceNormals[f]);
								faceVisible[f] = glm::dot(normalWorld, overlayCamera.Position - centerWorld) > 0.0f;
							}
							const int boxEdges[12][4] = {
								{ 0, 1, 3, 5 }, { 2, 3, 2, 5 }, { 4, 5, 3, 4 }, { 6, 7, 2, 4 },
								{ 0, 2, 1, 5 }, { 1, 3, 0, 5 }, { 4, 6, 1, 4 }, { 5, 7, 0, 4 },
								{ 0, 4, 1, 3 }, { 1, 5, 0, 3 }, { 2, 6, 1, 2 }, { 3, 7, 0, 2 },
							};
							for (const auto& edge : boxEdges)
							{
								if (faceVisible[edge[2]] || faceVisible[edge[3]])
								{
									PushOccludedWorldSegment(worldCorners[edge[0]], worldCorners[edge[1]],
										wbOutlineColor, wbThickness, selHandle);
								}
							}
						}
						else if (wb.GetShapeType() == WhiteboxComponent::ShapeType::Cylinder)
						{
							const float r = wb.Dimensions.x * 0.5f;
							const float halfH = wb.Dimensions.y * 0.5f;
							constexpr int kSegs = 24;
							constexpr float k2Pi = 6.2831853f;
							for (int i = 0; i < kSegs; ++i)
							{
								const float a0 = static_cast<float>(i) * k2Pi / static_cast<float>(kSegs);
								const float a1 = static_cast<float>(i + 1) * k2Pi / static_cast<float>(kSegs);

								const glm::vec3 pTop0 = glm::vec3(world * glm::vec4(std::cos(a0) * r,  halfH, std::sin(a0) * r, 1.0f));
								const glm::vec3 pTop1 = glm::vec3(world * glm::vec4(std::cos(a1) * r,  halfH, std::sin(a1) * r, 1.0f));
								PushOccludedWorldSegment(pTop0, pTop1, wbOutlineColor, wbThickness, selHandle);

								const glm::vec3 pBot0 = glm::vec3(world * glm::vec4(std::cos(a0) * r, -halfH, std::sin(a0) * r, 1.0f));
								const glm::vec3 pBot1 = glm::vec3(world * glm::vec4(std::cos(a1) * r, -halfH, std::sin(a1) * r, 1.0f));
								PushOccludedWorldSegment(pBot0, pBot1, wbOutlineColor, wbThickness, selHandle);

								if (i % (kSegs / 4) == 0)
								{
									PushOccludedWorldSegment(pBot0, pTop0, wbOutlineColor, wbThickness, selHandle);
								}
							}
						}
						else if (wb.GetShapeType() == WhiteboxComponent::ShapeType::Wedge)
						{
							const glm::vec3 h = wb.Dimensions * 0.5f;
							const glm::vec3 w[6] = {
								{ -h.x, -h.y, -h.z }, {  h.x, -h.y, -h.z },
								{  h.x, -h.y,  h.z }, { -h.x, -h.y,  h.z },
								{ -h.x,  h.y,  h.z }, {  h.x,  h.y,  h.z }
							};
							glm::vec3 worldW[6];
							for (int i = 0; i < 6; ++i)
								worldW[i] = glm::vec3(world * glm::vec4(w[i], 1.0f));

							const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
							const glm::vec3 slopeNormalLocal = glm::normalize(glm::vec3(0.0f, h.z, -h.y));
							const glm::vec3 faceNormals[5] = {
								{ 0, -1, 0 }, { 0, 0, 1 }, slopeNormalLocal, { -1, 0, 0 }, { 1, 0, 0 }
							};
							const glm::vec3 faceCenters[5] = {
								{ 0, -h.y, 0 }, { 0, 0, h.z }, { 0, 0, 0 }, { -h.x, 0, 0 }, { h.x, 0, 0 }
							};
							bool faceVisible[5] = {};
							for (int f = 0; f < 5; ++f)
							{
								const glm::vec3 centerWorld = glm::vec3(world * glm::vec4(faceCenters[f], 1.0f));
								const glm::vec3 normalWorld = glm::normalize(normalMatrix * faceNormals[f]);
								faceVisible[f] = glm::dot(normalWorld, overlayCamera.Position - centerWorld) > 0.0f;
							}
							const int wedgeEdges[9][4] = {
								{ 0, 1, 0, 2 }, { 1, 2, 0, 4 }, { 2, 3, 0, 1 }, { 3, 0, 0, 3 },
								{ 4, 5, 1, 2 }, { 3, 4, 1, 3 }, { 2, 5, 1, 4 },
								{ 0, 4, 2, 3 }, { 1, 5, 2, 4 }
							};
							for (const auto& edge : wedgeEdges)
							{
								if (faceVisible[edge[2]] || faceVisible[edge[3]])
								{
									PushOccludedWorldSegment(worldW[edge[0]], worldW[edge[1]], wbOutlineColor, wbThickness, selHandle);
								}
							}
						}
						else if (wb.GetShapeType() == WhiteboxComponent::ShapeType::Stairs)
						{
							const uint32_t nSteps = std::max(1u, static_cast<uint32_t>(wb.Steps));
							const float stepH = wb.Dimensions.y / static_cast<float>(nSteps);
							const float stepD = wb.Dimensions.z / static_cast<float>(nSteps);
							const float halfW = wb.Dimensions.x * 0.5f;

							for (uint32_t i = 0; i < nSteps; ++i)
							{
								const float y0 = static_cast<float>(i) * stepH;
								const float y1 = y0 + stepH;
								const float z0 = static_cast<float>(i) * stepD;
								const float z1 = z0 + stepD;

								const glm::vec3 t0 = glm::vec3(world * glm::vec4(-halfW, y1, z1, 1.0f));
								const glm::vec3 t1 = glm::vec3(world * glm::vec4( halfW, y1, z1, 1.0f));
								const glm::vec3 t2 = glm::vec3(world * glm::vec4(-halfW, y1, z0, 1.0f));
								const glm::vec3 t3 = glm::vec3(world * glm::vec4( halfW, y1, z0, 1.0f));

								PushOccludedWorldSegment(t0, t1, wbOutlineColor, wbThickness, selHandle);
								PushOccludedWorldSegment(t2, t0, wbOutlineColor, wbThickness, selHandle);
								PushOccludedWorldSegment(t3, t1, wbOutlineColor, wbThickness, selHandle);

								const glm::vec3 r0 = glm::vec3(world * glm::vec4(-halfW, y0, z0, 1.0f));
								const glm::vec3 r1 = glm::vec3(world * glm::vec4( halfW, y0, z0, 1.0f));
								PushOccludedWorldSegment(r0, r1, wbOutlineColor, wbThickness, selHandle);
								PushOccludedWorldSegment(r0, t2, wbOutlineColor, wbThickness, selHandle);
								PushOccludedWorldSegment(r1, t3, wbOutlineColor, wbThickness, selHandle);
							}
						}
						else // Sphere / Arch
						{
							const float r = wb.Dimensions.x * 0.5f;
							constexpr int kSegs = 24;
							constexpr float k2Pi = 6.2831853f;
							for (int i = 0; i < kSegs; ++i)
							{
								const float a0 = static_cast<float>(i) * k2Pi / static_cast<float>(kSegs);
								const float a1 = static_cast<float>(i + 1) * k2Pi / static_cast<float>(kSegs);

								const glm::vec3 p0 = glm::vec3(world * glm::vec4(std::cos(a0) * r, 0.0f, std::sin(a0) * r, 1.0f));
								const glm::vec3 p1 = glm::vec3(world * glm::vec4(std::cos(a1) * r, 0.0f, std::sin(a1) * r, 1.0f));
								PushOccludedWorldSegment(p0, p1, wbOutlineColor, wbThickness, selHandle);

								const glm::vec3 q0 = glm::vec3(world * glm::vec4(std::cos(a0) * r, std::sin(a0) * r, 0.0f, 1.0f));
								const glm::vec3 q1 = glm::vec3(world * glm::vec4(std::cos(a1) * r, std::sin(a1) * r, 0.0f, 1.0f));
								PushOccludedWorldSegment(q0, q1, wbOutlineColor, wbThickness, selHandle);
							}
						}
						}
					}

					// ---- 选中样条线与控制点 3D 曲线 (SplineComponent) ----
					Entity splineEntity;
					if (selected.IsValid())
					{
						if (selected.HasComponent<SplineComponent>())
						{
							splineEntity = selected;
						}
						else
						{
							const auto* hc = registry.try_get<HierarchyComponent>(selectedHandle);
							const entt::entity parentHandle = hc ? hc->Parent : entt::null;
							if (parentHandle != entt::null && registry.valid(parentHandle) && registry.all_of<SplineComponent>(parentHandle))
							{
								splineEntity = Entity(overlayScene.get(), parentHandle);
							}
						}
					}

					if (splineEntity.IsValid() && splineEntity.HasComponent<SplineComponent>())
					{
						const auto& sc = splineEntity.GetComponent<SplineComponent>();
						std::vector<Entity> knots;
						const auto& children = Hierarchy::ChildrenOf(registry, static_cast<entt::entity>(splineEntity));
						for (const entt::entity h : children)
						{
							knots.push_back(Entity(overlayScene.get(), h));
						}

						if (knots.size() >= 2)
						{
							Spline3D spline;
							std::vector<glm::vec3> knotWorldPositions;
							knotWorldPositions.reserve(knots.size());
							for (auto& k : knots)
							{
								glm::vec3 wp = k.GetComponent<TransformComponent>().Location;
								if (k.HasComponent<WorldTransformComponent>())
									wp = glm::vec3(k.GetComponent<WorldTransformComponent>().Matrix[3]);
								spline.AddPoint(wp);
								knotWorldPositions.push_back(wp);
							}

							const Wui::WuiColor splineGold { 1.0f, 0.82f, 0.18f, 0.95f };
							const Wui::WuiColor roadEdgeColor { 0.95f, 0.72f, 0.15f, 0.55f };
							const Wui::WuiColor roadRungColor { 0.85f, 0.65f, 0.10f, 0.35f };

							constexpr int kSplineSamples = 80;
							glm::vec3 prevCenter { 0.0f };
							glm::vec3 prevLeft { 0.0f };
							glm::vec3 prevRight { 0.0f };

							const float halfRoadW = sc.RoadWidth * 0.5f;

							for (int i = 0; i <= kSplineSamples; ++i)
							{
								const float t = static_cast<float>(i) / static_cast<float>(kSplineSamples);
								glm::vec3 curPos { 0.0f };
								glm::vec3 curTan { 0.0f, 0.0f, 1.0f };
								spline.SamplePoint(t, curPos, curTan);

								glm::vec3 side = glm::normalize(glm::cross(curTan, glm::vec3(0.0f, 1.0f, 0.0f)));
								if (glm::length(side) < 1e-4f)
									side = glm::vec3(1.0f, 0.0f, 0.0f);
								side *= halfRoadW;

								const glm::vec3 curLeft = curPos - side;
								const glm::vec3 curRight = curPos + side;

								if (i > 0)
								{
									PushOccludedWorldSegment(prevCenter, curPos, splineGold, 2.4f, static_cast<entt::entity>(splineEntity));

									if (sc.ExtrudeRoad)
									{
										PushOccludedWorldSegment(prevLeft, curLeft, roadEdgeColor, 1.4f, static_cast<entt::entity>(splineEntity));
										PushOccludedWorldSegment(prevRight, curRight, roadEdgeColor, 1.4f, static_cast<entt::entity>(splineEntity));

										if (i % 4 == 0)
										{
											PushOccludedWorldSegment(curLeft, curRight, roadRungColor, 1.0f, static_cast<entt::entity>(splineEntity));
										}
									}
								}

								prevCenter = curPos;
								prevLeft = curLeft;
								prevRight = curRight;
							}

							for (size_t k = 0; k < knotWorldPositions.size(); ++k)
							{
								const glm::vec3 kp = knotWorldPositions[k];
								const bool isKnotSelected = (static_cast<entt::entity>(knots[k]) == selectedHandle);
								const Wui::WuiColor knotColor = isKnotSelected
									? Wui::WuiColor { 1.0f, 1.0f, 1.0f, 0.95f }
									: Wui::WuiColor { 0.20f, 0.90f, 0.40f, 0.85f };

								const float kd = isKnotSelected ? 0.45f : 0.30f;
								const glm::vec3 dX0 = kp + glm::vec3(-kd, 0.0f, 0.0f);
								const glm::vec3 dX1 = kp + glm::vec3( kd, 0.0f, 0.0f);
								const glm::vec3 dZ0 = kp + glm::vec3(0.0f, 0.0f, -kd);
								const glm::vec3 dZ1 = kp + glm::vec3(0.0f, 0.0f,  kd);
								const glm::vec3 dY0 = kp + glm::vec3(0.0f, -kd, 0.0f);
								const glm::vec3 dY1 = kp + glm::vec3(0.0f,  kd, 0.0f);

								const float knotThick = isKnotSelected ? 2.2f : 1.5f;
								PushOccludedWorldSegment(dX0, dZ0, knotColor, knotThick, static_cast<entt::entity>(knots[k]));
								PushOccludedWorldSegment(dZ0, dX1, knotColor, knotThick, static_cast<entt::entity>(knots[k]));
								PushOccludedWorldSegment(dX1, dZ1, knotColor, knotThick, static_cast<entt::entity>(knots[k]));
								PushOccludedWorldSegment(dZ1, dX0, knotColor, knotThick, static_cast<entt::entity>(knots[k]));
								PushOccludedWorldSegment(dY0, dY1, knotColor, knotThick, static_cast<entt::entity>(knots[k]));

								const glm::vec4 clip = overlayCamera.ViewProjection * glm::vec4(kp, 1.0f);
								if (clip.w > 0.01f)
								{
									const float scrX = sceneRect.X + (clip.x / clip.w * 0.5f + 0.5f) * sceneRect.W;
									const float scrY = sceneRect.Y + (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * sceneRect.H;
									if (scrX >= sceneRect.X && scrX <= sceneRect.X + sceneRect.W &&
										scrY >= sceneRect.Y && scrY <= sceneRect.Y + sceneRect.H)
									{
										char kTag[32];
										std::snprintf(kTag, sizeof(kTag), "K%zu", k);
										Wui::Label(ctx, { scrX + 6.0f, scrY - 8.0f }, kTag,
											isKnotSelected ? theme.Warning : theme.Text, 11.0f);
									}
								}
							}
						}
					}

					
					// ---- 辅助遮挡圆环绘制 (顶点测试+二分边缘，极速60FPS且杜绝穿透) ----
					auto drawOccludedCircle = [&](const glm::vec3& center, float radius, int planeAxis,
						const Wui::WuiColor& col, float th, entt::entity ign)
					{
						constexpr int kSegments = 32;
						constexpr float kTwoPi = 6.28318530718f;
						glm::vec3 pts[kSegments + 1];
						bool vis[kSegments + 1];
						for (int i = 0; i <= kSegments; ++i)
						{
							const float angle = static_cast<float>(i % kSegments) * (kTwoPi / static_cast<float>(kSegments));
							const float ca = std::cos(angle) * radius;
							const float sa = std::sin(angle) * radius;
							glm::vec3 curPoint = center;
							if (planeAxis == 0) // YZ
								curPoint += glm::vec3(0.0f, ca, sa);
							else if (planeAxis == 1) // XZ (水平)
								curPoint += glm::vec3(ca, 0.0f, sa);
							else // XY
								curPoint += glm::vec3(ca, sa, 0.0f);
							pts[i] = curPoint;
							vis[i] = !isOccluded(curPoint, false, 0.0f, ign);
						}

						for (int i = 0; i < kSegments; ++i)
						{
							const bool v0 = vis[i];
							const bool v1 = vis[i + 1];
							if (v0 && v1)
							{
								PushProjectedSegment(ctx, overlayCamera.ViewProjection, sceneRect,
									overlayCamera.ViewProjection * glm::vec4(pts[i], 1.0f),
									overlayCamera.ViewProjection * glm::vec4(pts[i + 1], 1.0f),
									col, th);
							}
							else if (v0 && !v1)
							{
								glm::vec3 pVis = pts[i], pOcc = pts[i + 1];
								for (int b = 0; b < 2; ++b)
								{
									glm::vec3 pMid = (pVis + pOcc) * 0.5f;
									if (!isOccluded(pMid, false, 0.0f, ign)) pVis = pMid; else pOcc = pMid;
								}
								PushProjectedSegment(ctx, overlayCamera.ViewProjection, sceneRect,
									overlayCamera.ViewProjection * glm::vec4(pts[i], 1.0f),
									overlayCamera.ViewProjection * glm::vec4(pVis, 1.0f),
									col, th);
							}
							else if (!v0 && v1)
							{
								glm::vec3 pOcc = pts[i], pVis = pts[i + 1];
								for (int b = 0; b < 2; ++b)
								{
									glm::vec3 pMid = (pVis + pOcc) * 0.5f;
									if (!isOccluded(pMid, false, 0.0f, ign)) pVis = pMid; else pOcc = pMid;
								}
								PushProjectedSegment(ctx, overlayCamera.ViewProjection, sceneRect,
									overlayCamera.ViewProjection * glm::vec4(pVis, 1.0f),
									overlayCamera.ViewProjection * glm::vec4(pts[i + 1], 1.0f),
									col, th);
							}
						}
					};

					// ---- 1. PCG 生态群落程序化散布视口可视化 (BiomePCGComponent) ----
					if (selected.IsValid() && selected.HasComponent<BiomePCGComponent>() &&
						selected.HasComponent<TransformComponent>())
					{
						const auto& pcg = selected.GetComponent<BiomePCGComponent>();
						const auto& xf = selected.GetComponent<TransformComponent>();
						const glm::vec3 entCenter = xf.Location;

						// 空间变换尺度一致性：半径与体积高度严格遵循 Transform.Scale 与 BoundsExtents
						const float scaleFactor = std::max({ std::abs(xf.Scale.x), std::abs(xf.Scale.z), 0.1f });
						const float pcgRadius = std::max(2.0f, pcg.BoundsRadius * scaleFactor);
						const float heightScale = std::max(std::abs(xf.Scale.y), 0.1f);
						const float halfH = std::max(2.0f, pcg.BoundsHeight * heightScale);

						// 1. 绘制带深度遮挡剔除的 3D 生成空间体积线框（双圆环 + 4 立柱）
						const Wui::WuiColor pcgGold { 0.95f, 0.80f, 0.20f, 0.90f };
						const Wui::WuiColor pcgDimGold { 0.95f, 0.80f, 0.20f, 0.40f };

						// 底部圆环与顶部圆环 (执行逐段深度遮挡)
						drawOccludedCircle(glm::vec3(entCenter.x, entCenter.y - halfH, entCenter.z), pcgRadius, 1, pcgGold, 2.0f, selectedHandle);
						drawOccludedCircle(glm::vec3(entCenter.x, entCenter.y + halfH, entCenter.z), pcgRadius, 1, pcgGold, 2.0f, selectedHandle);
						drawOccludedCircle(entCenter, pcgRadius * 0.5f, 1, pcgDimGold, 1.2f, selectedHandle);

						// 4 根垂直连接柱 (深度遮挡)
						for (int colIdx = 0; colIdx < 4; ++colIdx)
						{
							const float angle = static_cast<float>(colIdx) * 1.57079632679f;
							const float px = entCenter.x + std::cos(angle) * pcgRadius;
							const float pz = entCenter.z + std::sin(angle) * pcgRadius;
							PushOccludedWorldSegment(glm::vec3(px, entCenter.y - halfH, pz),
								glm::vec3(px, entCenter.y + halfH, pz), pcgGold, 1.8f, selectedHandle);
						}

						// 2. 在地表绘制纯净空间点云 (依据图层 ColorTag 渲染，零写死假生态)
						const bool hasSharedPoints = PCGBiomeGenerator::IsSharedPreviewActive() && !PCGBiomeGenerator::GetSharedPreviewPoints().empty();
						const auto& previewPoints = hasSharedPoints ? PCGBiomeGenerator::GetSharedPreviewPoints() : g_PCGPreviewState.Points;
						const bool previewActive = hasSharedPoints || (g_PCGPreviewState.Active && g_PCGPreviewState.TargetEntity == selectedHandle && !g_PCGPreviewState.Points.empty());

						if (previewActive && !previewPoints.empty())
						{
							const size_t totalPts = previewPoints.size();
							const size_t maxVisible = 600;
							const size_t stride = std::max<size_t>(1, totalPts / maxVisible);
							const float crossSz = 0.25f * scaleFactor;
							for (size_t pIdx = 0; pIdx < totalPts; pIdx += stride)
							{
								const auto& pt = previewPoints[pIdx];
								if (glm::distance(cameraPos, pt.Position) > 300.0f)
									continue;

								const glm::vec4 clip = overlayCamera.ViewProjection * glm::vec4(pt.Position, 1.0f);
								if (clip.w <= 0.01f || std::abs(clip.x) > clip.w * 1.15f || std::abs(clip.y) > clip.w * 1.15f)
									continue;

								if (isOccluded(pt.Position, true, 0.04f, selectedHandle))
									continue;

								const Wui::WuiColor col { pt.ColorTag.r, pt.ColorTag.g, pt.ColorTag.b, pt.ColorTag.a };
								const glm::vec4 cX0 = overlayCamera.ViewProjection * glm::vec4(pt.Position - glm::vec3(crossSz, 0.0f, 0.0f), 1.0f);
								const glm::vec4 cX1 = overlayCamera.ViewProjection * glm::vec4(pt.Position + glm::vec3(crossSz, 0.0f, 0.0f), 1.0f);
								const glm::vec4 cZ0 = overlayCamera.ViewProjection * glm::vec4(pt.Position - glm::vec3(0.0f, 0.0f, crossSz), 1.0f);
								const glm::vec4 cZ1 = overlayCamera.ViewProjection * glm::vec4(pt.Position + glm::vec3(0.0f, 0.0f, crossSz), 1.0f);
								PushProjectedSegment(ctx, overlayCamera.ViewProjection, sceneRect, cX0, cX1, col, 1.6f);
								PushProjectedSegment(ctx, overlayCamera.ViewProjection, sceneRect, cZ0, cZ1, col, 1.6f);
							}
						}
						else
						{
							// 动态快速采样预览网格 (10x10)，保持缩放一致性
							constexpr int kGrid = 10;
							const float kStep = std::max(2.0f, (pcgRadius * 2.0f) / static_cast<float>(kGrid));
							const float cs = 0.25f * scaleFactor;
							const Wui::WuiColor col { 0.95f, 0.85f, 0.35f, 0.80f };
							for (int z = -kGrid / 2; z <= kGrid / 2; ++z)
							{
								for (int x = -kGrid / 2; x <= kGrid / 2; ++x)
								{
									glm::vec3 samplePos = entCenter + glm::vec3(static_cast<float>(x) * kStep, 0.0f, static_cast<float>(z) * kStep);
									if (glm::distance(samplePos, entCenter) > pcgRadius) continue;
									float gH = entCenter.y;
									if (sampleTerrainWorldHeight(samplePos.x, samplePos.z, gH))
										samplePos.y = gH + 0.05f;

									if (isOccluded(samplePos, true, 0.04f, selectedHandle))
										continue;

									const glm::vec4 cX0 = overlayCamera.ViewProjection * glm::vec4(samplePos - glm::vec3(cs, 0.0f, 0.0f), 1.0f);
									const glm::vec4 cX1 = overlayCamera.ViewProjection * glm::vec4(samplePos + glm::vec3(cs, 0.0f, 0.0f), 1.0f);
									const glm::vec4 cZ0 = overlayCamera.ViewProjection * glm::vec4(samplePos - glm::vec3(0.0f, 0.0f, cs), 1.0f);
									const glm::vec4 cZ1 = overlayCamera.ViewProjection * glm::vec4(samplePos + glm::vec3(0.0f, 0.0f, cs), 1.0f);
									PushProjectedSegment(ctx, overlayCamera.ViewProjection, sceneRect, cX0, cX1, col, 1.4f);
									PushProjectedSegment(ctx, overlayCamera.ViewProjection, sceneRect, cZ0, cZ1, col, 1.4f);
								}
							}
						}

						// 3. 若开启样条道路自动平整，绘制道路双侧过渡放坡边界线
						if (pcg.AutoDeformTerrain != 0)
						{
							const Wui::WuiColor roadSlopeCol { 0.90f, 0.75f, 0.30f, 0.80f };
							for (auto [sh, splineComp] : registry.view<SplineComponent>().each())
							{
								Spline3D spline;
								std::vector<glm::vec3> knots;
								const auto& children = Hierarchy::ChildrenOf(registry, sh);
								for (entt::entity ch : children)
								{
									if (const auto* kxf = registry.try_get<TransformComponent>(ch))
										knots.push_back(kxf->Location);
								}
								if (knots.size() >= 2)
								{
									for (const auto& k : knots)
										spline.AddPoint(k);
									spline.SetClosedLoop(splineComp.ClosedLoop != 0);
									spline.UpdateTangents();
									const float totalLen = spline.GetTotalLength();
									if (totalLen > 1.0f)
									{
										const float halfW = splineComp.RoadWidth * 0.5f;
										const float slopeMargin = 3.0f; // 过渡放坡宽度
										const float step = 2.0f; // 2.0m 采样步长保证极速且平滑
										const int numSamples = static_cast<int>(totalLen / step);
										glm::vec3 prevLeftSlope(0.0f), prevRightSlope(0.0f);
										bool prevLeftVis = false, prevRightVis = false;
										for (int i = 0; i <= numSamples; ++i)
										{
											const float d = std::min(static_cast<float>(i) * step, totalLen);
											glm::vec3 pos, tan;
											spline.SampleAtDistance(d, pos, tan);
											const glm::vec3 side = glm::normalize(glm::cross(tan, glm::vec3(0, 1, 0)));
											const glm::vec3 curLeftSlope = pos - side * (halfW + slopeMargin);
											const glm::vec3 curRightSlope = pos + side * (halfW + slopeMargin);
											const bool curLeftVis = !isOccluded(curLeftSlope, true, 0.05f, selectedHandle);
											const bool curRightVis = !isOccluded(curRightSlope, true, 0.05f, selectedHandle);

											if (i > 0)
											{
												if (prevLeftVis && curLeftVis)
													PushProjectedSegment(ctx, overlayCamera.ViewProjection, sceneRect,
														overlayCamera.ViewProjection * glm::vec4(prevLeftSlope, 1.0f),
														overlayCamera.ViewProjection * glm::vec4(curLeftSlope, 1.0f),
														roadSlopeCol, 1.5f);
												if (prevRightVis && curRightVis)
													PushProjectedSegment(ctx, overlayCamera.ViewProjection, sceneRect,
														overlayCamera.ViewProjection * glm::vec4(prevRightSlope, 1.0f),
														overlayCamera.ViewProjection * glm::vec4(curRightSlope, 1.0f),
														roadSlopeCol, 1.5f);
											}
											prevLeftSlope = curLeftSlope;
											prevRightSlope = curRightSlope;
											prevLeftVis = curLeftVis;
											prevRightVis = curRightVis;
										}
									}
								}
							}
						}
					}


					// ---- 2. EQS 战术掩体查询可视化 (BehaviorTree / 全局预览点云) ----
					const bool hasBtPreview = g_EQSPreviewState.Evaluated && !g_EQSPreviewState.Candidates.empty();

					if (hasBtPreview)
					{
						const entt::entity targetHandle = g_EQSPreviewState.TargetEntity;
						glm::vec3 querierPos = g_EQSPreviewState.QuerierPos;
						float queryR = std::max(2.0f, g_EQSPreviewState.QueryRadius);
						uint32_t threatHint = g_EQSPreviewState.ThreatEntityHint;
						uint8_t bestCoverFound = g_EQSPreviewState.Evaluated ? 1 : 0;

						// 1. 绘制查询范围圆环（青色 0.2f, 0.8f, 0.95f, 0.85f）
						const Wui::WuiColor eqsCyan { 0.20f, 0.80f, 0.95f, 0.85f };
						drawOccludedCircle(querierPos, queryR, 1, eqsCyan, 2.0f, targetHandle);
						drawOccludedCircle(querierPos, queryR * 0.5f, 1, Wui::WuiColor{ 0.20f, 0.80f, 0.95f, 0.40f }, 1.2f, targetHandle);

						// 2. 威胁源位置标记（红色十字球）与射击视线射线
						const glm::vec3 threatPos = g_EQSPreviewState.ThreatPos;
						const Wui::WuiColor threatColor { 0.95f, 0.15f, 0.15f, 0.95f };
						const float tCross = 0.5f;
						PushOccludedWorldSegment(threatPos - glm::vec3(tCross, 0, 0), threatPos + glm::vec3(tCross, 0, 0), threatColor, 2.2f, targetHandle);
						PushOccludedWorldSegment(threatPos - glm::vec3(0, tCross, 0), threatPos + glm::vec3(0, tCross, 0), threatColor, 2.2f, targetHandle);
						PushOccludedWorldSegment(threatPos - glm::vec3(0, 0, tCross), threatPos + glm::vec3(0, 0, tCross), threatColor, 2.2f, targetHandle);
						drawOccludedCircle(threatPos, 0.35f, 1, threatColor, 1.8f, targetHandle);
						// 射击视线射线 (Threat -> Querier)
						PushOccludedWorldSegment(threatPos, querierPos, Wui::WuiColor{ 0.95f, 0.25f, 0.15f, 0.65f }, 1.4f, targetHandle);

						// 3. 绘制三维多维热力候选网格点阵 (深红(淘汰/暴露) -> 橙黄(及格) -> 荧光翠绿(最优))
						if (!g_EQSPreviewState.Candidates.empty())
						{
							float minScore = 1e9f;
							float maxScore = -1e9f;
							for (const auto& cand : g_EQSPreviewState.Candidates)
							{
								if (!cand.FilteredOut)
								{
									minScore = std::min(minScore, cand.TotalScore);
									maxScore = std::max(maxScore, cand.TotalScore);
								}
							}
							if (maxScore <= minScore)
							{
								minScore = 0.0f;
								maxScore = 1.0f;
							}

							for (const auto& cand : g_EQSPreviewState.Candidates)
							{
								if (cand.FilteredOut)
								{
									// 硬过滤淘汰点：深暗红微缩十字标记
									const Wui::WuiColor cCol { 0.55f, 0.06f, 0.08f, 0.45f };
									const float cs = 0.10f;
									PushOccludedWorldSegment(cand.Position - glm::vec3(cs, 0, 0), cand.Position + glm::vec3(cs, 0, 0), cCol, 1.0f, targetHandle);
									PushOccludedWorldSegment(cand.Position - glm::vec3(0, 0, cs), cand.Position + glm::vec3(0, 0, cs), cCol, 1.0f, targetHandle);
									continue;
								}

								// 根据 TotalScore 计算平滑多维热力渐变颜色:
								// 深红(淘汰/暴露) -> 橙黄(及格) -> 荧光翠绿(最优)
								const float t = std::clamp((cand.TotalScore - minScore) / (maxScore - minScore), 0.0f, 1.0f);
								Wui::WuiColor cCol;
								if (t < 0.5f)
								{
									const float u = t / 0.5f;
									cCol.R = glm::mix(0.85f, 0.98f, u);
									cCol.G = glm::mix(0.10f, 0.72f, u);
									cCol.B = glm::mix(0.15f, 0.10f, u);
									cCol.A = glm::mix(0.75f, 0.90f, u);
								}
								else
								{
									const float u = (t - 0.5f) / 0.5f;
									cCol.R = glm::mix(0.98f, 0.10f, u);
									cCol.G = glm::mix(0.72f, 0.98f, u);
									cCol.B = glm::mix(0.10f, 0.35f, u);
									cCol.A = glm::mix(0.90f, 0.98f, u);
								}

								const float cs = 0.16f + t * 0.12f;
								const float thickness = 1.5f + t * 1.0f;
								PushOccludedWorldSegment(cand.Position - glm::vec3(cs, 0, 0), cand.Position + glm::vec3(cs, 0, 0), cCol, thickness, targetHandle);
								PushOccludedWorldSegment(cand.Position - glm::vec3(0, 0, cs), cand.Position + glm::vec3(0, 0, cs), cCol, thickness, targetHandle);

								// 优秀高分点位增设纵向战术光标
								if (t > 0.85f)
								{
									PushOccludedWorldSegment(cand.Position, cand.Position + glm::vec3(0.0f, 0.40f, 0.0f), cCol, thickness, targetHandle);
								}
							}
						}


						// 4. 绘制最优战术站位：金黄战术菱形 Marker（0.98f, 0.85f, 0.15f, 1.0f）与垂直地面引导光柱
						if (bestCoverFound != 0 || g_EQSPreviewState.Evaluated)
						{
							const glm::vec3 bestCover = g_EQSPreviewState.Evaluated
								? g_EQSPreviewState.BestCoverPos : (querierPos + glm::vec3(5.0f, 0.0f, 5.0f));
							const Wui::WuiColor goldMarker { 0.98f, 0.85f, 0.15f, 1.0f };

							// 垂直引导光柱 (3.8m 高度)
							PushOccludedWorldSegment(bestCover, bestCover + glm::vec3(0.0f, 3.8f, 0.0f), goldMarker, 2.8f, targetHandle);

							// 地面同心战术目标圆环
							drawOccludedCircle(bestCover, 0.50f, 1, goldMarker, 2.2f, targetHandle);
							drawOccludedCircle(bestCover, 1.00f, 1, Wui::WuiColor{ 0.98f, 0.85f, 0.15f, 0.45f }, 1.5f, targetHandle);

							// 金黄战术菱形 (八面体 Diamond Marker, 悬浮在 1.6m 战术瞄准线高度)
							const float dSz = 0.45f;
							const float dH = 1.6f;
							const glm::vec3 pN = bestCover + glm::vec3(0, dH, dSz);
							const glm::vec3 pS = bestCover + glm::vec3(0, dH, -dSz);
							const glm::vec3 pE = bestCover + glm::vec3(dSz, dH, 0);
							const glm::vec3 pW = bestCover + glm::vec3(-dSz, dH, 0);
							const glm::vec3 pTop = bestCover + glm::vec3(0, dH + dSz, 0);
							const glm::vec3 pBot = bestCover + glm::vec3(0, dH - dSz, 0);

							// 水平腰线
							PushOccludedWorldSegment(pN, pE, goldMarker, 2.2f, targetHandle);
							PushOccludedWorldSegment(pE, pS, goldMarker, 2.2f, targetHandle);
							PushOccludedWorldSegment(pS, pW, goldMarker, 2.2f, targetHandle);
							PushOccludedWorldSegment(pW, pN, goldMarker, 2.2f, targetHandle);

							// 上锥体棱线
							PushOccludedWorldSegment(pN, pTop, goldMarker, 2.0f, targetHandle);
							PushOccludedWorldSegment(pE, pTop, goldMarker, 2.0f, targetHandle);
							PushOccludedWorldSegment(pS, pTop, goldMarker, 2.0f, targetHandle);
							PushOccludedWorldSegment(pW, pTop, goldMarker, 2.0f, targetHandle);

							// 下锥体棱线
							PushOccludedWorldSegment(pN, pBot, goldMarker, 2.0f, targetHandle);
							PushOccludedWorldSegment(pE, pBot, goldMarker, 2.0f, targetHandle);
							PushOccludedWorldSegment(pS, pBot, goldMarker, 2.0f, targetHandle);
							PushOccludedWorldSegment(pW, pBot, goldMarker, 2.0f, targetHandle);
						}
					}


					// ---- 3. 植被物理推挤 Gizmo (FoliageInteractionComponent) ----
					if (selected.IsValid() && selected.HasComponent<FoliageInteractionComponent>() &&
						selected.HasComponent<TransformComponent>())
					{
						const auto& fol = selected.GetComponent<FoliageInteractionComponent>();
						const auto& xf = selected.GetComponent<TransformComponent>();
						const glm::vec3 entCenter = xf.Location;

						// 1. 绘制推挤半径球体线框（赤道水平圆 + 两条垂直经线圆，水蓝色 0.2f, 0.9f, 0.85f, 0.9f）
						const Wui::WuiColor aquaColor { 0.20f, 0.90f, 0.85f, 0.90f };
						const float radius = std::max(0.2f, fol.PushRadius);
						drawOccludedCircle(entCenter, radius, 1, aquaColor, 2.0f, selectedHandle); // 赤道水平圆 (XZ)
						drawOccludedCircle(entCenter, radius, 0, Wui::WuiColor{ 0.20f, 0.90f, 0.85f, 0.65f }, 1.5f, selectedHandle); // 经线圆 (YZ)
						drawOccludedCircle(entCenter, radius, 2, Wui::WuiColor{ 0.20f, 0.90f, 0.85f, 0.65f }, 1.5f, selectedHandle); // 经线圆 (XY)

						// 2. 绘制推挤倒伏向量箭头
						glm::vec3 disp(fol.PushStrength, 0.0f, fol.PushStrength * 0.5f);
						if (g_FoliagePreviewState.TargetEntity == selectedHandle && g_FoliagePreviewState.Active)
							disp = g_FoliagePreviewState.Displacement;
						if (glm::length(disp) < 0.01f)
							disp = glm::vec3(fol.PushStrength, 0.0f, 0.0f);

						const glm::vec3 arrowStart = entCenter;
						const glm::vec3 arrowEnd = entCenter + glm::normalize(disp) * std::min(radius * 1.2f, glm::length(disp) * 1.5f);
						const Wui::WuiColor arrowCol { 0.15f, 0.95f, 0.50f, 0.95f };
						PushOccludedWorldSegment(arrowStart, arrowEnd, arrowCol, 2.6f, selectedHandle);

						const glm::vec3 arrowDir = glm::normalize(arrowEnd - arrowStart);
						const glm::vec3 arrowSide = glm::cross(arrowDir, glm::vec3(0, 1, 0)) * 0.25f;
						PushOccludedWorldSegment(arrowEnd, arrowEnd - arrowDir * 0.35f + arrowSide, arrowCol, 2.2f, selectedHandle);
						PushOccludedWorldSegment(arrowEnd, arrowEnd - arrowDir * 0.35f - arrowSide, arrowCol, 2.2f, selectedHandle);

						// 3. 地表接触圆形印记 (LeaveTracks)
						if (fol.LeaveTracks != 0)
						{
							const Wui::WuiColor trackCol { 0.10f, 0.80f, 0.60f, 0.80f };
							const glm::vec3 groundCenter { entCenter.x, entCenter.y - 0.05f, entCenter.z };
							drawOccludedCircle(groundCenter, radius * 0.85f, 1, trackCol, 1.8f, selectedHandle);
							const float tRad = radius * 0.85f;
							PushOccludedWorldSegment(groundCenter - glm::vec3(tRad, 0, 0), groundCenter + glm::vec3(tRad, 0, 0), trackCol, 1.2f, selectedHandle);
							PushOccludedWorldSegment(groundCenter - glm::vec3(0, 0, tRad), groundCenter + glm::vec3(0, 0, tRad), trackCol, 1.2f, selectedHandle);
						}
					}

					// ---- 物理画笔视口贴地光圈与 HUD (PhysicsPainterPanel) ----
					if (isPainterMode && PhysicsPainterPanel::IsActive() && !play && (focused || hovered))
					{
						const glm::vec2 mousePos = ctx.Input().MousePos;
						if (mousePos.x >= sceneRect.X && mousePos.x <= sceneRect.X + sceneRect.W &&
							mousePos.y >= sceneRect.Y && mousePos.y <= sceneRect.Y + sceneRect.H &&
							!m_Host.IsFlyNavigating())
						{
							const float ndcX = ((mousePos.x - sceneRect.X) / sceneRect.W) * 2.0f - 1.0f;
							const float ndcY = 1.0f - ((mousePos.y - sceneRect.Y) / sceneRect.H) * 2.0f;
							const glm::mat4 invVP = glm::inverse(overlayCamera.ViewProjection);
							glm::vec4 nearPointClip(ndcX, ndcY, -1.0f, 1.0f);
							glm::vec4 farPointClip(ndcX, ndcY, 1.0f, 1.0f);
							glm::vec4 nearPointWorld = invVP * nearPointClip;
							glm::vec4 farPointWorld = invVP * farPointClip;
							if (std::abs(nearPointWorld.w) > 1e-6f) nearPointWorld /= nearPointWorld.w;
							if (std::abs(farPointWorld.w) > 1e-6f) farPointWorld /= farPointWorld.w;
							glm::vec3 rayOrigin = overlayCamera.Position;
							glm::vec3 rayDir = glm::normalize(glm::vec3(farPointWorld) - rayOrigin);

							glm::vec3 hitPoint { 0.0f };
							glm::vec3 hitNormal { 0.0f, 1.0f, 0.0f };
							bool hasHit = false;

							glm::mat4 tWorld { 1.0f };
							for (auto [h, tc, xf] : registry.view<TerrainComponent, TransformComponent>().each())
							{
								tWorld = xf.GetLocalMatrix();
								break;
							}
							if (terrainData)
							{
								const glm::mat4 invTW = glm::inverse(tWorld);
								const glm::vec3 localOrigin = glm::vec3(invTW * glm::vec4(rayOrigin, 1.0f));
								const glm::vec3 localDir = glm::normalize(glm::vec3(invTW * glm::vec4(rayDir, 0.0f)));
								TerrainRaycastHit thit;
								if (terrainData->Raycast(localOrigin, localDir, 2000.0f, thit))
								{
									hitPoint = glm::vec3(tWorld * glm::vec4(thit.Point, 1.0f));
									hitNormal = glm::normalize(glm::vec3(tWorld * glm::vec4(thit.Normal, 0.0f)));
									hasHit = true;
								}
							}
							if (!hasHit && std::abs(rayDir.y) > 1e-4f)
							{
								const float t = -rayOrigin.y / rayDir.y;
								if (t > 0.0f && t < 2000.0f)
								{
									hitPoint = rayOrigin + rayDir * t;
									hitNormal = glm::vec3(0.0f, 1.0f, 0.0f);
									hasHit = true;
								}
							}

							if (hasHit)
							{
								const float pRadius = PhysicsPainterPanel::GetBrushRadius();
								const Wui::WuiColor brushColor { 1.0f, 0.55f, 0.10f, 0.90f };

								constexpr int kBrushSegs = 32;
								for (int s = 0; s < kBrushSegs; ++s)
								{
									const float a0 = static_cast<float>(s) / static_cast<float>(kBrushSegs) * 6.2831853f;
									const float a1 = static_cast<float>(s + 1) / static_cast<float>(kBrushSegs) * 6.2831853f;
									const glm::vec3 p0 = hitPoint + glm::vec3(pRadius * std::cos(a0), 0.05f, pRadius * std::sin(a0));
									const glm::vec3 p1 = hitPoint + glm::vec3(pRadius * std::cos(a1), 0.05f, pRadius * std::sin(a1));
									PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f),
										p0, p1, brushColor, 2.2f, &overlayMin, &overlayMax);
								}

								char hudBuf[128];
								std::snprintf(hudBuf, sizeof(hudBuf), "Physics Painter: R: %.1fm | 密度: %d | [点击视口散布]",
									pRadius, PhysicsPainterPanel::GetBrushDensity());
								const Wui::WuiRect hudRect { sceneRect.X + 16.0f, sceneRect.Y + sceneRect.H - 42.0f, 320.0f, 26.0f };
								Wui::PanelBackground(ctx, hudRect, Wui::WuiColor { 0.12f, 0.14f, 0.18f, 0.92f }, 4.0f);
								Wui::HighlightOutline(ctx, hudRect, brushColor, 4.0f, 1.0f);
								Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 6.0f }, hudBuf, brushColor, 12.0f);
							}
						}
					}
					// 空间八叉树包围盒线框 (Octree Bounds)
					if (s_ShowOctreeBounds && overlayScene)
					{
						std::vector<std::pair<glm::vec3, glm::vec3>> debugBoxes;
						overlayScene->GetOctree().GetDebugBoxes(debugBoxes);
						const Wui::WuiColor octreeBoxColor { 0.25f, 0.75f, 0.95f, 0.40f };
						const float thickness = 1.0f;
						for (const auto& [bMin, bMax] : debugBoxes)
						{
							const glm::vec3 c000(bMin.x, bMin.y, bMin.z);
							const glm::vec3 c100(bMax.x, bMin.y, bMin.z);
							const glm::vec3 c110(bMax.x, bMax.y, bMin.z);
							const glm::vec3 c010(bMin.x, bMax.y, bMin.z);

							const glm::vec3 c001(bMin.x, bMin.y, bMax.z);
							const glm::vec3 c101(bMax.x, bMin.y, bMax.z);
							const glm::vec3 c111(bMax.x, bMax.y, bMax.z);
							const glm::vec3 c011(bMin.x, bMax.y, bMax.z);

							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f), c000, c100, octreeBoxColor, thickness, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f), c100, c110, octreeBoxColor, thickness, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f), c110, c010, octreeBoxColor, thickness, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f), c010, c000, octreeBoxColor, thickness, &overlayMin, &overlayMax);

							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f), c001, c101, octreeBoxColor, thickness, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f), c101, c111, octreeBoxColor, thickness, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f), c111, c011, octreeBoxColor, thickness, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f), c011, c001, octreeBoxColor, thickness, &overlayMin, &overlayMax);

							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f), c000, c001, octreeBoxColor, thickness, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f), c100, c101, octreeBoxColor, thickness, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f), c110, c111, octreeBoxColor, thickness, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, glm::mat4(1.0f), c010, c011, octreeBoxColor, thickness, &overlayMin, &overlayMax);
						}
					}

				// 取证(与相机视锥同口径):脚本可断言"画了几个、覆盖到哪",不靠像素。
				if ((lightCount > 0 || colliderCount > 0 || audioCount > 0) && std::getenv("WLD_TRACE_UI"))
				{
					static uint64_t lastOverlayLog = ~0ull;
					const uint64_t stamp = ctx.Frame() / 120;
					if (stamp != lastOverlayLog)
					{
						lastOverlayLog = stamp;
						WLD_CORE_INFO("[ui] overlays lights={0} colliders={1} audio={2} allLights={3} allColliders={4} allAudio={5} "
							"bbox=({6},{7},{8},{9})",
							lightCount, colliderCount, audioCount, showAllLights ? 1 : 0, showAllColliders ? 1 : 0, showAllAudio ? 1 : 0,
							overlayMin.x, overlayMin.y, overlayMax.x - overlayMin.x, overlayMax.y - overlayMin.y);
					}
				}
			}
		}

		// ---- 双轨制视口辅助系统: 轨道一 (地面参考标尺 1m/10m) + 轨道二 (世界分区宏观沙盘 64m) ----
		const Wui::GizmoCamera levelCam = m_Host.GetGizmoCamera();
		const glm::vec3 camPos = levelCam.Position;
		// 复用前面已获取的 currentVp, currentProj, isOrtho

		// ==================== 轨道一：场景地面参考网格 (Reference Grid) ====================
		// 1. Free 自由漫游、Top 顶视、Bottom 底视下：地面位于 XZ 平面，由 SceneRenderer 原生 GPU 深度通道渲染，保证 100% 铺满视口、绝无半截黑屏截断；
		// 2. 纯立面视角 (Front/Back/Right/Left) 下：视线正对立面，由 WUI 在 XY 或 ZY 竖直平面绘制铺满视口的参考坐标图纸。
		const bool showGrid = m_Host.IsReferenceGridEnabled();
		const bool isElevationView = (currentVp == World::ViewportViewpoint::Front || currentVp == World::ViewportViewpoint::Back ||
			currentVp == World::ViewportViewpoint::Right || currentVp == World::ViewportViewpoint::Left);
		if (!play && showGrid && isOrtho && isElevationView)
		{
			Wui::ClipScope gridClip(ctx, sceneRect);
			const float orthoSpan = std::max(0.1f, levelCam.OrthographicSize);

			const float worldPerPixel = orthoSpan / std::max(1.0f, sceneRect.H);
			const float targetStep = worldPerPixel * 75.0f;
			const float exponent = std::floor(std::log10(std::max(1e-4f, targetStep)));
			const float baseStep = std::pow(10.0f, exponent);
			const float fracStep = targetStep / baseStep;
			const float cellSize = (fracStep >= 5.0f) ? (baseStep * 5.0f) : ((fracStep >= 2.0f) ? (baseStep * 2.0f) : baseStep);

			const glm::mat4 invVP = glm::inverse(levelCam.ViewProjection);
			const glm::vec4 ndcCorners[4] = {
				{ -1.0f, -1.0f, 0.0f, 1.0f },
				{  1.0f, -1.0f, 0.0f, 1.0f },
				{  1.0f,  1.0f, 0.0f, 1.0f },
				{ -1.0f,  1.0f, 0.0f, 1.0f }
			};
			glm::vec3 wc[4];
			for (int i = 0; i < 4; ++i)
			{
				const glm::vec4 p = invVP * ndcCorners[i];
				wc[i] = glm::vec3(p) / (std::abs(p.w) > 1e-6f ? p.w : 1.0f);
			}

			const Wui::WuiColor minorColor { theme.Border.R, theme.Border.G, theme.Border.B, 0.22f };
			const Wui::WuiColor majorColor { theme.BorderStrong.R, theme.BorderStrong.G, theme.BorderStrong.B, 0.40f };
			const Wui::WuiColor xAxisColor { theme.AxisX.R, theme.AxisX.G, theme.AxisX.B, 0.55f };
			const Wui::WuiColor yAxisColor { theme.AxisY.R, theme.AxisY.G, theme.AxisY.B, 0.55f };
			const Wui::WuiColor zAxisColor { theme.AxisZ.R, theme.AxisZ.G, theme.AxisZ.B, 0.55f };

			if (currentVp == World::ViewportViewpoint::Front || currentVp == World::ViewportViewpoint::Back)
			{
				// XY 立面参考网格: 水平 X, 垂直 Y
				const float rawMinX = std::min({ wc[0].x, wc[1].x, wc[2].x, wc[3].x });
				const float rawMaxX = std::max({ wc[0].x, wc[1].x, wc[2].x, wc[3].x });
				const float rawMinY = std::min({ wc[0].y, wc[1].y, wc[2].y, wc[3].y });
				const float rawMaxY = std::max({ wc[0].y, wc[1].y, wc[2].y, wc[3].y });

				const float minX = std::floor((rawMinX - cellSize * 2.0f) / cellSize) * cellSize;
				const float maxX = std::ceil((rawMaxX + cellSize * 2.0f) / cellSize) * cellSize;
				const float minY = std::floor((rawMinY - cellSize * 2.0f) / cellSize) * cellSize;
				const float maxY = std::ceil((rawMaxY + cellSize * 2.0f) / cellSize) * cellSize;

				for (float x = minX; x <= maxX + 0.01f; x += cellSize)
				{
					const bool isOrigin = std::abs(x) < (cellSize * 0.25f);
					const bool isMajor = std::fmod(std::abs(x), cellSize * 5.0f) < (cellSize * 0.25f);
					PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
						levelCam.ViewProjection * glm::vec4(x, minY, 0.0f, 1.0f),
						levelCam.ViewProjection * glm::vec4(x, maxY, 0.0f, 1.0f),
						isOrigin ? yAxisColor : (isMajor ? majorColor : minorColor), isOrigin ? 1.8f : 1.0f);
				}
				for (float y = minY; y <= maxY + 0.01f; y += cellSize)
				{
					const bool isOrigin = std::abs(y) < (cellSize * 0.25f);
					const bool isMajor = std::fmod(std::abs(y), cellSize * 5.0f) < (cellSize * 0.25f);
					PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
						levelCam.ViewProjection * glm::vec4(minX, y, 0.0f, 1.0f),
						levelCam.ViewProjection * glm::vec4(maxX, y, 0.0f, 1.0f),
						isOrigin ? xAxisColor : (isMajor ? majorColor : minorColor), isOrigin ? 1.8f : 1.0f);
				}
			}
			else if (currentVp == World::ViewportViewpoint::Right || currentVp == World::ViewportViewpoint::Left)
			{
				// ZY 立面参考网格: 水平 Z, 垂直 Y
				const float rawMinZ = std::min({ wc[0].z, wc[1].z, wc[2].z, wc[3].z });
				const float rawMaxZ = std::max({ wc[0].z, wc[1].z, wc[2].z, wc[3].z });
				const float rawMinY = std::min({ wc[0].y, wc[1].y, wc[2].y, wc[3].y });
				const float rawMaxY = std::max({ wc[0].y, wc[1].y, wc[2].y, wc[3].y });

				const float minZ = std::floor((rawMinZ - cellSize * 2.0f) / cellSize) * cellSize;
				const float maxZ = std::ceil((rawMaxZ + cellSize * 2.0f) / cellSize) * cellSize;
				const float minY = std::floor((rawMinY - cellSize * 2.0f) / cellSize) * cellSize;
				const float maxY = std::ceil((rawMaxY + cellSize * 2.0f) / cellSize) * cellSize;

				for (float z = minZ; z <= maxZ + 0.01f; z += cellSize)
				{
					const bool isOrigin = std::abs(z) < (cellSize * 0.25f);
					const bool isMajor = std::fmod(std::abs(z), cellSize * 5.0f) < (cellSize * 0.25f);
					PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
						levelCam.ViewProjection * glm::vec4(0.0f, minY, z, 1.0f),
						levelCam.ViewProjection * glm::vec4(0.0f, maxY, z, 1.0f),
						isOrigin ? yAxisColor : (isMajor ? majorColor : minorColor), isOrigin ? 1.8f : 1.0f);
				}
				for (float y = minY; y <= maxY + 0.01f; y += cellSize)
				{
					const bool isOrigin = std::abs(y) < (cellSize * 0.25f);
					const bool isMajor = std::fmod(std::abs(y), cellSize * 5.0f) < (cellSize * 0.25f);
					PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
						levelCam.ViewProjection * glm::vec4(0.0f, y, minZ, 1.0f),
						levelCam.ViewProjection * glm::vec4(0.0f, y, maxZ, 1.0f),
						isOrigin ? zAxisColor : (isMajor ? majorColor : minorColor), isOrigin ? 1.8f : 1.0f);
				}
			}
		}

		// 视口右上角 HUD 基础数据声明
		size_t entityCount = 0;
		if (const Ref<Scene> gridScene = m_Host.GetActiveScene())
		{
			const entt::registry& gridRegistry = std::as_const(*gridScene).GetRegistry();
			entityCount = gridRegistry.view<UUIDComponent>().size();
		}
		char hudLine1[160] = {};
		char hudLine2[160] = {};

		// ==================== 轨道二：世界分区系统 (World Partition / Streaming Grid) ====================
		int32_t activeChunkX = 0;
		int32_t activeChunkY = 0;
		int32_t activeChunkZ = 0;
		size_t authoredChunkCount = 0;
		if (!play && m_ShowWorldPartition)
		{
			const Level::LevelPartitionSettings partSettings = m_Host.GetLevelPartitionSettings();
			const float chunkSize = static_cast<float>(partSettings.GridCellSize > 0 ? partSettings.GridCellSize : 64);
			const Level::PartitionGridPlane planeMode = partSettings.GridPlane;

			activeChunkX = static_cast<int32_t>(std::floor(camPos.x / chunkSize));
			activeChunkY = static_cast<int32_t>(std::floor(camPos.y / chunkSize));
			activeChunkZ = static_cast<int32_t>(std::floor(camPos.z / chunkSize));

			// 统计当前关卡在各 Chunk 的实体分布热度
			const Ref<Scene> activeScene = m_Host.GetActiveScene();
			if (activeScene)
			{
				std::unordered_set<int64_t> chunkSet;
				const entt::registry& reg = std::as_const(*activeScene).GetRegistry();
				auto transformView = reg.view<TransformComponent>();
				for (auto entity : transformView)
				{
					const auto& tc = transformView.get<TransformComponent>(entity);
					int32_t cA = static_cast<int32_t>(std::floor(tc.Location.x / chunkSize));
					int32_t cB = static_cast<int32_t>(std::floor(tc.Location.z / chunkSize));
					if (planeMode == Level::PartitionGridPlane::VerticalXY)
					{
						cA = static_cast<int32_t>(std::floor(tc.Location.x / chunkSize));
						cB = static_cast<int32_t>(std::floor(tc.Location.y / chunkSize));
					}
					else if (planeMode == Level::PartitionGridPlane::VerticalZY)
					{
						cA = static_cast<int32_t>(std::floor(tc.Location.z / chunkSize));
						cB = static_cast<int32_t>(std::floor(tc.Location.y / chunkSize));
					}
					const int64_t key = (static_cast<int64_t>(cA) << 32) ^ static_cast<uint32_t>(cB);
					chunkSet.insert(key);
				}
				authoredChunkCount = chunkSet.size();
			}

			const float viewSpan = isOrtho ? levelCam.OrthographicSize : std::max(20.0f, levelCam.Distance);
			const float proximityWeight = isOrtho ? 1.0f : std::clamp((viewSpan - 40.0f) / 60.0f, 0.0f, 1.0f);

			if (proximityWeight > 0.01f)
			{
				Wui::ClipScope partClip(ctx, sceneRect);
				const int32_t gridRadius = std::clamp(static_cast<int32_t>(std::ceil((viewSpan * 1.5f) / chunkSize)), 4, 16);
				const Wui::WuiColor chunkBorderColor { theme.Border.R, theme.Border.G, theme.Border.B, 0.35f * proximityWeight };

				// 1. 全局大世界空间分区棋盘 (支持 XZ / XY / ZY / Isometric / Full3D)
				for (int32_t da = -gridRadius; da <= gridRadius; ++da)
				{
					for (int32_t db = -gridRadius; db <= gridRadius; ++db)
					{
						const float distSq = static_cast<float>(da * da + db * db);
						const float normDist = std::sqrt(distSq) / static_cast<float>(gridRadius);
						const float fadeAlpha = std::clamp(1.0f - normDist * normDist, 0.0f, 1.0f);
						if (fadeAlpha <= 0.02f)
							continue;

						Wui::WuiColor borderColor = chunkBorderColor;
						borderColor.A *= fadeAlpha;

						if (planeMode == Level::PartitionGridPlane::VerticalXY)
						{
							const float minX = (activeChunkX + da) * chunkSize;
							const float minY = (activeChunkY + db) * chunkSize;
							const float maxX = minX + chunkSize;
							const float maxY = minY + chunkSize;
							const glm::vec3 corners[4] = {
								{ minX, minY, 0.05f }, { maxX, minY, 0.05f },
								{ maxX, maxY, 0.05f }, { minX, maxY, 0.05f }
							};
							for (int i = 0; i < 4; ++i)
								PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
									levelCam.ViewProjection * glm::vec4(corners[i], 1.0f),
									levelCam.ViewProjection * glm::vec4(corners[(i + 1) % 4], 1.0f),
									borderColor, 1.0f);
						}
						else if (planeMode == Level::PartitionGridPlane::VerticalZY)
						{
							const float minZ = (activeChunkZ + da) * chunkSize;
							const float minY = (activeChunkY + db) * chunkSize;
							const float maxZ = minZ + chunkSize;
							const float maxY = minY + chunkSize;
							const glm::vec3 corners[4] = {
								{ 0.05f, minY, minZ }, { 0.05f, minY, maxZ },
								{ 0.05f, maxY, maxZ }, { 0.05f, maxY, minZ }
							};
							for (int i = 0; i < 4; ++i)
								PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
									levelCam.ViewProjection * glm::vec4(corners[i], 1.0f),
									levelCam.ViewProjection * glm::vec4(corners[(i + 1) % 4], 1.0f),
									borderColor, 1.0f);
						}
						else if (planeMode == Level::PartitionGridPlane::Isometric)
						{
							const float ca = (activeChunkX + da) * chunkSize;
							const float cb = (activeChunkZ + db) * chunkSize;
							const glm::vec3 corners[4] = {
								{ (ca) * 0.7071f + (cb) * 0.7071f, 0.05f, (ca) * 0.7071f - (cb) * 0.7071f },
								{ (ca + chunkSize) * 0.7071f + (cb) * 0.7071f, 0.05f, (ca + chunkSize) * 0.7071f - (cb) * 0.7071f },
								{ (ca + chunkSize) * 0.7071f + (cb + chunkSize) * 0.7071f, 0.05f, (ca + chunkSize) * 0.7071f - (cb + chunkSize) * 0.7071f },
								{ (ca) * 0.7071f + (cb + chunkSize) * 0.7071f, 0.05f, (ca) * 0.7071f - (cb + chunkSize) * 0.7071f },
							};
							for (int i = 0; i < 4; ++i)
								PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
									levelCam.ViewProjection * glm::vec4(corners[i], 1.0f),
									levelCam.ViewProjection * glm::vec4(corners[(i + 1) % 4], 1.0f),
									borderColor, 1.0f);
						}
						else if (planeMode == Level::PartitionGridPlane::Full3D)
						{
							const float minX = (activeChunkX + da) * chunkSize;
							const float minZ = (activeChunkZ + db) * chunkSize;
							const float maxX = minX + chunkSize;
							const float maxZ = minZ + chunkSize;
							const float minY = 0.0f;
							const float maxY = chunkSize;
							const glm::vec3 bCorners[8] = {
								{ minX, minY, minZ }, { maxX, minY, minZ }, { maxX, minY, maxZ }, { minX, minY, maxZ },
								{ minX, maxY, minZ }, { maxX, maxY, minZ }, { maxX, maxY, maxZ }, { minX, maxY, maxZ },
							};
							const int bEdges[12][2] = {
								{0,1}, {1,2}, {2,3}, {3,0}, {4,5}, {5,6}, {6,7}, {7,4},
								{0,4}, {1,5}, {2,6}, {3,7}
							};
							for (int e = 0; e < 12; ++e)
								PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
									levelCam.ViewProjection * glm::vec4(bCorners[bEdges[e][0]], 1.0f),
									levelCam.ViewProjection * glm::vec4(bCorners[bEdges[e][1]], 1.0f),
									borderColor, 1.0f);
						}
						else // HorizontalXZ
						{
							const float minX = (activeChunkX + da) * chunkSize;
							const float minZ = (activeChunkZ + db) * chunkSize;
							const float maxX = minX + chunkSize;
							const float maxZ = minZ + chunkSize;
							const glm::vec3 corners[4] = {
								{ minX, 0.05f, minZ }, { maxX, 0.05f, minZ },
								{ maxX, 0.05f, maxZ }, { minX, 0.05f, maxZ }
							};
							for (int i = 0; i < 4; ++i)
								PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
									levelCam.ViewProjection * glm::vec4(corners[i], 1.0f),
									levelCam.ViewProjection * glm::vec4(corners[(i + 1) % 4], 1.0f),
									borderColor, 1.0f);
						}
					}
				}

				// 2. 特定区域局部覆盖呈现 (PartitionVolumeComponent 3D包围盒与细网格)
				const Ref<Scene> activeScene = m_Host.GetActiveScene();
			if (activeScene)
				{
					const entt::registry& reg = std::as_const(*activeScene).GetRegistry();
					auto volumeView = reg.view<PartitionVolumeComponent, TransformComponent>();
					for (auto entity : volumeView)
					{
						const auto& pvc = volumeView.get<PartitionVolumeComponent>(entity);
						const auto& tc = volumeView.get<TransformComponent>(entity);
						if (!pvc.Active)
							continue;

						const glm::vec3 center = tc.Location;
						const glm::vec3 h = pvc.HalfExtents;
						const Wui::WuiColor volumeBoxColor { theme.Warning.R, theme.Warning.G, theme.Warning.B, 0.65f };

						const glm::vec3 bCorners[8] = {
							center + glm::vec3(-h.x, -h.y, -h.z),
							center + glm::vec3( h.x, -h.y, -h.z),
							center + glm::vec3( h.x, -h.y,  h.z),
							center + glm::vec3(-h.x, -h.y,  h.z),
							center + glm::vec3(-h.x,  h.y, -h.z),
							center + glm::vec3( h.x,  h.y, -h.z),
							center + glm::vec3( h.x,  h.y,  h.z),
							center + glm::vec3(-h.x,  h.y,  h.z),
						};
						const int edges[12][2] = {
							{0,1}, {1,2}, {2,3}, {3,0},
							{4,5}, {5,6}, {6,7}, {7,4},
							{0,4}, {1,5}, {2,6}, {3,7}
						};
						for (int e = 0; e < 12; ++e)
						{
							PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
								levelCam.ViewProjection * glm::vec4(bCorners[edges[e][0]], 1.0f),
								levelCam.ViewProjection * glm::vec4(bCorners[edges[e][1]], 1.0f),
								volumeBoxColor, 1.8f);
						}

						// 盒内地表细分网格呈现
						const float subCell = static_cast<float>(pvc.OverrideCellSize > 0 ? pvc.OverrideCellSize : 16);
						const Wui::WuiColor subGridColor { theme.Warning.R, theme.Warning.G, theme.Warning.B, 0.28f };
						const float startX = std::floor((center.x - h.x) / subCell) * subCell;
						const float endX = std::ceil((center.x + h.x) / subCell) * subCell;
						const float startZ = std::floor((center.z - h.z) / subCell) * subCell;
						const float endZ = std::ceil((center.z + h.z) / subCell) * subCell;

						for (float x = startX; x <= endX; x += subCell)
						{
							PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
								levelCam.ViewProjection * glm::vec4(x, center.y - h.y + 0.05f, center.z - h.z, 1.0f),
								levelCam.ViewProjection * glm::vec4(x, center.y - h.y + 0.05f, center.z + h.z, 1.0f),
								subGridColor, 1.2f);
						}
						for (float z = startZ; z <= endZ; z += subCell)
						{
							PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
								levelCam.ViewProjection * glm::vec4(center.x - h.x, center.y - h.y + 0.05f, z, 1.0f),
								levelCam.ViewProjection * glm::vec4(center.x + h.x, center.y - h.y + 0.05f, z, 1.0f),
								subGridColor, 1.2f);
						}
					}
				}
			}

			const char* planeTag = "XZ";
			if (planeMode == Level::PartitionGridPlane::VerticalXY) planeTag = "XY";
			else if (planeMode == Level::PartitionGridPlane::VerticalZY) planeTag = "ZY";
			else if (planeMode == Level::PartitionGridPlane::Isometric) planeTag = "ISO";
			else if (planeMode == Level::PartitionGridPlane::Full3D) planeTag = "3D";

			std::snprintf(hudLine1, sizeof(hudLine1),
				"Partition %um [%s] | Chunk (%d, %d) | Authored: %zu Chunks",
				partSettings.GridCellSize, planeTag,
				activeChunkX, (planeMode == Level::PartitionGridPlane::VerticalXY || planeMode == Level::PartitionGridPlane::VerticalZY) ? activeChunkY : activeChunkZ, authoredChunkCount);

			if (m_Host.IsFlyNavigating() || m_Host.GetFlySpeedTimer() > 0.0f)
			{
				char flyInfo[48] = {};
				const float curSpeed = 12.0f * m_Host.GetFlySpeedScale();
				std::snprintf(flyInfo, sizeof(flyInfo), " | Fly: %.1f m/s (%.1fx)", curSpeed, m_Host.GetFlySpeedScale());
				std::strncat(hudLine1, flyInfo, sizeof(hudLine1) - std::strlen(hudLine1) - 1);
			}
		}
		else if (m_ShowReferenceGrid)
		{
			const char* axisTag = "Free (XZ)";
			switch (currentVp)
			{
			case World::ViewportViewpoint::Free:   axisTag = "Free (XZ)"; break;
			case World::ViewportViewpoint::Top:    axisTag = "Top (XZ)"; break;
			case World::ViewportViewpoint::Bottom: axisTag = "Bottom (XZ)"; break;
			case World::ViewportViewpoint::Front:  axisTag = "Front (XY)"; break;
			case World::ViewportViewpoint::Back:   axisTag = "Back (XY)"; break;
			case World::ViewportViewpoint::Right:  axisTag = "Right (ZY)"; break;
			case World::ViewportViewpoint::Left:   axisTag = "Left (ZY)"; break;
			}
			const float scaleValue = isOrtho ? levelCam.OrthographicSize : levelCam.Distance;
			const char* gridStepLabel = "Grid 1m/10m";
			if (scaleValue > 300.0f)
				gridStepLabel = "Grid 50m/100m";
			else if (scaleValue > 60.0f)
				gridStepLabel = "Grid 10m/50m";

			std::snprintf(hudLine1, sizeof(hudLine1),
				"%s | %s | Scale: %.1fu", gridStepLabel, axisTag, scaleValue);

			if (m_Host.IsFlyNavigating() || m_Host.GetFlySpeedTimer() > 0.0f)
			{
				char flyInfo[48] = {};
				const float curSpeed = 12.0f * m_Host.GetFlySpeedScale();
				std::snprintf(flyInfo, sizeof(flyInfo), " | Fly: %.1f m/s (%.1fx)", curSpeed, m_Host.GetFlySpeedScale());
				std::strncat(hudLine1, flyInfo, sizeof(hudLine1) - std::strlen(hudLine1) - 1);
			}
		}
			std::string layersText;
			for (const Level::DataLayerDefinition& layer : m_Host.GetLevelDataLayers())
			{
				if (layer.Name.empty())
					continue;
				if ((layer.LayerMask & m_Host.GetActiveDataLayers()) == 0)
					continue;
				if (!layersText.empty())
					layersText += "+";
				layersText += layer.Name;
			}
			if (layersText.empty())
				layersText = "all";
			std::snprintf(hudLine2, sizeof(hudLine2),
				"Layers: [%s] | Entities: %zu", layersText.c_str(), entityCount);

		// 视口状态卡片延后至 3D 导航网格图层之后绘制，确保 2D UI 永远位于 3D 调试线框之上

		// ⑥ 飞行漫游实时速度浮动胶囊 (对标 UE5 Camera Speed Floating Badge, 居中高亮醒目呈现)
		if (m_Host.IsFlyNavigating() || m_Host.GetFlySpeedTimer() > 0.0f)
		{
			const float speedScale = m_Host.GetFlySpeedScale();
			const float curSpeed = 12.0f * speedScale;
			char speedText[64] = {};
			std::snprintf(speedText, sizeof(speedText), "飞行速度: %.1f m/s  (%.1fx)", curSpeed, speedScale);

			const float badgeTextW = ctx.MeasureTextWidth(speedText, 13.0f);
			const float badgeW = badgeTextW + 36.0f;
			const float badgeH = 28.0f;
			const Wui::WuiRect badgeRect {
				sceneRect.X + (sceneRect.W - badgeW) * 0.5f,
				sceneRect.Y + kChromePad + kPillH + 8.0f,
				badgeW,
				badgeH
			};

			const Wui::WuiColor badgeBg = theme.CardBg;
			const Wui::WuiColor badgeBorder = theme.Info;
			const Wui::WuiColor badgeTextCol = theme.TextBright;

			Wui::PanelBackground(ctx, badgeRect, badgeBg, 14.0f);
			Wui::HighlightOutline(ctx, badgeRect, badgeBorder, 14.0f, 1.2f);
			Wui::Label(ctx, { badgeRect.X + 18.0f, badgeRect.Y + 7.0f }, speedText, badgeTextCol, 13.0f);
			ctx.RegisterOverlayRect(badgeRect);
		}

		// ---- 相机预览小窗(PiP):直接显示"场景相机看到的东西" ----
		//
		// 物理调试线框(P1b D6)也画在这一段之前:P4-U4 起开关有两处来源 ——
		// 场景头 `World.physics_debug`(设置面板可改、随场景保存)或环境变量
		// `WLD_PHYSICS_DEBUG`(自动化/一次性诊断,保持向后兼容)。
		const Ref<Scene> debugToggleScene = m_Host.GetActiveScene();
		const bool physicsDebugEnabled = std::getenv("WLD_PHYSICS_DEBUG") != nullptr
			|| (debugToggleScene && debugToggleScene->GetWorldSettings().PhysicsDebug);
		if (physicsDebugEnabled)
		{
			// P1b D6:3D 物理调试线框(碰撞体世界空间线段,复用视锥那套投影/裁剪)。
			// 只在世界已启动(Play/Simulate)时存在;缓冲按帧复用,避免每帧分配。
			const Ref<Scene> physicsScene = m_Host.GetActiveScene();
			if (physicsScene && physicsScene->IsPhysics3DRunning())
			{
				static std::vector<DebugLine> physicsLines;
				physicsLines.clear();
				physicsScene->GetPhysics3DWorld()->CollectDebugLines(physicsLines);
				if (!physicsLines.empty())
				{
					const Wui::GizmoCamera debugCamera = m_Host.GetGizmoCamera();
					const Wui::WuiColor physicsColor = theme.Success;
					{
						// P1c-W3.4:物理调试线框的裁剪走库件 Wui::ClipScope。
						Wui::ClipScope physicsClip(ctx, sceneRect);
						for (const DebugLine& line : physicsLines)
						{
							PushProjectedSegment(ctx, debugCamera.ViewProjection, sceneRect,
								debugCamera.ViewProjection * glm::vec4 { line.Begin, 1.0f },
								debugCamera.ViewProjection * glm::vec4 { line.End, 1.0f },
								physicsColor, 1.4f);
						}
					}
					if (std::getenv("WLD_TRACE_UI"))
					{
						static uint64_t lastPhysicsLog = ~0ull;
						const uint64_t stamp = ctx.Frame() / 120;
						if (stamp != lastPhysicsLog)
						{
							lastPhysicsLog = stamp;
							WLD_CORE_INFO("[ui] physics debug lines={0} viewport=({1},{2},{3},{4})",
								physicsLines.size(), sceneRect.X, sceneRect.Y, sceneRect.W, sceneRect.H);
						}
					}
				}
			}
		}

						// 代理路径缓存结构体与哈希表
		struct CachedAgentPath
		{
			glm::vec3 LastStart { 1e9f };
			glm::vec3 LastDest { 1e9f };
			std::vector<glm::vec3> Path;
			std::chrono::steady_clock::time_point LastTime {};
			uint8_t LastCrouch = 0;
			float LastHeight = 0.0f;
		};
		static std::unordered_map<uint32_t, CachedAgentPath> s_AgentPathCache;

// ---- 3D 导航网格 (NavMesh & AI Navigation) 视口可视化 ----
		// 3D 几何要素（多边形半透明填充面、边界轮廓线、代理圆柱体、路径折线、障碍盒）已全面移交 SceneRenderer
		// 并在原生 GPU 渲染流中进行硬件像素级深度测试 (DepthTest = true, DepthWrite = false)；
		// ViewportPanel 仅在屏幕空间保留 2D 文本标牌气泡。
		const bool anyNavRequested = !play && (s_ShowNavMesh ||
			NavMeshPanel::ShowPolygons ||
			NavMeshPanel::ShowPaths ||
			NavMeshPanel::ShowObstacles ||
			NavMeshPanel::ShowOffMeshLinks ||
			NavMeshPanel::ShowAreaCosts);
		const Ref<Scene> navScene = m_Host.GetActiveScene();
		if (anyNavRequested && navScene && m_Host.HasRenderedScene())
		{
			const Wui::GizmoCamera debugCamera = m_Host.GetGizmoCamera();
			const glm::mat4& viewProj = debugCamera.ViewProjection;
			Wui::ClipScope navClip(ctx, sceneRect);

			const auto& registry = std::as_const(*navScene).GetRegistry();
			for (auto [handle, areaComp] : registry.view<NavMeshAreaModifierComponent>().each())
			{
				if (areaComp.ClearanceHeight > 0.0f)
				{
					glm::vec3 badgeWorld { 0.0f };
					if (const auto* xf = registry.try_get<TransformComponent>(handle))
						badgeWorld = xf->Location + glm::vec3(0.0f, areaComp.Extents.y + 0.35f, 0.0f);
					const glm::vec4 clipPos = viewProj * glm::vec4(badgeWorld, 1.0f);
					if (clipPos.w > 0.05f)
					{
						const glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
						if (ndc.z >= -1.0f && ndc.z <= 1.0f)
						{
							const glm::vec2 badgePos {
								sceneRect.X + (ndc.x * 0.5f + 0.5f) * sceneRect.W,
								sceneRect.Y + (1.0f - (ndc.y * 0.5f + 0.5f)) * sceneRect.H
							};
							if (badgePos.x >= sceneRect.X && badgePos.x <= sceneRect.X + sceneRect.W &&
								badgePos.y >= sceneRect.Y && badgePos.y <= sceneRect.Y + sceneRect.H)
							{
								char clearText[64];
								std::snprintf(clearText, sizeof(clearText), "Clearance: %.1fm", areaComp.ClearanceHeight);
								const float fontSz = 11.0f;
								const float textW = ctx.MeasureTextWidth(clearText, fontSz);
								const Wui::WuiRect tagRect { badgePos.x - textW * 0.5f - 6.0f, badgePos.y - 18.0f, textW + 12.0f, 18.0f };
								Wui::PanelBackground(ctx, tagRect, Wui::WuiColor{ 0.05f, 0.12f, 0.20f, 0.88f }, 4.0f);
								Wui::HighlightOutline(ctx, tagRect, Wui::WuiColor{ 0.20f, 0.85f, 1.00f, 0.90f }, 4.0f, 1.2f);
								Wui::Label(ctx, { tagRect.X + 6.0f, tagRect.Y + 2.5f }, clearText, Wui::WuiColor{ 0.20f, 0.85f, 1.00f, 0.90f }, fontSz);
							}
						}
					}
				}
			}
		}

		// ⑤ 视口状态信息卡片下沉至右下角 (避免遮挡顶部工具栏与 3D AxisGizmo 坐标系)
		const bool hasHudData = (!play && (hudLine1[0] != '\0' || entityCount > 0 || !m_Host.GetLevelDataLayers().empty()));
		if (hasHudData)
		{
			const float axisAvoid = (sceneRect.W >= 420.0f) ? 56.0f : 0.0f;
			const float maxHudW = std::max(120.0f, sceneRect.W - axisAvoid - 24.0f);

			const float text1W = ctx.MeasureTextWidth(hudLine1, 11.5f);
			const float text2W = ctx.MeasureTextWidth(hudLine2, 11.5f);
			const float hudW = std::clamp(std::max(text1W, text2W) + 20.0f, 180.0f, maxHudW);
			const float hudH = 42.0f;

			const float hudX = std::max(sceneRect.X + 8.0f, sceneRect.X + sceneRect.W - hudW - 12.0f - axisAvoid);
			const Wui::WuiRect hudRect { hudX, sceneRect.Y + sceneRect.H - hudH - 12.0f, hudW, hudH };

			Wui::PanelBackground(ctx, hudRect, theme.CardBg, 6.0f);
			Wui::HighlightOutline(ctx, hudRect, theme.BorderStrong, 6.0f, 1.0f);

			const std::string shownLine2 = Wui::EllipsizeMiddleToWidth(ctx, hudLine2, hudW - 20.0f, 11.5f);
			if (hudLine1[0] != '\0')
			{
				const std::string shownLine1 = Wui::EllipsizeMiddleToWidth(ctx, hudLine1, hudW - 20.0f, 11.5f);
				Wui::Label(ctx, { hudRect.X + 10.0f, hudRect.Y + 4.0f }, shownLine1,
					theme.Info, 11.5f);
				Wui::Label(ctx, { hudRect.X + 10.0f, hudRect.Y + 22.0f }, shownLine2,
					theme.Text, 11.5f);
			}
			else
			{
				Wui::Label(ctx, { hudRect.X + 10.0f, hudRect.Y + 12.0f }, shownLine2,
					theme.Text, 11.5f);
			}

			ctx.RegisterOverlayRect(hudRect);
		}

		// ⑥ LOD 与空间八叉树性能遥测 HUD (LOD & Octree HUD)
		const Ref<Scene> hudScene = m_Host.GetActiveScene();
		if (!play && s_ShowLODHud && hudScene && m_Host.HasRenderedScene())
		{
			const auto& lodStats = MeshLODManager::Get().GetStats();
			const size_t octreeNodes = hudScene->GetOctree().GetTotalNodeCount();
			const size_t octreeElements = hudScene->GetOctree().GetTotalElementCount();

			char line1[96];
			std::snprintf(line1, sizeof(line1), "八叉树: %zu 节点 | %zu 元素", octreeNodes, octreeElements);

			char line2[128];
			if (IsMeshletClustersVisible())
			{
				std::snprintf(line2, sizeof(line2), "Meshlets: GPU 集群真彩染色激活 (≤64V/126T) | 剔除: %u", lodStats.CulledCount);
			}
			else
			{
				std::snprintf(line2, sizeof(line2), "LOD 分布: LOD0: %u | LOD1: %u | LOD2: %u | 剔除: %u",
					lodStats.LOD0Count, lodStats.LOD1Count, lodStats.LOD2Count, lodStats.CulledCount);
			}

			const float t1W = ctx.MeasureTextWidth(line1, 11.5f);
			const float t2W = ctx.MeasureTextWidth(line2, 11.5f);
			const float lodHudW = std::max(260.0f, std::max(t1W, t2W) + 24.0f);
			const float lodHudH = 46.0f;
			const float axisAvoid = (sceneRect.W >= 420.0f) ? 56.0f : 0.0f;

			float lodHudY = sceneRect.Y + sceneRect.H - lodHudH - 12.0f;
			if (hasHudData)
			{
				lodHudY -= (42.0f + 8.0f);
			}

			const Wui::WuiRect lodHudRect {
				sceneRect.X + sceneRect.W - lodHudW - 12.0f - axisAvoid,
				lodHudY,
				lodHudW,
				lodHudH
			};

			Wui::PanelBackground(ctx, lodHudRect, theme.CardBg, 6.0f);
			Wui::HighlightOutline(ctx, lodHudRect, theme.Accent, 6.0f, 1.0f);

			Wui::Label(ctx, { lodHudRect.X + 10.0f, lodHudRect.Y + 5.0f }, line1,
				theme.Accent, 11.5f);
			Wui::Label(ctx, { lodHudRect.X + 10.0f, lodHudRect.Y + 23.0f }, line2,
				theme.Text, 11.5f);

			ctx.RegisterOverlayRect(lodHudRect);
		}

		if (previewVisible)
		{
			const uint64_t previewTexture = m_Host.GetCameraPreviewTextureId();
			Wui::PanelBackground(ctx, previewRect, theme.WindowBg, 3.0f);
			Label(ctx, { previewRect.X + 6.0f, previewRect.Y + 3.0f },
				"相机: " + previewLabel, theme.Text, 12.0f);
			// UV 与主视口一致(场景纹理按 {0,1,1,-1} 贴,预览渲染器同一条路径)。
			Image(ctx, { previewRect.X + 1.0f, previewRect.Y + 18.0f, previewRect.W - 2.0f, previewRect.H - 19.0f },
				previewTexture, { 0, 1, 1, -1 }, theme);
			// P1c-W3.4:描边走库件(与手写 RectOutline 逐字段等价:色/圆角 3/厚度 1)。
			Wui::HighlightOutline(ctx, previewRect, theme.Border, 3.0f, 1.0f);
		}

		// ---- W5-L1:文档场景外部改动提示:只提示 + 一键重开,**不自动替换**文档 ----
		if (bannerVisible)
		{
			Wui::PanelBackground(ctx, bannerRect, theme.Warning, 3.0f);
			RegisterReadonlyNode(Wui::HashId("scene.external.changed"), "status", "scene external change",
				"场景文件已在磁盘上被外部修改(未自动替换)", bannerRect);
			Label(ctx, { bannerRect.X + 10.0f, bannerRect.Y + 6.0f },
				"场景已在磁盘上改动", theme.TextBright, 12.0f);
			if (Button(ctx, Wui::HashId("scene.external.reopen"),
				{ bannerRect.X + bannerRect.W - 104.0f, bannerRect.Y + 3.0f, 96.0f, 20.0f }, "重新打开", theme))
				m_Host.ReopenExternalScene();
			// P1c-W3.4:描边走库件(与手写 RectOutline 逐字段等价)。注:该横幅要"场景文件被外部改"
			// 才出现,本片未驱动该状态(报告 §5.4 未做项);同一条命令的另一处(预览小窗边框)已实测。
			Wui::HighlightOutline(ctx, bannerRect, theme.Border, 3.0f, 1.0f);
		}
		(void)theme;
	}

	void ViewportPanel::SetNavMeshVisible(bool visible)
	{
		s_ShowNavMesh = visible;
	}

	bool ViewportPanel::IsNavMeshVisible()
	{
		return s_ShowNavMesh;
	}

	void ViewportPanel::SetOctreeBoundsVisible(bool visible)
	{
		s_ShowOctreeBounds = visible;
	}

	bool ViewportPanel::IsOctreeBoundsVisible()
	{
		return s_ShowOctreeBounds;
	}

	void ViewportPanel::SetLODHudVisible(bool visible)
	{
		s_ShowLODHud = visible;
	}

	bool ViewportPanel::IsLODHudVisible()
	{
		return s_ShowLODHud;
	}

	void ViewportPanel::SetMeshletClustersVisible(bool visible)
	{
		s_ShowMeshletClusters = visible;
#if defined(_WIN32)
		_putenv_s("WLD_MESHLET_CLUSTERS_DEBUG", visible ? "1" : "0");
#endif
	}

	bool ViewportPanel::IsMeshletClustersVisible()
	{
		const char* env = std::getenv("WLD_MESHLET_CLUSTERS_DEBUG");
		return s_ShowMeshletClusters || (env != nullptr && std::strcmp(env, "0") != 0);
	}

	bool ViewportPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		// P: 切换 3D 导航网格 (NavMesh) 视口显示 (对标 Unreal Engine 5 行业标准快捷键)
		if (!ctrl && !shift && !alt && keyCode == KeyCodes::P)
		{
			SetNavMeshVisible(!s_ShowNavMesh);
			return true;
		}
		if (shift && !ctrl && !alt)
		{
			if (keyCode == KeyCodes::D1)
			{
				m_Host.SetEditorToolMode(EditorToolMode::Select);
				return true;
			}
			if (keyCode == KeyCodes::D2)
			{
				m_Host.SetEditorToolMode(EditorToolMode::Landscape);
				m_Host.ActivatePanel("terrain_editor");
				return true;
			}
			if (keyCode == KeyCodes::D3)
			{
				m_Host.SetEditorToolMode(EditorToolMode::Foliage);
				m_Host.ActivatePanel("foliage_editor");
				return true;
			}
		}

		if (m_Host.GetEditorToolMode() == EditorToolMode::Landscape)
		{
			TerrainPanel* tp = m_Host.GetTerrainPanel();
			if (tp)
			{
				if (keyCode == KeyCodes::LeftBracket)
				{
					if (shift)
						tp->SetBrushStrength(tp->GetBrushStrength() - 0.05f);
					else
						tp->SetBrushRadius(tp->GetBrushRadius() - 1.0f);
					return true;
				}
				if (keyCode == KeyCodes::RightBracket)
				{
					if (shift)
						tp->SetBrushStrength(tp->GetBrushStrength() + 0.05f);
					else
						tp->SetBrushRadius(tp->GetBrushRadius() + 1.0f);
					return true;
				}
			}
		}

		if (m_Host.GetEditorToolMode() == EditorToolMode::Foliage)
		{
			FoliagePanel* fp = m_Host.GetFoliagePanel();
			if (fp)
			{
				if (keyCode == KeyCodes::LeftBracket)
				{
					if (shift)
						fp->SetBrushDensity(std::clamp(fp->GetBrushDensity() - 0.2f, 0.1f, 10.0f));
					else
						fp->SetBrushRadius(std::clamp(fp->GetBrushRadius() - 1.0f, 1.0f, 100.0f));
					return true;
				}
				if (keyCode == KeyCodes::RightBracket)
				{
					if (shift)
						fp->SetBrushDensity(std::clamp(fp->GetBrushDensity() + 0.2f, 0.1f, 10.0f));
					else
						fp->SetBrushRadius(std::clamp(fp->GetBrushRadius() + 1.0f, 1.0f, 100.0f));
					return true;
				}
			}
		}

		return false;
	}
}



