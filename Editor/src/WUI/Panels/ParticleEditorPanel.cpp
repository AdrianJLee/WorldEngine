#include "wldpch.h"
#include <glm/vec2.hpp>
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

	void ParticleEditorPanel::NewAsset()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "NewVFX";
		m_Asset.Seed = 1337;

		ParticleEmitterLayer em;
		em.Name = "CoreEmitter";
		em.MaxParticles = 500;
		em.Lifetime = 2.0f;
		em.EmissionRate = 50.0f;
		m_Asset.Emitters.push_back(em);

		m_LoadedPath = "assets/vfx/new_vfx.wvfx";
		m_Dirty = true;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "新建粒子特效资产";
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
			else
			{
				NewAsset();
			}
		}

		float curY = rect.Y + 8.0f;
		const float padX = 12.0f;
		const float contentW = rect.W - padX * 2.0f;

		// 1. Top Bar: Title & Actions
		const Wui::WuiRect topBar { rect.X + padX, curY, contentW, 36.0f };
		Wui::BarSurface(ctx, topBar, theme.PanelHeader, theme.Border);

		Wui::Label(ctx, { topBar.X + 12.0f, topBar.Y + 9.0f }, "粒子系统编辑器 (Particle System Editor)", theme.Accent, 13.5f);

		std::string titleTag = m_LoadedPath.empty() ? "[未命名资产]" : (m_LoadedPath + (m_Dirty ? " *" : ""));
		Wui::Label(ctx, { topBar.X + 260.0f, topBar.Y + 10.0f }, "当前文件: " + titleTag, m_Dirty ? Wui::WuiColor { 1.0f, 0.7f, 0.2f, 1.0f } : theme.TextMuted, 11.5f);

		const float btnW = 80.0f;
		// New Button
		const Wui::WuiRect newBtnRect { topBar.X + topBar.W - btnW * 3.0f - 24.0f, topBar.Y + 6.0f, btnW, 24.0f };
		Wui::Tooltip(ctx, newBtnRect, "新建粒子特效资产 (.wvfx)");
		if (Wui::Button(ctx, Wui::HashId("vfx.btn.new"), newBtnRect, "新建", theme))
		{
			NewAsset();
		}

		// Save Button
		const Wui::WuiRect saveBtnRect { topBar.X + topBar.W - btnW * 2.0f - 16.0f, topBar.Y + 6.0f, btnW, 24.0f };
		Wui::Tooltip(ctx, saveBtnRect, "保存当前粒子特效资产修改 (Ctrl+S)");
		if (Wui::Button(ctx, Wui::HashId("vfx.btn.save"), saveBtnRect, m_Dirty ? "保存 *" : "保存", theme))
		{
			SaveAsset();
		}

		// Explorer Button
		const Wui::WuiRect expBtnRect { topBar.X + topBar.W - btnW - 8.0f, topBar.Y + 6.0f, btnW, 24.0f };
		Wui::Tooltip(ctx, expBtnRect, "在资源管理器中定位当前文件");
		if (Wui::Button(ctx, Wui::HashId("vfx.btn.explorer"), expBtnRect, "定位", theme))
		{
			std::filesystem::path full = m_LoadedPath;
			if (!full.is_absolute()) full = World::Paths::AssetRoot() / m_LoadedPath;
			OpenInExplorer(full);
		}

		curY += topBar.H + 8.0f;

		// 2. Playback Transport Bar
		const Wui::WuiRect transportBar { rect.X + padX, curY, contentW, 32.0f };
		Wui::BarSurface(ctx, transportBar, theme.PanelBg, theme.Border);

		const float transBtnW = 60.0f;
		float tX = transportBar.X + 8.0f;

		const Wui::WuiRect playBtnRect { tX, transportBar.Y + 4.0f, transBtnW, 24.0f };
		Wui::Tooltip(ctx, playBtnRect, m_IsPlaying ? "暂停视口模拟 (Pause)" : "开始视口模拟 (Play)");
		if (Wui::Button(ctx, Wui::HashId("vfx.btn.play"), playBtnRect, m_IsPlaying ? "暂停" : "播放", theme))
		{
			m_IsPlaying = !m_IsPlaying;
		}
		tX += transBtnW + 8.0f;

		const Wui::WuiRect restartBtnRect { tX, transportBar.Y + 4.0f, transBtnW, 24.0f };
		Wui::Tooltip(ctx, restartBtnRect, "重置粒子池并重新从第 0 秒发射");
		if (Wui::Button(ctx, Wui::HashId("vfx.btn.restart"), restartBtnRect, "重置", theme))
		{
			m_TimelineScrub = 0.0f;
			m_StatusFeedback = "粒子池已重置";
		}
		tX += transBtnW + 16.0f;

		// Seed Display
		Wui::Label(ctx, { tX, transportBar.Y + 8.0f }, "固定种子 (Seed): " + std::to_string(m_Asset.Seed), theme.Text, 11.5f);
		tX += 140.0f;

		// Status Feedback
		if (!m_StatusFeedback.empty())
		{
			Wui::Label(ctx, { tX, transportBar.Y + 8.0f }, m_StatusFeedback, theme.TextMuted, 11.0f);
		}

		curY += transportBar.H + 8.0f;

		// 3. Emitter Layer Tabs
		const Wui::WuiRect layerBar { rect.X + padX, curY, contentW, 28.0f };
		Wui::BarSurface(ctx, layerBar, theme.PanelHeader, theme.Border);

		float lX = layerBar.X + 8.0f;
		Wui::Label(ctx, { lX, layerBar.Y + 7.0f }, "发射器层级:", theme.TextMuted, 11.5f);
		lX += 80.0f;

		for (size_t i = 0; i < m_Asset.Emitters.size(); ++i)
		{
			const bool isSelected = (i == m_SelectedEmitterIndex);
			const std::string tabTitle = m_Asset.Emitters[i].Name.empty() ? ("Layer " + std::to_string(i + 1)) : m_Asset.Emitters[i].Name;
			const float tabW = 90.0f;
			const Wui::WuiRect tabRect { lX, layerBar.Y + 3.0f, tabW, 22.0f };

			if (Wui::Button(ctx, Wui::HashId(("vfx.layer.tab." + std::to_string(i)).c_str()), tabRect, tabTitle.c_str(), theme))
			{
				m_SelectedEmitterIndex = i;
			}
			lX += tabW + 6.0f;
		}

		// Add Layer Button [+]
		const Wui::WuiRect addLayerRect { lX, layerBar.Y + 3.0f, 32.0f, 22.0f };
		Wui::Tooltip(ctx, addLayerRect, "在当前复合资产中追加新的发射器层 (Add Emitter Layer)");
		if (Wui::Button(ctx, Wui::HashId("vfx.layer.add"), addLayerRect, "+", theme))
		{
			ParticleEmitterLayer newLayer;
			newLayer.Name = "Layer" + std::to_string(m_Asset.Emitters.size() + 1);
			m_Asset.Emitters.push_back(newLayer);
			m_SelectedEmitterIndex = m_Asset.Emitters.size() - 1;
			m_Dirty = true;
		}

		curY += layerBar.H + 12.0f;

		// 4. Module Card Stack for Selected Emitter
		if (m_SelectedEmitterIndex < m_Asset.Emitters.size())
		{
			auto& em = m_Asset.Emitters[m_SelectedEmitterIndex];

			// Card 1: Spawning & Emitter Shape
			const Wui::WuiRect card1Rect { rect.X + padX, curY, contentW, 56.0f };
			Wui::BarSurface(ctx, card1Rect, theme.PanelBg, theme.Border);
			Wui::Label(ctx, { card1Rect.X + 12.0f, card1Rect.Y + 8.0f }, "▶ [x] 发射与形状 (Emitter & Spawning)", theme.Accent, 12.0f);

			std::string statsStr = "最大粒子: " + std::to_string(em.MaxParticles) + " | 发射率: " + std::to_string(static_cast<int>(em.EmissionRate)) + " p/s | 寿命: " + std::to_string(em.Lifetime) + "s";
			Wui::Label(ctx, { card1Rect.X + 24.0f, card1Rect.Y + 30.0f }, statsStr, theme.Text, 11.5f);
			curY += card1Rect.H + 8.0f;

			// Card 2: Material & Sub-UV
			const Wui::WuiRect card2Rect { rect.X + padX, curY, contentW, 56.0f };
			Wui::BarSurface(ctx, card2Rect, theme.PanelBg, theme.Border);
			Wui::Label(ctx, { card2Rect.X + 12.0f, card2Rect.Y + 8.0f }, "▶ [x] 材质与序列帧 (Material & Sub-UV)", theme.Accent, 12.0f);

			std::string matStr = "混合模式: " + std::to_string(static_cast<int>(em.Material.BlendMode)) + " | Sub-UV切片: " + std::to_string(em.Material.SubUVTilesX) + "x" + std::to_string(em.Material.SubUVTilesY) + " | HDR发光: " + std::to_string(em.Material.EmissiveBoost);
			Wui::Label(ctx, { card2Rect.X + 24.0f, card2Rect.Y + 30.0f }, matStr, theme.Text, 11.5f);
			curY += card2Rect.H + 8.0f;

			// Card 3: Motion & Forces
			const Wui::WuiRect card3Rect { rect.X + padX, curY, contentW, 56.0f };
			Wui::BarSurface(ctx, card3Rect, theme.PanelBg, theme.Border);
			Wui::Label(ctx, { card3Rect.X + 12.0f, card3Rect.Y + 8.0f }, "▶ [x] 速度与运动力场 (Motion & Forces)", theme.Accent, 12.0f);

			std::string motStr = "初始初速: " + std::to_string(em.Speed) + " (±" + std::to_string(em.SpeedVariance) + ") | 重力: (" + std::to_string(em.Gravity.x) + ", " + std::to_string(em.Gravity.y) + ", " + std::to_string(em.Gravity.z) + ")";
			Wui::Label(ctx, { card3Rect.X + 24.0f, card3Rect.Y + 30.0f }, motStr, theme.Text, 11.5f);
			curY += card3Rect.H + 8.0f;

			// Card 4: Physics Collision & Events
			const Wui::WuiRect card4Rect { rect.X + padX, curY, contentW, 56.0f };
			Wui::BarSurface(ctx, card4Rect, theme.PanelBg, theme.Border);
			Wui::Label(ctx, { card4Rect.X + 12.0f, card4Rect.Y + 8.0f }, "▶ [x] 物理碰撞与事件通信 (Collision & Events)", theme.Accent, 12.0f);

			std::string colStr = "碰撞模式: " + std::to_string(static_cast<int>(em.Collision.Mode)) + " | 反弹弹性: " + std::to_string(em.Collision.Restitution) + " | 摩擦阻尼: " + std::to_string(em.Collision.Friction) + " | 队列通信: " + (em.Collision.EnableEvents ? "开启" : "关闭(零开销)");
			Wui::Label(ctx, { card4Rect.X + 24.0f, card4Rect.Y + 30.0f }, colStr, theme.Text, 11.5f);
		}
	}
}
