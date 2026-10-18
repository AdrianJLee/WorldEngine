#include "wldpch.h"
#include "WUI/Panels/ParticleEditorPanel.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Utils/Paths.h"
#include "World/Core/KeyCodes.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>

namespace World
{
	std::string ParticleEditorPanel::s_PendingOpenPath;

	void ParticleEditorPanel::RequestOpenAsset(const std::string& path)
	{
		s_PendingOpenPath = path;
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
					if (ext == ".wvfx")
					{
						std::string rel = std::filesystem::relative(entry.path(), assetRoot, ec).generic_string();
						m_AvailableAssets.push_back(rel);
					}
				}
			}
		}
	}

	void ParticleEditorPanel::SyncCodeFromAsset()
	{
		m_CodeBuffer = ParticleAssetSerializer::SerializeToString(m_Asset);
	}

	void ParticleEditorPanel::ApplyCodeToAsset()
	{
		ParticleAsset parsed;
		if (ParticleAssetSerializer::DeserializeFromString(m_CodeBuffer, parsed))
		{
			m_Asset = std::move(parsed);
			m_Dirty = true;
			m_SelectedEmitterIndex = std::min(m_SelectedEmitterIndex, m_Asset.Emitters.empty() ? 0 : m_Asset.Emitters.size() - 1);
			SyncSimulationFromAsset();
			m_StatusFeedback = "✓ YAML 代码应用成功，视口已热重载！";
		}
		else
		{
			m_StatusFeedback = "✗ YAML 语法或字段解析失败，请检查格式与缩进！";
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
			pool->SetLinearDrag(em.LinearDrag);
			pool->SetVortex(em.VortexStrength, em.VortexAxis);
			pool->SetTurbulence(em.TurbulenceStrength, em.TurbulenceFrequency);
			for (const auto& b : em.Bursts)
				pool->AddBurst(b.Time, b.Count);
			for (const auto& sub : em.SubEmitters)
				pool->AddSubEmitter(sub);
			m_LivePools.push_back(std::move(pool));
		}
		m_SimTime = 0.0f;
		SyncCodeFromAsset();
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
			if (!em.Enabled) continue;
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
			comp.RenderMode = em.FacingMode;
			comp.BlendMode = em.Material.BlendMode;
			comp.LockToXYPlane = em.LockToXY;
			comp.IsPlaying = 1;
			comp.Looping = m_Asset.Looping ? 1 : 0;
			comp.ColorStart = 0xFFFFFFFF;
			comp.ColorEnd = 0x00FFFFFF;

			pool->SetCollisionSettings(em.Collision);
			pool->SetLinearDrag(em.LinearDrag);
			pool->SetVortex(em.VortexStrength, em.VortexAxis);
			pool->SetTurbulence(em.TurbulenceStrength, em.TurbulenceFrequency);
			pool->Update(dt * m_PlaybackSpeed, comp, glm::vec3(0.0f));
		}
	}

	void ParticleEditorPanel::NewAsset()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "CampfireEffect";
		m_Asset.Seed = 1337;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.5f;

		// 1. Core Flame
		ParticleEmitterLayer flame;
		flame.Name = "CoreFlame";
		flame.Enabled = true;
		flame.MaxParticles = 400;
		flame.Lifetime = 1.4f;
		flame.EmissionRate = 75.0f;
		flame.EmitterType = ParticleEmitterType::Cone;
		flame.FacingMode = ParticleRenderMode::Billboard;
		flame.Material.BlendMode = ParticleBlendMode::Additive;
		flame.Material.EmissiveBoost = 2.5f;
		flame.Speed = 3.2f;
		flame.SpeedVariance = 0.6f;
		flame.Gravity = glm::vec3(0.0f, 2.5f, 0.0f);
		flame.LinearDrag = 0.4f;
		flame.TurbulenceStrength = 1.2f;
		flame.SizeStart = 0.65f;
		flame.SizeEnd = 0.12f;
		flame.ColorStart = glm::vec4(1.0f, 0.7f, 0.2f, 1.0f);
		flame.ColorEnd = glm::vec4(0.9f, 0.1f, 0.0f, 0.0f);

		// 2. Rising Smoke
		ParticleEmitterLayer smoke;
		smoke.Name = "RisingSmoke";
		smoke.Enabled = true;
		smoke.MaxParticles = 200;
		smoke.Lifetime = 2.5f;
		smoke.EmissionRate = 25.0f;
		smoke.EmitterType = ParticleEmitterType::Cone;
		smoke.FacingMode = ParticleRenderMode::Billboard;
		smoke.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		smoke.Material.SoftParticlesEnabled = true;
		smoke.Material.EmissiveBoost = 1.0f;
		smoke.Speed = 1.8f;
		smoke.SpeedVariance = 0.4f;
		smoke.Gravity = glm::vec3(0.0f, 1.2f, 0.0f);
		smoke.LinearDrag = 0.8f;
		smoke.TurbulenceStrength = 0.8f;
		smoke.SizeStart = 0.3f;
		smoke.SizeEnd = 1.4f;
		smoke.ColorStart = glm::vec4(0.35f, 0.35f, 0.35f, 0.55f);
		smoke.ColorEnd = glm::vec4(0.15f, 0.15f, 0.15f, 0.0f);

		// 3. Floating Embers
		ParticleEmitterLayer embers;
		embers.Name = "FloatingEmbers";
		embers.Enabled = true;
		embers.MaxParticles = 250;
		embers.Lifetime = 2.0f;
		embers.EmissionRate = 35.0f;
		embers.EmitterType = ParticleEmitterType::Sphere;
		embers.FacingMode = ParticleRenderMode::StretchedVelocity;
		embers.Material.BlendMode = ParticleBlendMode::Additive;
		embers.Material.EmissiveBoost = 3.5f;
		embers.Speed = 4.5f;
		embers.SpeedVariance = 1.2f;
		embers.Gravity = glm::vec3(0.0f, 0.8f, 0.0f);
		embers.LinearDrag = 0.15f;
		embers.VortexStrength = 1.8f;
		embers.TurbulenceStrength = 1.6f;
		embers.SizeStart = 0.12f;
		embers.SizeEnd = 0.02f;
		embers.ColorStart = glm::vec4(1.0f, 0.9f, 0.4f, 1.0f);
		embers.ColorEnd = glm::vec4(1.0f, 0.2f, 0.0f, 0.0f);

		m_Asset.Emitters.push_back(flame);
		m_Asset.Emitters.push_back(smoke);
		m_Asset.Emitters.push_back(embers);

		m_LoadedPath = "vfx/campfire.wvfx";
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "新建复合特效系统: " + m_Asset.Name;
		SyncSimulationFromAsset();
	}

	void ParticleEditorPanel::LoadAsset(const std::string& path)
	{
		std::filesystem::path fullPath = path;
		if (!fullPath.is_absolute())
		{
			fullPath = World::Paths::AssetRoot() / path;
		}

		std::error_code ec;
		std::string normalized = std::filesystem::relative(fullPath, World::Paths::AssetRoot(), ec).generic_string();
		if (normalized.empty()) normalized = path;

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
		if (ctrl && keyCode == KeyCodes::Enter)
		{
			ApplyCodeToAsset();
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
			bool foundDefault = false;
			for (const auto& a : m_AvailableAssets)
			{
				if (a.find("campfire") != std::string::npos)
				{
					LoadAsset(a);
					foundDefault = true;
					break;
				}
			}
			if (!foundDefault && !m_AvailableAssets.empty())
			{
				LoadAsset(m_AvailableAssets[0]);
			}
			else if (!foundDefault)
			{
				NewAsset();
			}
		}

		if (m_IsPlaying)
		{
			StepSimulation(0.0166f);
		}

		float curY = rect.Y + 8.0f;
		const float padX = 12.0f;
		const float contentW = rect.W - padX * 2.0f;

		// 1. 顶部操作栏 (含模式切换视觉 vs 代码)
		const Wui::WuiRect topBar { rect.X + padX, curY, contentW, 36.0f };
		Wui::BarSurface(ctx, topBar, theme.PanelHeader, theme.Border);

		Wui::Label(ctx, { topBar.X + 12.0f, topBar.Y + 9.0f }, "视觉特效系统 (VFX System Editor)", theme.Accent, 13.5f);

		float tbX = topBar.X + 250.0f;

		const Wui::WuiRect newBtn { tbX, topBar.Y + 6.0f, 48.0f, 24.0f };
		Wui::Tooltip(ctx, newBtn, "新建工业级复合特效预设");
		if (Wui::Button(ctx, Wui::HashId("vfx.bar.new"), newBtn, "新建", theme))
		{
			NewAsset();
		}
		tbX += 54.0f;

		const Wui::WuiRect saveBtn { tbX, topBar.Y + 6.0f, 48.0f, 24.0f };
		Wui::Tooltip(ctx, saveBtn, "保存当前特效到 .wvfx 资产文件 (Ctrl+S)");
		if (Wui::Button(ctx, Wui::HashId("vfx.bar.save"), saveBtn, "保存", theme))
		{
			SaveAsset();
		}
		tbX += 58.0f;

		const float modeBtnW = 105.0f;
		const Wui::WuiRect visualModeBtn { tbX, topBar.Y + 6.0f, modeBtnW, 24.0f };
		Wui::Tooltip(ctx, visualModeBtn, "切换到参数可视化模块堆栈视图");
		if (Wui::Button(ctx, Wui::HashId("vfx.mode.visual"), visualModeBtn,
			m_ViewMode == ParticleEditorViewMode::Visual ? "[● 视觉检视]" : "  视觉检视", theme))
		{
			m_ViewMode = ParticleEditorViewMode::Visual;
		}
		tbX += modeBtnW + 6.0f;

		const Wui::WuiRect codeModeBtn { tbX, topBar.Y + 6.0f, modeBtnW, 24.0f };
		Wui::Tooltip(ctx, codeModeBtn, "切换到声明式代码/YAML 编辑视图，可直接写代码热重载");
		if (Wui::Button(ctx, Wui::HashId("vfx.mode.code"), codeModeBtn,
			m_ViewMode == ParticleEditorViewMode::Code ? "[● 代码编辑]" : "  代码编辑", theme))
		{
			m_ViewMode = ParticleEditorViewMode::Code;
			SyncCodeFromAsset();
		}
		tbX += modeBtnW + 16.0f;

		if (!m_StatusFeedback.empty())
		{
			Wui::WuiColor fbColor = (m_StatusFeedback.find("✓") != std::string::npos || m_StatusFeedback.find("成功") != std::string::npos)
				? Wui::WuiColor { 0.3f, 0.85f, 0.4f, 1.0f }
				: (m_StatusFeedback.find("✗") != std::string::npos || m_StatusFeedback.find("失败") != std::string::npos)
					? Wui::WuiColor { 0.95f, 0.3f, 0.3f, 1.0f }
					: theme.TextMuted;
			Wui::Label(ctx, { tbX, topBar.Y + 10.0f }, m_StatusFeedback, fbColor, 11.0f);
		}

		curY += topBar.H + 8.0f;

		// 2. 双栏工业级布局 (左侧: 3D 自由交互视口 56% | 右侧: 属性/代码面板 44%)
		const float gap = 12.0f;
		const float availH = std::max(360.0f, rect.H - (curY - rect.Y) - 12.0f);
		const float leftW = std::floor((contentW - gap) * 0.56f);
		const float rightW = contentW - gap - leftW;

		// ---- 左侧 3D 粒子交互视口 ----
		const Wui::WuiRect vpRect { rect.X + padX, curY, leftW, availH };
		Wui::BarSurface(ctx, vpRect, Wui::WuiColor { 0.08f, 0.09f, 0.11f, 1.0f }, theme.Border);

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

		auto drawProjectedSegment = [&](const glm::vec3& a, const glm::vec3& b, const Wui::WuiColor& col, float thickness) {
			const int steps = 10;
			for (int s = 0; s <= steps; ++s)
			{
				float t = static_cast<float>(s) / static_cast<float>(steps);
				glm::vec3 pt = glm::mix(a, b, t);
				glm::vec2 sp;
				if (projectPoint(pt, sp))
				{
					Wui::BarSurface(ctx, { sp.x - thickness * 0.5f, sp.y - thickness * 0.5f, thickness, thickness }, col, col);
				}
			}
		};

		const Wui::WuiColor gridCol { 0.22f, 0.26f, 0.32f, 0.40f };
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

		for (int m = 1; m <= 3; ++m)
		{
			glm::vec2 smX, smZ;
			if (projectPoint(glm::vec3(static_cast<float>(m), 0.0f, 0.0f), smX))
				Wui::Label(ctx, { smX.x - 8.0f, smX.y + 4.0f }, std::to_string(m) + "m", Wui::WuiColor { 0.6f, 0.6f, 0.65f, 0.5f }, 9.5f);
			if (projectPoint(glm::vec3(0.0f, 0.0f, static_cast<float>(m)), smZ))
				Wui::Label(ctx, { smZ.x + 4.0f, smZ.y - 6.0f }, std::to_string(m) + "m", Wui::WuiColor { 0.6f, 0.6f, 0.65f, 0.5f }, 9.5f);
		}

		const Wui::WuiColor axisXCol { 0.95f, 0.22f, 0.22f, 0.95f };
		const Wui::WuiColor axisYCol { 0.22f, 0.90f, 0.32f, 0.95f };
		const Wui::WuiColor axisZCol { 0.22f, 0.55f, 0.98f, 0.95f };

		drawProjectedSegment(glm::vec3(0.0f), glm::vec3(1.5f, 0.0f, 0.0f), axisXCol, 2.0f);
		glm::vec2 xTip;
		if (projectPoint(glm::vec3(1.55f, 0.0f, 0.0f), xTip)) Wui::Label(ctx, { xTip.x + 3.0f, xTip.y - 6.0f }, "X (+1.5m)", axisXCol, 11.0f);

		drawProjectedSegment(glm::vec3(0.0f), glm::vec3(0.0f, 1.5f, 0.0f), axisYCol, 2.0f);
		glm::vec2 yTip;
		if (projectPoint(glm::vec3(0.0f, 1.55f, 0.0f), yTip)) Wui::Label(ctx, { yTip.x + 3.0f, yTip.y - 6.0f }, "Y (+1.5m)", axisYCol, 11.0f);

		drawProjectedSegment(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.5f), axisZCol, 2.0f);
		glm::vec2 zTip;
		if (projectPoint(glm::vec3(0.0f, 0.0f, 1.55f), zTip)) Wui::Label(ctx, { zTip.x + 3.0f, zTip.y - 6.0f }, "Z (+1.5m)", axisZCol, 11.0f);

		const Wui::WuiColor gizmoCol { 0.95f, 0.85f, 0.20f, 0.75f };
		if (m_SelectedEmitterIndex < m_Asset.Emitters.size())
		{
			const auto& curEm = m_Asset.Emitters[m_SelectedEmitterIndex];
			if (curEm.EmitterType == ParticleEmitterType::Cone)
			{
				const float coneH = 1.2f;
				const float coneR = 0.55f;
				const int segs = 12;
				for (int s = 0; s < segs; ++s)
				{
					float a0 = static_cast<float>(s) * 6.2831853f / static_cast<float>(segs);
					float a1 = static_cast<float>(s + 1) * 6.2831853f / static_cast<float>(segs);
					glm::vec3 p0(coneR * std::cos(a0), coneH, coneR * std::sin(a0));
					glm::vec3 p1(coneR * std::cos(a1), coneH, coneR * std::sin(a1));
					drawProjectedSegment(p0, p1, gizmoCol, 1.2f);
					if (s % 3 == 0) drawProjectedSegment(glm::vec3(0.0f), p0, gizmoCol, 1.2f);
				}
			}
			else if (curEm.EmitterType == ParticleEmitterType::Sphere)
			{
				const float spR = 0.6f;
				const int segs = 12;
				for (int s = 0; s < segs; ++s)
				{
					float a0 = static_cast<float>(s) * 6.2831853f / static_cast<float>(segs);
					float a1 = static_cast<float>(s + 1) * 6.2831853f / static_cast<float>(segs);
					drawProjectedSegment(glm::vec3(spR * std::cos(a0), spR * std::sin(a0), 0.0f), glm::vec3(spR * std::cos(a1), spR * std::sin(a1), 0.0f), gizmoCol, 1.2f);
					drawProjectedSegment(glm::vec3(spR * std::cos(a0), 0.0f, spR * std::sin(a0)), glm::vec3(spR * std::cos(a1), 0.0f, spR * std::sin(a1)), gizmoCol, 1.2f);
				}
			}
			else if (curEm.EmitterType == ParticleEmitterType::Box)
			{
				const float hs = 0.4f;
				drawProjectedSegment(glm::vec3(-hs, 0.0f, -hs), glm::vec3(hs, 0.0f, -hs), gizmoCol, 1.2f);
				drawProjectedSegment(glm::vec3(hs, 0.0f, -hs), glm::vec3(hs, 0.0f, hs), gizmoCol, 1.2f);
				drawProjectedSegment(glm::vec3(hs, 0.0f, hs), glm::vec3(-hs, 0.0f, hs), gizmoCol, 1.2f);
				drawProjectedSegment(glm::vec3(-hs, 0.0f, hs), glm::vec3(-hs, 0.0f, -hs), gizmoCol, 1.2f);
				drawProjectedSegment(glm::vec3(-hs, hs*2.0f, -hs), glm::vec3(hs, hs*2.0f, -hs), gizmoCol, 1.2f);
				drawProjectedSegment(glm::vec3(hs, hs*2.0f, -hs), glm::vec3(hs, hs*2.0f, hs), gizmoCol, 1.2f);
				drawProjectedSegment(glm::vec3(hs, hs*2.0f, hs), glm::vec3(-hs, hs*2.0f, hs), gizmoCol, 1.2f);
				drawProjectedSegment(glm::vec3(-hs, hs*2.0f, hs), glm::vec3(-hs, hs*2.0f, -hs), gizmoCol, 1.2f);
			}
		}

		glm::vec2 originScreen;
		if (projectPoint(glm::vec3(0.0f, 0.0f, 0.0f), originScreen))
		{
			Wui::BarSurface(ctx, { originScreen.x - 4.0f, originScreen.y - 4.0f, 8.0f, 8.0f }, theme.Accent, Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f });
		}

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

		const float viewBtnW = 42.0f;
		float vx = vpRect.X + vpRect.W - (viewBtnW * 4.0f + 24.0f);
		const float vy = vpRect.Y + 8.0f;
		const Wui::WuiRect vIsland { vx - 4.0f, vy - 2.0f, viewBtnW * 4.0f + 20.0f, 26.0f };
		Wui::BarSurface(ctx, vIsland, Wui::WuiColor { 0.05f, 0.07f, 0.09f, 0.75f }, theme.Border);

		const Wui::WuiRect btn3D { vx, vy, viewBtnW, 22.0f };
		Wui::Tooltip(ctx, btn3D, "3D 自由轨道透视视角 (Orbit View)");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.3d"), btn3D, "3D", theme)) { m_OrbitYaw = 0.785f; m_OrbitPitch = 0.45f; }
		vx += viewBtnW + 4.0f;

		const Wui::WuiRect btnTop { vx, vy, viewBtnW, 22.0f };
		Wui::Tooltip(ctx, btnTop, "顶视图: 垂直俯视 XZ 地面扩散 (Top View)");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.top"), btnTop, "顶视", theme)) { m_OrbitYaw = 0.0f; m_OrbitPitch = 1.45f; }
		vx += viewBtnW + 4.0f;

		const Wui::WuiRect btnFront { vx, vy, viewBtnW, 22.0f };
		Wui::Tooltip(ctx, btnFront, "正视图: 水平平视 XY 喷射高度 (Front View)");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.front"), btnFront, "正视", theme)) { m_OrbitYaw = 0.0f; m_OrbitPitch = 0.0f; }
		vx += viewBtnW + 4.0f;

		const Wui::WuiRect btnSide { vx, vy, viewBtnW, 22.0f };
		Wui::Tooltip(ctx, btnSide, "侧视图: 水平侧视 ZY 抛物线轨迹 (Side View)");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.side"), btnSide, "侧视", theme)) { m_OrbitYaw = 1.57f; m_OrbitPitch = 0.0f; }

		const glm::vec2 compassCenter { vpRect.X + 45.0f, vpRect.Y + vpRect.H - 72.0f };
		const Wui::WuiRect compassBg { compassCenter.x - 28.0f, compassCenter.y - 28.0f, 56.0f, 56.0f };
		Wui::BarSurface(ctx, compassBg, Wui::WuiColor { 0.05f, 0.07f, 0.09f, 0.80f }, theme.Border);
		Wui::Label(ctx, { compassBg.X + 4.0f, compassBg.Y + 2.0f }, "视角方位", theme.TextMuted, 9.5f);

		const glm::mat3 rotMat = glm::mat3(view);
		const glm::vec3 camX = rotMat * glm::vec3(1.0f, 0.0f, 0.0f);
		const glm::vec3 camY = rotMat * glm::vec3(0.0f, 1.0f, 0.0f);
		const glm::vec3 camZ = rotMat * glm::vec3(0.0f, 0.0f, 1.0f);

		const float armLen = 20.0f;
		const glm::vec2 sX = compassCenter + glm::vec2(camX.x, -camX.y) * armLen;
		const glm::vec2 sY = compassCenter + glm::vec2(camY.x, -camY.y) * armLen;
		const glm::vec2 sZ = compassCenter + glm::vec2(camZ.x, -camZ.y) * armLen;

		auto drawCompassArm = [&](const glm::vec2& tip, const Wui::WuiColor& c, const char* name) {
			const int segs = 6;
			for (int i = 0; i <= segs; ++i)
			{
				float t = static_cast<float>(i) / static_cast<float>(segs);
				glm::vec2 p = glm::mix(compassCenter, tip, t);
				Wui::BarSurface(ctx, { p.x - 1.0f, p.y - 1.0f, 2.0f, 2.0f }, c, c);
			}
			Wui::Label(ctx, { tip.x + (tip.x >= compassCenter.x ? 2.0f : -8.0f), tip.y - 5.0f }, name, c, 10.5f);
		};

		drawCompassArm(sX, axisXCol, "X");
		drawCompassArm(sY, axisYCol, "Y");
		drawCompassArm(sZ, axisZCol, "Z");
		Wui::BarSurface(ctx, { compassCenter.x - 2.0f, compassCenter.y - 2.0f, 4.0f, 4.0f }, Wui::WuiColor { 1.0f, 1.0f, 1.0f, 0.9f }, theme.Accent);

		const Wui::WuiRect hudRect { vpRect.X + 8.0f, vpRect.Y + 8.0f, 220.0f, 68.0f };
		Wui::BarSurface(ctx, hudRect, Wui::WuiColor { 0.05f, 0.07f, 0.09f, 0.85f }, theme.Border);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 6.0f }, "● 3D 复合特效视口 (VFX Viewport)", theme.Accent, 11.5f);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 22.0f }, "活跃粒子: " + std::to_string(totalActiveParticles) + " | 发射层: " + std::to_string(m_Asset.Emitters.size()), theme.Text, 11.0f);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 36.0f }, "模拟时间: " + std::to_string(m_SimTime).substr(0, 5) + "s", theme.Text, 10.5f);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 50.0f }, "左键拖拽旋转 | 滚轮推拉视距", theme.TextMuted, 10.0f);

		const Wui::WuiRect transIsland { vpRect.X + 8.0f, vpRect.Y + vpRect.H - 38.0f, vpRect.W - 16.0f, 30.0f };
		Wui::BarSurface(ctx, transIsland, Wui::WuiColor { 0.06f, 0.08f, 0.10f, 0.85f }, theme.Border);

		float tx = transIsland.X + 6.0f;
		const float tBtnW = 52.0f;

		const Wui::WuiRect pBtn { tx, transIsland.Y + 3.0f, tBtnW, 24.0f };
		Wui::Tooltip(ctx, pBtn, m_IsPlaying ? "暂停视口模拟 (Pause)" : "开始视口模拟 (Play)");
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.play"), pBtn, m_IsPlaying ? "暂停" : "播放", theme))
		{
			m_IsPlaying = !m_IsPlaying;
		}
		tx += tBtnW + 6.0f;

		const Wui::WuiRect rBtn { tx, transIsland.Y + 3.0f, tBtnW, 24.0f };
		Wui::Tooltip(ctx, rBtn, "重置粒子池从第 0 秒开始模拟 (Restart)");
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.restart"), rBtn, "重置", theme))
		{
			SyncSimulationFromAsset();
		}
		tx += tBtnW + 6.0f;

		const Wui::WuiRect stepBtn { tx, transIsland.Y + 3.0f, tBtnW, 24.0f };
		Wui::Tooltip(ctx, stepBtn, "单步推演 1 帧 (Step Frame, 0.016s)");
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.step"), stepBtn, "单步", theme))
		{
			m_IsPlaying = false;
			StepSimulation(0.0166f);
		}
		tx += tBtnW + 12.0f;

		Wui::Label(ctx, { tx, transIsland.Y + 7.0f }, "倍速: " + std::to_string(m_PlaybackSpeed).substr(0, 3) + "x", theme.Text, 10.5f);
		const Wui::WuiRect spToggle { tx + 75.0f, transIsland.Y + 3.0f, 42.0f, 24.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.speed"), spToggle, "切换", theme))
		{
			if (m_PlaybackSpeed >= 2.0f) m_PlaybackSpeed = 0.25f;
			else if (m_PlaybackSpeed >= 1.0f) m_PlaybackSpeed = 2.0f;
			else if (m_PlaybackSpeed >= 0.5f) m_PlaybackSpeed = 1.0f;
			else m_PlaybackSpeed = 0.5f;
		}
		// ---- 右侧面板 (视觉检视器 vs 代码编辑器) ----
		const float rightX = rect.X + padX + leftW + gap;
		float ry = curY;

		if (m_ViewMode == ParticleEditorViewMode::Code)
		{
			// ======== 代码编辑模式 (Code / YAML Editor) ========
			const Wui::WuiRect codeToolbar { rightX, ry, rightW, 32.0f };
			Wui::BarSurface(ctx, codeToolbar, theme.PanelHeader, theme.Border);

			float cx = codeToolbar.X + 8.0f;
			const Wui::WuiRect applyBtn { cx, codeToolbar.Y + 4.0f, 130.0f, 24.0f };
			Wui::Tooltip(ctx, applyBtn, "编译并应用当前 YAML 代码到视口与特效系统 (Ctrl+Enter)");
			if (Wui::Button(ctx, Wui::HashId("vfx.code.apply"), applyBtn, "▶ 应用代码 (Ctrl+Enter)", theme))
			{
				ApplyCodeToAsset();
			}
			cx += 136.0f;

			const Wui::WuiRect syncBtn { cx, codeToolbar.Y + 4.0f, 100.0f, 24.0f };
			Wui::Tooltip(ctx, syncBtn, "从当前视口与模块参数重新生成 YAML 代码");
			if (Wui::Button(ctx, Wui::HashId("vfx.code.sync"), syncBtn, "⟳ 从视口同步", theme))
			{
				SyncCodeFromAsset();
			}
			cx += 106.0f;

			const Wui::WuiRect snipBurst { cx, codeToolbar.Y + 4.0f, 54.0f, 24.0f };
			Wui::Tooltip(ctx, snipBurst, "插入爆发 Bursts 片段代码");
			if (Wui::Button(ctx, Wui::HashId("vfx.snip.burst"), snipBurst, "+爆发", theme))
			{
				m_CodeBuffer += "\n    Bursts:\n      - Time: 0.0\n        Count: 30\n";
				ApplyCodeToAsset();
			}
			cx += 58.0f;

			const Wui::WuiRect snipVortex { cx, codeToolbar.Y + 4.0f, 54.0f, 24.0f };
			Wui::Tooltip(ctx, snipVortex, "插入涡流 Vortex 片段代码");
			if (Wui::Button(ctx, Wui::HashId("vfx.snip.vortex"), snipVortex, "+涡流", theme))
			{
				m_CodeBuffer += "      VortexStrength: 3.5\n      VortexAxis: [0, 1, 0]\n";
				ApplyCodeToAsset();
			}

			ry += codeToolbar.H + 6.0f;

			const float codeAreaH = availH - (ry - curY);
			const Wui::WuiRect codeBox { rightX, ry, rightW, codeAreaH };
			Wui::BarSurface(ctx, codeBox, Wui::WuiColor { 0.06f, 0.07f, 0.08f, 1.0f }, theme.Border);

			if (ctx.IsHovered(codeBox))
			{
				if (ctx.Input().Wheel != 0.0f)
				{
					m_CodeScrollY = std::max(0.0f, m_CodeScrollY - ctx.Input().Wheel * 30.0f);
				}
			}

			std::istringstream stream(m_CodeBuffer);
			std::string line;
			int lineIdx = 0;
			float lineY = codeBox.Y + 8.0f - m_CodeScrollY;
			const float lineHeight = 16.0f;

			while (std::getline(stream, line))
			{
				lineIdx++;
				if (lineY >= codeBox.Y - lineHeight && lineY <= codeBox.Y + codeBox.H - lineHeight)
				{
					std::string lineNumStr = (lineIdx < 10 ? "0" : "") + std::to_string(lineIdx);
					Wui::Label(ctx, { codeBox.X + 8.0f, lineY }, lineNumStr, Wui::WuiColor { 0.4f, 0.45f, 0.5f, 0.6f }, 10.0f);

					Wui::WuiColor textCol = theme.Text;
					if (line.find(":") != std::string::npos)
					{
						if (line.find("Name:") != std::string::npos || line.find("Emitters:") != std::string::npos)
							textCol = theme.Accent;
						else if (line.find("Material:") != std::string::npos || line.find("Motion:") != std::string::npos)
							textCol = Wui::WuiColor { 0.4f, 0.8f, 0.95f, 1.0f };
						else
							textCol = Wui::WuiColor { 0.85f, 0.85f, 0.9f, 1.0f };
					}
					else if (line.find("-") != std::string::npos)
					{
						textCol = Wui::WuiColor { 0.95f, 0.8f, 0.3f, 1.0f };
					}

					Wui::Label(ctx, { codeBox.X + 36.0f, lineY }, line, textCol, 10.5f);
				}
				lineY += lineHeight;
			}
		}
		else
		{
			// ======== 视觉模块堆栈模式 (Visual Inspector Mode) ========
			const Wui::WuiRect layerHeader { rightX, ry, rightW, 34.0f };
			Wui::BarSurface(ctx, layerHeader, theme.PanelHeader, theme.Border);

			float lx = layerHeader.X + 8.0f;
			Wui::Label(ctx, { lx, layerHeader.Y + 9.0f }, "发射层:", theme.Accent, 11.5f);
			lx += 52.0f;

			for (size_t i = 0; i < m_Asset.Emitters.size(); ++i)
			{
				const auto& em = m_Asset.Emitters[i];
				const bool isSelected = (i == m_SelectedEmitterIndex);
				const float tabW = 80.0f;
				const Wui::WuiRect tabRect { lx, layerHeader.Y + 4.0f, tabW, 24.0f };

				std::string tabLabel = em.Name;
				if (tabLabel.size() > 8) tabLabel = tabLabel.substr(0, 7) + "..";
				if (isSelected) tabLabel = "● " + tabLabel;

				if (Wui::Button(ctx, Wui::HashId(("vfx.tab." + std::to_string(i)).c_str()), tabRect, tabLabel.c_str(), theme))
				{
					m_SelectedEmitterIndex = i;
				}
				lx += tabW + 4.0f;
			}

			const Wui::WuiRect addLayerBtn { lx, layerHeader.Y + 4.0f, 48.0f, 24.0f };
			Wui::Tooltip(ctx, addLayerBtn, "添加新的发射器层 (Add Emitter Layer)");
			if (Wui::Button(ctx, Wui::HashId("vfx.layer.add"), addLayerBtn, "+加层", theme))
			{
				ParticleEmitterLayer layer;
				layer.Name = "Layer_" + std::to_string(m_Asset.Emitters.size() + 1);
				layer.MaxParticles = 300;
				layer.Lifetime = 1.5f;
				layer.EmissionRate = 50.0f;
				layer.Gravity = glm::vec3(0.0f, 1.0f, 0.0f);
				layer.SizeStart = 0.4f;
				layer.SizeEnd = 0.1f;
				layer.Material.BlendMode = ParticleBlendMode::Additive;
				m_Asset.Emitters.push_back(layer);
				m_SelectedEmitterIndex = m_Asset.Emitters.size() - 1;
				m_Dirty = true;
				SyncSimulationFromAsset();
			}
			lx += 52.0f;

			if (m_Asset.Emitters.size() > 1)
			{
				const Wui::WuiRect delLayerBtn { lx, layerHeader.Y + 4.0f, 48.0f, 24.0f };
				Wui::Tooltip(ctx, delLayerBtn, "删除当前选中的发射层");
				if (Wui::Button(ctx, Wui::HashId("vfx.layer.del"), delLayerBtn, "-删层", theme))
				{
					m_Asset.Emitters.erase(m_Asset.Emitters.begin() + m_SelectedEmitterIndex);
					if (m_SelectedEmitterIndex >= m_Asset.Emitters.size())
						m_SelectedEmitterIndex = m_Asset.Emitters.size() - 1;
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
			}

			ry += layerHeader.H + 8.0f;

			if (m_SelectedEmitterIndex < m_Asset.Emitters.size())
			{
				auto& em = m_Asset.Emitters[m_SelectedEmitterIndex];

				// 1. [x] Emitter & Spawning
				const Wui::WuiRect c1 { rightX, ry, rightW, 64.0f };
				Wui::BarSurface(ctx, c1, theme.PanelBg, theme.Border);
				Wui::Label(ctx, { c1.X + 10.0f, c1.Y + 6.0f }, "▶ [x] 发射与形状 (Emitter & Spawning)", theme.Accent, 11.5f);

				std::string emTypeStr = "Cone (圆锥)";
				if (em.EmitterType == ParticleEmitterType::Sphere) emTypeStr = "Sphere (球体)";
				else if (em.EmitterType == ParticleEmitterType::Box) emTypeStr = "Box (盒体)";
				else if (em.EmitterType == ParticleEmitterType::Point) emTypeStr = "Point (单点)";

				Wui::Label(ctx, { c1.X + 16.0f, c1.Y + 24.0f }, "形状: " + emTypeStr, theme.Text, 11.0f);
				const Wui::WuiRect toggleShape { c1.X + 140.0f, c1.Y + 22.0f, 54.0f, 18.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.shape.toggle"), toggleShape, "切换", theme))
				{
					if (em.EmitterType == ParticleEmitterType::Cone) em.EmitterType = ParticleEmitterType::Sphere;
					else if (em.EmitterType == ParticleEmitterType::Sphere) em.EmitterType = ParticleEmitterType::Box;
					else if (em.EmitterType == ParticleEmitterType::Box) em.EmitterType = ParticleEmitterType::Point;
					else em.EmitterType = ParticleEmitterType::Cone;
					m_Dirty = true;
					SyncSimulationFromAsset();
				}

				Wui::Label(ctx, { c1.X + 16.0f, c1.Y + 44.0f }, "发射率: " + std::to_string(static_cast<int>(em.EmissionRate)) + " p/s | 容量: " + std::to_string(em.MaxParticles), theme.Text, 10.5f);
				const Wui::WuiRect rSub { c1.X + 220.0f, c1.Y + 42.0f, 26.0f, 18.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.r.sub"), rSub, "-10", theme)) { em.EmissionRate = std::max(0.0f, em.EmissionRate - 10.0f); m_Dirty = true; }
				const Wui::WuiRect rAdd { c1.X + 250.0f, c1.Y + 42.0f, 26.0f, 18.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.r.add"), rAdd, "+10", theme)) { em.EmissionRate += 10.0f; m_Dirty = true; }
				ry += c1.H + 6.0f;

				// 2. [x] Material & Rendering
				const Wui::WuiRect c2 { rightX, ry, rightW, 64.0f };
				Wui::BarSurface(ctx, c2, theme.PanelBg, theme.Border);
				Wui::Label(ctx, { c2.X + 10.0f, c2.Y + 6.0f }, "▶ [x] 材质与渲染 (Material & Rendering)", theme.Accent, 11.5f);

				std::string bModeStr = (em.Material.BlendMode == ParticleBlendMode::Additive) ? "叠加(Additive)" : "透明(AlphaBlend)";
				Wui::Label(ctx, { c2.X + 16.0f, c2.Y + 24.0f }, "混合: " + bModeStr, theme.Text, 11.0f);
				const Wui::WuiRect toggleBlend { c2.X + 140.0f, c2.Y + 22.0f, 48.0f, 18.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.m.blend"), toggleBlend, "切换", theme))
				{
					em.Material.BlendMode = (em.Material.BlendMode == ParticleBlendMode::Additive) ? ParticleBlendMode::AlphaBlend : ParticleBlendMode::Additive;
					m_Dirty = true;
					SyncSimulationFromAsset();
				}

				std::string fModeStr = (em.FacingMode == ParticleRenderMode::StretchedVelocity) ? "速度拉伸" : (em.FacingMode == ParticleRenderMode::HorizontalQuad ? "地面水平" : "相机对齐");
				Wui::Label(ctx, { c2.X + 16.0f, c2.Y + 44.0f }, "朝向: " + fModeStr + " | 发光: " + std::to_string(em.Material.EmissiveBoost).substr(0, 3) + "x", theme.TextMuted, 10.5f);
				const Wui::WuiRect toggleFacing { c2.X + 220.0f, c2.Y + 42.0f, 54.0f, 18.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.m.facing"), toggleFacing, "切换", theme))
				{
					if (em.FacingMode == ParticleRenderMode::Billboard) em.FacingMode = ParticleRenderMode::StretchedVelocity;
					else if (em.FacingMode == ParticleRenderMode::StretchedVelocity) em.FacingMode = ParticleRenderMode::HorizontalQuad;
					else em.FacingMode = ParticleRenderMode::Billboard;
					em.Material.FacingMode = em.FacingMode;
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
				ry += c2.H + 6.0f;

				// 3. [x] Velocity & Forces
				const Wui::WuiRect c3 { rightX, ry, rightW, 76.0f };
				Wui::BarSurface(ctx, c3, theme.PanelBg, theme.Border);
				Wui::Label(ctx, { c3.X + 10.0f, c3.Y + 6.0f }, "▶ [x] 速度与力场 (Velocity & Forces)", theme.Accent, 11.5f);

				Wui::Label(ctx, { c3.X + 16.0f, c3.Y + 24.0f }, "初速: " + std::to_string(em.Speed).substr(0, 4) + " | 重力: " + std::to_string(em.Gravity.y).substr(0, 4), theme.Text, 10.5f);
				const Wui::WuiRect sSub { c3.X + 180.0f, c3.Y + 22.0f, 24.0f, 18.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.s.sub"), sSub, "-1", theme)) { em.Speed = std::max(0.0f, em.Speed - 1.0f); m_Dirty = true; }
				const Wui::WuiRect sAdd { c3.X + 208.0f, c3.Y + 22.0f, 24.0f, 18.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.s.add"), sAdd, "+1", theme)) { em.Speed += 1.0f; m_Dirty = true; }

				Wui::Label(ctx, { c3.X + 16.0f, c3.Y + 42.0f }, "阻力: " + std::to_string(em.LinearDrag).substr(0, 4) + " | 涡流: " + std::to_string(em.VortexStrength).substr(0, 4), theme.Text, 10.5f);
				const Wui::WuiRect dAdd { c3.X + 180.0f, c3.Y + 40.0f, 42.0f, 18.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.d.tog"), dAdd, "+阻力", theme)) { em.LinearDrag = (em.LinearDrag > 1.5f ? 0.0f : em.LinearDrag + 0.3f); m_Dirty = true; }
				const Wui::WuiRect vAdd { c3.X + 226.0f, c3.Y + 40.0f, 42.0f, 18.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.v.tog"), vAdd, "+涡流", theme)) { em.VortexStrength = (em.VortexStrength > 5.0f ? 0.0f : em.VortexStrength + 1.5f); m_Dirty = true; }

				Wui::Label(ctx, { c3.X + 16.0f, c3.Y + 58.0f }, "湍流扰动: " + std::to_string(em.TurbulenceStrength).substr(0, 4), theme.TextMuted, 10.0f);
				const Wui::WuiRect tTog { c3.X + 180.0f, c3.Y + 56.0f, 42.0f, 18.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.t.tog"), tTog, "+湍流", theme)) { em.TurbulenceStrength = (em.TurbulenceStrength > 3.0f ? 0.0f : em.TurbulenceStrength + 0.8f); m_Dirty = true; }
				ry += c3.H + 6.0f;

				// 4. [x] Size & Rotation
				const Wui::WuiRect c4 { rightX, ry, rightW, 56.0f };
				Wui::BarSurface(ctx, c4, theme.PanelBg, theme.Border);
				Wui::Label(ctx, { c4.X + 10.0f, c4.Y + 6.0f }, "▶ [x] 尺寸与旋转 (Size & Rotation)", theme.Accent, 11.5f);

				Wui::Label(ctx, { c4.X + 16.0f, c4.Y + 24.0f }, "尺寸: " + std::to_string(em.SizeStart).substr(0, 4) + " → " + std::to_string(em.SizeEnd).substr(0, 4), theme.Text, 11.0f);
				const Wui::WuiRect curveBtn { c4.X + 140.0f, c4.Y + 22.0f, 96.0f, 20.0f };
				Wui::Tooltip(ctx, curveBtn, "打开工业级贝塞尔曲线编辑器调节粒子生命周期尺寸曲线");
				if (Wui::Button(ctx, Wui::HashId("vfx.curve.link"), curveBtn, "~ 曲线编辑", theme))
				{
					host.OpenCurveEditor("");
				}
				ry += c4.H + 6.0f;

				// 5. [x] Color & Alpha
				const Wui::WuiRect c5 { rightX, ry, rightW, 56.0f };
				Wui::BarSurface(ctx, c5, theme.PanelBg, theme.Border);
				Wui::Label(ctx, { c5.X + 10.0f, c5.Y + 6.0f }, "▶ [x] 颜色与渐变 (Color & Alpha)", theme.Accent, 11.5f);

				const Wui::WuiColor cs { em.ColorStart.r, em.ColorStart.g, em.ColorStart.b, 1.0f };
				Wui::BarSurface(ctx, { c5.X + 16.0f, c5.Y + 26.0f, 28.0f, 16.0f }, cs, theme.Border);
				Wui::Label(ctx, { c5.X + 48.0f, c5.Y + 26.0f }, "起始色", theme.Text, 10.5f);

				const Wui::WuiColor ce { em.ColorEnd.r, em.ColorEnd.g, em.ColorEnd.b, 1.0f };
				Wui::BarSurface(ctx, { c5.X + 110.0f, c5.Y + 26.0f, 28.0f, 16.0f }, ce, theme.Border);
				Wui::Label(ctx, { c5.X + 142.0f, c5.Y + 26.0f }, "终止色", theme.Text, 10.5f);
				ry += c5.H + 6.0f;

				// 6. [x] Collision & Events
				const Wui::WuiRect c6 { rightX, ry, rightW, 58.0f };
				Wui::BarSurface(ctx, c6, theme.PanelBg, theme.Border);
				Wui::Label(ctx, { c6.X + 10.0f, c6.Y + 6.0f }, "▶ [x] 物理碰撞与事件 (Collision & Events)", theme.Accent, 11.5f);

				std::string colModeStr = (em.Collision.Mode == ParticleCollisionMode::Plane) ? "地面反弹 (Plane)" : "无 (None)";
				Wui::Label(ctx, { c6.X + 16.0f, c6.Y + 24.0f }, "碰撞: " + colModeStr, theme.Text, 11.0f);
				const Wui::WuiRect toggleCol { c6.X + 140.0f, c6.Y + 22.0f, 48.0f, 18.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.col.toggle"), toggleCol, "切换", theme))
				{
					em.Collision.Mode = (em.Collision.Mode == ParticleCollisionMode::Plane) ? ParticleCollisionMode::None : ParticleCollisionMode::Plane;
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
				Wui::Label(ctx, { c6.X + 16.0f, c6.Y + 40.0f }, "反弹系数: " + std::to_string(em.Collision.Restitution).substr(0, 4) + " | 摩擦: " + std::to_string(em.Collision.Friction).substr(0, 4), theme.TextMuted, 10.0f);
				ry += c6.H + 6.0f;

				// 7. [x] Sub-Emitters
				const Wui::WuiRect c7 { rightX, ry, rightW, 46.0f };
				Wui::BarSurface(ctx, c7, theme.PanelBg, theme.Border);
				Wui::Label(ctx, { c7.X + 10.0f, c7.Y + 6.0f }, "▶ [x] 次级发射器派生 (Sub-Emitters)", theme.Accent, 11.5f);
				Wui::Label(ctx, { c7.X + 16.0f, c7.Y + 24.0f }, "碰撞派生: OnCollision 沿法线火星飞溅", theme.TextMuted, 10.5f);
				ry += c7.H + 6.0f;
			}
		}
	}
}
