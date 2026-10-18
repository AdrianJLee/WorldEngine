#include "wldpch.h"
#include <glm/vec2.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "WUI/Panels/ParticleEditorPanel.h"

#include "World/Core/Log.h"
#include "World/Utils/Paths.h"
#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <cmath>

namespace World
{
	std::string ParticleEditorPanel::s_PendingOpenPath;

	void ParticleEditorPanel::RequestOpenAsset(const std::string& path)
	{
		s_PendingOpenPath = path;
	}

	static void OpenInExplorer(const std::filesystem::path& path)
	{
#ifdef _WIN32
		std::error_code ec;
		std::filesystem::path target = std::filesystem::absolute(path);
		if (std::filesystem::exists(target, ec) && !std::filesystem::is_directory(target, ec))
		{
			const std::wstring parameters = L"/select,\"" + target.wstring() + L"\"";
			ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
		}
		else
		{
			std::filesystem::path parent = target.parent_path();
			if (std::filesystem::exists(parent, ec))
			{
				ShellExecuteW(nullptr, L"open", parent.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
		}
#else
		(void)path;
#endif
	}

	ParticleEditorPanel::ParticleEditorPanel()
	{
		NewAsset();
	}

	ParticleEditorPanel::~ParticleEditorPanel() = default;

	void ParticleEditorPanel::RefreshAvailableAssets()
	{
		m_AvailableAssets.clear();
		std::error_code ec;
		const std::filesystem::path assetRoot = World::Paths::AssetRoot();
		if (std::filesystem::exists(assetRoot, ec))
		{
			for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot, ec))
			{
				if (entry.is_regular_file())
				{
					const std::string ext = entry.path().extension().string();
					if (ext == ".wvfx" || ext == ".wparticle")
					{
						std::string rel = std::filesystem::relative(entry.path(), assetRoot, ec).generic_string();
						m_AvailableAssets.push_back(rel);
					}
				}
			}
		}
	}

	void ParticleEditorPanel::SyncSimulationFromAsset()
	{
		m_LivePools.clear();
		for (size_t i = 0; i < m_Asset.Emitters.size(); ++i)
		{
			const auto& em = m_Asset.Emitters[i];
			auto pool = std::make_unique<ParticlePool>(std::max(em.MaxParticles, 128u));
			pool->SetSeed(m_Asset.Seed + static_cast<uint32_t>(i * 1013));
			pool->SetMaterialSettings(em.Material);
			pool->SetCollisionSettings(em.Collision);
			for (const auto& sub : em.SubEmitters)
				pool->AddSubEmitter(sub);
			m_LivePools.push_back(std::move(pool));
		}
		m_SimTime = 0.0f;
	}

	void ParticleEditorPanel::StepSimulation(float dt)
	{
		if (m_LivePools.size() != m_Asset.Emitters.size())
		{
			SyncSimulationFromAsset();
		}

		m_SimTime += dt * m_PlaybackSpeed;
		for (size_t i = 0; i < m_Asset.Emitters.size() && i < m_LivePools.size(); ++i)
		{
			const auto& em = m_Asset.Emitters[i];
			auto& pool = m_LivePools[i];

			ParticleSystemComponent comp;
			comp.MaxParticles = em.MaxParticles;
			comp.Lifetime = em.Lifetime;
			comp.EmissionRate = em.EmissionRate;
			comp.Speed = em.Speed;
			comp.SpeedVariance = em.SpeedVariance;
			comp.Gravity = em.Gravity;
			comp.SizeStart = em.SizeStart;
			comp.SizeEnd = em.SizeEnd;
			comp.EmitterType = em.EmitterType;
			comp.LockToXYPlane = em.LockToXY;
			comp.IsPlaying = 1;
			comp.ColorStart = 0xFFFFFFFF;
			comp.ColorEnd = 0x00FFFFFF;

			pool->SetCollisionSettings(em.Collision);
			pool->Update(dt * m_PlaybackSpeed, comp, glm::vec3(0.0f));
		}
	}

	void ParticleEditorPanel::NewAsset()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "NewVFX";
		m_Asset.Seed = 1337;

