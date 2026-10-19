#include "wldpch.h"
#include "WUI/Panels/ParticleEditorPanel.h"
#include "WUI/Panels/ViewportPanel.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Renderer/ParticleAsset.h"
#include "World/Renderer/ParticleSystem.h"
#include "World/RHI/Rhi.h"
#include "World/Scene/Components.h"
#include "World/Utils/Paths.h"
#include "World/Core/KeyCodes.h"


#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>


namespace World
{
	std::string ParticleEditorPanel::s_PendingOpenPath;


void ParticleEditorPanel::RequestOpenAsset(const std::string& path)
	{
		s_PendingOpenPath = path;
	}


	// 2D [marker] Cohen-Sutherland æ¿§
	static bool ClipLineSegment2D(glm::vec2& p0, glm::vec2& p1, float minX, float maxX, float minY, float maxY)
	{
		auto computeOutCode = [minX, maxX, minY, maxY](const glm::vec2& p) -> int {
			int code = 0;
			if (p.x < minX) code |= 1;
			else if (p.x > maxX) code |= 2;
			if (p.y < minY) code |= 4;
			else if (p.y > maxY) code |= 8;
			return code;
		};


		int code0 = computeOutCode(p0);
		int code1 = computeOutCode(p1);
		bool accept = false;


		while (true)
		{
			if ((code0 | code1) == 0)
		{
				accept = true;
				break;
			}
			else if (code0 & code1)
		{
				break;
			}
			else
			{
				float x = 0.0f;
				float y = 0.0f;
				int outcodeOut = code0 ? code0 : code1;


				if (outcodeOut & 8)
				{
					x = p0.x + (p1.x - p0.x) * (maxY - p0.y) / (p1.y - p0.y);
					y = maxY;
				}
				else if (outcodeOut & 4)
				{
					x = p0.x + (p1.x - p0.x) * (minY - p0.y) / (p1.y - p0.y);
			y = minY;
				}
				else if (outcodeOut & 2)
				{
					y = p0.y + (p1.y - p0.y) * (maxX - p0.x) / (p1.x - p0.x);
					x = maxX;
				}
				else if (outcodeOut & 1)
				{
					y = p0.y + (p1.y - p0.y) * (minX - p0.x) / (p1.x - p0.x);
					x = minX;
				}


				if (outcodeOut == code0)
				{
					p0.x = x;
					p0.y = y;
					code0 = computeOutCode(p0);
				}
				else
				{
					p1.x = x;
					p1.y = y;
					code1 = computeOutCode(p1);
				}
			}
		}
		return accept;
	}


	ParticleEditorPanel::ParticleEditorPanel()
	{
		NewAsset();
		LoadPresetFlameTornado3D();
	}


	ParticleEditorPanel::~ParticleEditorPanel() = default;


void ParticleEditorPanel::PushUndoSnapshot()
	{
		m_UndoStack.push_back(m_Asset);
		m_UndoExtendedStack.push_back(m_EmitterExtended);
		if (m_UndoStack.size() > 50)
		{
			m_UndoStack.erase(m_UndoStack.begin());
			m_UndoExtendedStack.erase(m_UndoExtendedStack.begin());
		}
		m_RedoStack.clear();
		m_RedoExtendedStack.clear();
	}


void ParticleEditorPanel::Undo()
	{
		if (m_UndoStack.empty()) return;
		m_RedoStack.push_back(m_Asset);
		m_RedoExtendedStack.push_back(m_EmitterExtended);
		m_Asset = std::move(m_UndoStack.back());
		m_UndoStack.pop_back();
		if (!m_UndoExtendedStack.empty())
		{
			m_EmitterExtended = std::move(m_UndoExtendedStack.back());
			m_UndoExtendedStack.pop_back();
		}
		m_Dirty = true;
		m_StatusFeedback = "已撤销 (Undo)";
		EnsurePerEmitterState();
		SyncSimulationFromAsset();
	}


void ParticleEditorPanel::Redo()
	{
		if (m_RedoStack.empty()) return;
		m_UndoStack.push_back(m_Asset);
		m_UndoExtendedStack.push_back(m_EmitterExtended);
		m_Asset = std::move(m_RedoStack.back());
		m_RedoStack.pop_back();
		if (!m_RedoExtendedStack.empty())
		{
			m_EmitterExtended = std::move(m_RedoExtendedStack.back());
			m_RedoExtendedStack.pop_back();
		}
		m_Dirty = true;
		m_StatusFeedback = "已重做 (Redo)";
		EnsurePerEmitterState();
		SyncSimulationFromAsset();
	}


void ParticleEditorPanel::NewAsset()
	{
		m_UndoStack.clear();
		m_RedoStack.clear();
		m_UndoExtendedStack.clear();
		m_RedoExtendedStack.clear();
		m_Asset = ParticleAsset();
		m_Asset.Name = "NewVFXSystem";
		m_Asset.Seed = 1337;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.0f;
		m_TimelineDuration = 5.0f;


		ParticleEmitterLayer defaultEmitter;
		defaultEmitter.Name = "MainEmitter";
		defaultEmitter.MaxParticles = 500;
		defaultEmitter.Lifetime = 2.0f;
		defaultEmitter.EmissionRate = 50.0f;
		defaultEmitter.EmitterType = ParticleEmitterType::Cone;
		defaultEmitter.Speed = 5.0f;
		defaultEmitter.SpeedVariance = 1.0f;
		defaultEmitter.Gravity = glm::vec3(0.0f, -2.0f, 0.0f);
		defaultEmitter.SizeStart = 0.4f;
		defaultEmitter.SizeEnd = 0.05f;
		defaultEmitter.ColorStart = glm::vec4(1.0f, 0.6f, 0.2f, 1.0f);
		defaultEmitter.ColorEnd = glm::vec4(1.0f, 0.1f, 0.0f, 0.0f);
		defaultEmitter.Material.BlendMode = ParticleBlendMode::Additive;
		defaultEmitter.Material.EmissiveBoost = 2.0f;


		m_Asset.Emitters.clear();
		m_Asset.Emitters.push_back(defaultEmitter);


		m_LoadedPath.clear();
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "已新建特效资产";
		EnsurePerEmitterState();
		SyncSimulationFromAsset();
	}


	// =========================================================================
	// 5 大高水准官方 3D 特效案例集 (Industrial 3D Showcase Presets)
	// =========================================================================


	void ParticleEditorPanel::LoadPresetFlameTornado3D()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "FlameTornado3D";
		m_Asset.Seed = 2026;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.8f;
		m_TimelineDuration = 5.0f;


		// 1. 龙卷风核心内焰 (Core Flame Vortex)
		ParticleEmitterLayer coreFlame;
		coreFlame.Name = "CoreFlameVortex";
		coreFlame.Enabled = true;
		coreFlame.MaxParticles = 650;
		coreFlame.Lifetime = 1.8f;
		coreFlame.EmissionRate = 120.0f;
		coreFlame.EmitterType = ParticleEmitterType::Cone;
		coreFlame.FacingMode = ParticleRenderMode::Billboard;
		coreFlame.Material.BlendMode = ParticleBlendMode::Additive;
		coreFlame.Material.EmissiveBoost = 3.6f;
		coreFlame.Speed = 6.2f;
		coreFlame.SpeedVariance = 1.2f;
		coreFlame.Gravity = glm::vec3(0.0f, 4.0f, 0.0f);
		coreFlame.LinearDrag = 0.35f;
		coreFlame.VortexStrength = 22.0f;
		coreFlame.VortexAxis = glm::vec3(0.0f, 1.0f, 0.0f);
		coreFlame.TurbulenceStrength = 2.0f;
		coreFlame.TurbulenceFrequency = 1.2f;
		coreFlame.SizeStart = 0.75f;
		coreFlame.SizeEnd = 0.15f;
		coreFlame.ColorStart = glm::vec4(1.0f, 0.75f, 0.25f, 1.0f);
		coreFlame.ColorEnd = glm::vec4(0.95f, 0.15f, 0.02f, 0.0f);


		// 2. 高速飞旋火星群 (Spiraling Fiery Sparks)
		ParticleEmitterLayer sparks;
		sparks.Name = "SpiralingSparks";
		sparks.Enabled = true;
		sparks.MaxParticles = 400;
		sparks.Lifetime = 2.2f;
		sparks.EmissionRate = 75.0f;
		sparks.EmitterType = ParticleEmitterType::Cone;
		sparks.FacingMode = ParticleRenderMode::StretchedVelocity;
		sparks.Material.BlendMode = ParticleBlendMode::Additive;
		sparks.Material.EmissiveBoost = 5.2f;
		sparks.Speed = 8.5f;
		sparks.SpeedVariance = 2.5f;
		sparks.Gravity = glm::vec3(0.0f, 2.0f, 0.0f);
		sparks.LinearDrag = 0.15f;
		sparks.VortexStrength = 28.0f;
		sparks.VortexAxis = glm::vec3(0.0f, 1.0f, 0.0f);
		sparks.TurbulenceStrength = 2.8f;
		sparks.TurbulenceFrequency = 1.5f;
		sparks.SizeStart = 0.16f;
		sparks.SizeEnd = 0.02f;
		sparks.ColorStart = glm::vec4(1.0f, 0.95f, 0.5f, 1.0f);
		sparks.ColorEnd = glm::vec4(1.0f, 0.3f, 0.0f, 0.0f);


		// 3. 旋度噪声扰动升腾浓烟 (Rising Smoke Funnel)
		ParticleEmitterLayer smoke;
		smoke.Name = "RisingSmokeFunnel";
		smoke.Enabled = true;
		smoke.MaxParticles = 300;
		smoke.Lifetime = 3.2f;
		smoke.EmissionRate = 35.0f;
		smoke.EmitterType = ParticleEmitterType::Cone;
		smoke.FacingMode = ParticleRenderMode::Billboard;
		smoke.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		smoke.Material.SoftParticlesEnabled = true;
		smoke.Material.SoftParticleFadeDistance = 0.6f;
		smoke.Material.EmissiveBoost = 1.0f;
		smoke.Speed = 3.0f;
		smoke.SpeedVariance = 0.8f;
		smoke.Gravity = glm::vec3(0.0f, 2.5f, 0.0f);
		smoke.LinearDrag = 0.7f;
		smoke.VortexStrength = 14.0f;
		smoke.VortexAxis = glm::vec3(0.0f, 1.0f, 0.0f);
		smoke.TurbulenceStrength = 3.2f;
		smoke.TurbulenceFrequency = 0.8f;
		smoke.SizeStart = 0.4f;
		smoke.SizeEnd = 2.4f;
		smoke.ColorStart = glm::vec4(0.30f, 0.28f, 0.25f, 0.65f);
		smoke.ColorEnd = glm::vec4(0.12f, 0.12f, 0.12f, 0.0f);


		m_Asset.Emitters.clear();
		m_Asset.Emitters.push_back(coreFlame);
		m_Asset.Emitters.push_back(sparks);
		m_Asset.Emitters.push_back(smoke);