		ParticleEmitterLayer em;
		em.Name = "CoreFlame";
		em.MaxParticles = 400;
		em.Lifetime = 1.5f;
		em.EmissionRate = 65.0f;
		em.Speed = 3.0f;
		em.SpeedVariance = 0.8f;
		em.Gravity = glm::vec3(0.0f, 1.2f, 0.0f);
		em.SizeStart = 0.5f;
		em.SizeEnd = 0.1f;
		em.ColorStart = glm::vec4(1.0f, 0.7f, 0.2f, 1.0f);
		em.ColorEnd = glm::vec4(0.9f, 0.1f, 0.0f, 0.0f);
		em.Material.BlendMode = ParticleBlendMode::Additive;
		em.Material.EmissiveBoost = 2.5f;
		m_Asset.Emitters.push_back(em);

		m_LoadedPath = "assets/vfx/new_vfx.wvfx";
		m_Dirty = true;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "新建粒子特效资产";

		SyncSimulationFromAsset();
	}

	void ParticleEditorPanel::LoadAsset(const std::string& path)
	{
		std::string normalized = path;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');

		std::filesystem::path fullPath = normalized;
		if (!fullPath.is_absolute())
		{
			fullPath = World::Paths::AssetRoot() / normalized;
		}

		ParticleAsset loaded;
		if (ParticleAssetSerializer::Deserialize(fullPath.generic_string(), loaded))
		{
			m_Asset = std::move(loaded);
			m_LoadedPath = normalized;
			m_Dirty = false;
			m_SelectedEmitterIndex = 0;
			m_StatusFeedback = "成功加载: " + normalized;
			SyncSimulationFromAsset();
		}
		else
		{
			m_StatusFeedback = "加载失败: " + normalized;
		}
	}

	void ParticleEditorPanel::SaveAsset()
	{
		if (m_LoadedPath.empty())
		{
			m_StatusFeedback = "请先指定保存路径";
			return;
		}

		std::filesystem::path fullPath = m_LoadedPath;
		if (!fullPath.is_absolute())
		{
			fullPath = World::Paths::AssetRoot() / m_LoadedPath;
		}

		std::error_code ec;
		std::filesystem::create_directories(fullPath.parent_path(), ec);

		if (ParticleAssetSerializer::Serialize(m_Asset, fullPath.generic_string()))
		{
			m_Dirty = false;
			m_StatusFeedback = "保存成功: " + m_LoadedPath;
			RefreshAvailableAssets();
		}
		else
		{
			m_StatusFeedback = "保存失败: " + m_LoadedPath;
		}
	}

	bool ParticleEditorPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		(void)shift;
		(void)alt;
		if (ctrl && keyCode == KeyCodes::S)
		{
			SaveAsset();
			return true;
		}
		return false;
	}

	void ParticleEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg);

		if (!s_PendingOpenPath.empty())
		{
			std::string p = s_PendingOpenPath;
			s_PendingOpenPath.clear();
			LoadAsset(p);
		}

		if (m_AvailableAssets.empty() && m_LoadedPath.empty())
		{
			RefreshAvailableAssets();
			if (!m_AvailableAssets.empty())
			{
				LoadAsset(m_AvailableAssets[0]);
			}
		}

		// 模拟步进
		if (m_IsPlaying)
		{
			StepSimulation(0.0166f);
		}

		float curY = rect.Y + 8.0f;
		const float padX = 12.0f;
		const float contentW = rect.W - padX * 2.0f;

		// 1. 顶部操作栏
		const Wui::WuiRect topBar { rect.X + padX, curY, contentW, 36.0f };
		Wui::BarSurface(ctx, topBar, theme.PanelHeader, theme.Border);

		Wui::Label(ctx, { topBar.X + 12.0f, topBar.Y + 9.0f }, "粒子系统编辑器 (Particle System Editor)", theme.Accent, 13.5f);

		std::string titleTag = m_LoadedPath.empty() ? "[未命名资产]" : (m_LoadedPath + (m_Dirty ? " *" : ""));
		Wui::Label(ctx, { topBar.X + 260.0f, topBar.Y + 10.0f }, "当前文件: " + titleTag, m_Dirty ? Wui::WuiColor { 1.0f, 0.7f, 0.2f, 1.0f } : theme.TextMuted, 11.5f);

		const float btnW = 76.0f;
		const Wui::WuiRect newBtnRect { topBar.X + topBar.W - btnW * 3.0f - 24.0f, topBar.Y + 6.0f, btnW, 24.0f };
		Wui::Tooltip(ctx, newBtnRect, "新建粒子特效资产 (.wvfx)");
		if (Wui::Button(ctx, Wui::HashId("vfx.btn.new"), newBtnRect, "新建", theme))
		{
			NewAsset();
		}

		const Wui::WuiRect saveBtnRect { topBar.X + topBar.W - btnW * 2.0f - 16.0f, topBar.Y + 6.0f, btnW, 24.0f };
		Wui::Tooltip(ctx, saveBtnRect, "保存当前粒子特效资产修改 (Ctrl+S)");
		if (Wui::Button(ctx, Wui::HashId("vfx.btn.save"), saveBtnRect, m_Dirty ? "保存 *" : "保存", theme))
		{
			SaveAsset();
		}

		const Wui::WuiRect expBtnRect { topBar.X + topBar.W - btnW - 8.0f, topBar.Y + 6.0f, btnW, 24.0f };
		Wui::Tooltip(ctx, expBtnRect, "在资源管理器中定位当前文件");
		if (Wui::Button(ctx, Wui::HashId("vfx.btn.explorer"), expBtnRect, "定位", theme))
		{
			std::filesystem::path full = m_LoadedPath;
			if (!full.is_absolute()) full = World::Paths::AssetRoot() / m_LoadedPath;
			OpenInExplorer(full);
		}

		curY += topBar.H + 8.0f;

		// 2. 双栏工业级布局 (左侧: 3D 自由交互视口 58% | 右侧: 模块卡片堆栈 42%)
		const float gap = 12.0f;
		const float availH = std::max(300.0f, rect.H - (curY - rect.Y) - 12.0f);
		const float leftW = std::floor((contentW - gap) * 0.58f);
		const float rightW = contentW - gap - leftW;

		// ---- 左侧 3D 粒子视口 ----
		const Wui::WuiRect vpRect { rect.X + padX, curY, leftW, availH };
		Wui::BarSurface(ctx, vpRect, Wui::WuiColor { 0.08f, 0.09f, 0.11f, 1.0f }, theme.Border);

		// 相机交互
		const bool isVpHovered = ctx.IsHovered(vpRect);
		if (isVpHovered)
		{
			if (ctx.Input().Wheel != 0.0f)
			{
				m_CameraDistance = std::clamp(m_CameraDistance - ctx.Input().Wheel * 0.5f, 1.5f, 25.0f);
			}
			if (ctx.Input().MouseClicked[0])
			{
				m_IsOrbiting = true;
				m_LastMousePos = ctx.Input().MousePos;
			}
		}
		if (m_IsOrbiting)
		{
			if (ctx.Input().MouseDown[0])
			{
				const glm::vec2 delta = ctx.Input().MousePos - m_LastMousePos;
				m_LastMousePos = ctx.Input().MousePos;
				m_OrbitYaw += delta.x * 0.008f;
				m_OrbitPitch = std::clamp(m_OrbitPitch - delta.y * 0.008f, -1.4f, 1.4f);
			}
			else
			{
				m_IsOrbiting = false;
			}
		}

		// 3D 投影变换
		const glm::vec3 eye {
			m_CameraFocus.x + m_CameraDistance * std::cos(m_OrbitPitch) * std::sin(m_OrbitYaw),
			m_CameraFocus.y + m_CameraDistance * std::sin(m_OrbitPitch),
			m_CameraFocus.z + m_CameraDistance * std::cos(m_OrbitPitch) * std::cos(m_OrbitYaw)
		};
		const glm::mat4 view = glm::lookAt(eye, m_CameraFocus, glm::vec3(0.0f, 1.0f, 0.0f));
		const float aspect = std::max(0.1f, vpRect.W / std::max(1.0f, vpRect.H));
		const glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
		const glm::mat4 vpMat = proj * view;

		auto projectPoint = [&](const glm::vec3& p, glm::vec2& outScreen) -> bool {
			const glm::vec4 clip = vpMat * glm::vec4(p, 1.0f);
			if (clip.w <= 0.05f) return false;
			const float invW = 1.0f / clip.w;
			const float ndcX = clip.x * invW;
			const float ndcY = clip.y * invW;
			outScreen.x = vpRect.X + (ndcX * 0.5f + 0.5f) * vpRect.W;
			outScreen.y = vpRect.Y + (-ndcY * 0.5f + 0.5f) * vpRect.H;
			return (outScreen.x >= vpRect.X && outScreen.x <= vpRect.X + vpRect.W &&
			        outScreen.y >= vpRect.Y && outScreen.y <= vpRect.Y + vpRect.H);
		};

		// 绘制地面 3D 网格基准面 (Grid Plane at Y=0)
		const Wui::WuiColor gridCol { 0.22f, 0.26f, 0.32f, 0.45f };
		for (int line = -3; line <= 3; ++line)
		{
			const float coord = static_cast<float>(line) * 1.0f;
			glm::vec2 s0, s1;
			if (projectPoint(glm::vec3(coord, 0.0f, -3.0f), s0) && projectPoint(glm::vec3(coord, 0.0f, 3.0f), s1))
			{
				Wui::BarSurface(ctx, { std::min(s0.x, s1.x), std::min(s0.y, s1.y), std::max(1.0f, std::abs(s1.x - s0.x)), std::max(1.0f, std::abs(s1.y - s0.y)) }, (line == 0 ? theme.Accent : gridCol), gridCol);
			}
			if (projectPoint(glm::vec3(-3.0f, 0.0f, coord), s0) && projectPoint(glm::vec3(3.0f, 0.0f, coord), s1))
			{
				Wui::BarSurface(ctx, { std::min(s0.x, s1.x), std::min(s0.y, s1.y), std::max(1.0f, std::abs(s1.x - s0.x)), std::max(1.0f, std::abs(s1.y - s0.y)) }, (line == 0 ? Wui::WuiColor { 0.9f, 0.3f, 0.3f, 0.6f } : gridCol), gridCol);
			}
		}

		// 绘制中心发射器 Gizmo
		glm::vec2 originScreen;
		if (projectPoint(glm::vec3(0.0f, 0.0f, 0.0f), originScreen))
		{
			Wui::BarSurface(ctx, { originScreen.x - 3.0f, originScreen.y - 3.0f, 6.0f, 6.0f }, theme.Accent, theme.Accent);
		}

		// 实时渲染粒子四边形
		size_t totalActiveParticles = 0;
		for (size_t l = 0; l < m_LivePools.size(); ++l)
		{
			const auto& pool = m_LivePools[l];
			totalActiveParticles += pool->ActiveCount();
			for (const auto& p : pool->GetParticles())
			{
				if (!p.Active) continue;
				glm::vec2 pScreen;
				if (projectPoint(p.Position, pScreen))
				{
					const glm::vec4 clip = vpMat * glm::vec4(p.Position, 1.0f);
					const float pSize = std::clamp(p.Size.x * 320.0f / clip.w, 3.0f, 28.0f);
					const Wui::WuiRect pRect { pScreen.x - pSize * 0.5f, pScreen.y - pSize * 0.5f, pSize, pSize };

					Wui::WuiColor drawCol {
						std::clamp(p.Color.r, 0.0f, 1.0f),
						std::clamp(p.Color.g, 0.0f, 1.0f),
						std::clamp(p.Color.b, 0.0f, 1.0f),
						std::clamp(p.Color.a, 0.0f, 1.0f)
					};
					Wui::BarSurface(ctx, pRect, drawCol, drawCol);
				}
			}
		}

		// 视口悬浮 HUD
		const Wui::WuiRect hudRect { vpRect.X + 8.0f, vpRect.Y + 8.0f, 190.0f, 52.0f };
		Wui::BarSurface(ctx, hudRect, Wui::WuiColor { 0.05f, 0.07f, 0.09f, 0.75f }, theme.Border);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 6.0f }, "● 3D 视口 (3D Orbit Viewport)", theme.Accent, 11.0f);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 22.0f }, "活跃粒子: " + std::to_string(totalActiveParticles) + " | FPS: 60", theme.Text, 11.5f);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 36.0f }, "拖拽: 旋转视角 | 滚轮: 缩放距离", theme.TextMuted, 10.5f);

		// 视口底部走带控制岛
		const Wui::WuiRect transIsland { vpRect.X + 8.0f, vpRect.Y + vpRect.H - 38.0f, vpRect.W - 16.0f, 30.0f };
		Wui::BarSurface(ctx, transIsland, Wui::WuiColor { 0.06f, 0.08f, 0.10f, 0.85f }, theme.Border);

		float tx = transIsland.X + 6.0f;
		const float tBtnW = 54.0f;

		// 播放/暂停
		const Wui::WuiRect pBtn { tx, transIsland.Y + 3.0f, tBtnW, 24.0f };
		Wui::Tooltip(ctx, pBtn, m_IsPlaying ? "暂停视口模拟 (Pause)" : "开始视口模拟 (Play)");
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.play"), pBtn, m_IsPlaying ? "暂停" : "播放", theme))
		{
			m_IsPlaying = !m_IsPlaying;
		}
		tx += tBtnW + 6.0f;

		// 重置
		const Wui::WuiRect rBtn { tx, transIsland.Y + 3.0f, tBtnW, 24.0f };
		Wui::Tooltip(ctx, rBtn, "重置粒子池从第 0 秒开始模拟 (Restart)");
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.restart"), rBtn, "重置", theme))
		{
			SyncSimulationFromAsset();
		}
		tx += tBtnW + 6.0f;

		// 单帧步进
		const Wui::WuiRect sBtn { tx, transIsland.Y + 3.0f, 66.0f, 24.0f };
		Wui::Tooltip(ctx, sBtn, "单步推演 0.016 秒 (Step +16ms)");
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.step"), sBtn, "步进+16ms", theme))
		{
			m_IsPlaying = false;
			StepSimulation(0.0166f);
		}
		tx += 66.0f + 12.0f;

		// 倍速选择
		Wui::Label(ctx, { tx, transIsland.Y + 7.0f }, "倍速:", theme.TextMuted, 11.5f);
		tx += 36.0f;
		const float spdW = 42.0f;

		const Wui::WuiRect spd1 { tx, transIsland.Y + 3.0f, spdW, 24.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.spd.half"), spd1, "0.5x", theme)) m_PlaybackSpeed = 0.5f;
		tx += spdW + 4.0f;

		const Wui::WuiRect spd2 { tx, transIsland.Y + 3.0f, spdW, 24.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.spd.one"), spd2, "1.0x", theme)) m_PlaybackSpeed = 1.0f;
		tx += spdW + 4.0f;

		const Wui::WuiRect spd3 { tx, transIsland.Y + 3.0f, spdW, 24.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.spd.two"), spd3, "2.0x", theme)) m_PlaybackSpeed = 2.0f;
		tx += spdW + 12.0f;

		Wui::Label(ctx, { tx, transIsland.Y + 7.0f }, "时间: " + std::to_string(m_SimTime).substr(0, 4) + "s", theme.Text, 11.5f);


		// ---- 右侧模块卡片堆栈 ----
		const float rightX = vpRect.X + vpRect.W + gap;
		float ry = curY;

		// 发射器层标签
		const Wui::WuiRect layerHeader { rightX, ry, rightW, 28.0f };
		Wui::BarSurface(ctx, layerHeader, theme.PanelHeader, theme.Border);
		Wui::Label(ctx, { layerHeader.X + 8.0f, layerHeader.Y + 7.0f }, "发射器层级:", theme.TextMuted, 11.5f);

		float lx = layerHeader.X + 80.0f;
		for (size_t i = 0; i < m_Asset.Emitters.size(); ++i)
		{
			const std::string name = m_Asset.Emitters[i].Name.empty() ? ("Layer" + std::to_string(i + 1)) : m_Asset.Emitters[i].Name;
			const float tabW = 80.0f;
			const Wui::WuiRect tabRect { lx, layerHeader.Y + 3.0f, tabW, 22.0f };
			if (Wui::Button(ctx, Wui::HashId(("vfx.em.tab." + std::to_string(i)).c_str()), tabRect, name.c_str(), theme))
			{
				m_SelectedEmitterIndex = i;
			}
			lx += tabW + 6.0f;
		}

		// 追加新层按钮 [+]
		const Wui::WuiRect addBtn { lx, layerHeader.Y + 3.0f, 26.0f, 22.0f };
		Wui::Tooltip(ctx, addBtn, "添加新的发射器层 (Add Emitter Layer)");
		if (Wui::Button(ctx, Wui::HashId("vfx.em.add"), addBtn, "+", theme))
		{
			ParticleEmitterLayer layer;
			layer.Name = "Layer" + std::to_string(m_Asset.Emitters.size() + 1);
			m_Asset.Emitters.push_back(layer);
			m_SelectedEmitterIndex = m_Asset.Emitters.size() - 1;
			m_Dirty = true;
			SyncSimulationFromAsset();
		}

		ry += layerHeader.H + 8.0f;

		// 选中发射器的模块卡片
		if (m_SelectedEmitterIndex < m_Asset.Emitters.size())
		{
			auto& em = m_Asset.Emitters[m_SelectedEmitterIndex];

			// Card 1: Emitter & Spawning
			const Wui::WuiRect c1 { rightX, ry, rightW, 70.0f };
			Wui::BarSurface(ctx, c1, theme.PanelBg, theme.Border);
			Wui::Label(ctx, { c1.X + 10.0f, c1.Y + 6.0f }, "▶ [x] 发射参数 (Emitter & Spawning)", theme.Accent, 12.0f);

			// 发射率微调
			Wui::Label(ctx, { c1.X + 16.0f, c1.Y + 28.0f }, "发射率 (Rate): " + std::to_string(static_cast<int>(em.EmissionRate)) + " p/s", theme.Text, 11.5f);
			const Wui::WuiRect rSub { c1.X + 180.0f, c1.Y + 26.0f, 32.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId("vfx.r.sub"), rSub, "-10", theme))
			{
				em.EmissionRate = std::max(0.0f, em.EmissionRate - 10.0f);
				m_Dirty = true;
			}
			const Wui::WuiRect rAdd { c1.X + 216.0f, c1.Y + 26.0f, 32.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId("vfx.r.add"), rAdd, "+10", theme))
			{
				em.EmissionRate += 10.0f;
				m_Dirty = true;
			}

			// 寿命微调
			Wui::Label(ctx, { c1.X + 16.0f, c1.Y + 48.0f }, "粒子寿命 (Life): " + std::to_string(em.Lifetime).substr(0, 4) + "s", theme.Text, 11.5f);
			const Wui::WuiRect lSub { c1.X + 180.0f, c1.Y + 46.0f, 32.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId("vfx.l.sub"), lSub, "-0.2", theme))
			{
				em.Lifetime = std::max(0.1f, em.Lifetime - 0.2f);
				m_Dirty = true;
			}
			const Wui::WuiRect lAdd { c1.X + 216.0f, c1.Y + 46.0f, 32.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId("vfx.l.add"), lAdd, "+0.2", theme))
			{
				em.Lifetime += 0.2f;
				m_Dirty = true;
			}
			ry += c1.H + 8.0f;

			// Card 2: Material & Sub-UV
			const Wui::WuiRect c2 { rightX, ry, rightW, 64.0f };
			Wui::BarSurface(ctx, c2, theme.PanelBg, theme.Border);
			Wui::Label(ctx, { c2.X + 10.0f, c2.Y + 6.0f }, "▶ [x] 材质与混合 (Material & Blend)", theme.Accent, 12.0f);

			std::string bModeStr = (em.Material.BlendMode == ParticleBlendMode::Additive) ? "发光叠加 (Additive)" : "透明混合 (AlphaBlend)";
			Wui::Label(ctx, { c2.X + 16.0f, c2.Y + 26.0f }, "混合模式: " + bModeStr, theme.Text, 11.5f);
			const Wui::WuiRect toggleBlend { c2.X + 200.0f, c2.Y + 24.0f, 60.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId("vfx.m.toggle"), toggleBlend, "切换", theme))
			{
				em.Material.BlendMode = (em.Material.BlendMode == ParticleBlendMode::Additive) ? ParticleBlendMode::AlphaBlend : ParticleBlendMode::Additive;
				m_Dirty = true;
				SyncSimulationFromAsset();
			}

			Wui::Label(ctx, { c2.X + 16.0f, c2.Y + 44.0f }, "发光增强 (Emissive): " + std::to_string(em.Material.EmissiveBoost).substr(0, 3) + "x", theme.Text, 11.5f);
			const Wui::WuiRect bSub { c2.X + 200.0f, c2.Y + 42.0f, 28.0f, 18.0f };
			if (Wui::Button(ctx, Wui::HashId("vfx.b.sub"), bSub, "-0.5", theme))
			{
				em.Material.EmissiveBoost = std::max(0.5f, em.Material.EmissiveBoost - 0.5f);
				m_Dirty = true;
			}
			const Wui::WuiRect bAdd { c2.X + 232.0f, c2.Y + 42.0f, 28.0f, 18.0f };
			if (Wui::Button(ctx, Wui::HashId("vfx.b.add"), bAdd, "+0.5", theme))
			{
				em.Material.EmissiveBoost += 0.5f;
				m_Dirty = true;
			}
			ry += c2.H + 8.0f;

			// Card 3: Motion & Forces
			const Wui::WuiRect c3 { rightX, ry, rightW, 64.0f };
			Wui::BarSurface(ctx, c3, theme.PanelBg, theme.Border);
			Wui::Label(ctx, { c3.X + 10.0f, c3.Y + 6.0f }, "▶ [x] 运动与重力 (Motion & Forces)", theme.Accent, 12.0f);

			Wui::Label(ctx, { c3.X + 16.0f, c3.Y + 26.0f }, "初速度 (Speed): " + std::to_string(em.Speed).substr(0, 4), theme.Text, 11.5f);
			const Wui::WuiRect sSub { c3.X + 180.0f, c3.Y + 24.0f, 32.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId("vfx.s.sub"), sSub, "-1.0", theme))
			{
				em.Speed = std::max(0.0f, em.Speed - 1.0f);
				m_Dirty = true;
			}
			const Wui::WuiRect sAdd { c3.X + 216.0f, c3.Y + 24.0f, 32.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId("vfx.s.add"), sAdd, "+1.0", theme))
			{
				em.Speed += 1.0f;
				m_Dirty = true;
			}

			Wui::Label(ctx, { c3.X + 16.0f, c3.Y + 44.0f }, "重力 (Gravity Y): " + std::to_string(em.Gravity.y).substr(0, 4), theme.Text, 11.5f);
			const Wui::WuiRect gSub { c3.X + 180.0f, c3.Y + 42.0f, 32.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId("vfx.g.sub"), gSub, "-1.0", theme))
			{
				em.Gravity.y -= 1.0f;
				m_Dirty = true;
			}
			const Wui::WuiRect gAdd { c3.X + 216.0f, c3.Y + 42.0f, 32.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId("vfx.g.add"), gAdd, "+1.0", theme))
			{
				em.Gravity.y += 1.0f;
				m_Dirty = true;
			}
			ry += c3.H + 8.0f;

			// Card 4: Physics Collision & Events
			const Wui::WuiRect c4 { rightX, ry, rightW, 64.0f };
			Wui::BarSurface(ctx, c4, theme.PanelBg, theme.Border);
			Wui::Label(ctx, { c4.X + 10.0f, c4.Y + 6.0f }, "▶ [x] 物理碰撞与反弹 (Collision & Bounce)", theme.Accent, 12.0f);

			std::string cModeStr = (em.Collision.Mode == ParticleCollisionMode::Plane) ? "地面平面反弹 (Plane)" : "关闭碰撞 (None)";
			Wui::Label(ctx, { c4.X + 16.0f, c4.Y + 26.0f }, "碰撞模式: " + cModeStr, theme.Text, 11.5f);
			const Wui::WuiRect cToggle { c4.X + 180.0f, c4.Y + 24.0f, 60.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId("vfx.c.toggle"), cToggle, "开关", theme))
			{
				em.Collision.Mode = (em.Collision.Mode == ParticleCollisionMode::Plane) ? ParticleCollisionMode::None : ParticleCollisionMode::Plane;
				em.Collision.PlaneOrigin = glm::vec3(0.0f, 0.0f, 0.0f);
				em.Collision.PlaneNormal = glm::vec3(0.0f, 1.0f, 0.0f);
				m_Dirty = true;
				SyncSimulationFromAsset();
			}

			Wui::Label(ctx, { c4.X + 16.0f, c4.Y + 44.0f }, "反弹系数: " + std::to_string(em.Collision.Restitution).substr(0, 4) + " | 阻尼: " + std::to_string(em.Collision.Friction).substr(0, 4), theme.TextMuted, 11.0f);
		}
	}
}