		m_LoadedPath = "vfx/presets/FlameTornado3D.wvfx";
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "已加载案例: 3D 火焰龙卷风 (FlameTornado3D)";
		EnsurePerEmitterState();
		if (!m_EmitterExtended.empty())
		{
			m_EmitterExtended[0].CurlNoiseStrength = 2.0f;
			m_EmitterExtended[1].CurlNoiseStrength = 2.5f;
			m_EmitterExtended[2].CurlNoiseStrength = 3.5f;
			m_EmitterExtended[2].ModuleCurlNoiseEnabled = true;
		}
		SyncSimulationFromAsset();
	}


	void ParticleEditorPanel::LoadPresetLightningPlasmaBeam3D()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "LightningPlasmaBeam3D";
		m_Asset.Seed = 31415;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.3f;
		m_TimelineDuration = 4.0f;


		// 1. 核心高能等离子光束 (Electric Core Beam)
		ParticleEmitterLayer coreBeam;
		coreBeam.Name = "ElectricCoreBeam";
		coreBeam.Enabled = true;
		coreBeam.MaxParticles = 550;
		coreBeam.Lifetime = 0.8f;
		coreBeam.EmissionRate = 220.0f;
		coreBeam.EmitterType = ParticleEmitterType::Point;
		coreBeam.FacingMode = ParticleRenderMode::StretchedVelocity;
		coreBeam.Material.BlendMode = ParticleBlendMode::Additive;
		coreBeam.Material.EmissiveBoost = 4.8f;
		coreBeam.Speed = 16.0f;
		coreBeam.SpeedVariance = 2.0f;
		coreBeam.Gravity = glm::vec3(0.0f);
		coreBeam.LinearDrag = 0.05f;
		coreBeam.SizeStart = 0.35f;
		coreBeam.SizeEnd = 0.08f;
		coreBeam.ColorStart = glm::vec4(0.2f, 0.85f, 1.0f, 1.0f);
		coreBeam.ColorEnd = glm::vec4(0.05f, 0.4f, 1.0f, 0.0f);


		// 2. 空间螺旋放电电弧带 (Plasma Arc Spiral)
		ParticleEmitterLayer arcSpiral;
		arcSpiral.Name = "PlasmaArcSpiral";
		arcSpiral.Enabled = true;
		arcSpiral.MaxParticles = 450;
		arcSpiral.Lifetime = 1.0f;
		arcSpiral.EmissionRate = 140.0f;
		arcSpiral.EmitterType = ParticleEmitterType::Point;
		arcSpiral.FacingMode = ParticleRenderMode::Billboard;
		arcSpiral.Material.BlendMode = ParticleBlendMode::Additive;
		arcSpiral.Material.EmissiveBoost = 4.2f;
		arcSpiral.Speed = 14.0f;
		arcSpiral.SpeedVariance = 3.5f;
		arcSpiral.Gravity = glm::vec3(0.0f);
		arcSpiral.LinearDrag = 0.1f;
		arcSpiral.VortexStrength = 18.0f;
		arcSpiral.VortexAxis = glm::vec3(0.0f, 0.0f, 1.0f);
		arcSpiral.TurbulenceStrength = 4.0f;
		arcSpiral.TurbulenceFrequency = 2.5f;
		arcSpiral.SizeStart = 0.22f;
		arcSpiral.SizeEnd = 0.04f;
		arcSpiral.ColorStart = glm::vec4(0.85f, 0.3f, 1.0f, 1.0f);
		arcSpiral.ColorEnd = glm::vec4(0.2f, 0.9f, 1.0f, 0.0f);


		// 3. 目标击中点剧烈反弹火花 (Impact Scatter Sparks)
		ParticleEmitterLayer impactSparks;
		impactSparks.Name = "ImpactScatterSparks";
		impactSparks.Enabled = true;
		impactSparks.MaxParticles = 350;
		impactSparks.Lifetime = 1.4f;
		impactSparks.EmissionRate = 90.0f;
		impactSparks.EmitterType = ParticleEmitterType::Sphere;
		impactSparks.FacingMode = ParticleRenderMode::StretchedVelocity;
		impactSparks.Material.BlendMode = ParticleBlendMode::Additive;
		impactSparks.Material.EmissiveBoost = 5.0f;
		impactSparks.Speed = 8.0f;
		impactSparks.SpeedVariance = 3.0f;
		impactSparks.Gravity = glm::vec3(0.0f, -9.8f, 0.0f);
		impactSparks.LinearDrag = 0.3f;
		impactSparks.Collision.Mode = ParticleCollisionMode::Plane;
		impactSparks.Collision.Restitution = 0.85f;
		impactSparks.Collision.Friction = 0.15f;
		impactSparks.SizeStart = 0.14f;
		impactSparks.SizeEnd = 0.02f;
		impactSparks.ColorStart = glm::vec4(0.5f, 1.0f, 0.95f, 1.0f);
		impactSparks.ColorEnd = glm::vec4(0.1f, 0.3f, 1.0f, 0.0f);


		m_Asset.Emitters.clear();
		m_Asset.Emitters.push_back(coreBeam);
		m_Asset.Emitters.push_back(arcSpiral);
		m_Asset.Emitters.push_back(impactSparks);


		m_LoadedPath = "vfx/presets/LightningPlasmaBeam3D.wvfx";
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "已加载案例: 3D 等离子闪电束 (LightningPlasmaBeam3D)";
		EnsurePerEmitterState();
		SyncSimulationFromAsset();
	}


	void ParticleEditorPanel::LoadPresetOrbitalMagicShield3D()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "OrbitalMagicShield3D";
		m_Asset.Seed = 777;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.5f;
		m_TimelineDuration = 6.0f;


		// 1. 外层空间符文旋转球环 (Orbital Rune Spheres)
		ParticleEmitterLayer runeRing;
		runeRing.Name = "OrbitalRuneSpheres";
		runeRing.Enabled = true;
		runeRing.MaxParticles = 480;
		runeRing.Lifetime = 2.8f;
		runeRing.EmissionRate = 80.0f;
		runeRing.EmitterType = ParticleEmitterType::Sphere;
		runeRing.FacingMode = ParticleRenderMode::Billboard;
		runeRing.Material.BlendMode = ParticleBlendMode::Additive;
		runeRing.Material.EmissiveBoost = 3.8f;
		runeRing.Speed = 2.0f;
		runeRing.SpeedVariance = 0.5f;
		runeRing.Gravity = glm::vec3(0.0f);
		runeRing.LinearDrag = 0.2f;
		runeRing.VortexStrength = 15.0f;
		runeRing.VortexAxis = glm::vec3(0.0f, 1.0f, 0.0f);
		runeRing.SizeStart = 0.28f;
		runeRing.SizeEnd = 0.12f;
		runeRing.ColorStart = glm::vec4(0.8f, 0.25f, 1.0f, 1.0f);
		runeRing.ColorEnd = glm::vec4(0.2f, 0.8f, 1.0f, 0.0f);


		// 2. 轨道自发光粒子动态点光源 (Orbiting Point Lights)
		ParticleEmitterLayer lightOrbs;
		lightOrbs.Name = "OrbitingPointLights";
		lightOrbs.Enabled = true;
		lightOrbs.MaxParticles = 120;
		lightOrbs.Lifetime = 3.5f;
		lightOrbs.EmissionRate = 25.0f;
		lightOrbs.EmitterType = ParticleEmitterType::Circle;
		lightOrbs.FacingMode = ParticleRenderMode::Billboard;
		lightOrbs.Material.BlendMode = ParticleBlendMode::Additive;
		lightOrbs.Material.EmissiveBoost = 5.5f;
		lightOrbs.Speed = 3.5f;
		lightOrbs.SpeedVariance = 0.2f;
		lightOrbs.Gravity = glm::vec3(0.0f);
		lightOrbs.VortexStrength = 10.0f;
		lightOrbs.VortexAxis = glm::vec3(0.0f, 1.0f, 0.0f);
		lightOrbs.SizeStart = 0.45f;
		lightOrbs.SizeEnd = 0.15f;
		lightOrbs.ColorStart = glm::vec4(1.0f, 0.9f, 0.3f, 1.0f);
		lightOrbs.ColorEnd = glm::vec4(1.0f, 0.4f, 0.1f, 0.0f);


		// 3. 脉冲能量内核 (Pulsing Shield Core)
		ParticleEmitterLayer shieldCore;
		shieldCore.Name = "PulsingShieldCore";
		shieldCore.Enabled = true;
		shieldCore.MaxParticles = 300;
		shieldCore.Lifetime = 2.0f;
		shieldCore.EmissionRate = 60.0f;
		shieldCore.EmitterType = ParticleEmitterType::Sphere;
		shieldCore.FacingMode = ParticleRenderMode::Billboard;
		shieldCore.Material.BlendMode = ParticleBlendMode::Additive;
		shieldCore.Material.EmissiveBoost = 2.8f;
		shieldCore.Speed = 0.8f;
		shieldCore.SpeedVariance = 0.3f;
		shieldCore.Gravity = glm::vec3(0.0f);
		shieldCore.LinearDrag = 0.5f;
		shieldCore.SizeStart = 1.2f;
		shieldCore.SizeEnd = 0.2f;
		shieldCore.ColorStart = glm::vec4(0.3f, 0.6f, 1.0f, 0.85f);
		shieldCore.ColorEnd = glm::vec4(0.1f, 0.2f, 0.8f, 0.0f);


		m_Asset.Emitters.clear();
		m_Asset.Emitters.push_back(runeRing);
		m_Asset.Emitters.push_back(lightOrbs);
		m_Asset.Emitters.push_back(shieldCore);


		m_LoadedPath = "vfx/presets/OrbitalMagicShield3D.wvfx";
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "已加载案例: 3D 环形魔法护盾 (OrbitalMagicShield3D)";
		EnsurePerEmitterState();
		if (m_EmitterExtended.size() >= 2)
		{
			m_EmitterExtended[1].LightEnabled = true;
			m_EmitterExtended[1].LightRadius = 4.5f;
			m_EmitterExtended[1].LightIntensity = 2.5f;
			m_EmitterExtended[1].ModuleLightEnabled = true;
		}
		SyncSimulationFromAsset();
	}


	void ParticleEditorPanel::LoadPresetMeteorImpactExplosion3D()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "MeteorImpactExplosion3D";
		m_Asset.Seed = 8888;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.0f;
		m_TimelineDuration = 4.5f;


		// 1. 地面瞬态冲击波扩散环 (Ground Shockwave Ring)
		ParticleEmitterLayer shockwave;
		shockwave.Name = "GroundShockwave";
		shockwave.Enabled = true;
		shockwave.MaxParticles = 180;
		shockwave.Lifetime = 1.2f;
		shockwave.EmissionRate = 0.0f;
		shockwave.Bursts.push_back({ 0.0f, 40 });
		shockwave.EmitterType = ParticleEmitterType::Circle;
		shockwave.FacingMode = ParticleRenderMode::HorizontalQuad;
		shockwave.Material.BlendMode = ParticleBlendMode::Additive;
		shockwave.Material.EmissiveBoost = 4.0f;
		shockwave.Speed = 6.5f;
		shockwave.SpeedVariance = 0.5f;
		shockwave.Gravity = glm::vec3(0.0f);
		shockwave.LinearDrag = 1.2f;
		shockwave.SizeStart = 0.5f;
		shockwave.SizeEnd = 4.8f;
		shockwave.ColorStart = glm::vec4(1.0f, 0.85f, 0.4f, 1.0f);
		shockwave.ColorEnd = glm::vec4(1.0f, 0.2f, 0.0f, 0.0f);


		// 2. 爆心炽热火球 (Core Explosion Fireball)
		ParticleEmitterLayer fireball;
		fireball.Name = "CoreExplosionFireball";
		fireball.Enabled = true;
		fireball.MaxParticles = 450;
		fireball.Lifetime = 1.6f;
		fireball.EmissionRate = 0.0f;
		fireball.Bursts.push_back({ 0.0f, 160 });
		fireball.EmitterType = ParticleEmitterType::Sphere;
		fireball.FacingMode = ParticleRenderMode::Billboard;
		fireball.Material.BlendMode = ParticleBlendMode::Additive;
		fireball.Material.EmissiveBoost = 4.5f;
		fireball.Speed = 9.0f;
		fireball.SpeedVariance = 3.5f;
		fireball.Gravity = glm::vec3(0.0f, 1.0f, 0.0f);
		fireball.LinearDrag = 2.2f;
		fireball.SizeStart = 1.8f;
		fireball.SizeEnd = 0.1f;
		fireball.ColorStart = glm::vec4(1.0f, 0.95f, 0.6f, 1.0f);
		fireball.ColorEnd = glm::vec4(0.9f, 0.15f, 0.0f, 0.0f);


		// 3. 撞地反弹重力碎石火星 (Bouncing Debris Sparks)
		ParticleEmitterLayer debris;
		debris.Name = "BouncingDebrisSparks";
		debris.Enabled = true;
		debris.MaxParticles = 300;
		debris.Lifetime = 2.5f;
		debris.EmissionRate = 0.0f;
		debris.Bursts.push_back({ 0.02f, 90 });
		debris.EmitterType = ParticleEmitterType::Cone;
		debris.FacingMode = ParticleRenderMode::StretchedVelocity;
		debris.Material.BlendMode = ParticleBlendMode::Additive;
		debris.Material.EmissiveBoost = 5.0f;
		debris.Speed = 12.0f;
		debris.SpeedVariance = 4.0f;
		debris.Gravity = glm::vec3(0.0f, -14.0f, 0.0f);
		debris.LinearDrag = 0.25f;
		debris.Collision.Mode = ParticleCollisionMode::Plane;
		debris.Collision.Restitution = 0.65f;
		debris.Collision.Friction = 0.25f;
		debris.SizeStart = 0.18f;
		debris.SizeEnd = 0.03f;
		debris.ColorStart = glm::vec4(1.0f, 0.7f, 0.2f, 1.0f);
		debris.ColorEnd = glm::vec4(1.0f, 0.1f, 0.0f, 0.0f);


		// 4. 升腾黑烟蘑菇云 (Rising Mushroom Smoke)
		ParticleEmitterLayer smoke;
		smoke.Name = "RisingMushroomSmoke";
		smoke.Enabled = true;
		smoke.MaxParticles = 320;
		smoke.Lifetime = 3.5f;
		smoke.EmissionRate = 0.0f;
		smoke.Bursts.push_back({ 0.05f, 110 });
		smoke.EmitterType = ParticleEmitterType::Cone;
		smoke.FacingMode = ParticleRenderMode::Billboard;
		smoke.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		smoke.Material.SoftParticlesEnabled = true;
		smoke.Material.EmissiveBoost = 1.0f;
		smoke.Speed = 4.5f;
		smoke.SpeedVariance = 1.5f;
		smoke.Gravity = glm::vec3(0.0f, 2.8f, 0.0f);
		smoke.LinearDrag = 1.5f;
		smoke.TurbulenceStrength = 2.5f;
		smoke.SizeStart = 0.6f;
		smoke.SizeEnd = 3.2f;
		smoke.ColorStart = glm::vec4(0.25f, 0.22f, 0.20f, 0.75f);
		smoke.ColorEnd = glm::vec4(0.08f, 0.08f, 0.08f, 0.0f);


		m_Asset.Emitters.clear();
		m_Asset.Emitters.push_back(shockwave);
		m_Asset.Emitters.push_back(fireball);
		m_Asset.Emitters.push_back(debris);
		m_Asset.Emitters.push_back(smoke);


		m_LoadedPath = "vfx/presets/MeteorImpactExplosion3D.wvfx";
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "已加载案例: 3D 流星陨石撞击高爆 (MeteorImpactExplosion3D)";
		EnsurePerEmitterState();
		SyncSimulationFromAsset();
	}


	void ParticleEditorPanel::LoadPresetBlizzardFrostNova3D()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "BlizzardFrostNova3D";
		m_Asset.Seed = 10086;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.5f;
		m_TimelineDuration = 5.0f;


		// 1. 向外爆发冰晶冲击波 (Frost Shockwave Nova)
		ParticleEmitterLayer shockwave;
		shockwave.Name = "FrostShockwaveNova";
		shockwave.Enabled = true;
		shockwave.MaxParticles = 250;
		shockwave.Lifetime = 1.5f;
		shockwave.EmissionRate = 0.0f;
		shockwave.Bursts.push_back({ 0.0f, 60 });
		shockwave.EmitterType = ParticleEmitterType::Circle;
		shockwave.FacingMode = ParticleRenderMode::HorizontalQuad;
		shockwave.Material.BlendMode = ParticleBlendMode::Additive;
		shockwave.Material.EmissiveBoost = 3.5f;
		shockwave.Speed = 7.0f;
		shockwave.SpeedVariance = 0.8f;
		shockwave.Gravity = glm::vec3(0.0f);
		shockwave.LinearDrag = 1.0f;
		shockwave.SizeStart = 0.4f;
		shockwave.SizeEnd = 4.2f;
		shockwave.ColorStart = glm::vec4(0.7f, 0.95f, 1.0f, 1.0f);
		shockwave.ColorEnd = glm::vec4(0.1f, 0.4f, 0.9f, 0.0f);


		// 2. 低温地表旋转冷气雾 (Ground Swirling Cold Mist)
		ParticleEmitterLayer mist;
		mist.Name = "GroundSwirlingMist";
		mist.Enabled = true;
		mist.MaxParticles = 400;
		mist.Lifetime = 2.8f;
		mist.EmissionRate = 60.0f;
		mist.EmitterType = ParticleEmitterType::Cone;
		mist.FacingMode = ParticleRenderMode::Billboard;
		mist.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		mist.Material.SoftParticlesEnabled = true;
		mist.Material.EmissiveBoost = 1.2f;
		mist.Speed = 2.2f;
		mist.SpeedVariance = 0.5f;
		mist.Gravity = glm::vec3(0.0f, 0.3f, 0.0f);
		mist.LinearDrag = 0.6f;
		mist.VortexStrength = 8.5f;
		mist.VortexAxis = glm::vec3(0.0f, 1.0f, 0.0f);
		mist.TurbulenceStrength = 2.0f;
		mist.SizeStart = 0.5f;
		mist.SizeEnd = 2.2f;
		mist.ColorStart = glm::vec4(0.85f, 0.92f, 1.0f, 0.55f);
		mist.ColorEnd = glm::vec4(0.5f, 0.7f, 0.9f, 0.0f);


		// 3. 悬浮漫游冰霜雪花 (Floating Frost Flakes)
		ParticleEmitterLayer flakes;
		flakes.Name = "FloatingFrostFlakes";
		flakes.Enabled = true;
		flakes.MaxParticles = 500;
		flakes.Lifetime = 3.5f;
		flakes.EmissionRate = 85.0f;
		flakes.EmitterType = ParticleEmitterType::Box;
		flakes.FacingMode = ParticleRenderMode::Billboard;
		flakes.Material.BlendMode = ParticleBlendMode::Additive;
		flakes.Material.EmissiveBoost = 3.0f;
		flakes.Speed = 1.8f;
		flakes.SpeedVariance = 0.8f;
		flakes.Gravity = glm::vec3(0.0f, -1.2f, 0.0f);
		flakes.LinearDrag = 0.9f;
		flakes.TurbulenceStrength = 1.8f;
		flakes.TurbulenceFrequency = 1.0f;
		flakes.SizeStart = 0.12f;
		flakes.SizeEnd = 0.06f;
		flakes.ColorStart = glm::vec4(0.95f, 0.98f, 1.0f, 1.0f);
		flakes.ColorEnd = glm::vec4(0.4f, 0.7f, 1.0f, 0.0f);


		m_Asset.Emitters.clear();
		m_Asset.Emitters.push_back(shockwave);
		m_Asset.Emitters.push_back(mist);
		m_Asset.Emitters.push_back(flakes);


		m_LoadedPath = "vfx/presets/BlizzardFrostNova3D.wvfx";
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "已加载案例: 3D 暴风雪冰霜新星 (BlizzardFrostNova3D)";
		EnsurePerEmitterState();
		SyncSimulationFromAsset();
	}


	void ParticleEditorPanel::LoadPresetCampfire()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "Campfire_Compound";
		m_Asset.Seed = 1337;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.5f;
		m_TimelineDuration = 5.0f;


		ParticleEmitterLayer flame;
		flame.Name = "CoreFlame";
		flame.Enabled = true;
		flame.MaxParticles = 450;
		flame.Lifetime = 1.4f;
		flame.EmissionRate = 80.0f;
		flame.EmitterType = ParticleEmitterType::Cone;
		flame.FacingMode = ParticleRenderMode::Billboard;
		flame.Material.BlendMode = ParticleBlendMode::Additive;
		flame.Material.EmissiveBoost = 2.8f;
		flame.Speed = 3.2f;
		flame.SpeedVariance = 0.6f;
		flame.Gravity = glm::vec3(0.0f, 2.5f, 0.0f);
		flame.LinearDrag = 0.4f;
		flame.TurbulenceStrength = 1.2f;
		flame.SizeStart = 0.65f;
		flame.SizeEnd = 0.12f;
		flame.ColorStart = glm::vec4(1.0f, 0.7f, 0.2f, 1.0f);
		flame.ColorEnd = glm::vec4(0.9f, 0.1f, 0.0f, 0.0f);


		ParticleEmitterLayer smoke;
		smoke.Name = "RisingSmoke";
		smoke.Enabled = true;
		smoke.MaxParticles = 220;
		smoke.Lifetime = 2.5f;
		smoke.EmissionRate = 28.0f;
		smoke.EmitterType = ParticleEmitterType::Cone;
		smoke.FacingMode = ParticleRenderMode::Billboard;
		smoke.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		smoke.Material.SoftParticlesEnabled = true;
		smoke.Material.EmissiveBoost = 1.0f;
		smoke.Speed = 1.8f;
		smoke.SpeedVariance = 0.4f;
		smoke.Gravity = glm::vec3(0.0f, 1.4f, 0.0f);
		smoke.LinearDrag = 0.8f;
		smoke.TurbulenceStrength = 0.9f;
		smoke.SizeStart = 0.3f;
		smoke.SizeEnd = 1.5f;
		smoke.ColorStart = glm::vec4(0.35f, 0.35f, 0.35f, 0.6f);
		smoke.ColorEnd = glm::vec4(0.15f, 0.15f, 0.15f, 0.0f);


		ParticleEmitterLayer embers;
		embers.Name = "FloatingEmbers";
		embers.Enabled = true;
		embers.MaxParticles = 260;
		embers.Lifetime = 2.0f;
		embers.EmissionRate = 35.0f;
		embers.EmitterType = ParticleEmitterType::Sphere;
		embers.FacingMode = ParticleRenderMode::StretchedVelocity;
		embers.Material.BlendMode = ParticleBlendMode::Additive;
		embers.Material.EmissiveBoost = 3.8f;
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
		m_StatusFeedback = "已加载预设: 营火 (Campfire)";
		EnsurePerEmitterState();
		SyncSimulationFromAsset();
	}


	void ParticleEditorPanel::LoadPresetSparksBurst()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "Sparks_Burst";
		m_Asset.Seed = 42;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.0f;
		m_TimelineDuration = 3.0f;


		ParticleEmitterLayer sparks;
		sparks.Name = "ImpactSparks";
		sparks.Enabled = true;
		sparks.MaxParticles = 300;
		sparks.Lifetime = 1.2f;
		sparks.EmissionRate = 60.0f;
		sparks.EmitterType = ParticleEmitterType::Point;
		sparks.FacingMode = ParticleRenderMode::StretchedVelocity;
		sparks.Material.BlendMode = ParticleBlendMode::Additive;
		sparks.Material.EmissiveBoost = 4.0f;
		sparks.Speed = 8.0f;
		sparks.SpeedVariance = 2.5f;
		sparks.Gravity = glm::vec3(0.0f, -9.8f, 0.0f);
		sparks.LinearDrag = 0.2f;
		sparks.Collision.Mode = ParticleCollisionMode::Plane;
		sparks.Collision.Restitution = 0.7f;
		sparks.Collision.Friction = 0.1f;
		sparks.SizeStart = 0.15f;
		sparks.SizeEnd = 0.02f;
		sparks.ColorStart = glm::vec4(1.0f, 0.8f, 0.2f, 1.0f);
		sparks.ColorEnd = glm::vec4(1.0f, 0.1f, 0.0f, 0.0f);


		m_Asset.Emitters.push_back(sparks);
		m_LoadedPath = "vfx/sparks.wvfx";
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "已加载预设: 火星飞溅 (SparksBurst)";
		EnsurePerEmitterState();
		SyncSimulationFromAsset();
	}


	void ParticleEditorPanel::LoadPresetSmokePlume()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "Smoke_Plume";
		m_Asset.Seed = 999;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.5f;
		m_TimelineDuration = 6.0f;


		ParticleEmitterLayer smoke;
		smoke.Name = "DenseSmoke";
		smoke.Enabled = true;
		smoke.MaxParticles = 400;
		smoke.Lifetime = 3.5f;
		smoke.EmissionRate = 45.0f;
		smoke.EmitterType = ParticleEmitterType::Cone;
		smoke.FacingMode = ParticleRenderMode::Billboard;
		smoke.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		smoke.Material.SoftParticlesEnabled = true;
		smoke.Material.EmissiveBoost = 1.0f;
		smoke.Speed = 2.5f;
		smoke.SpeedVariance = 0.5f;
		smoke.Gravity = glm::vec3(0.0f, 1.8f, 0.0f);
		smoke.LinearDrag = 0.6f;
		smoke.VortexStrength = 2.0f;
		smoke.TurbulenceStrength = 1.5f;
		smoke.SizeStart = 0.4f;
		smoke.SizeEnd = 2.8f;
		smoke.ColorStart = glm::vec4(0.3f, 0.3f, 0.3f, 0.7f);
		smoke.ColorEnd = glm::vec4(0.1f, 0.1f, 0.1f, 0.0f);


		m_Asset.Emitters.push_back(smoke);
		m_LoadedPath = "vfx/smoke.wvfx";
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "已加载预设: 浓烟羽流 (SmokePlume)";
		EnsurePerEmitterState();
		SyncSimulationFromAsset();
	}


	void ParticleEditorPanel::LoadPresetBlizzard()
	{
		LoadPresetBlizzardFrostNova3D();
	}


	void ParticleEditorPanel::LoadPresetMagicPortal()
	{
		LoadPresetOrbitalMagicShield3D();
	}


	void ParticleEditorPanel::LoadPresetEnergyBeam()
	{
		LoadPresetLightningPlasmaBeam3D();
	}


	void ParticleEditorPanel::LoadPreset(const std::string& name)
	{
		PushUndoSnapshot();
		if (name == "FlameTornado3D") LoadPresetFlameTornado3D();
		else if (name == "LightningPlasmaBeam3D") LoadPresetLightningPlasmaBeam3D();
		else if (name == "OrbitalMagicShield3D") LoadPresetOrbitalMagicShield3D();
		else if (name == "MeteorImpactExplosion3D") LoadPresetMeteorImpactExplosion3D();
		else if (name == "BlizzardFrostNova3D") LoadPresetBlizzardFrostNova3D();
		else if (name == "Campfire") LoadPresetCampfire();
		else if (name == "SparksBurst") LoadPresetSparksBurst();
		else if (name == "SmokePlume") LoadPresetSmokePlume();
		else if (name == "Blizzard") LoadPresetBlizzard();
		else if (name == "MagicPortal") LoadPresetMagicPortal();
		else if (name == "EnergyBeam") LoadPresetEnergyBeam();
		else LoadPresetFlameTornado3D();
	}


	// =========================================================================
	// 发射器大纲管理核心操作 (Outliner Operations)
	// =========================================================================


	void ParticleEditorPanel::AddEmitterFromTemplate(const std::string& templateType)
	{
		PushUndoSnapshot();
		ParticleEmitterLayer em;
		em.Enabled = true;


		if (templateType == "Fountain")
		{
			em.Name = "Fountain_" + std::to_string(m_Asset.Emitters.size() + 1);
			em.MaxParticles = 600;
			em.Lifetime = 2.5f;
			em.EmissionRate = 80.0f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.Speed = 8.0f;
			em.SpeedVariance = 1.5f;
			em.Gravity = glm::vec3(0.0f, -9.8f, 0.0f);
			em.LinearDrag = 0.1f;
			em.SizeStart = 0.25f;
			em.SizeEnd = 0.05f;
			em.ColorStart = glm::vec4(0.3f, 0.8f, 1.0f, 1.0f);
			em.ColorEnd = glm::vec4(0.05f, 0.2f, 0.9f, 0.0f);
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 2.5f;
		}
		else if (templateType == "Fire")
		{
			em.Name = "Fire_" + std::to_string(m_Asset.Emitters.size() + 1);
			em.MaxParticles = 450;
			em.Lifetime = 1.5f;
			em.EmissionRate = 70.0f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.Speed = 4.0f;
			em.SpeedVariance = 0.8f;
			em.Gravity = glm::vec3(0.0f, 3.0f, 0.0f);
			em.LinearDrag = 0.3f;
			em.VortexStrength = 3.0f;
			em.SizeStart = 0.6f;
			em.SizeEnd = 0.1f;
			em.ColorStart = glm::vec4(1.0f, 0.8f, 0.2f, 1.0f);
			em.ColorEnd = glm::vec4(0.9f, 0.1f, 0.0f, 0.0f);
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 3.0f;
		}
		else if (templateType == "Sparks")
		{
			em.Name = "Sparks_" + std::to_string(m_Asset.Emitters.size() + 1);
			em.MaxParticles = 300;
			em.Lifetime = 1.2f;
			em.EmissionRate = 60.0f;
			em.EmitterType = ParticleEmitterType::Point;
			em.FacingMode = ParticleRenderMode::StretchedVelocity;
			em.Speed = 9.0f;
			em.SpeedVariance = 2.0f;
			em.Gravity = glm::vec3(0.0f, -12.0f, 0.0f);
			em.LinearDrag = 0.2f;
			em.Collision.Mode = ParticleCollisionMode::Plane;
			em.Collision.Restitution = 0.75f;
			em.SizeStart = 0.15f;
			em.SizeEnd = 0.02f;
			em.ColorStart = glm::vec4(1.0f, 0.9f, 0.4f, 1.0f);
			em.ColorEnd = glm::vec4(1.0f, 0.2f, 0.0f, 0.0f);
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 4.5f;
		}
		else if (templateType == "Smoke")
		{
			em.Name = "Smoke_" + std::to_string(m_Asset.Emitters.size() + 1);
			em.MaxParticles = 250;
			em.Lifetime = 3.0f;
			em.EmissionRate = 30.0f;
			em.EmitterType = ParticleEmitterType::Cone;
			em.Speed = 2.0f;
			em.SpeedVariance = 0.5f;
			em.Gravity = glm::vec3(0.0f, 1.5f, 0.0f);
			em.LinearDrag = 0.8f;
			em.SizeStart = 0.35f;
			em.SizeEnd = 2.0f;
			em.ColorStart = glm::vec4(0.35f, 0.35f, 0.35f, 0.65f);
			em.ColorEnd = glm::vec4(0.12f, 0.12f, 0.12f, 0.0f);
			em.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		}
		else if (templateType == "Beam")
		{
			em.Name = "Beam_" + std::to_string(m_Asset.Emitters.size() + 1);
			em.MaxParticles = 400;
			em.Lifetime = 0.7f;
			em.EmissionRate = 180.0f;
			em.EmitterType = ParticleEmitterType::Point;
			em.FacingMode = ParticleRenderMode::StretchedVelocity;
			em.Speed = 18.0f;
			em.SpeedVariance = 1.0f;
			em.Gravity = glm::vec3(0.0f);
			em.SizeStart = 0.3f;
			em.SizeEnd = 0.05f;
			em.ColorStart = glm::vec4(0.3f, 0.9f, 1.0f, 1.0f);
			em.ColorEnd = glm::vec4(0.1f, 0.3f, 1.0f, 0.0f);
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 4.5f;
		}
		else if (templateType == "Ribbon")
		{
			em.Name = "Ribbon_" + std::to_string(m_Asset.Emitters.size() + 1);
			em.MaxParticles = 200;
			em.Lifetime = 1.8f;
			em.EmissionRate = 40.0f;
			em.EmitterType = ParticleEmitterType::Sphere;
			em.Speed = 3.0f;
			em.SpeedVariance = 0.5f;
			em.Gravity = glm::vec3(0.0f, 0.5f, 0.0f);
			em.SizeStart = 0.2f;
			em.SizeEnd = 0.02f;
			em.ColorStart = glm::vec4(0.2f, 1.0f, 0.8f, 1.0f);
			em.ColorEnd = glm::vec4(0.0f, 0.4f, 0.8f, 0.0f);
			em.Material.BlendMode = ParticleBlendMode::Additive;
			em.Material.EmissiveBoost = 3.0f;
		}
		else if (templateType == "Mesh")
		{
			em.Name = "MeshDebris_" + std::to_string(m_Asset.Emitters.size() + 1);
			em.MaxParticles = 150;
			em.Lifetime = 2.0f;
			em.EmissionRate = 25.0f;
			em.EmitterType = ParticleEmitterType::Box;
			em.Speed = 5.0f;
			em.SpeedVariance = 2.0f;
			em.Gravity = glm::vec3(0.0f, -9.8f, 0.0f);
			em.LinearDrag = 0.3f;
			em.Collision.Mode = ParticleCollisionMode::Plane;
			em.Collision.Restitution = 0.5f;
			em.SizeStart = 0.25f;
			em.SizeEnd = 0.25f;
			em.ColorStart = glm::vec4(0.9f, 0.4f, 1.0f, 1.0f);
			em.ColorEnd = glm::vec4(0.4f, 0.1f, 0.8f, 0.0f);
			em.Material.BlendMode = ParticleBlendMode::Additive;
		}
		else // Empty
		{
			em.Name = "Emitter_" + std::to_string(m_Asset.Emitters.size() + 1);
			em.MaxParticles = 300;
			em.Lifetime = 2.0f;
			em.EmissionRate = 50.0f;
			em.EmitterType = ParticleEmitterType::Point;
			em.Speed = 5.0f;
			em.SpeedVariance = 1.0f;
			em.Gravity = glm::vec3(0.0f, -2.0f, 0.0f);
			em.SizeStart = 0.3f;
			em.SizeEnd = 0.05f;
			em.ColorStart = glm::vec4(1.0f);
			em.ColorEnd = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
		}


		m_Asset.Emitters.push_back(em);
		m_SelectedEmitterIndex = m_Asset.Emitters.size() - 1;
		m_Dirty = true;
		m_StatusFeedback = "新建发射器: " + em.Name;
		EnsurePerEmitterState();
		if (templateType == "Ribbon" && !m_EmitterExtended.empty())
		{
			m_EmitterExtended.back().RibbonEnabled = true;
			m_EmitterExtended.back().ModuleRibbonEnabled = true;
		}
		else if (templateType == "Mesh" && !m_EmitterExtended.empty())
		{
			m_EmitterExtended.back().MeshEnabled = true;
			m_EmitterExtended.back().ModuleMeshEnabled = true;
		}
		SyncSimulationFromAsset();
	}


	void ParticleEditorPanel::CloneSelectedEmitter()
	{
		if (m_SelectedEmitterIndex >= m_Asset.Emitters.size()) return;
		PushUndoSnapshot();


		ParticleEmitterLayer clone = m_Asset.Emitters[m_SelectedEmitterIndex];
		clone.Name += "_Copy";
		m_Asset.Emitters.push_back(clone);


		EnsurePerEmitterState();
		size_t newIdx = m_Asset.Emitters.size() - 1;
		m_EmitterSolo[newIdx] = false;
		m_EmitterMute[newIdx] = false;
		m_EmitterLocked[newIdx] = false;
		m_EmitterGradients[newIdx] = m_EmitterGradients[m_SelectedEmitterIndex];
		m_EmitterSizeCurves[newIdx] = m_EmitterSizeCurves[m_SelectedEmitterIndex];
		m_EmitterAlphaCurves[newIdx] = m_EmitterAlphaCurves[m_SelectedEmitterIndex];
		m_EmitterSpeedCurves[newIdx] = m_EmitterSpeedCurves[m_SelectedEmitterIndex];
		m_EmitterCurveType[newIdx] = m_EmitterCurveType[m_SelectedEmitterIndex];
		m_EmitterExtended[newIdx] = m_EmitterExtended[m_SelectedEmitterIndex];


		m_SelectedEmitterIndex = newIdx;
		m_Dirty = true;
		m_StatusFeedback = "已克隆发射器: " + clone.Name;
		SyncSimulationFromAsset();
	}


	void ParticleEditorPanel::DeleteSelectedEmitter()
	{
		if (m_Asset.Emitters.size() <= 1)
		{
			m_StatusFeedback = "无法删除: 粒子系统至少保留一个发射器图层";
			return;
		}
		if (m_SelectedEmitterIndex >= m_Asset.Emitters.size()) return;
		PushUndoSnapshot();


		std::string delName = m_Asset.Emitters[m_SelectedEmitterIndex].Name;
		m_Asset.Emitters.erase(m_Asset.Emitters.begin() + m_SelectedEmitterIndex);


		if (m_SelectedEmitterIndex < m_EmitterSolo.size()) m_EmitterSolo.erase(m_EmitterSolo.begin() + m_SelectedEmitterIndex);
		if (m_SelectedEmitterIndex < m_EmitterMute.size()) m_EmitterMute.erase(m_EmitterMute.begin() + m_SelectedEmitterIndex);
		if (m_SelectedEmitterIndex < m_EmitterLocked.size()) m_EmitterLocked.erase(m_EmitterLocked.begin() + m_SelectedEmitterIndex);
		if (m_SelectedEmitterIndex < m_EmitterGradients.size()) m_EmitterGradients.erase(m_EmitterGradients.begin() + m_SelectedEmitterIndex);
		if (m_SelectedEmitterIndex < m_EmitterSizeCurves.size()) m_EmitterSizeCurves.erase(m_EmitterSizeCurves.begin() + m_SelectedEmitterIndex);
		if (m_SelectedEmitterIndex < m_EmitterAlphaCurves.size()) m_EmitterAlphaCurves.erase(m_EmitterAlphaCurves.begin() + m_SelectedEmitterIndex);
		if (m_SelectedEmitterIndex < m_EmitterSpeedCurves.size()) m_EmitterSpeedCurves.erase(m_EmitterSpeedCurves.begin() + m_SelectedEmitterIndex);
		if (m_SelectedEmitterIndex < m_EmitterCurveType.size()) m_EmitterCurveType.erase(m_EmitterCurveType.begin() + m_SelectedEmitterIndex);
		if (m_SelectedEmitterIndex < m_EmitterExtended.size()) m_EmitterExtended.erase(m_EmitterExtended.begin() + m_SelectedEmitterIndex);


		if (m_SelectedEmitterIndex >= m_Asset.Emitters.size())
			m_SelectedEmitterIndex = m_Asset.Emitters.size() - 1;


		m_Dirty = true;
		m_StatusFeedback = "已删除发射器: " + delName;
		SyncSimulationFromAsset();
	}


	void ParticleEditorPanel::MoveEmitterUp()
	{
		if (m_SelectedEmitterIndex == 0 || m_SelectedEmitterIndex >= m_Asset.Emitters.size()) return;
		PushUndoSnapshot();


		size_t prev = m_SelectedEmitterIndex - 1;
		std::swap(m_Asset.Emitters[m_SelectedEmitterIndex], m_Asset.Emitters[prev]);
		std::swap(m_EmitterSolo[m_SelectedEmitterIndex], m_EmitterSolo[prev]);
		std::swap(m_EmitterMute[m_SelectedEmitterIndex], m_EmitterMute[prev]);
		std::swap(m_EmitterLocked[m_SelectedEmitterIndex], m_EmitterLocked[prev]);
		std::swap(m_EmitterGradients[m_SelectedEmitterIndex], m_EmitterGradients[prev]);
		std::swap(m_EmitterSizeCurves[m_SelectedEmitterIndex], m_EmitterSizeCurves[prev]);
		std::swap(m_EmitterAlphaCurves[m_SelectedEmitterIndex], m_EmitterAlphaCurves[prev]);
		std::swap(m_EmitterSpeedCurves[m_SelectedEmitterIndex], m_EmitterSpeedCurves[prev]);
		std::swap(m_EmitterCurveType[m_SelectedEmitterIndex], m_EmitterCurveType[prev]);
		std::swap(m_EmitterExtended[m_SelectedEmitterIndex], m_EmitterExtended[prev]);


		m_SelectedEmitterIndex = prev;
		m_Dirty = true;
		m_StatusFeedback = "已上移发射器层级";
		SyncSimulationFromAsset();
	}


	void ParticleEditorPanel::MoveEmitterDown()
	{
		if (m_SelectedEmitterIndex + 1 >= m_Asset.Emitters.size()) return;
		PushUndoSnapshot();


		size_t next = m_SelectedEmitterIndex + 1;
		std::swap(m_Asset.Emitters[m_SelectedEmitterIndex], m_Asset.Emitters[next]);
		std::swap(m_EmitterSolo[m_SelectedEmitterIndex], m_EmitterSolo[next]);
		std::swap(m_EmitterMute[m_SelectedEmitterIndex], m_EmitterMute[next]);
		std::swap(m_EmitterLocked[m_SelectedEmitterIndex], m_EmitterLocked[next]);
		std::swap(m_EmitterGradients[m_SelectedEmitterIndex], m_EmitterGradients[next]);
		std::swap(m_EmitterSizeCurves[m_SelectedEmitterIndex], m_EmitterSizeCurves[next]);
		std::swap(m_EmitterAlphaCurves[m_SelectedEmitterIndex], m_EmitterAlphaCurves[next]);
		std::swap(m_EmitterSpeedCurves[m_SelectedEmitterIndex], m_EmitterSpeedCurves[next]);
		std::swap(m_EmitterCurveType[m_SelectedEmitterIndex], m_EmitterCurveType[next]);
		std::swap(m_EmitterExtended[m_SelectedEmitterIndex], m_EmitterExtended[next]);


		m_SelectedEmitterIndex = next;
		m_Dirty = true;
		m_StatusFeedback = "已下移发射器层级";
		SyncSimulationFromAsset();
	}


	void ParticleEditorPanel::EnsurePerEmitterState()
	{
		const size_t count = m_Asset.Emitters.size();
		if (m_EmitterSolo.size() < count) m_EmitterSolo.resize(count, false);
		if (m_EmitterMute.size() < count) m_EmitterMute.resize(count, false);
		if (m_EmitterLocked.size() < count) m_EmitterLocked.resize(count, false);
		if (m_EmitterCurveType.size() < count) m_EmitterCurveType.resize(count, "Linear");
		if (m_EmitterExtended.size() < count) m_EmitterExtended.resize(count);


		while (m_EmitterGradients.size() < count)
		{
			ColorGradient g;
			g.AddColorStop(0.0f, glm::vec3(1.0f, 0.7f, 0.2f));
			g.AddColorStop(1.0f, glm::vec3(0.9f, 0.1f, 0.0f));
			g.AddAlphaStop(0.0f, 0.0f);
			g.AddAlphaStop(0.15f, 1.0f);
			g.AddAlphaStop(0.85f, 0.9f);
			g.AddAlphaStop(1.0f, 0.0f);
			g.PrecomputeLut(256);
			m_EmitterGradients.push_back(std::move(g));
		}


		while (m_EmitterSizeCurves.size() < count)
		{
			CubicBezierCurve c = CubicBezierCurve::EaseOut();
			c.PrecomputeLut(256);
			m_EmitterSizeCurves.push_back(std::move(c));
		}


		while (m_EmitterAlphaCurves.size() < count)
		{
			CubicBezierCurve a = CubicBezierCurve::Linear();
			a.Clear();
			a.AddKeyframe({ 0.0f, 0.0f });
			a.AddKeyframe({ 0.2f, 1.0f });
			a.AddKeyframe({ 0.8f, 1.0f });
			a.AddKeyframe({ 1.0f, 0.0f });
			a.AutoSmoothTangents();
			a.PrecomputeLut(256);
			m_EmitterAlphaCurves.push_back(std::move(a));
		}


		while (m_EmitterSpeedCurves.size() < count)
		{
			CubicBezierCurve s = CubicBezierCurve::Linear();
			s.PrecomputeLut(256);
			m_EmitterSpeedCurves.push_back(std::move(s));
		}


		if (m_SelectedEmitterIndex >= count && count > 0)
		{
			m_SelectedEmitterIndex = count - 1;
		}
	}


	bool ParticleEditorPanel::IsEmitterActive(size_t index) const
	{
		if (index >= m_Asset.Emitters.size()) return false;
		if (!m_Asset.Emitters[index].Enabled) return false;
		if (index < m_EmitterMute.size() && m_EmitterMute[index]) return false;


		bool hasAnySolo = false;
		for (bool s : m_EmitterSolo)
		{
			if (s) { hasAnySolo = true; break; }
		}
		if (hasAnySolo)
		{
			return (index < m_EmitterSolo.size() && m_EmitterSolo[index]);
		}
		return true;
	}


	bool ParticleEditorPanel::IsEmitterLocked(size_t index) const
	{
		if (index < m_EmitterLocked.size()) return m_EmitterLocked[index];
		return false;
	}


	void ParticleEditorPanel::RefreshAvailableAssets()
	{
		m_AvailableAssets.clear();
		std::filesystem::path assetRoot = World::Paths::AssetRoot();
		std::error_code ec;
		if (std::filesystem::exists(assetRoot, ec))
		{
			for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot, ec))
			{
				if (entry.is_regular_file() && entry.path().extension() == ".wvfx")
				{
					std::string rel = std::filesystem::relative(entry.path(), assetRoot, ec).generic_string();
					m_AvailableAssets.push_back(rel);
				}
			}
		}
	}


	void ParticleEditorPanel::SyncSimulationFromAsset()
	{
		EnsurePerEmitterState();
		m_LivePools.clear();
		m_SimTime = 0.0f;


		for (size_t i = 0; i < m_Asset.Emitters.size(); ++i)
		{
			const auto& em = m_Asset.Emitters[i];
			auto pool = std::make_unique<ParticlePool>(std::max<size_t>(100, em.MaxParticles));
			pool->SetSeed(m_Asset.Seed + static_cast<uint32_t>(i * 101));
			pool->SetMaterialSettings(em.Material);
			pool->SetCollisionSettings(em.Collision);
			pool->SetLinearDrag(em.LinearDrag);
			pool->SetVortex(em.VortexStrength, em.VortexAxis);
			pool->SetTurbulence(em.TurbulenceStrength, em.TurbulenceFrequency);


			if (i < m_EmitterExtended.size())
			{
				const auto& ext = m_EmitterExtended[i];
				if (ext.CurlNoiseStrength > 0.0f || ext.ModuleCurlNoiseEnabled)
				{
					pool->SetCurlNoise(ext.CurlNoiseStrength, ext.CurlNoiseFrequency, ext.CurlNoiseOctaves);
				}
				pool->SetRibbonEnabled(ext.RibbonEnabled);
			}


			if (i < m_EmitterSizeCurves.size()) pool->SetSizeCurve(m_EmitterSizeCurves[i]);
			if (i < m_EmitterSpeedCurves.size()) pool->SetSpeedCurve(m_EmitterSpeedCurves[i]);
			if (i < m_EmitterGradients.size()) pool->SetColorGradient(m_EmitterGradients[i]);


			pool->ClearBursts();
			for (const auto& b : em.Bursts)
			{
				pool->AddBurst(b.Time, b.Count);
			}


			pool->ClearSubEmitters();
			for (const auto& s : em.SubEmitters)
			{
				pool->AddSubEmitter(s);
			}


			m_LivePools.push_back(std::move(pool));
		}
	}


	void ParticleEditorPanel::StepSimulation(float dt)
	{
		if (m_LivePools.size() != m_Asset.Emitters.size())
		{
			SyncSimulationFromAsset();
		}


		m_SimTime += dt * m_PlaybackSpeed;
		if (m_Asset.Looping && m_TimelineDuration > 0.1f)
		{
			if (m_SimTime > m_TimelineDuration)
			{
				m_SimTime = std::fmod(m_SimTime, m_TimelineDuration);
			}
		}


		for (size_t i = 0; i < m_Asset.Emitters.size() && i < m_LivePools.size(); ++i)
		{
			if (!IsEmitterActive(i)) continue;


			const auto& em = m_Asset.Emitters[i];
			auto& pool = m_LivePools[i];


			ParticleSystemComponent comp;
			comp.EmissionRate = em.EmissionRate;
			comp.Lifetime = em.Lifetime;
			comp.Speed = em.Speed;
			comp.SpeedVariance = em.SpeedVariance;
			comp.Gravity = em.Gravity;
			comp.SizeStart = em.SizeStart;
			comp.SizeEnd = em.SizeEnd;
			comp.MaxParticles = em.MaxParticles;
			comp.EmitterType = em.EmitterType;
			comp.RenderMode = em.FacingMode;
			comp.BlendMode = em.Material.BlendMode;
			comp.LockToXYPlane = em.LockToXY;
			comp.Looping = m_Asset.Looping;
			comp.IsPlaying = true;


			// 点引力场物理推演更新 (Point Attractor Force Simulation)
			if (i < m_EmitterExtended.size())
			{
				const auto& ext = m_EmitterExtended[i];
				if (ext.ModuleAttractorEnabled && ext.PointAttractorStrength != 0.0f)
				{
					for (auto& p : pool->GetParticlesMut())
					{
						if (!p.Active) continue;
						glm::vec3 toAttractor = ext.PointAttractorCenter - p.Position;
						float dist = glm::length(toAttractor);
						if (dist > 0.05f && dist < ext.PointAttractorRadius)
						{
							float factor = 1.0f - dist / ext.PointAttractorRadius;
							glm::vec3 pull = glm::normalize(toAttractor) * (ext.PointAttractorStrength * factor);
							p.Velocity += pull * dt;
						}
					}
				}
			}


			pool->Update(dt * m_PlaybackSpeed, comp, glm::vec3(0.0f));
		}
	}


	void ParticleEditorPanel::ScrubToTime(float targetTime)
	{
		targetTime = std::clamp(targetTime, 0.0f, m_TimelineDuration);
		SyncSimulationFromAsset();
		const float stepDt = 0.0333f;
		float current = 0.0f;
		while (current < targetTime)
		{
			float dt = std::min(stepDt, targetTime - current);
			StepSimulation(dt);
			current += dt;
		}
		m_SimTime = targetTime;
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
			m_UndoStack.clear();
			m_RedoStack.clear();
			m_UndoExtendedStack.clear();
			m_RedoExtendedStack.clear();
			m_Asset = std::move(loaded);
			m_LoadedPath = normalized;
			m_Dirty = false;
			m_SelectedEmitterIndex = 0;
			m_StatusFeedback = "成功加载: " + normalized;
			EnsurePerEmitterState();
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


	void ParticleEditorPanel::ApplyCodeToAsset()
	{
		ParticleAsset parsed;
		if (ParticleAssetSerializer::DeserializeFromString(m_CodeBuffer, parsed))
		{
			PushUndoSnapshot();
			m_Asset = std::move(parsed);
			m_Dirty = true;
			m_StatusFeedback = "[成功] YAML 代码已成功编译应用到视口";
			EnsurePerEmitterState();
			SyncSimulationFromAsset();
		}
		else
		{
			m_StatusFeedback = "[错误] YAML 代码格式错误，请检查语法";
		}
	}


	void ParticleEditorPanel::SyncCodeFromAsset()
	{
		m_CodeBuffer = ParticleAssetSerializer::SerializeToString(m_Asset);
		m_StatusFeedback = "已从视口同步最新 YAML 声明代码";
	}


	bool ParticleEditorPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		(void)alt;
		if (ctrl && !shift && (keyCode == KeyCodes::Z || keyCode == 90))
		{
			if (CanUndo()) { Undo(); return true; }
		}
		if (ctrl && ((shift && (keyCode == KeyCodes::Z || keyCode == 90)) || keyCode == KeyCodes::Y || keyCode == 89))
		{
			if (CanRedo()) { Redo(); return true; }
		}
		if (ctrl && !shift && (keyCode == KeyCodes::S || keyCode == 83))
		{
			SaveAsset();
			return true;
		}
		if (ctrl && keyCode == KeyCodes::Enter)
		{
			ApplyCodeToAsset();
			return true;
		}
		if (!ctrl && !shift && !alt && (keyCode == KeyCodes::F || keyCode == 70))
		{
			FocusSelectedEmitter();
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
		}


		if (m_IsPlaying)
		{
			StepSimulation(0.0166f);
		}


		// 平滑相机聚焦过渡
		m_CameraFocus = glm::mix(m_CameraFocus, m_TargetCameraFocus, 0.10f);


		const float padX = 10.0f;
		const float contentW = rect.W - padX * 2.0f;
		float curY = rect.Y + 8.0f;


		// =========================================================================
		// 1. 顶部操作工具条 (Top Bar)
		// =========================================================================
		const Wui::WuiRect topBarRect { rect.X + padX, curY, contentW, 36.0f };
		RenderTopBar(ctx, topBarRect, theme, host);
		curY += topBarRect.H + 8.0f;


		const float availH = std::max(400.0f, rect.H - (curY - rect.Y) - 10.0f);


		// =========================================================================
		// 2. 主工作区模式分流 (Visual vs Code)
		// =========================================================================
		if (m_ViewMode == ParticleEditorViewMode::Code)
		{
			// ======== 代码编辑模式 (YAML Code Editor) ========
			const Wui::WuiRect codeToolbar { rect.X + padX, curY, contentW, 32.0f };
			Wui::BarSurface(ctx, codeToolbar, theme.PanelHeader, theme.Border);


			float cx = codeToolbar.X + 8.0f;
			const Wui::WuiRect applyBtn { cx, codeToolbar.Y + 4.0f, 130.0f, 24.0f };
			Wui::Tooltip(ctx, applyBtn, "编译并应用当前 YAML 代码到视口与特效系统 (Ctrl+Enter)");
			if (Wui::Button(ctx, Wui::HashId("vfx.code.apply"), applyBtn, "[运行] 应用代码 (Ctrl+Enter)", theme))
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
				m_CodeBuffer += "\n    VortexStrength: 3.5\n    VortexAxis: [0.0, 1.0, 0.0]\n";
				ApplyCodeToAsset();
			}


			const Wui::WuiRect codeBox { rect.X + padX, curY + 36.0f, contentW, availH - 36.0f };
			Wui::BarSurface(ctx, codeBox, Wui::WuiColor { 0.05f, 0.06f, 0.08f, 1.0f }, theme.Border);


			std::istringstream stream(m_CodeBuffer);
			std::string line;
			int lineIdx = 0;
			float lineY = codeBox.Y + 8.0f;
			const float lineHeight = 18.0f;


			while (std::getline(stream, line) && lineY < codeBox.Y + codeBox.H - 16.0f)
			{
				lineIdx++;
				if (lineY >= codeBox.Y + 4.0f)
				{
					Wui::Label(ctx, { codeBox.X + 8.0f, lineY }, std::to_string(lineIdx), Wui::WuiColor { 0.35f, 0.40f, 0.48f, 0.8f }, 9.5f);
					Wui::WuiColor textCol = theme.Text;
					if (line.find(":") != std::string::npos && line.find("-") == std::string::npos)
					{
						if (line.find("Emitters") != std::string::npos || line.find("Name") != std::string::npos)
							textCol = theme.Accent;
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
			// =========================================================================
			// 3. 现代四分区专业工作区布局 (4-Panel Professional Workspace)
			// =========================================================================
			EnsurePerEmitterState();


			// 限制分区尺寸在合理区间内 (大纲拓宽至 270px 消除截字)
			m_OutlinerWidth = std::clamp(m_OutlinerWidth, 200.0f, 450.0f);
			m_InspectorWidth = std::clamp(m_InspectorWidth, 260.0f, 520.0f);
			m_TimelineHeight = std::clamp(m_TimelineHeight, 140.0f, 400.0f);


			const float splitterThickness = 4.0f;
			const float centerW = std::max(200.0f, contentW - m_OutlinerWidth - m_InspectorWidth - splitterThickness * 2.0f);


			const float leftX = rect.X + padX;
			const float leftW = m_OutlinerWidth;


			const float leftSplitterX = leftX + leftW;
			const float centerX = leftSplitterX + splitterThickness;


			const float rightSplitterX = centerX + centerW;
			const float rightX = rightSplitterX + splitterThickness;
			const float rightW = m_InspectorWidth;


			const float vpHeight = std::max(120.0f, availH - m_TimelineHeight - splitterThickness);
			const float horizSplitterY = curY + vpHeight;
			const float timelineY = horizSplitterY + splitterThickness;
			const float timelineH = m_TimelineHeight;


			// 四大分区矩形定义
			const Wui::WuiRect outlinerRect { leftX, curY, leftW, availH };
			const Wui::WuiRect leftSplitterRect { leftSplitterX, curY, splitterThickness, availH };


			const Wui::WuiRect vpRect { centerX, curY, centerW, vpHeight };
			const Wui::WuiRect horizSplitterRect { centerX, horizSplitterY, centerW, splitterThickness };
			const Wui::WuiRect timelineRect { centerX, timelineY, centerW, timelineH };


			const Wui::WuiRect rightSplitterRect { rightSplitterX, curY, splitterThickness, availH };
			const Wui::WuiRect inspectorRect { rightX, curY, rightW, availH };


			// 分别渲染各大专业分区
			RenderOutliner(ctx, outlinerRect, theme);
			RenderViewport(ctx, vpRect, theme, host);
			RenderTimeline(ctx, timelineRect, theme, host);
			RenderInspector(ctx, inspectorRect, theme, host);


			// 交互式 Draggable Splitters (可拖动调节分区大小)
			Wui::Splitter(ctx, Wui::HashId("vfx.split.outliner"), leftSplitterRect, true,
				m_OutlinerWidth, 200.0f, 450.0f, theme);


			Wui::Splitter(ctx, Wui::HashId("vfx.split.timeline"), horizSplitterRect, false,
				m_TimelineHeight, 140.0f, 400.0f, theme, /*reverse=*/true);


			Wui::Splitter(ctx, Wui::HashId("vfx.split.inspector"), rightSplitterRect, true,
				m_InspectorWidth, 260.0f, 520.0f, theme, /*reverse=*/true);
		}


		// 右键上下文菜单 (若激活则在最顶层绘制)
		RenderContextMenu(ctx, theme);
	}


	void ParticleEditorPanel::RenderTopBar(Wui::WuiContext& ctx, const Wui::WuiRect& barRect, const Wui::WuiTheme& theme, PanelHost& host)
	{
		(void)host;
		Wui::BarSurface(ctx, barRect, theme.PanelHeader, theme.Border);


		Wui::Label(ctx, { barRect.X + 10.0f, barRect.Y + 9.0f }, "视觉特效系统 (WorldVFX Studio 2.0)", theme.Accent, 12.5f);


		float tbX = barRect.X + 225.0f;


		const Wui::WuiRect newBtn { tbX, barRect.Y + 6.0f, 44.0f, 24.0f };
		Wui::Tooltip(ctx, newBtn, "新建复合特效工程 (New VFX Asset)");
		if (Wui::Button(ctx, Wui::HashId("vfx.bar.new"), newBtn, "新建", theme))
		{
			PushUndoSnapshot();
			NewAsset();
		}
		tbX += 48.0f;


		const Wui::WuiRect saveBtn { tbX, barRect.Y + 6.0f, 44.0f, 24.0f };
		Wui::Tooltip(ctx, saveBtn, "保存当前特效到 .wvfx 资产文件 (Ctrl+S)");
		if (Wui::Button(ctx, Wui::HashId("vfx.bar.save"), saveBtn, "保存", theme))
		{
			SaveAsset();
		}
		tbX += 50.0f;


		// 撤销 / 重做按钮组
		const bool canUndo = CanUndo();
		const Wui::WuiRect undoBtn { tbX, barRect.Y + 6.0f, 48.0f, 24.0f };
		Wui::Tooltip(ctx, undoBtn, canUndo ? "撤销上一步操作 (Ctrl+Z)" : "撤销栈为空");
		if (Wui::Button(ctx, Wui::HashId("vfx.bar.undo"), undoBtn, "撤销 (Z)", theme) && canUndo)
		{
			Undo();
		}
		tbX += 52.0f;


		const bool canRedo = CanRedo();
		const Wui::WuiRect redoBtn { tbX, barRect.Y + 6.0f, 48.0f, 24.0f };
		Wui::Tooltip(ctx, redoBtn, canRedo ? "重做下一步操作 (Ctrl+Y / Ctrl+Shift+Z)" : "重做栈为空");
		if (Wui::Button(ctx, Wui::HashId("vfx.bar.redo"), redoBtn, "重做 (Y)", theme) && canRedo)
		{
			Redo();
		}
		tbX += 54.0f;


		const float modeBtnW = 68.0f;
		const Wui::WuiRect visualModeBtn { tbX, barRect.Y + 6.0f, modeBtnW, 24.0f };
		Wui::Tooltip(ctx, visualModeBtn, "切换到参数可视化模块工作区");
		if (Wui::Button(ctx, Wui::HashId("vfx.mode.visual"), visualModeBtn,
			m_ViewMode == ParticleEditorViewMode::Visual ? "[检视]" : "  检视", theme))
		{
			m_ViewMode = ParticleEditorViewMode::Visual;
		}
		tbX += modeBtnW + 4.0f;


		const Wui::WuiRect codeModeBtn { tbX, barRect.Y + 6.0f, modeBtnW, 24.0f };
		Wui::Tooltip(ctx, codeModeBtn, "切换到声明式代码/YAML 编辑视图，热重载实时生效");
		if (Wui::Button(ctx, Wui::HashId("vfx.mode.code"), codeModeBtn,
			m_ViewMode == ParticleEditorViewMode::Code ? "[代码]" : "  代码", theme))
		{
			m_ViewMode = ParticleEditorViewMode::Code;
			SyncCodeFromAsset();
		}
		tbX += modeBtnW + 10.0f;


		// 5 大高水准 3D 特效案例集一键切换 (Industrial 3D Showcase Presets)
		Wui::Label(ctx, { tbX, barRect.Y + 10.0f }, "3D案例:", theme.TextMuted, 10.5f);
		tbX += 45.0f;


		const struct PresetEntry {
			const char* Id;
			const char* Label;
			const char* Name;
			const char* Tip;
		} presets[] = {
			{ "vfx.p.tornado", "龙卷火", "FlameTornado3D", "3D 火焰龙卷风: 圆锥内焰 + 强 3D 涡流飞旋火星 + 旋度噪声上升浓烟" },
			{ "vfx.p.plasma", "等离子束", "LightningPlasmaBeam3D", "3D 等离子闪电束: 核心高能光束 + 空间螺旋放电电弧 + 撞击点剧烈反弹火花" },
			{ "vfx.p.shield", "魔法护盾", "OrbitalMagicShield3D", "3D 环形魔法护盾: 空间符文旋转球环 + 轨道自发光动态点光源 + 脉冲能量内核" },
			{ "vfx.p.meteor", "流星撞击", "MeteorImpactExplosion3D", "3D 流星陨石高爆: 瞬态扩散冲击波 + 撞地反弹重力碎石 + 爆心火球 + 升腾黑烟" },
			{ "vfx.p.blizzard", "冰霜新星", "BlizzardFrostNova3D", "3D 暴风雪冰霜新星: 爆发冰晶冲击波 + 地表旋转冷气雾 + 悬浮漫游冰霜雪花" }
		};


		for (const auto& p : presets)
		{
			const float pBtnW = 72.0f;
			if (tbX + pBtnW + 10.0f < barRect.X + barRect.W - 140.0f)
			{
				const Wui::WuiRect pRect { tbX, barRect.Y + 6.0f, pBtnW, 24.0f };
				Wui::Tooltip(ctx, pRect, p.Tip);
				if (Wui::Button(ctx, Wui::HashId(p.Id), pRect, p.Label, theme))
				{
					LoadPreset(p.Name);
				}
				tbX += pBtnW + 3.0f;
			}
		}


		if (!m_StatusFeedback.empty())
		{
			Wui::WuiColor fbColor = (m_StatusFeedback.find("成功") != std::string::npos || m_StatusFeedback.find("成功") != std::string::npos)
				? Wui::WuiColor { 0.3f, 0.85f, 0.4f, 1.0f }
				: (m_StatusFeedback.find("失败") != std::string::npos || m_StatusFeedback.find("失败") != std::string::npos)
					? Wui::WuiColor { 0.95f, 0.3f, 0.3f, 1.0f }
					: theme.TextMuted;
			Wui::Label(ctx, { tbX + 8.0f, barRect.Y + 10.0f }, m_StatusFeedback, fbColor, 10.5f);
		}
	}


	// =========================================================================
	// 左侧发射器大纲管理 (Outliner)
	// =========================================================================


	void ParticleEditorPanel::RenderOutliner(Wui::WuiContext& ctx, const Wui::WuiRect& outlinerRect, const Wui::WuiTheme& theme)
	{
		EnsurePerEmitterState();
		Wui::BarSurface(ctx, outlinerRect, theme.PanelBg, theme.Border);


		// 顶部大纲标题栏 (28px)
		const Wui::WuiRect headRect { outlinerRect.X, outlinerRect.Y, outlinerRect.W, 28.0f };
		Wui::BarSurface(ctx, headRect, theme.PanelHeader, theme.Border);
		Wui::Label(ctx, { headRect.X + 8.0f, headRect.Y + 7.0f }, "发射器大纲 (Outliner)", theme.Accent, 11.5f);
		std::string cntStr = "(" + std::to_string(m_Asset.Emitters.size()) + ")";
		Wui::Label(ctx, { headRect.X + outlinerRect.W - 38.0f, headRect.Y + 7.0f }, cntStr, theme.TextMuted, 10.5f);


		// 搜索过滤栏 (22px)
		const Wui::WuiRect searchRect { outlinerRect.X + 6.0f, outlinerRect.Y + 32.0f, outlinerRect.W - 12.0f, 22.0f };
		Wui::SearchField(ctx, Wui::HashId("vfx.outliner.search"), searchRect, m_FilterText, "过滤发射器... Q", theme);


		// 顶部功能工具栏: [+ 新建] [克隆] [删除] [^] [v] (24px)
		float toolY = outlinerRect.Y + 58.0f;
		float tx = outlinerRect.X + 6.0f;


		const Wui::WuiRect newEmBtn { tx, toolY, 56.0f, 22.0f };
		Wui::Tooltip(ctx, newEmBtn, "从模板新建特效发射器图层 (喷泉/火焰/火花/浓烟/光束/拖尾/碎屑/空)");
		if (Wui::Button(ctx, Wui::HashId("vfx.out.new"), newEmBtn, "+ 新建", theme))
		{
			m_NewEmitterMenuOpen = !m_NewEmitterMenuOpen;
		}
		tx += 59.0f;


		const Wui::WuiRect cloneBtn { tx, toolY, 44.0f, 22.0f };
		Wui::Tooltip(ctx, cloneBtn, "克隆当前选中的发射器图层");
		if (Wui::Button(ctx, Wui::HashId("vfx.out.clone"), cloneBtn, "克隆", theme))
		{
			CloneSelectedEmitter();
		}
		tx += 47.0f;


		const bool canDelete = (m_Asset.Emitters.size() > 1);
		const Wui::WuiRect delBtn { tx, toolY, 44.0f, 22.0f };
		Wui::Tooltip(ctx, delBtn, canDelete ? "删除选中的发射器图层" : "至少保留一个发射器图层");
		if (Wui::Button(ctx, Wui::HashId("vfx.out.del"), delBtn, "删除", theme) && canDelete)
		{
			DeleteSelectedEmitter();
		}
		tx += 47.0f;


		const bool canUp = (m_SelectedEmitterIndex > 0);
		const Wui::WuiRect upBtn { tx, toolY, 26.0f, 22.0f };
		Wui::Tooltip(ctx, upBtn, "上移图层层级");
		if (Wui::Button(ctx, Wui::HashId("vfx.out.up"), upBtn, "[^]", theme) && canUp)
		{
			MoveEmitterUp();
		}
		tx += 29.0f;


		const bool canDown = (m_SelectedEmitterIndex + 1 < m_Asset.Emitters.size());
		const Wui::WuiRect downBtn { tx, toolY, 26.0f, 22.0f };
		Wui::Tooltip(ctx, downBtn, "下移图层层级");
		if (Wui::Button(ctx, Wui::HashId("vfx.out.down"), downBtn, "[v]", theme) && canDown)
		{
			MoveEmitterDown();
		}


		// 模板弹出菜单 ([+ 新建] 下拉抽屉)
		float listY = toolY + 26.0f;
		if (m_NewEmitterMenuOpen)
		{
			const Wui::WuiRect menuRect { outlinerRect.X + 6.0f, listY, outlinerRect.W - 12.0f, 130.0f };
			Wui::BarSurface(ctx, menuRect, Wui::WuiColor { 0.12f, 0.14f, 0.18f, 0.98f }, theme.Accent);


			const struct TemplateEntry {
				const char* Id;
				const char* Label;
				const char* Type;
			} tEntries[] = {
				{ "vfx.t.fount", "[喷泉] Fountain", "Fountain" },
				{ "vfx.t.fire", "[火焰] Fire", "Fire" },
				{ "vfx.t.spark", "[火花] Sparks", "Sparks" },
				{ "vfx.t.smoke", "[浓烟] Smoke", "Smoke" },
				{ "vfx.t.beam", "[光束] Beam", "Beam" },
				{ "vfx.t.ribb", "~ 拖尾 (Ribbon)", "Ribbon" },
				{ "vfx.t.mesh", "[碎屑] Mesh", "Mesh" },
				{ "vfx.t.empty", "[空模板] Empty", "Empty" }
			};


			float my = menuRect.Y + 4.0f;
			for (size_t t = 0; t < 8; ++t)
			{
				const float colW = (menuRect.W - 12.0f) * 0.5f;
				const float mx = menuRect.X + 4.0f + (t % 2) * (colW + 4.0f);
				if (t > 0 && t % 2 == 0) my += 28.0f;


				const Wui::WuiRect btnR { mx, my, colW, 24.0f };
				if (Wui::Button(ctx, Wui::HashId(tEntries[t].Id), btnR, tEntries[t].Label, theme))
				{
					AddEmitterFromTemplate(tEntries[t].Type);
					m_NewEmitterMenuOpen = false;
				}
			}
			listY += 136.0f;
		}


		// 根系统节点条目 (Root System Node)
		const Wui::WuiRect sysRow { outlinerRect.X + 4.0f, listY, outlinerRect.W - 8.0f, 24.0f };
		Wui::BarSurface(ctx, sysRow, theme.PanelHeader, theme.Border);
		std::string sysTitle = "[系统] " + m_Asset.Name;
		if (sysTitle.size() > 22) sysTitle = sysTitle.substr(0, 20) + "..";
		Wui::Label(ctx, { sysRow.X + 6.0f, sysRow.Y + 5.0f }, sysTitle, theme.Text, 11.0f);
		std::string durStr = std::to_string(m_TimelineDuration).substr(0, 3) + "s" + (m_Asset.Looping ? " ⟳" : "");
		Wui::Label(ctx, { sysRow.X + sysRow.W - 46.0f, sysRow.Y + 5.0f }, durStr, theme.TextMuted, 9.5f);
		listY += 26.0f;


		// 大纲发射器列表容器
		const float bottomPad = 10.0f;
		float listH = std::max(60.0f, outlinerRect.H - (listY - outlinerRect.Y) - bottomPad);
		const Wui::WuiRect listRect { outlinerRect.X + 4.0f, listY, outlinerRect.W - 8.0f, listH };


		// 发射器列表项循环渲染
		float rowY = listRect.Y;
		for (size_t i = 0; i < m_Asset.Emitters.size() && rowY + 28.0f <= listRect.Y + listRect.H; ++i)
		{
			auto& em = m_Asset.Emitters[i];
			if (!m_FilterText.empty())
			{
				std::string fLow = m_FilterText;
				std::transform(fLow.begin(), fLow.end(), fLow.begin(), ::tolower);
				std::string nLow = em.Name;
				std::transform(nLow.begin(), nLow.end(), nLow.begin(), ::tolower);
				if (nLow.find(fLow) == std::string::npos) continue;
			}


			const bool isSelected = (i == m_SelectedEmitterIndex);
			const bool isRenaming = (m_RenamingEmitterIndex == static_cast<int>(i));
			const Wui::WuiRect rowRect { listRect.X, rowY, listRect.W, 26.0f };


			if (isSelected)
			{
				Wui::BarSurface(ctx, rowRect, theme.Selection, theme.Accent);
			}
			else if (ctx.IsHovered(rowRect))
			{
				Wui::BarSurface(ctx, rowRect, theme.HoverBg, theme.Border);
			}


			// 发射器类型彩色徽标 (Type Icon)
			Wui::WuiColor iconCol { 1.0f, 0.55f, 0.15f, 1.0f }; // 默认火苗橙 (Sprite)
			std::string iconStr = "[S]";
			if (i < m_EmitterExtended.size())
			{
				if (m_EmitterExtended[i].RibbonEnabled) { iconCol = Wui::WuiColor { 0.15f, 0.85f, 0.95f, 1.0f }; iconStr = "~"; }
				else if (m_EmitterExtended[i].MeshEnabled) { iconCol = Wui::WuiColor { 0.75f, 0.40f, 0.95f, 1.0f }; iconStr = "[M]"; }
				else if (m_EmitterExtended[i].LightEnabled) { iconCol = Wui::WuiColor { 1.0f, 0.85f, 0.20f, 1.0f }; iconStr = "[L]"; }
			}
			Wui::Label(ctx, { rowRect.X + 4.0f, rowRect.Y + 5.0f }, iconStr, iconCol, 11.5f);


			// 激活使能勾选 (Checkbox)
			const Wui::WuiRect chkRect { rowRect.X + 16.0f, rowRect.Y + 4.0f, 18.0f, 18.0f };
			bool en = em.Enabled;
			if (Wui::Checkbox(ctx, Wui::HashId(("vfx.out.chk." + std::to_string(i)).c_str()), chkRect, "", en, theme))
			{
				PushUndoSnapshot();
				m_Asset.Emitters[i].Enabled = en;
				SyncSimulationFromAsset();
			}


			// 发射器名称与就地双击重命名 (Inline Rename)
			const float opsW = 68.0f;
			const float nameW = std::max(40.0f, rowRect.W - 36.0f - opsW);
			const Wui::WuiRect nameRect { rowRect.X + 36.0f, rowRect.Y, nameW, 26.0f };


			if (isRenaming)
			{
				std::string rBufferStr(m_RenameBuffer);
				const Wui::WuiRect rInputRect { nameRect.X, nameRect.Y + 2.0f, nameRect.W - 4.0f, 22.0f };
				if (Wui::TextField(ctx, Wui::HashId("vfx.out.rename.field"), rInputRect, rBufferStr, theme))
				{
					std::snprintf(m_RenameBuffer, sizeof(m_RenameBuffer), "%s", rBufferStr.c_str());
				}
				if (ctx.WasKeyPressed(KeyCodes::Enter) || (ctx.Input().MouseClicked[0] && !ctx.IsHovered(rInputRect)))
				{
					if (rBufferStr.empty()) rBufferStr = "Emitter";
					PushUndoSnapshot();
					em.Name = rBufferStr;
					m_RenamingEmitterIndex = -1;
					m_Dirty = true;
				}
			}
			else
			{
				if (ctx.IsHovered(nameRect))
				{
					if (ctx.Input().MouseDoubleClicked[0])
					{
						m_RenamingEmitterIndex = static_cast<int>(i);
						std::snprintf(m_RenameBuffer, sizeof(m_RenameBuffer), "%s", em.Name.c_str());
					}
					else if (ctx.Input().MouseClicked[0])
					{
						if (m_SelectedEmitterIndex != i)
						{
							m_SelectedEmitterIndex = i;
							float focusY = std::clamp(em.Speed * 0.25f + em.Gravity.y * 0.1f, 0.5f, 2.5f);
							m_TargetCameraFocus = glm::vec3(0.0f, focusY, 0.0f);
						}
					}
				}


				// 宽裕名称显示，彻底消除截字
				std::string dName = em.Name;
				if (dName.size() > 18) dName = dName.substr(0, 16) + "..";
				Wui::Label(ctx, { nameRect.X + 2.0f, nameRect.Y + 6.0f }, dName, isSelected ? theme.Text : theme.TextMuted, 10.5f);
				Wui::Tooltip(ctx, nameRect, "双击重命名: " + em.Name);
			}


			// 单行快捷操作: [S] 独奏, [M] 静音, [L] 锁定
			float bx = rowRect.X + rowRect.W - 64.0f;


			// 独奏 [S] (高亮亮黄)
			const Wui::WuiRect sRect { bx, rowRect.Y + 3.0f, 19.0f, 20.0f };
			const bool isSolo = (m_EmitterSolo.size() > i && m_EmitterSolo[i]);
			Wui::Tooltip(ctx, sRect, "独奏该发射器 (Solo) - 仅模拟与显示此图层");
			if (Wui::Button(ctx, Wui::HashId(("vfx.out.solo." + std::to_string(i)).c_str()), sRect, isSolo ? "[S]" : "S", theme))
			{
				m_EmitterSolo[i] = !m_EmitterSolo[i];
			}
			if (isSolo)
			{
				Wui::LineSegment(ctx, { sRect.X, sRect.Y + sRect.H }, { sRect.X + sRect.W, sRect.Y + sRect.H }, Wui::WuiColor { 1.0f, 0.85f, 0.2f, 1.0f }, 2.0f);
			}
			bx += 21.0f;


			// 静音 [M] (变红醒目)
			const Wui::WuiRect mRect { bx, rowRect.Y + 3.0f, 19.0f, 20.0f };
			const bool isMute = (m_EmitterMute.size() > i && m_EmitterMute[i]);
			Wui::Tooltip(ctx, mRect, "静音该发射器 (Mute) - 隐藏并暂停此图层推演");
			if (Wui::Button(ctx, Wui::HashId(("vfx.out.mute." + std::to_string(i)).c_str()), mRect, isMute ? "[M]" : "M", theme))
			{
				m_EmitterMute[i] = !m_EmitterMute[i];
			}
			if (isMute)
			{
				Wui::LineSegment(ctx, { mRect.X, mRect.Y + mRect.H }, { mRect.X + mRect.W, mRect.Y + mRect.H }, Wui::WuiColor { 0.95f, 0.25f, 0.25f, 1.0f }, 2.0f);
			}
			bx += 21.0f;


			// 锁定 [L] (橙色防误触)
			const Wui::WuiRect lRect { bx, rowRect.Y + 3.0f, 19.0f, 20.0f };
			const bool isLock = (m_EmitterLocked.size() > i && m_EmitterLocked[i]);
			Wui::Tooltip(ctx, lRect, "锁定该发射器 (Lock) - 禁止在检视器与视口误触编辑");
			if (Wui::Button(ctx, Wui::HashId(("vfx.out.lock." + std::to_string(i)).c_str()), lRect, isLock ? "[L]" : "L", theme))
			{
				m_EmitterLocked[i] = !m_EmitterLocked[i];
			}
			if (isLock)
			{
				Wui::LineSegment(ctx, { lRect.X, lRect.Y + lRect.H }, { lRect.X + lRect.W, lRect.Y + lRect.H }, Wui::WuiColor { 1.0f, 0.55f, 0.15f, 1.0f }, 2.0f);
			}


			// 右键弹出上下文菜单
			if (ctx.IsHovered(rowRect) && ctx.Input().MouseClicked[1])
			{
				m_SelectedEmitterIndex = i;
				m_ContextMenuOpen = true;
				m_ContextMenuPos = ctx.Input().MousePos;
				m_ContextMenuTargetEmitter = i;
			}


			rowY += 28.0f;


			// 选中行可展开显示内部模块概览 (Spawn & Burst, Shape, Forces, Renderers)
			if (isSelected && i < m_EmitterExtended.size())
			{
				const Wui::WuiRect modRow { listRect.X + 16.0f, rowY, listRect.W - 20.0f, 18.0f };
				std::string shapeName = (em.EmitterType == ParticleEmitterType::Cone) ? "Cone" : (em.EmitterType == ParticleEmitterType::Sphere ? "Sphere" : (em.EmitterType == ParticleEmitterType::Box ? "Box" : "Point"));
				std::string modSummary = "|-- Spawn(" + std::to_string((int)em.EmissionRate) + "p/s) | " + shapeName + " | Forces";
				Wui::Label(ctx, { modRow.X, modRow.Y + 2.0f }, modSummary, Wui::WuiColor { 0.45f, 0.55f, 0.65f, 0.85f }, 9.5f);
				rowY += 20.0f;
			}
		}
	}


	void ParticleEditorPanel::RenderContextMenu(Wui::WuiContext& ctx, const Wui::WuiTheme& theme)
	{
		if (!m_ContextMenuOpen) return;


		const Wui::WuiRect menuRect { m_ContextMenuPos.x, m_ContextMenuPos.y, 140.0f, 150.0f };
		Wui::BarSurface(ctx, menuRect, Wui::WuiColor { 0.10f, 0.12f, 0.16f, 0.98f }, theme.Accent);


		auto menuBtn = [&](const char* id, const char* label, float y, auto action) {
			const Wui::WuiRect bRect { menuRect.X + 4.0f, y, menuRect.W - 8.0f, 22.0f };
			if (Wui::Button(ctx, Wui::HashId(id), bRect, label, theme))
			{
				action();
				m_ContextMenuOpen = false;
			}
		};


		float my = menuRect.Y + 4.0f;
		menuBtn("vfx.ctx.rename", "就地重命名 (F2)", my, [&]() {
			m_RenamingEmitterIndex = static_cast<int>(m_ContextMenuTargetEmitter);
			if (m_ContextMenuTargetEmitter < m_Asset.Emitters.size())
				std::snprintf(m_RenameBuffer, sizeof(m_RenameBuffer), "%s", m_Asset.Emitters[m_ContextMenuTargetEmitter].Name.c_str());
		});
		my += 24.0f;


		menuBtn("vfx.ctx.clone", "克隆发射器", my, [&]() {
			m_SelectedEmitterIndex = m_ContextMenuTargetEmitter;
			CloneSelectedEmitter();
		});
		my += 24.0f;


		menuBtn("vfx.ctx.solo", "切换独奏 (Solo)", my, [&]() {
			if (m_ContextMenuTargetEmitter < m_EmitterSolo.size())
				m_EmitterSolo[m_ContextMenuTargetEmitter] = !m_EmitterSolo[m_ContextMenuTargetEmitter];
		});
		my += 24.0f;


		menuBtn("vfx.ctx.mute", "切换静音 (Mute)", my, [&]() {
			if (m_ContextMenuTargetEmitter < m_EmitterMute.size())
				m_EmitterMute[m_ContextMenuTargetEmitter] = !m_EmitterMute[m_ContextMenuTargetEmitter];
		});
		my += 24.0f;


		menuBtn("vfx.ctx.up", "上移一层", my, [&]() {
			m_SelectedEmitterIndex = m_ContextMenuTargetEmitter;
			MoveEmitterUp();
		});
		my += 24.0f;


		menuBtn("vfx.ctx.del", "删除发射器", my, [&]() {
			m_SelectedEmitterIndex = m_ContextMenuTargetEmitter;
			DeleteSelectedEmitter();
		});


		if (ctx.Input().MouseClicked[0] && !ctx.IsHovered(menuRect))
		{
			m_ContextMenuOpen = false;
		}
	}


	// =========================================================================
	// 3D 空间朝向罗盘立方体/万向节 (Orientation Gimbal - 解决“无方向”)
	// =========================================================================


	void ParticleEditorPanel::RenderOrientationGimbal(Wui::WuiContext& ctx, const Wui::WuiRect& vpRect, const glm::mat4& viewMat, const Wui::WuiTheme& theme)
	{
		(void)theme;
		const glm::vec2 gimbalCenter { vpRect.X + vpRect.W - 45.0f, vpRect.Y + 52.0f };
		const float gimbalRadius = 24.0f;


		// 万向节半透明底盘
		const Wui::WuiRect discRect { gimbalCenter.x - gimbalRadius - 6.0f, gimbalCenter.y - gimbalRadius - 6.0f, (gimbalRadius + 6.0f) * 2.0f, (gimbalRadius + 6.0f) * 2.0f };
		Wui::BarSurface(ctx, discRect, Wui::WuiColor { 0.04f, 0.06f, 0.08f, 0.85f }, Wui::WuiColor { 0.20f, 0.25f, 0.35f, 0.6f });


		// 提取相机旋转矩阵
		const glm::mat3 viewRot = glm::mat3(viewMat);


		struct AxisDef {
			const char* Name;
			glm::vec3 WorldDir;
			Wui::WuiColor Color;
			float SnapYaw;
			float SnapPitch;
			glm::vec3 CamVec;
			glm::vec2 ScreenEnd;
		} axes[] = {
			{ "X", glm::vec3(1.0f, 0.0f, 0.0f), Wui::WuiColor { 0.95f, 0.22f, 0.22f, 1.0f }, 1.5708f, 0.0f, glm::vec3(0.0f), glm::vec2(0.0f) },
			{ "Y", glm::vec3(0.0f, 1.0f, 0.0f), Wui::WuiColor { 0.22f, 0.90f, 0.32f, 1.0f }, 0.0f, 1.45f, glm::vec3(0.0f), glm::vec2(0.0f) },
			{ "Z", glm::vec3(0.0f, 0.0f, 1.0f), Wui::WuiColor { 0.22f, 0.55f, 0.98f, 1.0f }, 0.0f, 0.0f, glm::vec3(0.0f), glm::vec2(0.0f) }
		};


		for (int a = 0; a < 3; ++a)
		{
			axes[a].CamVec = viewRot * axes[a].WorldDir;
			axes[a].ScreenEnd = gimbalCenter + glm::vec2(axes[a].CamVec.x, -axes[a].CamVec.y) * gimbalRadius;
		}


		// 从远到近绘制三个轴向 (Depth Sort)
		int order[3] = { 0, 1, 2 };
		std::sort(order, order + 3, [&](int a, int b) { return axes[a].CamVec.z < axes[b].CamVec.z; });


		for (int idx = 0; idx < 3; ++idx)
		{
			int a = order[idx];
			const auto& ax = axes[a];


			// 绘制指向端点的彩色轴线
			Wui::LineSegment(ctx, gimbalCenter, ax.ScreenEnd, ax.Color, 2.0f);


			// 轴端点圆形吸附按钮 (Radius 7px)
			const Wui::WuiRect handleRect { ax.ScreenEnd.x - 7.0f, ax.ScreenEnd.y - 7.0f, 14.0f, 14.0f };
			const bool isHover = ctx.IsHovered(handleRect);
			Wui::BarSurface(ctx, handleRect, isHover ? Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f } : ax.Color, ax.Color);
			Wui::Label(ctx, { handleRect.X + 3.0f, handleRect.Y + 1.0f }, ax.Name, Wui::WuiColor { 0.05f, 0.05f, 0.05f, 1.0f }, 9.0f);
			Wui::Tooltip(ctx, handleRect, std::string("吸附正交视角: +") + ax.Name);


			if (isHover && ctx.Input().MouseClicked[0])
			{
				m_OrbitYaw = ax.SnapYaw;
				m_OrbitPitch = ax.SnapPitch;
				m_IsOrbiting = false;
				m_IsPanning = false;
			}
		}


		// 罗盘中心点击吸附至 Iso 3D 轴侧透视
		const Wui::WuiRect centerRect { gimbalCenter.x - 4.0f, gimbalCenter.y - 4.0f, 8.0f, 8.0f };
		if (ctx.IsHovered(centerRect) && ctx.Input().MouseClicked[0])
		{
			m_OrbitYaw = 0.785f;
			m_OrbitPitch = 0.45f;
			m_IsOrbiting = false;
			m_IsPanning = false;
		}
	}


	// =========================================================================
	// 视口相机快捷操作: 复位与聚焦
	// =========================================================================

	void ParticleEditorPanel::ResetCamera()
	{
		m_OrbitYaw = 0.785f;
		m_OrbitPitch = 0.45f;
		m_CameraDistance = 6.0f;
		m_CameraFocus = glm::vec3(0.0f, 1.0f, 0.0f);
		m_TargetCameraFocus = glm::vec3(0.0f, 1.0f, 0.0f);
		m_IsOrbiting = false;
		m_IsPanning = false;
		m_IsDraggingGizmo = false;
		m_ActiveGizmoHandle = -1;
		m_StatusFeedback = "视口相机已复位至默认透视";
	}

	void ParticleEditorPanel::FocusSelectedEmitter()
	{
		if (m_SelectedEmitterIndex >= m_Asset.Emitters.size())
			return;

		const auto& em = m_Asset.Emitters[m_SelectedEmitterIndex];
		glm::vec3 focusPoint(0.0f, 1.0f, 0.0f);
		float targetDist = 6.0f;

		if (m_SelectedEmitterIndex < m_LivePools.size() && m_LivePools[m_SelectedEmitterIndex]->ActiveCount() > 0)
		{
			glm::vec3 minP(1e9f);
			glm::vec3 maxP(-1e9f);
			for (const auto& p : m_LivePools[m_SelectedEmitterIndex]->GetParticles())
			{
				if (!p.Active) continue;
				minP = glm::min(minP, p.Position);
				maxP = glm::max(maxP, p.Position);
			}
			focusPoint = (minP + maxP) * 0.5f;
			const float extent = glm::length(maxP - minP);
			targetDist = std::clamp(extent * 1.5f, 2.5f, 40.0f);
		}
		else
		{
			EnsurePerEmitterState();
			const auto& ext = m_EmitterExtended[m_SelectedEmitterIndex];
			if (em.EmitterType == ParticleEmitterType::Cone)
			{
				focusPoint = glm::vec3(0.0f, ext.ConeHeight * 0.5f, 0.0f);
				targetDist = std::clamp(ext.ConeHeight * 2.2f, 3.0f, 20.0f);
			}
			else if (em.EmitterType == ParticleEmitterType::Sphere || em.EmitterType == ParticleEmitterType::Circle)
			{
				focusPoint = glm::vec3(0.0f, 0.0f, 0.0f);
				targetDist = std::clamp(ext.SphereRadius * 3.0f, 3.0f, 25.0f);
			}
			else if (em.EmitterType == ParticleEmitterType::Box)
			{
				focusPoint = glm::vec3(0.0f, 0.0f, 0.0f);
				targetDist = std::clamp(glm::length(ext.BoxExtents) * 2.2f, 3.0f, 25.0f);
			}
			else
			{
				const float focusY = std::clamp(em.Speed * 0.25f + em.Gravity.y * 0.1f, 0.5f, 2.5f);
				focusPoint = glm::vec3(0.0f, focusY, 0.0f);
				targetDist = 6.0f;
			}
		}

		m_TargetCameraFocus = focusPoint;
		m_CameraDistance = targetDist;
		m_StatusFeedback = "已聚焦至发射器: " + em.Name;
	}

	// =========================================================================
	// 复合特效 3D 视口渲染与交互重塑 (工业级自由轨道/平移/缩放与 Gizmo 直接操纵)
	// =========================================================================

	void ParticleEditorPanel::RenderViewport(Wui::WuiContext& ctx, const Wui::WuiRect& vpRect, const Wui::WuiTheme& theme, PanelHost& host)
	{
		(void)host;
		Wui::BarSurface(ctx, vpRect, Wui::WuiColor { 0.07f, 0.08f, 0.10f, 1.0f }, theme.Border);

		// 1. 视口浮层 UI 几何区域预先计算 (防止点击 UI 时穿透触发相机拖拽)
		const float islandW = 478.0f;
		float vx = vpRect.X + vpRect.W - 85.0f - islandW;
		const float vy = vpRect.Y + 8.0f;
		const Wui::WuiRect vIsland { vx - 4.0f, vy - 2.0f, islandW + 8.0f, 26.0f };

		const glm::vec2 gimbalCenter { vpRect.X + vpRect.W - 45.0f, vpRect.Y + 52.0f };
		const float gimbalRadius = 24.0f;
		const Wui::WuiRect gimbalRect { gimbalCenter.x - gimbalRadius - 8.0f, gimbalCenter.y - gimbalRadius - 8.0f, (gimbalRadius + 8.0f) * 2.0f, (gimbalRadius + 8.0f) * 2.0f };

		const Wui::WuiRect hudRect { vpRect.X + 8.0f, vpRect.Y + 8.0f, 235.0f, 76.0f };

		const float hintBarW = std::min(520.0f, vpRect.W - 16.0f);
		const Wui::WuiRect hintBarRect { vpRect.X + 8.0f, vpRect.Y + vpRect.H - 26.0f, hintBarW, 20.0f };

		const bool isVpHovered = ctx.IsHovered(vpRect);
		const glm::vec2 mousePos = ctx.Input().MousePos;
		const bool isMouseOverViewportUI = isVpHovered && (vIsland.Contains(mousePos) || gimbalRect.Contains(mousePos) || hudRect.Contains(mousePos) || hintBarRect.Contains(mousePos));

		// 2. 轨道相机世界基与投影矩阵 (与 ViewChrome 统一规范)
		glm::vec3 camRight(1.0f, 0.0f, 0.0f);
		glm::vec3 camUp(0.0f, 1.0f, 0.0f);
		glm::vec3 camForward(0.0f, 0.0f, -1.0f);
		ViewChrome::OrbitBasis(m_OrbitYaw, m_OrbitPitch, &camRight, &camUp, &camForward);

		const glm::vec3 eye = m_CameraFocus - camForward * m_CameraDistance;
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
			return true;
		};

		auto drawProjectedSegment = [&](const glm::vec3& worldA, const glm::vec3& worldB, const Wui::WuiColor& col, float thickness) {
			constexpr float kMinW = 0.05f;
			glm::vec4 a = vpMat * glm::vec4(worldA, 1.0f);
			glm::vec4 b = vpMat * glm::vec4(worldB, 1.0f);

			// 两端都在相机近平面后方 -> 整段剔除
			if (a.w < kMinW && b.w < kMinW)
				return;

			// 遵循 perspective-line-clipping-w-overflow 知识契约：在齐次裁剪空间对近平面进行线段裁剪
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

			const glm::vec3 ndcA = glm::vec3(a) / a.w;
			const glm::vec3 ndcB = glm::vec3(b) / b.w;

			glm::vec2 s0 {
				vpRect.X + (ndcA.x * 0.5f + 0.5f) * vpRect.W,
				vpRect.Y + (-ndcA.y * 0.5f + 0.5f) * vpRect.H
			};
			glm::vec2 s1 {
				vpRect.X + (ndcB.x * 0.5f + 0.5f) * vpRect.W,
				vpRect.Y + (-ndcB.y * 0.5f + 0.5f) * vpRect.H
			};

			if (std::isnan(s0.x) || std::isnan(s0.y) || std::isnan(s1.x) || std::isnan(s1.y))
				return;

			// 视口矩形 2D 几何裁剪
			if (ClipLineSegment2D(s0, s1, vpRect.X, vpRect.X + vpRect.W, vpRect.Y, vpRect.Y + vpRect.H))
			{
				Wui::LineSegment(ctx, s0, s1, col, thickness);
			}
		};

		auto drawProjectedCircle = [&](const glm::vec3& center, float radius, const glm::vec3& normal, const Wui::WuiColor& col, float thickness, int segs) {
			glm::vec3 u(1.0f, 0.0f, 0.0f);
			if (std::abs(normal.y) > 0.9f) u = glm::vec3(1.0f, 0.0f, 0.0f);
			else u = glm::normalize(glm::cross(normal, glm::vec3(0.0f, 1.0f, 0.0f)));
			const glm::vec3 v = glm::normalize(glm::cross(normal, u));
			for (int s = 0; s < segs; ++s)
			{
				const float a0 = static_cast<float>(s) * 6.2831853f / static_cast<float>(segs);
				const float a1 = static_cast<float>(s + 1) * 6.2831853f / static_cast<float>(segs);
				const glm::vec3 p0 = center + radius * (std::cos(a0) * u + std::sin(a0) * v);
				const glm::vec3 p1 = center + radius * (std::cos(a1) * u + std::sin(a1) * v);
				drawProjectedSegment(p0, p1, col, thickness);
			}
		};

		// 3. 发射器 3D 空间形态操纵手柄 (Direct Manipulation Gizmos) 收集与命中判定
		struct GizmoHandleItem
		{
			int Id;
			glm::vec3 WorldPos;
			glm::vec3 DragDir;
			float InitialVal;
			Wui::WuiColor Color;
			std::string Tooltip;
			const char* Label;
			float MinVal;
			float MaxVal;
			bool IsAngle;
		};
		std::vector<GizmoHandleItem> handles;

		if (m_SelectedEmitterIndex < m_Asset.Emitters.size())
		{
			const auto& curEm = m_Asset.Emitters[m_SelectedEmitterIndex];
			EnsurePerEmitterState();
			auto& ext = m_EmitterExtended[m_SelectedEmitterIndex];

			if (curEm.EmitterType == ParticleEmitterType::Cone)
			{
				const float rBase = std::max(0.01f, ext.ConeRadius);
				const float h = std::max(0.1f, ext.ConeHeight);
				const float halfAngleRad = glm::radians(std::clamp(ext.ConeAngle * 0.5f, 0.0f, 85.0f));
				const float rTop = rBase + h * std::tan(halfAngleRad);

				char buf[64];
				std::snprintf(buf, sizeof(buf), "底面半径: %.2fm (左键拖拽调整)", ext.ConeRadius);
				handles.push_back({
					static_cast<int>(ParticleGizmoHandleId::ConeBaseRadius),
					glm::vec3(rBase, 0.0f, 0.0f),
					glm::vec3(1.0f, 0.0f, 0.0f),
					ext.ConeRadius,
					Wui::WuiColor { 1.0f, 0.85f, 0.20f, 1.0f },
					buf,
					"R",
					0.01f, 20.0f, false
				});

				std::snprintf(buf, sizeof(buf), "圆锥张角: %.1f° (左键拖拽调整)", ext.ConeAngle);
				handles.push_back({
					static_cast<int>(ParticleGizmoHandleId::ConeTopAngle),
					glm::vec3(rTop, h, 0.0f),
					glm::vec3(1.0f, 0.0f, 0.0f),
					ext.ConeAngle,
					Wui::WuiColor { 0.25f, 0.85f, 1.0f, 1.0f },
					buf,
					"A",
					0.0f, 120.0f, true
				});

				std::snprintf(buf, sizeof(buf), "圆锥高度: %.2fm (左键拖拽调整)", ext.ConeHeight);
				handles.push_back({
					static_cast<int>(ParticleGizmoHandleId::ConeHeight),
					glm::vec3(0.0f, h, 0.0f),
					glm::vec3(0.0f, 1.0f, 0.0f),
					ext.ConeHeight,
					Wui::WuiColor { 0.35f, 0.95f, 0.45f, 1.0f },
					buf,
					"H",
					0.1f, 30.0f, false
				});
			}
			else if (curEm.EmitterType == ParticleEmitterType::Sphere || curEm.EmitterType == ParticleEmitterType::Circle)
			{
				const float spR = std::max(0.05f, ext.SphereRadius);
				char buf[64];
				std::snprintf(buf, sizeof(buf), "球体半径 X: %.2fm (左键拖拽调整)", ext.SphereRadius);
				handles.push_back({
					static_cast<int>(ParticleGizmoHandleId::SphereRadiusX),
					glm::vec3(spR, 0.0f, 0.0f),
					glm::vec3(1.0f, 0.0f, 0.0f),
					ext.SphereRadius,
					Wui::WuiColor { 0.95f, 0.30f, 0.30f, 1.0f },
					buf,
					"X",
					0.05f, 20.0f, false
				});

				std::snprintf(buf, sizeof(buf), "球体半径 Y: %.2fm (左键拖拽调整)", ext.SphereRadius);
				handles.push_back({
					static_cast<int>(ParticleGizmoHandleId::SphereRadiusY),
					glm::vec3(0.0f, spR, 0.0f),
					glm::vec3(0.0f, 1.0f, 0.0f),
					ext.SphereRadius,
					Wui::WuiColor { 0.30f, 0.95f, 0.40f, 1.0f },
					buf,
					"Y",
					0.05f, 20.0f, false
				});

				std::snprintf(buf, sizeof(buf), "球体半径 Z: %.2fm (左键拖拽调整)", ext.SphereRadius);
				handles.push_back({
					static_cast<int>(ParticleGizmoHandleId::SphereRadiusZ),
					glm::vec3(0.0f, 0.0f, spR),
					glm::vec3(0.0f, 0.0f, 1.0f),
					ext.SphereRadius,
					Wui::WuiColor { 0.30f, 0.60f, 1.0f, 1.0f },
					buf,
					"Z",
					0.05f, 20.0f, false
				});
			}
			else if (curEm.EmitterType == ParticleEmitterType::Box)
			{
				const glm::vec3 extents = glm::max(ext.BoxExtents, glm::vec3(0.05f));
				char buf[64];

				std::snprintf(buf, sizeof(buf), "盒体尺寸 +X: %.2fm (左键拖拽调整)", ext.BoxExtents.x);
				handles.push_back({
					static_cast<int>(ParticleGizmoHandleId::BoxExtentPosX),
					glm::vec3(extents.x, 0.0f, 0.0f),
					glm::vec3(1.0f, 0.0f, 0.0f),
					ext.BoxExtents.x,
					Wui::WuiColor { 0.95f, 0.30f, 0.30f, 1.0f },
					buf,
					"+X",
					0.05f, 20.0f, false
				});

				std::snprintf(buf, sizeof(buf), "盒体尺寸 -X: %.2fm (左键拖拽调整)", ext.BoxExtents.x);
				handles.push_back({
					static_cast<int>(ParticleGizmoHandleId::BoxExtentNegX),
					glm::vec3(-extents.x, 0.0f, 0.0f),
					glm::vec3(-1.0f, 0.0f, 0.0f),
					ext.BoxExtents.x,
					Wui::WuiColor { 0.95f, 0.50f, 0.50f, 1.0f },
					buf,
					"-X",
					0.05f, 20.0f, false
				});

				std::snprintf(buf, sizeof(buf), "盒体尺寸 +Y: %.2fm (左键拖拽调整)", ext.BoxExtents.y);
				handles.push_back({
					static_cast<int>(ParticleGizmoHandleId::BoxExtentPosY),
					glm::vec3(0.0f, extents.y, 0.0f),
					glm::vec3(0.0f, 1.0f, 0.0f),
					ext.BoxExtents.y,
					Wui::WuiColor { 0.30f, 0.95f, 0.40f, 1.0f },
					buf,
					"+Y",
					0.05f, 20.0f, false
				});

				std::snprintf(buf, sizeof(buf), "盒体尺寸 -Y: %.2fm (左键拖拽调整)", ext.BoxExtents.y);
				handles.push_back({
					static_cast<int>(ParticleGizmoHandleId::BoxExtentNegY),
					glm::vec3(0.0f, -extents.y, 0.0f),
					glm::vec3(0.0f, -1.0f, 0.0f),
					ext.BoxExtents.y,
					Wui::WuiColor { 0.50f, 0.95f, 0.60f, 1.0f },
					buf,
					"-Y",
					0.05f, 20.0f, false
				});

				std::snprintf(buf, sizeof(buf), "盒体尺寸 +Z: %.2fm (左键拖拽调整)", ext.BoxExtents.z);
				handles.push_back({
					static_cast<int>(ParticleGizmoHandleId::BoxExtentPosZ),
					glm::vec3(0.0f, 0.0f, extents.z),
					glm::vec3(0.0f, 0.0f, 1.0f),
					ext.BoxExtents.z,
					Wui::WuiColor { 0.30f, 0.60f, 1.0f, 1.0f },
					buf,
					"+Z",
					0.05f, 20.0f, false
				});

				std::snprintf(buf, sizeof(buf), "盒体尺寸 -Z: %.2fm (左键拖拽调整)", ext.BoxExtents.z);
				handles.push_back({
					static_cast<int>(ParticleGizmoHandleId::BoxExtentNegZ),
					glm::vec3(0.0f, 0.0f, -extents.z),
					glm::vec3(0.0f, 0.0f, -1.0f),
					ext.BoxExtents.z,
					Wui::WuiColor { 0.50f, 0.75f, 1.0f, 1.0f },
					buf,
					"-Z",
					0.05f, 20.0f, false
				});
			}
		}

		// 4. 手柄悬停检测与左键直接操纵状态机
		int hoveredHandleId = -1;
		float closestDistSq = 144.0f; // 12px 命中判定半径
		if (!m_IsOrbiting && !m_IsPanning && isVpHovered && !isMouseOverViewportUI)
		{
			for (const auto& h : handles)
			{
				glm::vec2 sp;
				if (projectPoint(h.WorldPos, sp))
				{
					const float dSq = glm::dot(mousePos - sp, mousePos - sp);
					if (dSq < closestDistSq)
					{
						closestDistSq = dSq;
						hoveredHandleId = h.Id;
					}
				}
			}
		}
		m_HoveredGizmoHandle = hoveredHandleId;

		if (m_HoveredGizmoHandle != -1 && ctx.Input().MouseClicked[0] && !m_IsDraggingGizmo)
		{
			m_IsDraggingGizmo = true;
			m_ActiveGizmoHandle = m_HoveredGizmoHandle;
			m_GizmoDragStartPos = mousePos;
			PushUndoSnapshot();
			for (const auto& h : handles)
			{
				if (h.Id == m_ActiveGizmoHandle)
				{
					m_GizmoDragInitialParam = h.InitialVal;
					break;
				}
			}
		}

		if (m_IsDraggingGizmo)
		{
			if (ctx.Input().MouseDown[0])
			{
				const GizmoHandleItem* activeItem = nullptr;
				for (const auto& h : handles)
				{
					if (h.Id == m_ActiveGizmoHandle)
					{
						activeItem = &h;
						break;
					}
				}

				if (activeItem && m_SelectedEmitterIndex < m_EmitterExtended.size())
				{
					auto& ext = m_EmitterExtended[m_SelectedEmitterIndex];
					glm::vec2 s0, s1;
					projectPoint(activeItem->WorldPos, s0);
					projectPoint(activeItem->WorldPos + activeItem->DragDir, s1);
					const glm::vec2 dir = s1 - s0;
					const float len = glm::length(dir);
					float proj = 0.0f;
					if (len > 1e-4f)
					{
						proj = glm::dot(mousePos - m_GizmoDragStartPos, dir / len);
					}
					else
					{
						const glm::vec2 d = mousePos - m_GizmoDragStartPos;
						proj = d.x - d.y;
					}

					if (activeItem->IsAngle)
					{
						const float deltaAngle = proj * 0.35f;
						ext.ConeAngle = std::clamp(m_GizmoDragInitialParam + deltaAngle, activeItem->MinVal, activeItem->MaxVal);
					}
					else
					{
						const float deltaDist = proj * (m_CameraDistance * 0.0025f);
						const float newVal = std::clamp(m_GizmoDragInitialParam + deltaDist, activeItem->MinVal, activeItem->MaxVal);
						switch (static_cast<ParticleGizmoHandleId>(activeItem->Id))
						{
						case ParticleGizmoHandleId::ConeBaseRadius:
							ext.ConeRadius = newVal;
							break;
						case ParticleGizmoHandleId::ConeHeight:
							ext.ConeHeight = newVal;
							break;
						case ParticleGizmoHandleId::SphereRadiusX:
						case ParticleGizmoHandleId::SphereRadiusY:
						case ParticleGizmoHandleId::SphereRadiusZ:
							ext.SphereRadius = newVal;
							break;
						case ParticleGizmoHandleId::BoxExtentPosX:
						case ParticleGizmoHandleId::BoxExtentNegX:
							ext.BoxExtents.x = newVal;
							break;
						case ParticleGizmoHandleId::BoxExtentPosY:
						case ParticleGizmoHandleId::BoxExtentNegY:
							ext.BoxExtents.y = newVal;
							break;
						case ParticleGizmoHandleId::BoxExtentPosZ:
						case ParticleGizmoHandleId::BoxExtentNegZ:
							ext.BoxExtents.z = newVal;
							break;
						default:
							break;
						}
					}
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
			}
			else
			{
				m_IsDraggingGizmo = false;
				m_ActiveGizmoHandle = -1;
			}
		}

		// 5. 行业标准相机导航 (右键/Alt+左键=旋转; 中键/Shift+右键=平移; 滚轮=缩放; F=聚焦)
		if (isVpHovered && !isMouseOverViewportUI && ctx.Input().Wheel != 0.0f)
		{
			const float zoomStep = std::max(0.08f, m_CameraDistance * 0.12f);
			m_CameraDistance = std::clamp(m_CameraDistance - ctx.Input().Wheel * zoomStep, 0.5f, 60.0f);
		}

		if (!m_IsOrbiting && !m_IsPanning && !m_IsDraggingGizmo && isVpHovered && !isMouseOverViewportUI)
		{
			const bool wantOrbit = (ctx.Input().MouseClicked[1] && !ctx.Input().Shift)
			                    || (ctx.Input().MouseClicked[0] && ctx.Input().Alt);
			if (wantOrbit)
			{
				m_IsOrbiting = true;
				m_LastMousePos = mousePos;
			}
		}

		if (!m_IsOrbiting && !m_IsPanning && !m_IsDraggingGizmo && isVpHovered && !isMouseOverViewportUI)
		{
			const bool wantPan = ctx.Input().MouseClicked[2]
			                  || (ctx.Input().MouseClicked[1] && ctx.Input().Shift);
			if (wantPan)
			{
				m_IsPanning = true;
				m_LastMousePos = mousePos;
			}
		}

		if (m_IsOrbiting)
		{
			const bool stillDown = ctx.Input().MouseDown[1] || (ctx.Input().MouseDown[0] && ctx.Input().Alt);
			if (stillDown)
			{
				const glm::vec2 delta = mousePos - m_LastMousePos;
				m_LastMousePos = mousePos;
				ViewChrome::ApplyOrbitDrag(m_OrbitYaw, m_OrbitPitch, delta);
			}
			else
			{
				m_IsOrbiting = false;
			}
		}

		if (m_IsPanning)
		{
			const bool stillDown = ctx.Input().MouseDown[2] || (ctx.Input().MouseDown[1] && ctx.Input().Shift);
			if (stillDown)
			{
				const glm::vec2 delta = mousePos - m_LastMousePos;
				m_LastMousePos = mousePos;
				const glm::vec3 panDelta = (-camRight * delta.x + camUp * delta.y) * (m_CameraDistance * 0.0015f);
				m_CameraFocus += panDelta;
				m_TargetCameraFocus = m_CameraFocus;
			}
			else
			{
				m_IsPanning = false;
			}
		}

		if (isVpHovered && !ctx.Input().WantKeyboard && ctx.WasKeyPressed(KeyCodes::F))
		{
			FocusSelectedEmitter();
		}

		if (m_IsOrbiting || m_IsPanning || m_IsDraggingGizmo || m_HoveredGizmoHandle != -1)
		{
			ctx.SetCursor(Wui::WuiCursor::Hand);
		}
		else if (isVpHovered)
		{
			ctx.SetCursor(Wui::WuiCursor::Arrow);
		}

		// =========================================================================
		// 6. 低饱和度专业 3D 地面参考网格与原点坐标指示
		// =========================================================================
		const Wui::WuiColor gridCol { 0.20f, 0.23f, 0.28f, 0.35f };
		const Wui::WuiColor axisXGridCol { 0.70f, 0.30f, 0.30f, 0.65f };
		const Wui::WuiColor axisZGridCol { 0.30f, 0.50f, 0.80f, 0.65f };

		for (int line = -5; line <= 5; ++line)
		{
			const float coord = static_cast<float>(line) * 1.0f;
			drawProjectedSegment(glm::vec3(coord, 0.0f, -5.0f), glm::vec3(coord, 0.0f, 5.0f),
				(line == 0 ? axisZGridCol : gridCol), (line == 0 ? 1.4f : 1.0f));

			drawProjectedSegment(glm::vec3(-5.0f, 0.0f, coord), glm::vec3(5.0f, 0.0f, coord),
				(line == 0 ? axisXGridCol : gridCol), (line == 0 ? 1.4f : 1.0f));
		}

		for (int m = 1; m <= 3; ++m)
		{
			glm::vec2 smX, smZ;
			if (projectPoint(glm::vec3(static_cast<float>(m), 0.0f, 0.0f), smX))
			{
				if (smX.x >= vpRect.X && smX.x <= vpRect.X + vpRect.W && smX.y >= vpRect.Y && smX.y <= vpRect.Y + vpRect.H)
					Wui::Label(ctx, { smX.x - 8.0f, smX.y + 4.0f }, std::to_string(m) + "m", Wui::WuiColor { 0.6f, 0.6f, 0.65f, 0.5f }, 9.5f);
			}
			if (projectPoint(glm::vec3(0.0f, 0.0f, static_cast<float>(m)), smZ))
			{
				if (smZ.x >= vpRect.X && smZ.x <= vpRect.X + vpRect.W && smZ.y >= vpRect.Y && smZ.y <= vpRect.Y + vpRect.H)
					Wui::Label(ctx, { smZ.x + 4.0f, smZ.y - 6.0f }, std::to_string(m) + "m", Wui::WuiColor { 0.6f, 0.6f, 0.65f, 0.5f }, 9.5f);
			}
		}

		const Wui::WuiColor axisXCol { 0.95f, 0.22f, 0.22f, 0.95f };
		const Wui::WuiColor axisYCol { 0.22f, 0.90f, 0.32f, 0.95f };
		const Wui::WuiColor axisZCol { 0.22f, 0.55f, 0.98f, 0.95f };

		drawProjectedSegment(glm::vec3(0.0f), glm::vec3(1.2f, 0.0f, 0.0f), axisXCol, 2.0f);
		drawProjectedSegment(glm::vec3(0.0f), glm::vec3(0.0f, 1.2f, 0.0f), axisYCol, 2.0f);
		drawProjectedSegment(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.2f), axisZCol, 2.0f);

		// =========================================================================
		// 7. 真实粒子群推演与深度排序绘制 (消灭虚影与黑边)
		// =========================================================================
		struct ParticleDrawItem
		{
			glm::vec3 Position;
			glm::vec4 Color;
			glm::vec2 Size;
			float CamDistSq;
			ParticleBlendMode BlendMode;
			ParticleRenderMode FacingMode;
		};

		std::vector<ParticleDrawItem> drawItems;
		size_t totalActiveParticles = 0;
		size_t activeEmitterCount = 0;
		glm::vec3 minAABB(1e9f);
		glm::vec3 maxAABB(-1e9f);

		for (size_t l = 0; l < m_LivePools.size(); ++l)
		{
			if (!IsEmitterActive(l)) continue;
			activeEmitterCount++;
			const auto& pool = m_LivePools[l];
			totalActiveParticles += pool->ActiveCount();
			const auto& em = m_Asset.Emitters[l];

			for (const auto& p : pool->GetParticles())
			{
				if (!p.Active) continue;
				minAABB = glm::min(minAABB, p.Position);
				maxAABB = glm::max(maxAABB, p.Position);

				const float distSq = glm::dot(eye - p.Position, eye - p.Position);
				drawItems.push_back({ p.Position, p.Color, p.Size, distSq, em.Material.BlendMode, em.FacingMode });
			}
		}

		std::sort(drawItems.begin(), drawItems.end(), [](const ParticleDrawItem& a, const ParticleDrawItem& b) {
			return a.CamDistSq > b.CamDistSq;
		});

		for (const auto& item : drawItems)
		{
			glm::vec2 pScreen;
			if (projectPoint(item.Position, pScreen))
			{
				const glm::vec4 clip = vpMat * glm::vec4(item.Position, 1.0f);
				const float pSize = std::clamp(item.Size.x * 320.0f / clip.w, 3.0f, 32.0f);

				Wui::WuiColor col;
				if (m_HeatmapMode)
				{
					col = Wui::WuiColor { 1.0f, 0.15f, 0.15f, 0.18f };
				}
				else if (m_DisplayMode == ParticleViewportDisplayMode::Unlit)
				{
					col = Wui::WuiColor {
						std::clamp(item.Color.r, 0.0f, 1.0f),
						std::clamp(item.Color.g, 0.0f, 1.0f),
						std::clamp(item.Color.b, 0.0f, 1.0f),
						1.0f
					};
				}
				else
				{
					col = Wui::WuiColor {
						std::clamp(item.Color.r, 0.0f, 1.0f),
						std::clamp(item.Color.g, 0.0f, 1.0f),
						std::clamp(item.Color.b, 0.0f, 1.0f),
						std::clamp(item.Color.a, 0.0f, 1.0f)
					};
				}

				if (item.BlendMode == ParticleBlendMode::Additive && !m_HeatmapMode)
				{
					const float haloSize = pSize;
					const Wui::WuiRect haloRect { pScreen.x - haloSize * 0.5f, pScreen.y - haloSize * 0.5f, haloSize, haloSize };
					const Wui::WuiColor haloCol { col.R, col.G, col.B, col.A * 0.35f };
					Wui::BarSurface(ctx, haloRect, haloCol, haloCol);

					const float coreSize = pSize * 0.5f;
					const Wui::WuiRect coreRect { pScreen.x - coreSize * 0.5f, pScreen.y - coreSize * 0.5f, coreSize, coreSize };
					const Wui::WuiColor coreCol { std::min(1.0f, col.R * 1.3f), std::min(1.0f, col.G * 1.3f), std::min(1.0f, col.B * 1.3f), col.A * 0.9f };
					Wui::BarSurface(ctx, coreRect, coreCol, coreCol);
				}
				else
				{
					const Wui::WuiRect pRect { pScreen.x - pSize * 0.5f, pScreen.y - pSize * 0.5f, pSize, pSize };
					Wui::BarSurface(ctx, pRect, col, col);
				}
			}
		}

		// 粒子系统 AABB 包围盒线框
		if (m_BoundsMode && totalActiveParticles > 0)
		{
			const Wui::WuiColor boundsCol { 0.2f, 0.9f, 0.85f, 0.8f };
			drawProjectedSegment(glm::vec3(minAABB.x, minAABB.y, minAABB.z), glm::vec3(maxAABB.x, minAABB.y, minAABB.z), boundsCol, 1.2f);
			drawProjectedSegment(glm::vec3(maxAABB.x, minAABB.y, minAABB.z), glm::vec3(maxAABB.x, minAABB.y, maxAABB.z), boundsCol, 1.2f);
			drawProjectedSegment(glm::vec3(maxAABB.x, minAABB.y, maxAABB.z), glm::vec3(minAABB.x, minAABB.y, maxAABB.z), boundsCol, 1.2f);
			drawProjectedSegment(glm::vec3(minAABB.x, minAABB.y, maxAABB.z), glm::vec3(minAABB.x, minAABB.y, minAABB.z), boundsCol, 1.2f);

			drawProjectedSegment(glm::vec3(minAABB.x, maxAABB.y, minAABB.z), glm::vec3(maxAABB.x, maxAABB.y, minAABB.z), boundsCol, 1.2f);
			drawProjectedSegment(glm::vec3(maxAABB.x, maxAABB.y, minAABB.z), glm::vec3(maxAABB.x, maxAABB.y, maxAABB.z), boundsCol, 1.2f);
			drawProjectedSegment(glm::vec3(maxAABB.x, maxAABB.y, maxAABB.z), glm::vec3(minAABB.x, maxAABB.y, maxAABB.z), boundsCol, 1.2f);
			drawProjectedSegment(glm::vec3(minAABB.x, maxAABB.y, maxAABB.z), glm::vec3(minAABB.x, maxAABB.y, minAABB.z), boundsCol, 1.2f);

			drawProjectedSegment(glm::vec3(minAABB.x, minAABB.y, minAABB.z), glm::vec3(minAABB.x, maxAABB.y, minAABB.z), boundsCol, 1.2f);
			drawProjectedSegment(glm::vec3(maxAABB.x, minAABB.y, minAABB.z), glm::vec3(maxAABB.x, maxAABB.y, minAABB.z), boundsCol, 1.2f);
			drawProjectedSegment(glm::vec3(maxAABB.x, minAABB.y, maxAABB.z), glm::vec3(maxAABB.x, maxAABB.y, maxAABB.z), boundsCol, 1.2f);
			drawProjectedSegment(glm::vec3(minAABB.x, minAABB.y, maxAABB.z), glm::vec3(minAABB.x, maxAABB.y, maxAABB.z), boundsCol, 1.2f);
		}

		// =========================================================================
		// 8. 3D 发射器空间形态线框与 Gizmo 手柄渲染 (Cone / Sphere / Box 直接操纵)
		// =========================================================================
		const Wui::WuiColor gizmoWireCol { 0.95f, 0.85f, 0.20f, 0.85f };
		if (m_SelectedEmitterIndex < m_Asset.Emitters.size())
		{
			const auto& curEm = m_Asset.Emitters[m_SelectedEmitterIndex];
			EnsurePerEmitterState();
			const auto& ext = m_EmitterExtended[m_SelectedEmitterIndex];

			if (curEm.EmitterType == ParticleEmitterType::Cone)
			{
				const float rBase = std::max(0.01f, ext.ConeRadius);
				const float h = std::max(0.1f, ext.ConeHeight);
				const float halfAngleRad = glm::radians(std::clamp(ext.ConeAngle * 0.5f, 0.0f, 85.0f));
				const float rTop = rBase + h * std::tan(halfAngleRad);

				drawProjectedCircle(glm::vec3(0.0f, 0.0f, 0.0f), rBase, glm::vec3(0.0f, 1.0f, 0.0f), gizmoWireCol, 1.5f, 24);
				drawProjectedCircle(glm::vec3(0.0f, h, 0.0f), rTop, glm::vec3(0.0f, 1.0f, 0.0f), gizmoWireCol, 1.5f, 24);

				drawProjectedSegment(glm::vec3(rBase, 0.0f, 0.0f), glm::vec3(rTop, h, 0.0f), gizmoWireCol, 1.2f);
				drawProjectedSegment(glm::vec3(-rBase, 0.0f, 0.0f), glm::vec3(-rTop, h, 0.0f), gizmoWireCol, 1.2f);
				drawProjectedSegment(glm::vec3(0.0f, 0.0f, rBase), glm::vec3(0.0f, h, rTop), gizmoWireCol, 1.2f);
				drawProjectedSegment(glm::vec3(0.0f, 0.0f, -rBase), glm::vec3(0.0f, h, -rTop), gizmoWireCol, 1.2f);

				drawProjectedSegment(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, h, 0.0f), Wui::WuiColor { 0.95f, 0.85f, 0.20f, 0.40f }, 1.0f);
			}
			else if (curEm.EmitterType == ParticleEmitterType::Sphere || curEm.EmitterType == ParticleEmitterType::Circle)
			{
				const float spR = std::max(0.05f, ext.SphereRadius);
				drawProjectedCircle(glm::vec3(0.0f), spR, glm::vec3(0.0f, 1.0f, 0.0f), gizmoWireCol, 1.4f, 24);
				drawProjectedCircle(glm::vec3(0.0f), spR, glm::vec3(0.0f, 0.0f, 1.0f), gizmoWireCol, 1.4f, 24);
				drawProjectedCircle(glm::vec3(0.0f), spR, glm::vec3(1.0f, 0.0f, 0.0f), gizmoWireCol, 1.4f, 24);
			}
			else if (curEm.EmitterType == ParticleEmitterType::Box)
			{
				const glm::vec3 e = glm::max(ext.BoxExtents, glm::vec3(0.05f));
				drawProjectedSegment(glm::vec3(-e.x, -e.y, -e.z), glm::vec3(e.x, -e.y, -e.z), gizmoWireCol, 1.3f);
				drawProjectedSegment(glm::vec3(e.x, -e.y, -e.z), glm::vec3(e.x, e.y, -e.z), gizmoWireCol, 1.3f);
				drawProjectedSegment(glm::vec3(e.x, e.y, -e.z), glm::vec3(-e.x, e.y, -e.z), gizmoWireCol, 1.3f);
				drawProjectedSegment(glm::vec3(-e.x, e.y, -e.z), glm::vec3(-e.x, -e.y, -e.z), gizmoWireCol, 1.3f);

				drawProjectedSegment(glm::vec3(-e.x, -e.y, e.z), glm::vec3(e.x, -e.y, e.z), gizmoWireCol, 1.3f);
				drawProjectedSegment(glm::vec3(e.x, -e.y, e.z), glm::vec3(e.x, e.y, e.z), gizmoWireCol, 1.3f);
				drawProjectedSegment(glm::vec3(e.x, e.y, e.z), glm::vec3(-e.x, e.y, e.z), gizmoWireCol, 1.3f);
				drawProjectedSegment(glm::vec3(-e.x, e.y, e.z), glm::vec3(-e.x, -e.y, e.z), gizmoWireCol, 1.3f);

				drawProjectedSegment(glm::vec3(-e.x, -e.y, -e.z), glm::vec3(-e.x, -e.y, e.z), gizmoWireCol, 1.3f);
				drawProjectedSegment(glm::vec3(e.x, -e.y, -e.z), glm::vec3(e.x, -e.y, e.z), gizmoWireCol, 1.3f);
				drawProjectedSegment(glm::vec3(e.x, e.y, -e.z), glm::vec3(e.x, e.y, e.z), gizmoWireCol, 1.3f);
				drawProjectedSegment(glm::vec3(-e.x, e.y, -e.z), glm::vec3(-e.x, e.y, e.z), gizmoWireCol, 1.3f);
			}

			for (const auto& h : handles)
			{
				glm::vec2 sp;
				if (projectPoint(h.WorldPos, sp))
				{
					if (sp.x >= vpRect.X && sp.x <= vpRect.X + vpRect.W && sp.y >= vpRect.Y && sp.y <= vpRect.Y + vpRect.H)
					{
						const bool isHover = (m_HoveredGizmoHandle == h.Id);
						const bool isActive = (m_ActiveGizmoHandle == h.Id);
						const float hSize = (isActive || isHover) ? 14.0f : 10.0f;
						const Wui::WuiRect hRect { sp.x - hSize * 0.5f, sp.y - hSize * 0.5f, hSize, hSize };

						const Wui::WuiColor fillCol = (isActive || isHover)
							? Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f }
							: h.Color;
						const Wui::WuiColor borderCol = (isActive || isHover)
							? h.Color
							: Wui::WuiColor { 0.10f, 0.12f, 0.15f, 0.90f };

						Wui::BarSurface(ctx, hRect, fillCol, borderCol);

						if (isHover || isActive)
						{
							Wui::Tooltip(ctx, hRect, h.Tooltip);
						}
					}
				}
			}
		}

		// =========================================================================
		// 9. 视口 3D 朝向万向节与悬浮工具岛
		// =========================================================================
		RenderOrientationGimbal(ctx, vpRect, view, theme);

		Wui::BarSurface(ctx, vIsland, Wui::WuiColor { 0.05f, 0.07f, 0.09f, 0.85f }, theme.Border);

		const float viewBtnW = 34.0f;
		const Wui::WuiRect btn3D { vx, vy, viewBtnW, 22.0f };
		Wui::Tooltip(ctx, btn3D, "3D 自由轨道透视视角 (Perspective Orbit)");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.3d"), btn3D, "3D", theme)) { m_OrbitYaw = 0.785f; m_OrbitPitch = 0.45f; }
		vx += viewBtnW + 3.0f;

		const Wui::WuiRect btnTop { vx, vy, viewBtnW, 22.0f };
		Wui::Tooltip(ctx, btnTop, "顶视图: 垂直俯视 XZ 地面扩散 (Top View)");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.top"), btnTop, "顶视", theme)) { m_OrbitYaw = 0.0f; m_OrbitPitch = 1.45f; }
		vx += viewBtnW + 3.0f;

		const Wui::WuiRect btnFront { vx, vy, viewBtnW, 22.0f };
		Wui::Tooltip(ctx, btnFront, "正视图: 水平平视 XY 喷射高度 (Front View)");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.front"), btnFront, "正视", theme)) { m_OrbitYaw = 0.0f; m_OrbitPitch = 0.0f; }
		vx += viewBtnW + 3.0f;

		const Wui::WuiRect btnSide { vx, vy, viewBtnW, 22.0f };
		Wui::Tooltip(ctx, btnSide, "侧视图: 水平侧视 ZY 轨迹 (Side View)");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.side"), btnSide, "侧视", theme)) { m_OrbitYaw = 1.5708f; m_OrbitPitch = 0.0f; }
		vx += viewBtnW + 4.0f;

		const Wui::WuiRect modeBtn { vx, vy, 46.0f, 22.0f };
		Wui::Tooltip(ctx, modeBtn, "切换光照/无光照显示模式");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.mode"), modeBtn, (m_DisplayMode == ParticleViewportDisplayMode::Unlit ? "Unlit" : "Lit"), theme))
		{
			m_DisplayMode = (m_DisplayMode == ParticleViewportDisplayMode::Unlit ? ParticleViewportDisplayMode::Lit : ParticleViewportDisplayMode::Unlit);
		}
		vx += 49.0f;

		const Wui::WuiRect heatBtn { vx, vy, 60.0f, 22.0f };
		Wui::Tooltip(ctx, heatBtn, "切换视口 Overdraw 填充率热力图透视");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.heatmap"), heatBtn, m_HeatmapMode ? "[热力图]" : "  热力图", theme))
		{
			m_HeatmapMode = !m_HeatmapMode;
		}
		vx += 63.0f;

		const Wui::WuiRect boundsBtn { vx, vy, 60.0f, 22.0f };
		Wui::Tooltip(ctx, boundsBtn, "切换视口粒子系统 AABB 包围盒显示");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.bounds"), boundsBtn, m_BoundsMode ? "[包围盒]" : "  包围盒", theme))
		{
			m_BoundsMode = !m_BoundsMode;
		}
		vx += 63.0f;

		const Wui::WuiRect focusBtn { vx, vy, 44.0f, 22.0f };
		Wui::Tooltip(ctx, focusBtn, "聚焦视口至选中的发射器 (快捷键 F)");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.focus"), focusBtn, "聚焦", theme))
		{
			FocusSelectedEmitter();
		}
		vx += 47.0f;

		const Wui::WuiRect rstCamBtn { vx, vy, 64.0f, 22.0f };
		Wui::Tooltip(ctx, rstCamBtn, "重置视口相机至默认观察位置与角度");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.rstcam"), rstCamBtn, "复位视角", theme))
		{
			ResetCamera();
		}

		// 视口左上角状态 HUD 卡片
		const size_t estDrawCalls = std::max<size_t>(1, activeEmitterCount);
		std::string overdrawAlert;
		Wui::WuiColor overdrawCol;
		if (totalActiveParticles < 400)
		{
			overdrawAlert = "[低] 良好 (Low)";
			overdrawCol = Wui::WuiColor { 0.3f, 0.85f, 0.4f, 1.0f };
		}
		else if (totalActiveParticles < 1200)
		{
			overdrawAlert = "[中] 重叠预警 (Moderate)";
			overdrawCol = Wui::WuiColor { 0.95f, 0.8f, 0.2f, 1.0f };
		}
		else
		{
			overdrawAlert = "[极高] 严重瓶颈 (Critical)";
			overdrawCol = Wui::WuiColor { 0.95f, 0.3f, 0.3f, 1.0f };
		}

		Wui::BarSurface(ctx, hudRect, Wui::WuiColor { 0.05f, 0.07f, 0.09f, 0.85f }, theme.Border);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 6.0f }, "3D 复合特效视口 (VFX Viewport)", theme.Accent, 11.5f);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 22.0f },
			"活跃粒子: " + std::to_string(totalActiveParticles) + " | 发射层: " + std::to_string(activeEmitterCount) + "/" + std::to_string(m_Asset.Emitters.size()) + " | 预估 DC: " + std::to_string(estDrawCalls),
			theme.Text, 10.5f);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 38.0f },
			"模拟时间: " + std::to_string(m_SimTime).substr(0, 5) + "s | 热力透视: " + (m_HeatmapMode ? "[已启用]" : "[已关闭]"),
			theme.TextMuted, 10.0f);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 54.0f },
			"Overdraw 警报: " + overdrawAlert, overdrawCol, 10.5f);

		// 10. 视口底部快捷操作指引条
		Wui::BarSurface(ctx, hintBarRect, Wui::WuiColor { 0.04f, 0.06f, 0.08f, 0.85f }, theme.Border);
		Wui::Label(ctx, { hintBarRect.X + 8.0f, hintBarRect.Y + 3.0f },
			"[右键] 旋转  |  [中键/Shift+右键] 平移  |  [滚轮] 缩放  |  [左键] 操纵手柄  |  [F] 聚焦选中",
			theme.TextMuted, 9.5f);
	}


	void ParticleEditorPanel::RenderInteractiveCurveWorkbench(Wui::WuiContext& ctx, const Wui::WuiRect& canvasRect, const Wui::WuiTheme& theme)
	{
		Wui::BarSurface(ctx, canvasRect, Wui::WuiColor { 0.03f, 0.04f, 0.06f, 1.0f }, theme.Border);


		if (m_SelectedEmitterIndex >= m_Asset.Emitters.size()) return;


		// 1. 目标曲线选择器与预设工具栏 (Target Selector & Preset Bar)
		float hx = canvasRect.X + 6.0f;
		const float hy = canvasRect.Y + 4.0f;


		Wui::Label(ctx, { hx, hy + 2.0f }, "曲线通道:", theme.TextMuted, 10.0f);
		hx += 52.0f;


		const struct TargetDef {
			CurveTarget Target;
			const char* Id;
			const char* Label;
		} targets[] = {
			{ CurveTarget::Size, "vfx.c.tgt.size", "尺寸随寿命 (Size)" },
			{ CurveTarget::Alpha, "vfx.c.tgt.alpha", "透明度随寿命 (Alpha)" },
			{ CurveTarget::Speed, "vfx.c.tgt.speed", "速度随寿命 (Speed)" }
		};


		for (const auto& td : targets)
		{
			const float btnW = 110.0f;
			const Wui::WuiRect tRect { hx, hy, btnW, 20.0f };
			const bool isCur = (m_ActiveCurveTarget == td.Target);
			if (Wui::Button(ctx, Wui::HashId(td.Id), tRect, td.Label, theme))
			{
				m_ActiveCurveTarget = td.Target;
				m_SelectedKeyframeIndex = 0;
			}
			if (isCur)
			{
				Wui::LineSegment(ctx, { tRect.X, tRect.Y + tRect.H }, { tRect.X + tRect.W, tRect.Y + tRect.H }, theme.Accent, 2.0f);
			}
			hx += btnW + 4.0f;
		}


		hx += 10.0f;
		Wui::Label(ctx, { hx, hy + 2.0f }, "一键预设:", theme.TextMuted, 10.0f);
		hx += 50.0f;


		const struct PresetBarEntry {
			const char* Id;
			const char* Label;
			const char* Type;
		} cPresets[] = {
			{ "vfx.p.lin", "线性", "Linear" },
			{ "vfx.p.ein", "淡入", "EaseIn" },
			{ "vfx.p.eout", "淡出", "EaseOut" },
			{ "vfx.p.inout", "平滑", "EaseInOut" },
			{ "vfx.p.bell", "钟形", "Bell" },
			{ "vfx.p.bounce", "反弹", "Bounce" }
		};


		CubicBezierCurve* activeCurve = nullptr;
		if (m_ActiveCurveTarget == CurveTarget::Size) activeCurve = &m_EmitterSizeCurves[m_SelectedEmitterIndex];
		else if (m_ActiveCurveTarget == CurveTarget::Alpha) activeCurve = &m_EmitterAlphaCurves[m_SelectedEmitterIndex];
		else activeCurve = &m_EmitterSpeedCurves[m_SelectedEmitterIndex];


		for (const auto& cp : cPresets)
		{
			const float pW = 42.0f;
			if (hx + pW < canvasRect.X + canvasRect.W - 10.0f)
			{
				const Wui::WuiRect pRect { hx, hy, pW, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(cp.Id), pRect, cp.Label, theme))
				{
					PushUndoSnapshot();
					if (std::string(cp.Type) == "Linear") *activeCurve = CubicBezierCurve::Linear();
					else if (std::string(cp.Type) == "EaseIn") *activeCurve = CubicBezierCurve::EaseIn();
					else if (std::string(cp.Type) == "EaseOut") *activeCurve = CubicBezierCurve::EaseOut();
					else if (std::string(cp.Type) == "EaseInOut") *activeCurve = CubicBezierCurve::EaseInOut();
					else if (std::string(cp.Type) == "Bounce") *activeCurve = CubicBezierCurve::Bounce();
					else if (std::string(cp.Type) == "Bell")
					{
						activeCurve->Clear();
						activeCurve->AddKeyframe(Keyframe(0.0f, 0.0f));
						activeCurve->AddKeyframe(Keyframe(0.5f, 1.0f));
						activeCurve->AddKeyframe(Keyframe(1.0f, 0.0f));
						activeCurve->AutoSmoothTangents();
					}
					activeCurve->PrecomputeLut(256);
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
				hx += pW + 3.0f;
			}
		}


		// 2. 曲线交互绘制主画布 (Plot Area)
		const float plotTop = canvasRect.Y + 28.0f;
		const float plotH = canvasRect.H - 34.0f;
		const Wui::WuiRect plotRect { canvasRect.X + 35.0f, plotTop + 4.0f, canvasRect.W - 45.0f, plotH - 18.0f };
		Wui::BarSurface(ctx, plotRect, Wui::WuiColor { 0.02f, 0.03f, 0.04f, 1.0f }, Wui::WuiColor { 0.15f, 0.18f, 0.22f, 0.8f });


		// 绘制纵横标准标尺网格线 (横轴 Time 0.0~1.0s, 纵轴 Value 0.0~1.0)
		const Wui::WuiColor gridLineCol { 0.15f, 0.18f, 0.24f, 0.45f };


		for (int step = 0; step <= 4; ++step)
		{
			float frac = static_cast<float>(step) * 0.25f;
			float gx = plotRect.X + frac * plotRect.W;
			Wui::LineSegment(ctx, { gx, plotRect.Y }, { gx, plotRect.Y + plotRect.H }, gridLineCol, 1.0f);
			std::string tLabel = std::to_string(frac).substr(0, 4) + "s";
			Wui::Label(ctx, { gx - 8.0f, plotRect.Y + plotRect.H + 2.0f }, tLabel, Wui::WuiColor { 0.5f, 0.55f, 0.6f, 0.7f }, 9.0f);


			float gy = plotRect.Y + (1.0f - frac) * plotRect.H;
			Wui::LineSegment(ctx, { plotRect.X, gy }, { plotRect.X + plotRect.W, gy }, gridLineCol, 1.0f);
			std::string vLabel = std::to_string(frac).substr(0, 4);
			Wui::Label(ctx, { canvasRect.X + 4.0f, gy - 5.0f }, vLabel, Wui::WuiColor { 0.5f, 0.55f, 0.6f, 0.7f }, 9.0f);
		}


		auto timeToScreenX = [&](float t) -> float {
			return plotRect.X + std::clamp(t, 0.0f, 1.0f) * plotRect.W;
		};
		auto valToScreenY = [&](float v) -> float {
			return plotRect.Y + (1.0f - std::clamp(v, 0.0f, 1.0f)) * plotRect.H;
		};
		auto screenXToTime = [&](float sx) -> float {
			return std::clamp((sx - plotRect.X) / plotRect.W, 0.0f, 1.0f);
		};
		auto screenYToVal = [&](float sy) -> float {
			return std::clamp(1.0f - (sy - plotRect.Y) / plotRect.H, 0.0f, 1.0f);
		};


		// 3. 绘制平滑连续贝塞尔曲线 (Continuous Bezier Curve Spline)
		const int samples = 36;
		glm::vec2 prevPt { timeToScreenX(0.0f), valToScreenY(activeCurve->Evaluate(0.0f)) };
		for (int s = 1; s <= samples; ++s)
		{
			float t = static_cast<float>(s) / static_cast<float>(samples);
			float v = activeCurve->Evaluate(t);
			glm::vec2 curPt { timeToScreenX(t), valToScreenY(v) };
			Wui::LineSegment(ctx, prevPt, curPt, theme.Accent, 2.0f);
			prevPt = curPt;
		}


		// 4. 关键帧节点与切线手柄交互渲染 (Keyframe Nodes & Tangent Handles)
		const glm::vec2 mousePos = ctx.Input().MousePos;
		const bool inPlot = ctx.IsHovered(plotRect);


		const bool hasSelectedKf = (m_SelectedKeyframeIndex >= 0 && m_SelectedKeyframeIndex < static_cast<int>(activeCurve->KeyframeCount()));
		glm::vec2 selectedKfScreenPt { 0.0f };
		glm::vec2 inHandleScreenPos { 0.0f };
		glm::vec2 outHandleScreenPos { 0.0f };


		if (hasSelectedKf)
		{
			const auto& selKf = activeCurve->GetKeyframe(m_SelectedKeyframeIndex);
			selectedKfScreenPt = glm::vec2(timeToScreenX(selKf.Time), valToScreenY(selKf.Value));
			const float handleArmPixels = 36.0f;
			float inAngle = std::atan(selKf.InTangent);
			float outAngle = std::atan(selKf.OutTangent);


			inHandleScreenPos = selectedKfScreenPt + glm::vec2(-std::cos(inAngle), std::sin(inAngle)) * handleArmPixels;
			outHandleScreenPos = selectedKfScreenPt + glm::vec2(std::cos(outAngle), -std::sin(outAngle)) * handleArmPixels;


			// 绘制切线臂 (入切线蓝，出切线橙)
			Wui::LineSegment(ctx, selectedKfScreenPt, inHandleScreenPos, Wui::WuiColor { 0.25f, 0.65f, 1.0f, 0.9f }, 1.5f);
			Wui::LineSegment(ctx, selectedKfScreenPt, outHandleScreenPos, Wui::WuiColor { 1.0f, 0.65f, 0.2f, 0.9f }, 1.5f);


			// 入切线端点手柄
			const Wui::WuiRect inHRect { inHandleScreenPos.x - 4.0f, inHandleScreenPos.y - 4.0f, 8.0f, 8.0f };
			m_HoveredInHandle = ctx.IsHovered(inHRect);
			Wui::BarSurface(ctx, inHRect, m_HoveredInHandle ? Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f } : Wui::WuiColor { 0.25f, 0.65f, 1.0f, 1.0f }, theme.Border);
			Wui::Tooltip(ctx, inHRect, "入切线斜率手柄 (拖拽以调节)");


			// 出切线端点手柄
			const Wui::WuiRect outHRect { outHandleScreenPos.x - 4.0f, outHandleScreenPos.y - 4.0f, 8.0f, 8.0f };
			m_HoveredOutHandle = ctx.IsHovered(outHRect);
			Wui::BarSurface(ctx, outHRect, m_HoveredOutHandle ? Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f } : Wui::WuiColor { 1.0f, 0.65f, 0.2f, 1.0f }, theme.Border);
			Wui::Tooltip(ctx, outHRect, "出切线斜率手柄 (拖拽以调节)");
		}


		// 绘制各关键帧控制点 (菱形 Diamond)
		m_HoveredKeyframeIndex = -1;
		for (size_t k = 0; k < activeCurve->KeyframeCount(); ++k)
		{
			const auto& kf = activeCurve->GetKeyframe(k);
			glm::vec2 kp { timeToScreenX(kf.Time), valToScreenY(kf.Value) };
			const bool isSel = (static_cast<int>(k) == m_SelectedKeyframeIndex);
			const bool isHover = (glm::distance(mousePos, kp) <= 8.0f);


			if (isHover) m_HoveredKeyframeIndex = static_cast<int>(k);


			const float dSize = isSel ? 9.0f : (isHover ? 8.0f : 6.0f);
			const Wui::WuiRect dRect { kp.x - dSize * 0.5f, kp.y - dSize * 0.5f, dSize, dSize };
			const Wui::WuiColor kfCol = isSel ? Wui::WuiColor { 0.2f, 0.95f, 0.85f, 1.0f } : (isHover ? Wui::WuiColor { 1.0f, 0.85f, 0.2f, 1.0f } : theme.Accent);
			Wui::BarSurface(ctx, dRect, kfCol, theme.Border);


			if (isHover)
			{
				std::string tip = "关键帧 #" + std::to_string(k + 1) + ": t=" + std::to_string(kf.Time).substr(0, 4) + "s, val=" + std::to_string(kf.Value).substr(0, 4);
				Wui::Tooltip(ctx, dRect, tip);
			}
		}


		// 5. 鼠标交互判定: 拖拽关键帧 / 拖拽切线手柄 / 空白双击加点 / 右键删点
		if (inPlot && ctx.Input().MouseClicked[0])
		{
			bool hit = false;
			if (hasSelectedKf)
			{
				if (m_HoveredInHandle)
				{
					m_IsDraggingInHandle = true;
					hit = true;
				}
				else if (m_HoveredOutHandle)
				{
					m_IsDraggingOutHandle = true;
					hit = true;
				}
			}


			if (!hit && m_HoveredKeyframeIndex >= 0)
			{
				m_SelectedKeyframeIndex = m_HoveredKeyframeIndex;
				m_IsDraggingKeyframe = true;
				m_DragStartMouse = mousePos;
				const auto& kf = activeCurve->GetKeyframe(m_SelectedKeyframeIndex);
				m_DragKeyframeStartTime = kf.Time;
				m_DragKeyframeStartVal = kf.Value;
				hit = true;
			}


			// 双击空白处新增关键帧
			if (!hit && ctx.Input().MouseDoubleClicked[0])
			{
				float newT = screenXToTime(mousePos.x);
				float newV = screenYToVal(mousePos.y);
				PushUndoSnapshot();
				size_t newIdx = activeCurve->AddKeyframe(Keyframe(newT, newV));
				activeCurve->AutoSmoothTangents();
				activeCurve->PrecomputeLut(256);
				m_SelectedKeyframeIndex = static_cast<int>(newIdx);
				m_Dirty = true;
				SyncSimulationFromAsset();
			}
		}


		// 拖拽过程更新
		if (ctx.Input().MouseDown[0] && hasSelectedKf)
		{
			auto& mutKf = activeCurve->GetKeyframeMut(m_SelectedKeyframeIndex);


			if (m_IsDraggingKeyframe)
			{
				mutKf.Time = screenXToTime(mousePos.x);
				mutKf.Value = screenYToVal(mousePos.y);
				activeCurve->PrecomputeLut(256);
				m_Dirty = true;
				SyncSimulationFromAsset();
			}
			else if (m_IsDraggingInHandle)
			{
				float dt = screenXToTime(mousePos.x) - mutKf.Time;
				float dv = screenYToVal(mousePos.y) - mutKf.Value;
				if (std::abs(dt) > 1e-4f)
				{
					mutKf.InTangent = dv / dt;
					if (mutKf.Mode == TangentMode::Aligned) mutKf.OutTangent = mutKf.InTangent;
					activeCurve->PrecomputeLut(256);
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
			}
			else if (m_IsDraggingOutHandle)
			{
				float dt = screenXToTime(mousePos.x) - mutKf.Time;
				float dv = screenYToVal(mousePos.y) - mutKf.Value;
				if (std::abs(dt) > 1e-4f)
				{
					mutKf.OutTangent = dv / dt;
					if (mutKf.Mode == TangentMode::Aligned) mutKf.InTangent = mutKf.OutTangent;
					activeCurve->PrecomputeLut(256);
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
			}
		}
		else
		{
			if (m_IsDraggingKeyframe || m_IsDraggingInHandle || m_IsDraggingOutHandle)
			{
				m_IsDraggingKeyframe = false;
				m_IsDraggingInHandle = false;
				m_IsDraggingOutHandle = false;
				PushUndoSnapshot();
			}
		}


		// 右键关键帧弹出删除
		if (inPlot && ctx.Input().MouseClicked[1] && m_HoveredKeyframeIndex >= 0)
		{
			if (activeCurve->KeyframeCount() > 2)
			{
				PushUndoSnapshot();
				activeCurve->RemoveKeyframe(m_HoveredKeyframeIndex);
				activeCurve->PrecomputeLut(256);
				m_SelectedKeyframeIndex = std::max(0, m_HoveredKeyframeIndex - 1);
				m_Dirty = true;
				SyncSimulationFromAsset();
			}
		}
	}


	// =========================================================================
	// 时间轴控制器与多轨道显示 (Timeline)
	// =========================================================================


	void ParticleEditorPanel::RenderTimeline(Wui::WuiContext& ctx, const Wui::WuiRect& timelineRect, const Wui::WuiTheme& theme, PanelHost& host)
	{
		(void)host;
		Wui::BarSurface(ctx, timelineRect, Wui::WuiColor { 0.06f, 0.08f, 0.10f, 0.95f }, theme.Border);


		// 第 1 行: 走带控制器 (Transport Bar)
		float tx = timelineRect.X + 8.0f;
		const float ty = timelineRect.Y + 4.0f;


		const Wui::WuiRect rBtn { tx, ty, 36.0f, 22.0f };
		Wui::Tooltip(ctx, rBtn, "重置粒子推演至 0 秒 (Reset 0s)");
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.restart"), rBtn, "0s", theme))
		{
			SyncSimulationFromAsset();
		}
		tx += 40.0f;


		const Wui::WuiRect backBtn { tx, ty, 50.0f, 22.0f };
		Wui::Tooltip(ctx, backBtn, "后退 1 帧推演 (-16ms)");
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.back"), backBtn, "-16ms", theme))
		{
			float target = std::max(0.0f, m_SimTime - 0.0166f);
			ScrubToTime(target);
		}
		tx += 54.0f;


		const Wui::WuiRect pBtn { tx, ty, 48.0f, 22.0f };
		Wui::Tooltip(ctx, pBtn, m_IsPlaying ? "暂停视口模拟 (Pause)" : "开始视口模拟 (Play)");
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.play"), pBtn, m_IsPlaying ? "暂停" : "播放", theme))
		{
			m_IsPlaying = !m_IsPlaying;
		}
		tx += 52.0f;


		const Wui::WuiRect stepBtn { tx, ty, 50.0f, 22.0f };
		Wui::Tooltip(ctx, stepBtn, "前进 1 帧推演 (+16ms)");
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.step"), stepBtn, "+16ms", theme))
		{
			m_IsPlaying = false;
			StepSimulation(0.0166f);
		}
		tx += 56.0f;


		// 模拟时间数显
		std::string timeStr = std::to_string(m_SimTime).substr(0, 4) + "s / " + std::to_string(m_TimelineDuration).substr(0, 3) + "s";
		Wui::Label(ctx, { tx, ty + 4.0f }, timeStr, theme.Text, 10.5f);
		tx += 80.0f;


		// 倍速快捷切换按钮: 0.1, 0.25, 0.5, 1x, 2x, 4x
		const float speeds[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
		const char* speedLabels[] = { "0.1", "0.25", "0.5", "1x", "2x", "4x" };
		for (size_t s = 0; s < 6; ++s)
		{
			const float sBtnW = 28.0f;
			if (tx + sBtnW < timelineRect.X + timelineRect.W - 140.0f)
			{
				const Wui::WuiRect sRect { tx, ty, sBtnW, 22.0f };
				const bool isCur = std::abs(m_PlaybackSpeed - speeds[s]) < 0.05f;
				if (Wui::Button(ctx, Wui::HashId(("vfx.sp." + std::to_string(s)).c_str()), sRect, speedLabels[s], theme))
				{
					m_PlaybackSpeed = speeds[s];
				}
				if (isCur)
				{
					Wui::LineSegment(ctx, { sRect.X, sRect.Y + sRect.H }, { sRect.X + sRect.W, sRect.Y + sRect.H }, theme.Accent, 2.0f);
				}
				tx += sBtnW + 3.0f;
			}
		}


		// 曲线工作台展开切换按钮
		const float curveToggleW = 130.0f;
		const Wui::WuiRect curveToggleBtn { timelineRect.X + timelineRect.W - curveToggleW - 8.0f, ty, curveToggleW, 22.0f };
		Wui::Tooltip(ctx, curveToggleBtn, "折叠/展开内置完全可交互关键帧贝塞尔曲线工作台");
		if (Wui::Button(ctx, Wui::HashId("vfx.tl.curvetoggle"), curveToggleBtn,
			m_TimelineCurvesExpanded ? "[v] 内置曲线台" : "[>] 内置曲线台", theme))
		{
			m_TimelineCurvesExpanded = !m_TimelineCurvesExpanded;
		}


		// 第 2 行: 时间轴多轨道显示 (Multi-Track Timeline)
		float trackTopY = timelineRect.Y + 30.0f;
		const float trackAreaH = m_TimelineCurvesExpanded ? 52.0f : (timelineRect.H - 36.0f);
		const Wui::WuiRect trackAreaRect { timelineRect.X + 8.0f, trackTopY, timelineRect.W - 16.0f, trackAreaH };
		Wui::BarSurface(ctx, trackAreaRect, Wui::WuiColor { 0.04f, 0.05f, 0.07f, 0.95f }, theme.Border);


		const Wui::WuiRect rulerRect { trackAreaRect.X + 70.0f, trackAreaRect.Y + 2.0f, trackAreaRect.W - 74.0f, 14.0f };
		Wui::Label(ctx, { rulerRect.X, rulerRect.Y }, "0.0s", Wui::WuiColor { 0.5f, 0.55f, 0.6f, 0.7f }, 9.0f);
		Wui::Label(ctx, { rulerRect.X + rulerRect.W * 0.5f - 12.0f, rulerRect.Y }, std::to_string(m_TimelineDuration * 0.5f).substr(0, 3) + "s", Wui::WuiColor { 0.5f, 0.55f, 0.6f, 0.7f }, 9.0f);
		Wui::Label(ctx, { rulerRect.X + rulerRect.W - 24.0f, rulerRect.Y }, std::to_string(m_TimelineDuration).substr(0, 3) + "s", Wui::WuiColor { 0.5f, 0.55f, 0.6f, 0.7f }, 9.0f);


		const float normTime = std::clamp(m_SimTime / m_TimelineDuration, 0.0f, 1.0f);


		// 轨道 0: Master System Track
		float laneY = trackAreaRect.Y + 18.0f;
		const float laneH = 16.0f;
		const Wui::WuiRect masterLane { trackAreaRect.X + 4.0f, laneY, trackAreaRect.W - 8.0f, laneH };
		Wui::Label(ctx, { masterLane.X + 2.0f, masterLane.Y + 2.0f }, "Master", theme.Accent, 9.5f);
		const Wui::WuiRect masterBar { masterLane.X + 66.0f, masterLane.Y + 2.0f, masterLane.W - 68.0f, laneH - 4.0f };
		Wui::BarSurface(ctx, masterBar, Wui::WuiColor { 0.10f, 0.12f, 0.16f, 0.8f }, theme.Border);
		if (normTime > 0.0f)
		{
			Wui::BarSurface(ctx, { masterBar.X, masterBar.Y, masterBar.W * normTime, masterBar.H },
				Wui::WuiColor { theme.Accent.R, theme.Accent.G, theme.Accent.B, 0.35f }, theme.Accent);
		}


		// 播放指针 (Playhead needle)
		const float needleX = rulerRect.X + rulerRect.W * normTime;
		Wui::LineSegment(ctx, { needleX, trackAreaRect.Y }, { needleX, trackAreaRect.Y + trackAreaRect.H },
			Wui::WuiColor { 1.0f, 0.88f, 0.25f, 1.0f }, 2.0f);


		// 时间轴 Scrubbing 交互判定
		if (ctx.IsHovered(trackAreaRect) && ctx.Input().MouseDown[0])
		{
			const float scrubFrac = std::clamp((ctx.Input().MousePos.x - rulerRect.X) / rulerRect.W, 0.0f, 1.0f);
			ScrubToTime(scrubFrac * m_TimelineDuration);
		}


		// 第 3 行: 完全可交互的内置贝塞尔曲线工作台
		if (m_TimelineCurvesExpanded)
		{
			const float curveAreaY = trackTopY + trackAreaH + 4.0f;
			const float curveAreaH = timelineRect.H - (curveAreaY - timelineRect.Y) - 4.0f;
			const Wui::WuiRect curveCanvasRect { timelineRect.X + 8.0f, curveAreaY, timelineRect.W - 16.0f, curveAreaH };
			RenderInteractiveCurveWorkbench(ctx, curveCanvasRect, theme);
		}
	}


	// =========================================================================
	// 模块化特征堆栈与高自由度检视器 (Point 2 - 7大工业级模块全覆盖)
	// =========================================================================


	void ParticleEditorPanel::RenderInspector(Wui::WuiContext& ctx, const Wui::WuiRect& inspectorRect, const Wui::WuiTheme& theme, PanelHost& host)
	{
		(void)host;
		Wui::BarSurface(ctx, inspectorRect, theme.PanelBg, theme.Border);


		// 检视器顶部标题栏
		const Wui::WuiRect headRect { inspectorRect.X, inspectorRect.Y, inspectorRect.W, 28.0f };
		Wui::BarSurface(ctx, headRect, theme.PanelHeader, theme.Border);
		Wui::Label(ctx, { headRect.X + 8.0f, headRect.Y + 7.0f }, "参数检视器 (Details Inspector)", theme.Accent, 11.5f);


		if (m_SelectedEmitterIndex >= m_Asset.Emitters.size())
		{
			Wui::Label(ctx, { inspectorRect.X + 16.0f, inspectorRect.Y + 40.0f }, "未选择有效发射器图层", theme.TextMuted, 11.0f);
			return;
		}


		auto& em = m_Asset.Emitters[m_SelectedEmitterIndex];
		const bool isLocked = IsEmitterLocked(m_SelectedEmitterIndex);
		auto& ext = m_EmitterExtended[m_SelectedEmitterIndex];


		float curY = inspectorRect.Y + 32.0f;


		// 锁定提示条 (若图层被加锁锁定防误触)
		if (isLocked)
		{
			const Wui::WuiRect lockBanner { inspectorRect.X + 6.0f, curY, inspectorRect.W - 12.0f, 22.0f };
			Wui::BarSurface(ctx, lockBanner, Wui::WuiColor { 0.25f, 0.18f, 0.08f, 0.9f }, theme.Warning);
			Wui::Label(ctx, { lockBanner.X + 8.0f, lockBanner.Y + 4.0f }, "[锁定] 当前发射器已加锁 (只读保护中)", theme.Warning, 10.5f);
			curY += 26.0f;
		}


		// 检视器滚动区域
		const float scrollAreaH = inspectorRect.H - (curY - inspectorRect.Y) - 4.0f;
		const Wui::WuiRect scrollAreaRect { inspectorRect.X, curY, inspectorRect.W, scrollAreaH };


		// 动态统计展开卡片高度总和
		float totalCardsHeight = 0.0f;
		totalCardsHeight += (m_CardSpawningExpanded ? (180.0f + em.Bursts.size() * 26.0f) : 28.0f) + 6.0f;
		totalCardsHeight += (m_CardShapeExpanded ? 150.0f : 28.0f) + 6.0f;
		totalCardsHeight += (m_CardForcesExpanded ? 240.0f : 28.0f) + 6.0f;
		totalCardsHeight += (m_CardSizeRotationExpanded ? 140.0f : 28.0f) + 6.0f;
		totalCardsHeight += (m_CardColorAlphaExpanded ? 150.0f : 28.0f) + 6.0f;
		totalCardsHeight += (m_CardRenderersExpanded ? 160.0f : 28.0f) + 6.0f;
		totalCardsHeight += (m_CardCollisionExpanded ? 130.0f : 28.0f) + 40.0f;


		Wui::BeginScrollArea(ctx, scrollAreaRect, totalCardsHeight, m_InspectorScrollY, theme, Wui::HashId("vfx.inspector.scroll"));
		float cy = scrollAreaRect.Y - m_InspectorScrollY;


		// 封装 DragFloat 行与复位指示器
		auto renderDragFloatRow = [&](const char* fieldId, const char* label, const char* unit,
			float& val, float defaultVal, float speed, float minVal, float maxVal, const char* tip, float& y)
		{
			const float rowH = 22.0f;
			const float labelW = 96.0f;
			const float resetW = 20.0f;
			const float ctrlW = inspectorRect.W - 24.0f - labelW - resetW - 8.0f;
			const float rx = inspectorRect.X + 12.0f;


			std::string labelWithUnit = std::string(label) + (unit[0] ? (" (" + std::string(unit) + ")") : "");
			Wui::Label(ctx, { rx, y + 4.0f }, labelWithUnit, theme.Text, 10.5f);
			Wui::Tooltip(ctx, { rx, y, labelW, rowH }, tip);


			if (!isLocked)
			{
				float oldVal = val;
				Wui::DragFloat(ctx, Wui::HashId(fieldId), { rx + labelW + 4.0f, y, ctrlW, rowH }, val, speed, minVal, maxVal, theme);
				if (std::abs(val - oldVal) > 0.0001f)
				{
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
			}
			else
			{
				std::ostringstream ss;
				ss << std::fixed << std::setprecision(2) << val;
				Wui::Label(ctx, { rx + labelW + 4.0f, y + 4.0f }, ss.str(), theme.TextDisabled, 10.5f);
			}


			bool modified = std::abs(val - defaultVal) > 0.001f;
			const Wui::WuiRect resetRect { rx + labelW + 4.0f + ctrlW + 4.0f, y, resetW, rowH };
			if (!isLocked && Wui::ResetDefaultButton(ctx, Wui::HashId((std::string(fieldId) + ".rst").c_str()), resetRect, modified, theme, "", "恢复默认值 (" + std::to_string(defaultVal) + ")"))
			{
				PushUndoSnapshot();
				val = defaultVal;
				m_Dirty = true;
				SyncSimulationFromAsset();
			}
			y += rowH + 4.0f;
		};


		// -------------------------------------------------------------
		// 模块 1: 生成与生命周期 (Spawn & Burst)
		// -------------------------------------------------------------
		const float c1H = m_CardSpawningExpanded ? (180.0f + em.Bursts.size() * 26.0f) : 28.0f;
		const Wui::WuiRect c1 { inspectorRect.X + 4.0f, cy, inspectorRect.W - 8.0f, c1H };
		Wui::BarSurface(ctx, c1, theme.PanelBg, theme.Border);


		const Wui::WuiRect c1Header { c1.X, c1.Y, c1.W, 26.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.card1.fold"), c1Header,
			m_CardSpawningExpanded ? "[v] [1] 生成与脉冲 (Spawn & Burst)" : "[>] [1] 生成与脉冲 (Spawn & Burst)", theme))
		{
			m_CardSpawningExpanded = !m_CardSpawningExpanded;
		}


		if (m_CardSpawningExpanded)
		{
			float rowY = c1.Y + 28.0f;
			renderDragFloatRow("vfx.em.rate", "发射速率", "p/s", em.EmissionRate, 50.0f, 1.0f, 0.0f, 5000.0f, "每秒发射新粒子数量", rowY);


			float maxP = static_cast<float>(em.MaxParticles);
			renderDragFloatRow("vfx.em.max", "粒子容量", "p", maxP, 500.0f, 10.0f, 10.0f, 50000.0f, "当前发射器最大粒子承载池容量", rowY);
			em.MaxParticles = static_cast<uint32_t>(maxP);


			renderDragFloatRow("vfx.em.life", "基础寿命", "s", em.Lifetime, 2.0f, 0.05f, 0.05f, 60.0f, "单颗粒子存活基准时长", rowY);
			renderDragFloatRow("vfx.em.lifevar", "寿命扰动", "±s", ext.LifetimeVariance, 0.2f, 0.05f, 0.0f, 10.0f, "粒子随机寿命浮动方差", rowY);


			// 多段 Burst 脉冲列表管理 (Multi-Burst Table)
			Wui::Label(ctx, { c1.X + 12.0f, rowY + 3.0f }, "脉冲爆发列表 (Bursts):", theme.Accent, 10.5f);
			const Wui::WuiRect addBurstBtn { c1.X + c1.W - 84.0f, rowY, 74.0f, 20.0f };
			Wui::Tooltip(ctx, addBurstBtn, "向当前发射器添加一个瞬间脉冲爆发点");
			if (Wui::Button(ctx, Wui::HashId("vfx.burst.add"), addBurstBtn, "+添加脉冲", theme))
			{
				PushUndoSnapshot();
				em.Bursts.push_back({ 0.0f, 30 });
				m_Dirty = true;
				SyncSimulationFromAsset();
			}
			rowY += 24.0f;


			for (size_t b = 0; b < em.Bursts.size(); ++b)
			{
				auto& burst = em.Bursts[b];
				const Wui::WuiRect bRow { c1.X + 10.0f, rowY, c1.W - 20.0f, 22.0f };
				std::string bTitle = "#" + std::to_string(b + 1) + ":";
				Wui::Label(ctx, { bRow.X, bRow.Y + 3.0f }, bTitle, theme.TextMuted, 9.5f);


				Wui::DragFloat(ctx, Wui::HashId(("vfx.b.t." + std::to_string(b)).c_str()),
					{ bRow.X + 24.0f, bRow.Y, 75.0f, 20.0f }, burst.Time, 0.05f, 0.0f, m_TimelineDuration, theme);
				Wui::Tooltip(ctx, { bRow.X + 24.0f, bRow.Y, 75.0f, 20.0f }, "爆发触发时间 (s)");


				float bCount = static_cast<float>(burst.Count);
				Wui::DragFloat(ctx, Wui::HashId(("vfx.b.c." + std::to_string(b)).c_str()),
					{ bRow.X + 104.0f, bRow.Y, 65.0f, 20.0f }, bCount, 5.0f, 1.0f, 5000.0f, theme);
				burst.Count = static_cast<uint32_t>(bCount);
				Wui::Tooltip(ctx, { bRow.X + 104.0f, bRow.Y, 65.0f, 20.0f }, "爆发粒子数量 (p)");


				const Wui::WuiRect delBurstBtn { bRow.X + bRow.W - 32.0f, bRow.Y, 28.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(("vfx.b.del." + std::to_string(b)).c_str()), delBurstBtn, "X", theme))
				{
					PushUndoSnapshot();
					em.Bursts.erase(em.Bursts.begin() + b);
					m_Dirty = true;
					SyncSimulationFromAsset();
					break;
				}
				rowY += 24.0f;
			}
		}
		cy += c1H + 6.0f;


		// -------------------------------------------------------------
		// 模块 2: 空间形状与位置 (Shape & Location)
		// -------------------------------------------------------------
		const float c2H = m_CardShapeExpanded ? 150.0f : 28.0f;
		const Wui::WuiRect c2 { inspectorRect.X + 4.0f, cy, inspectorRect.W - 8.0f, c2H };
		Wui::BarSurface(ctx, c2, theme.PanelBg, theme.Border);


		const Wui::WuiRect c2Header { c2.X, c2.Y, c2.W, 26.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.card2.fold"), c2Header,
			m_CardShapeExpanded ? "[v] [2] 空间形状与位置 (Shape & Location)" : "[>] [2] 空间形状与位置 (Shape & Location)", theme))
		{
			m_CardShapeExpanded = !m_CardShapeExpanded;
		}


		if (m_CardShapeExpanded)
		{
			float rowY = c2.Y + 28.0f;
			std::string emTypeStr = "Cone (圆锥)";
			if (em.EmitterType == ParticleEmitterType::Sphere) emTypeStr = "Sphere (球体)";
			else if (em.EmitterType == ParticleEmitterType::Box) emTypeStr = "Box (盒体)";
			else if (em.EmitterType == ParticleEmitterType::Circle) emTypeStr = "Circle (圆环)";
			else if (em.EmitterType == ParticleEmitterType::Point) emTypeStr = "Point (单点)";


			Wui::Label(ctx, { c2.X + 12.0f, rowY + 3.0f }, "空间形状: " + emTypeStr, theme.Text, 10.5f);
			if (!isLocked)
			{
				const Wui::WuiRect toggleShape { c2.X + c2.W - 54.0f, rowY, 46.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.shape.toggle"), toggleShape, "切换", theme))
				{
					PushUndoSnapshot();
					if (em.EmitterType == ParticleEmitterType::Cone) em.EmitterType = ParticleEmitterType::Sphere;
					else if (em.EmitterType == ParticleEmitterType::Sphere) em.EmitterType = ParticleEmitterType::Box;
					else if (em.EmitterType == ParticleEmitterType::Box) em.EmitterType = ParticleEmitterType::Circle;
					else if (em.EmitterType == ParticleEmitterType::Circle) em.EmitterType = ParticleEmitterType::Point;
					else em.EmitterType = ParticleEmitterType::Cone;
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
			}
			rowY += 24.0f;


			if (em.EmitterType == ParticleEmitterType::Cone)
			{
				renderDragFloatRow("vfx.s.cone.ang", "圆锥开角", "°", ext.ConeAngle, 30.0f, 1.0f, 0.0f, 120.0f, "发射圆锥散射张角", rowY);
				renderDragFloatRow("vfx.s.cone.rad", "底面半径", "m", ext.ConeRadius, 0.5f, 0.05f, 0.01f, 20.0f, "圆锥基座底面圆环半径", rowY);
				renderDragFloatRow("vfx.s.cone.h", "圆锥高度", "m", ext.ConeHeight, 1.5f, 0.1f, 0.1f, 30.0f, "圆锥体基准参考高度", rowY);
			}
			else if (em.EmitterType == ParticleEmitterType::Sphere || em.EmitterType == ParticleEmitterType::Circle)
			{
				renderDragFloatRow("vfx.s.sph.rad", "球环半径", "m", ext.SphereRadius, 1.0f, 0.05f, 0.01f, 20.0f, "球体表面/圆环发射半径", rowY);
			}
			else if (em.EmitterType == ParticleEmitterType::Box)
			{
				renderDragFloatRow("vfx.s.box.x", "尺寸 X", "m", ext.BoxExtents.x, 1.0f, 0.05f, 0.01f, 20.0f, "盒体发射范围 X 半宽", rowY);
				renderDragFloatRow("vfx.s.box.y", "尺寸 Y", "m", ext.BoxExtents.y, 1.0f, 0.05f, 0.01f, 20.0f, "盒体发射范围 Y 半高", rowY);
				renderDragFloatRow("vfx.s.box.z", "尺寸 Z", "m", ext.BoxExtents.z, 1.0f, 0.05f, 0.01f, 20.0f, "盒体发射范围 Z 半深", rowY);
			}


			// 平面约束
			const Wui::WuiRect xyChk { c2.X + 12.0f, rowY + 2.0f, 18.0f, 18.0f };
			bool lockXY = em.LockToXY;
			if (Wui::Checkbox(ctx, Wui::HashId("vfx.s.lockxy"), xyChk, "", lockXY, theme))
			{
				PushUndoSnapshot();
				em.LockToXY = lockXY;
				m_Dirty = true;
				SyncSimulationFromAsset();
			}
			Wui::Label(ctx, { c2.X + 34.0f, rowY + 3.0f }, "约束在 2D XY 平面模拟 (Lock to XY)", theme.TextMuted, 10.0f);
		}
		cy += c2H + 6.0f;


		// -------------------------------------------------------------
		// 模块 3: 动力学、流体与扰动场 (Forces & Fields)
		// -------------------------------------------------------------
		const float c3H = m_CardForcesExpanded ? 240.0f : 28.0f;
		const Wui::WuiRect c3 { inspectorRect.X + 4.0f, cy, inspectorRect.W - 8.0f, c3H };
		Wui::BarSurface(ctx, c3, theme.PanelBg, theme.Border);


		const Wui::WuiRect c3Header { c3.X, c3.Y, c3.W, 26.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.card3.fold"), c3Header,
			m_CardForcesExpanded ? "[v] [3] 动力学、流体与力场 (Forces & Fields)" : "[>] [3] 动力学、流体与力场 (Forces & Fields)", theme))
		{
			m_CardForcesExpanded = !m_CardForcesExpanded;
		}


		if (m_CardForcesExpanded)
		{
			float rowY = c3.Y + 28.0f;
			renderDragFloatRow("vfx.f.spd", "初始速度", "m/s", em.Speed, 5.0f, 0.2f, 0.0f, 100.0f, "粒子喷射初始线速度", rowY);
			renderDragFloatRow("vfx.f.spdv", "速度方差", "m/s", em.SpeedVariance, 1.0f, 0.1f, 0.0f, 20.0f, "初速度随机扰动范围", rowY);
			renderDragFloatRow("vfx.f.grav", "竖直重力", "m/s²", em.Gravity.y, -9.8f, 0.5f, -50.0f, 50.0f, "竖直重力作用系数 (向上为正向喷发)", rowY);
			renderDragFloatRow("vfx.f.drag", "空气阻力", "x", em.LinearDrag, 0.0f, 0.05f, 0.0f, 10.0f, "速度衰减线性空气阻尼系数", rowY);


			// 3D 旋度噪声场 (Curl Noise Force)
			renderDragFloatRow("vfx.f.curl", "3D 旋度噪声", "x", ext.CurlNoiseStrength, 0.0f, 0.2f, 0.0f, 20.0f, "3D Curl Noise 向量场流体涡流强度", rowY);
			renderDragFloatRow("vfx.f.curlf", "噪声频率", "Hz", ext.CurlNoiseFrequency, 1.0f, 0.1f, 0.1f, 10.0f, "空间旋度噪声采样空间频率", rowY);


			// 三维旋涡力场 (Vortex Force)
			renderDragFloatRow("vfx.f.vort", "三维旋涡力", "x", em.VortexStrength, 0.0f, 0.5f, 0.0f, 50.0f, "绕中心旋转的涡流力场角加速度", rowY);


			// 空间点引力场 (Point Attractor Force)
			renderDragFloatRow("vfx.f.attr", "空间点引力", "x", ext.PointAttractorStrength, 0.0f, 0.5f, -50.0f, 50.0f, "引力中心拉扯力场 (负值排斥, 正值吸引)", rowY);
		}
		cy += c3H + 6.0f;


		// -------------------------------------------------------------
		// 模块 4: 尺寸与旋转 (Size & Rotation)
		// -------------------------------------------------------------
		const float c4H = m_CardSizeRotationExpanded ? 140.0f : 28.0f;
		const Wui::WuiRect c4 { inspectorRect.X + 4.0f, cy, inspectorRect.W - 8.0f, c4H };
		Wui::BarSurface(ctx, c4, theme.PanelBg, theme.Border);


		const Wui::WuiRect c4Header { c4.X, c4.Y, c4.W, 26.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.card4.fold"), c4Header,
			m_CardSizeRotationExpanded ? "[v] [4] 尺寸与旋转 (Size & Rotation)" : "[>] [4] 尺寸与旋转 (Size & Rotation)", theme))
		{
			m_CardSizeRotationExpanded = !m_CardSizeRotationExpanded;
		}


		if (m_CardSizeRotationExpanded)
		{
			float rowY = c4.Y + 28.0f;
			renderDragFloatRow("vfx.sz.start", "起始尺寸", "m", em.SizeStart, 0.5f, 0.05f, 0.01f, 20.0f, "粒子生成时刻初始尺寸", rowY);
			renderDragFloatRow("vfx.sz.end", "终止尺寸", "m", em.SizeEnd, 0.1f, 0.05f, 0.0f, 20.0f, "粒子消亡时刻终止尺寸", rowY);


			std::string fModeStr = (em.FacingMode == ParticleRenderMode::StretchedVelocity) ? "速度拉伸 (Stretched)" : (em.FacingMode == ParticleRenderMode::HorizontalQuad ? "贴地水平 (Horizontal)" : "相机对齐 (Billboard)");
			Wui::Label(ctx, { c4.X + 12.0f, rowY + 3.0f }, "朝向模式: " + fModeStr, theme.Text, 10.5f);
			if (!isLocked)
			{
				const Wui::WuiRect toggleFacing { c4.X + c4.W - 54.0f, rowY, 46.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.m.facing"), toggleFacing, "切换", theme))
				{
					PushUndoSnapshot();
					if (em.FacingMode == ParticleRenderMode::Billboard) em.FacingMode = ParticleRenderMode::StretchedVelocity;
					else if (em.FacingMode == ParticleRenderMode::StretchedVelocity) em.FacingMode = ParticleRenderMode::HorizontalQuad;
					else em.FacingMode = ParticleRenderMode::Billboard;
					em.Material.FacingMode = em.FacingMode;
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
			}
			rowY += 24.0f;


			renderDragFloatRow("vfx.r.rot", "初始旋转", "°", ext.InitialRotationDeg, 0.0f, 5.0f, 0.0f, 360.0f, "粒子生成时刻初始旋转偏角", rowY);
			renderDragFloatRow("vfx.r.angvel", "角速度", "°/s", ext.AngularVelocityDeg, 0.0f, 5.0f, -720.0f, 720.0f, "粒子绕法向旋转自转角速度", rowY);
		}
		cy += c4H + 6.0f;


		// -------------------------------------------------------------
		// 模块 5: 颜色与 HDR 辉光 (Color & Emissive)
		// -------------------------------------------------------------
		const float c5H = m_CardColorAlphaExpanded ? 150.0f : 28.0f;
		const Wui::WuiRect c5 { inspectorRect.X + 4.0f, cy, inspectorRect.W - 8.0f, c5H };
		Wui::BarSurface(ctx, c5, theme.PanelBg, theme.Border);


		const Wui::WuiRect c5Header { c5.X, c5.Y, c5.W, 26.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.card5.fold"), c5Header,
			m_CardColorAlphaExpanded ? "[v] [5] 颜色与 HDR 辉光 (Color & Emissive)" : "[>] [5] 颜色与 HDR 辉光 (Color & Emissive)", theme))
		{
			m_CardColorAlphaExpanded = !m_CardColorAlphaExpanded;
		}


		if (m_CardColorAlphaExpanded)
		{
			float rowY = c5.Y + 28.0f;


			std::string bModeStr = (em.Material.BlendMode == ParticleBlendMode::Additive) ? "叠加 (Additive)" : "透明 (AlphaBlend)";
			Wui::Label(ctx, { c5.X + 12.0f, rowY + 3.0f }, "混合模式: " + bModeStr, theme.Text, 10.5f);
			if (!isLocked)
			{
				const Wui::WuiRect toggleBlend { c5.X + c5.W - 54.0f, rowY, 46.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.m.blend"), toggleBlend, "切换", theme))
				{
					PushUndoSnapshot();
					em.Material.BlendMode = (em.Material.BlendMode == ParticleBlendMode::Additive) ? ParticleBlendMode::AlphaBlend : ParticleBlendMode::Additive;
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
			}
			rowY += 24.0f;


			// 起始色与终止色色块
			const Wui::WuiColor cs { em.ColorStart.r, em.ColorStart.g, em.ColorStart.b, 1.0f };
			Wui::BarSurface(ctx, { c5.X + 12.0f, rowY, 24.0f, 16.0f }, cs, theme.Border);
			Wui::Label(ctx, { c5.X + 40.0f, rowY }, "起始颜色", theme.Text, 10.5f);


			const Wui::WuiColor ce { em.ColorEnd.r, em.ColorEnd.g, em.ColorEnd.b, 1.0f };
			Wui::BarSurface(ctx, { c5.X + 110.0f, rowY, 24.0f, 16.0f }, ce, theme.Border);
			Wui::Label(ctx, { c5.X + 138.0f, rowY }, "终止颜色", theme.Text, 10.5f);
			rowY += 24.0f;


			renderDragFloatRow("vfx.c.emiss", "HDR 辉光", "x", em.Material.EmissiveBoost, 1.0f, 0.2f, 1.0f, 10.0f, "HDR 发光乘数，>=1.0 触发 Bloom 辉光泛光", rowY);
			renderDragFloatRow("vfx.m.fade", "软粒子渐变", "m", em.Material.SoftParticleFadeDistance, 0.5f, 0.05f, 0.0f, 5.0f, "与场景几何相交时的淡入淡出软化距离", rowY);
		}
		cy += c5H + 6.0f;


		// -------------------------------------------------------------
		// 模块 6: 多元渲染器 (Renderers)
		// -------------------------------------------------------------
		const float c6H = m_CardRenderersExpanded ? 160.0f : 28.0f;
		const Wui::WuiRect c6 { inspectorRect.X + 4.0f, cy, inspectorRect.W - 8.0f, c6H };
		Wui::BarSurface(ctx, c6, theme.PanelBg, theme.Border);


		const Wui::WuiRect c6Header { c6.X, c6.Y, c6.W, 26.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.card6.fold"), c6Header,
			m_CardRenderersExpanded ? "[v] [6] 多元渲染器切换 (Renderers)" : "[>] [6] 多元渲染器切换 (Renderers)", theme))
		{
			m_CardRenderersExpanded = !m_CardRenderersExpanded;
		}


		if (m_CardRenderersExpanded)
		{
			float rowY = c6.Y + 28.0f;
			std::string rendType = "Sprite (常规序列帧)";
			if (ext.RibbonEnabled) rendType = "Ribbon (带状拖尾体)";
			else if (ext.MeshEnabled) rendType = "Mesh (3D 网格粒子)";
			else if (ext.LightEnabled) rendType = "Light (动态点光源)";


			Wui::Label(ctx, { c6.X + 12.0f, rowY + 3.0f }, "渲染器: " + rendType, theme.Text, 10.5f);
			if (!isLocked)
			{
				const Wui::WuiRect toggleRend { c6.X + c6.W - 54.0f, rowY, 46.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.rend.toggle"), toggleRend, "切换", theme))
				{
					PushUndoSnapshot();
					if (!ext.RibbonEnabled && !ext.MeshEnabled && !ext.LightEnabled)
					{
						ext.RibbonEnabled = true; ext.ModuleRibbonEnabled = true;
					}
					else if (ext.RibbonEnabled)
					{
						ext.RibbonEnabled = false; ext.MeshEnabled = true; ext.ModuleMeshEnabled = true;
					}
					else if (ext.MeshEnabled)
					{
						ext.MeshEnabled = false; ext.LightEnabled = true; ext.ModuleLightEnabled = true;
					}
					else
					{
						ext.LightEnabled = false;
					}
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
			}
			rowY += 24.0f;


			if (ext.RibbonEnabled)
			{
				renderDragFloatRow("vfx.rib.len", "拖尾长度", "m", ext.RibbonLength, 1.0f, 0.1f, 0.1f, 20.0f, "Ribbon 拖尾带物理存留延展长度", rowY);
				renderDragFloatRow("vfx.rib.w", "带体宽度", "m", ext.RibbonWidth, 0.1f, 0.02f, 0.01f, 5.0f, "Ribbon 拖尾横向几何面宽度", rowY);
			}
			else if (ext.LightEnabled)
			{
				renderDragFloatRow("vfx.lit.r", "光源半径", "m", ext.LightRadius, 3.0f, 0.2f, 0.1f, 30.0f, "每个粒子向场景发射的点光源照亮范围", rowY);
				renderDragFloatRow("vfx.lit.int", "光照强度", "lux", ext.LightIntensity, 1.0f, 0.1f, 0.1f, 20.0f, "动态点光源光强乘数", rowY);
			}
			else
			{
				float tilesX = static_cast<float>(em.Material.SubUVTilesX);
				renderDragFloatRow("vfx.sub.x", "Sub-UV 列数", "cols", tilesX, 1.0f, 1.0f, 1.0f, 16.0f, "Flipbook 序列帧纹理横向分块数", rowY);
				em.Material.SubUVTilesX = static_cast<uint32_t>(tilesX);


				float tilesY = static_cast<float>(em.Material.SubUVTilesY);
				renderDragFloatRow("vfx.sub.y", "Sub-UV 行数", "rows", tilesY, 1.0f, 1.0f, 1.0f, 16.0f, "Flipbook 序列帧纹理纵向分块数", rowY);
				em.Material.SubUVTilesY = static_cast<uint32_t>(tilesY);
			}
		}
		cy += c6H + 6.0f;


		// -------------------------------------------------------------
		// 模块 7: 碰撞与次级事件 (Collision & Sub-Emitters)
		// -------------------------------------------------------------
		const float c7H = m_CardCollisionExpanded ? 130.0f : 28.0f;
		const Wui::WuiRect c7 { inspectorRect.X + 4.0f, cy, inspectorRect.W - 8.0f, c7H };
		Wui::BarSurface(ctx, c7, theme.PanelBg, theme.Border);


		const Wui::WuiRect c7Header { c7.X, c7.Y, c7.W, 26.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.card7.fold"), c7Header,
			m_CardCollisionExpanded ? "[v] [7] 物理碰撞与次级事件 (Collision & Events)" : "[>] [7] 物理碰撞与次级事件 (Collision & Events)", theme))
		{
			m_CardCollisionExpanded = !m_CardCollisionExpanded;
		}


		if (m_CardCollisionExpanded)
		{
			float rowY = c7.Y + 28.0f;
			std::string colModeStr = (em.Collision.Mode == ParticleCollisionMode::Plane) ? "地面反弹 (Plane)" : "无碰撞 (None)";
			Wui::Label(ctx, { c7.X + 12.0f, rowY + 3.0f }, "碰撞模式: " + colModeStr, theme.Text, 10.5f);
			if (!isLocked)
			{
				const Wui::WuiRect toggleCol { c7.X + c7.W - 54.0f, rowY, 46.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.col.toggle"), toggleCol, "切换", theme))
				{
					PushUndoSnapshot();
					em.Collision.Mode = (em.Collision.Mode == ParticleCollisionMode::Plane) ? ParticleCollisionMode::None : ParticleCollisionMode::Plane;
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
			}
			rowY += 24.0f;


			renderDragFloatRow("vfx.col.rest", "弹性系数", "x", em.Collision.Restitution, 0.5f, 0.05f, 0.0f, 1.0f, "法向碰撞反弹弹性 (0=无反弹, 1=完全弹性)", rowY);
			renderDragFloatRow("vfx.col.fric", "摩擦阻尼", "x", em.Collision.Friction, 0.1f, 0.02f, 0.0f, 1.0f, "碰撞切向摩擦减速系数", rowY);
		}
		cy += c7H + 10.0f;


		// 底部 [+ 添加新模块...] 按钮
		const Wui::WuiRect addModBtn { inspectorRect.X + 12.0f, cy, inspectorRect.W - 24.0f, 26.0f };
		Wui::Tooltip(ctx, addModBtn, "动态挂载或卸载高阶粒子功能模块 (Niagara Module Feature Stack)");
		if (Wui::Button(ctx, Wui::HashId("vfx.mod.add"), addModBtn, "+ 添加/卸载高级模块 (Add Module...)", theme))
		{
			m_AddModulePopupOpen = !m_AddModulePopupOpen;
		}


		Wui::EndScrollArea(ctx);
	}
}
