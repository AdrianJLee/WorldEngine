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

	void ParticleEditorPanel::EnsurePerEmitterState()
	{
		const size_t count = m_Asset.Emitters.size();
		if (m_EmitterSolo.size() != count) m_EmitterSolo.resize(count, false);
		if (m_EmitterMute.size() != count) m_EmitterMute.resize(count, false);
		if (m_EmitterGradients.size() != count)
		{
			const size_t oldSize = m_EmitterGradients.size();
			m_EmitterGradients.resize(count);
			for (size_t i = oldSize; i < count; ++i)
			{
				const auto& em = m_Asset.Emitters[i];
				ColorGradient grad;
				grad.AddColorStop(0.0f, glm::vec3(em.ColorStart));
				grad.AddColorStop(1.0f, glm::vec3(em.ColorEnd));
				grad.AddAlphaStop(0.0f, em.ColorStart.a);
				grad.AddAlphaStop(1.0f, em.ColorEnd.a);
				grad.PrecomputeLut(256);
				m_EmitterGradients[i] = std::move(grad);
			}
		}
		if (m_EmitterSizeCurves.size() != count)
		{
			const size_t oldSize = m_EmitterSizeCurves.size();
			m_EmitterSizeCurves.resize(count);
			m_EmitterCurveType.resize(count);
			for (size_t i = oldSize; i < count; ++i)
			{
				auto curve = CubicBezierCurve::EaseOut();
				curve.PrecomputeLut(256);
				m_EmitterSizeCurves[i] = std::move(curve);
				m_EmitterCurveType[i] = "EaseOut";
			}
		}
	}

	bool ParticleEditorPanel::IsEmitterActive(size_t index) const
	{
		if (index >= m_Asset.Emitters.size()) return false;
		if (m_EmitterMute.size() > index && m_EmitterMute[index]) return false;

		bool anySolo = false;
		for (size_t s = 0; s < m_EmitterSolo.size(); ++s)
		{
			if (m_EmitterSolo[s]) { anySolo = true; break; }
		}
		if (anySolo)
		{
			return (m_EmitterSolo.size() > index && m_EmitterSolo[index]);
		}
		return m_Asset.Emitters[index].Enabled;
	}

	void ParticleEditorPanel::ScrubToTime(float targetTime)
	{
		targetTime = std::clamp(targetTime, 0.0f, m_TimelineDuration);
		m_IsPlaying = false;
		SyncSimulationFromAsset();
		const float dt = 0.0333f;
		float t = 0.0f;
		while (t < targetTime)
		{
			float step = std::min(dt, targetTime - t);
			StepSimulation(step);
			t += step;
		}
		m_SimTime = targetTime;
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
		EnsurePerEmitterState();
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
			if (i < m_EmitterGradients.size())
			{
				pool->SetColorGradient(m_EmitterGradients[i]);
			}
			if (i < m_EmitterSizeCurves.size())
			{
				pool->SetSizeCurve(m_EmitterSizeCurves[i]);
			}
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
		if (m_TimelineDuration > 0.0f && m_SimTime > m_TimelineDuration && m_Asset.Looping)
		{
			m_SimTime = std::fmod(m_SimTime, m_TimelineDuration);
		}

		for (size_t i = 0; i < m_Asset.Emitters.size() && i < m_LivePools.size(); ++i)
		{
			if (!IsEmitterActive(i)) continue;
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
			if (i < m_EmitterGradients.size())
			{
				pool->SetColorGradient(m_EmitterGradients[i]);
			}
			if (i < m_EmitterSizeCurves.size())
			{
				pool->SetSizeCurve(m_EmitterSizeCurves[i]);
			}
			pool->Update(dt * m_PlaybackSpeed, comp, glm::vec3(0.0f));
		}
	}

	void ParticleEditorPanel::LoadPresetCampfire()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "Campfire_Compound";
		m_Asset.Seed = 1337;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.5f;
		m_TimelineDuration = 5.0f;

		// 1. Core Flame
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

		// 2. Rising Smoke
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

		// 3. Floating Embers
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
		m_StatusFeedback = "✓ 已载入商业预设: 篝火复合系统 (Campfire)";

		EnsurePerEmitterState();
		m_EmitterGradients[0] = ColorGradient::DefaultFire();
		m_EmitterGradients[1] = ColorGradient::DefaultSmoke();
		SyncSimulationFromAsset();
	}

	void ParticleEditorPanel::LoadPresetSparksBurst()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "SparksBurst_Collision";
		m_Asset.Seed = 2048;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.0f;
		m_TimelineDuration = 4.0f;

		// 1. Ground Sparks with bouncy collision
		ParticleEmitterLayer sparks;
		sparks.Name = "GroundSparks";
		sparks.Enabled = true;
		sparks.MaxParticles = 550;
		sparks.Lifetime = 1.6f;
		sparks.EmissionRate = 140.0f;
		sparks.EmitterType = ParticleEmitterType::Sphere;
		sparks.FacingMode = ParticleRenderMode::StretchedVelocity;
		sparks.Material.BlendMode = ParticleBlendMode::Additive;
		sparks.Material.EmissiveBoost = 4.2f;
		sparks.Speed = 8.5f;
		sparks.SpeedVariance = 2.5f;
		sparks.Gravity = glm::vec3(0.0f, -9.8f, 0.0f);
		sparks.LinearDrag = 0.2f;
		sparks.SizeStart = 0.18f;
		sparks.SizeEnd = 0.02f;
		sparks.ColorStart = glm::vec4(1.0f, 0.95f, 0.7f, 1.0f);
		sparks.ColorEnd = glm::vec4(1.0f, 0.3f, 0.0f, 0.0f);
		sparks.Collision.Mode = ParticleCollisionMode::Plane;
		sparks.Collision.Restitution = 0.65f;
		sparks.Collision.Friction = 0.35f;

		// 2. High Velocity Ricochet
		ParticleEmitterLayer ricochet;
		ricochet.Name = "RicochetTrail";
		ricochet.Enabled = true;
		ricochet.MaxParticles = 300;
		ricochet.Lifetime = 1.2f;
		ricochet.EmissionRate = 70.0f;
		ricochet.EmitterType = ParticleEmitterType::Cone;
		ricochet.FacingMode = ParticleRenderMode::StretchedVelocity;
		ricochet.Material.BlendMode = ParticleBlendMode::Additive;
		ricochet.Material.EmissiveBoost = 3.5f;
		ricochet.Speed = 12.0f;
		ricochet.Gravity = glm::vec3(0.0f, -11.0f, 0.0f);
		ricochet.SizeStart = 0.12f;
		ricochet.SizeEnd = 0.01f;
		ricochet.ColorStart = glm::vec4(1.0f, 0.7f, 0.1f, 1.0f);
		ricochet.ColorEnd = glm::vec4(0.8f, 0.1f, 0.0f, 0.0f);
		ricochet.Collision.Mode = ParticleCollisionMode::Plane;
		ricochet.Collision.Restitution = 0.5f;

		// 3. Central Flash Burst
		ParticleEmitterLayer flash;
		flash.Name = "CoreFlash";
		flash.Enabled = true;
		flash.MaxParticles = 90;
		flash.Lifetime = 0.35f;
		flash.EmissionRate = 25.0f;
		flash.EmitterType = ParticleEmitterType::Point;
		flash.FacingMode = ParticleRenderMode::Billboard;
		flash.Material.BlendMode = ParticleBlendMode::Additive;
		flash.Material.EmissiveBoost = 5.5f;
		flash.Speed = 2.0f;
		flash.SizeStart = 0.85f;
		flash.SizeEnd = 0.0f;
		flash.ColorStart = glm::vec4(1.0f, 1.0f, 0.85f, 1.0f);
		flash.ColorEnd = glm::vec4(1.0f, 0.5f, 0.1f, 0.0f);

		m_Asset.Emitters.push_back(sparks);
		m_Asset.Emitters.push_back(ricochet);
		m_Asset.Emitters.push_back(flash);

		m_LoadedPath = "vfx/sparks_burst.wvfx";
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "✓ 已载入商业预设: 火星飞溅碰撞系统 (SparksBurst)";

		EnsurePerEmitterState();
		SyncSimulationFromAsset();
	}

	void ParticleEditorPanel::LoadPresetSmokePlume()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "SmokePlume_Vortex";
		m_Asset.Seed = 4096;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.8f;
		m_TimelineDuration = 6.0f;

		// 1. Swirling Smoke
		ParticleEmitterLayer swirl;
		swirl.Name = "SwirlingSmoke";
		swirl.Enabled = true;
		swirl.MaxParticles = 420;
		swirl.Lifetime = 3.5f;
		swirl.EmissionRate = 50.0f;
		swirl.EmitterType = ParticleEmitterType::Cone;
		swirl.FacingMode = ParticleRenderMode::Billboard;
		swirl.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		swirl.Material.SoftParticlesEnabled = true;
		swirl.Speed = 2.2f;
		swirl.Gravity = glm::vec3(0.0f, 2.0f, 0.0f);
		swirl.VortexStrength = 3.6f;
		swirl.VortexAxis = glm::vec3(0.0f, 1.0f, 0.0f);
		swirl.TurbulenceStrength = 2.2f;
		swirl.LinearDrag = 0.6f;
		swirl.SizeStart = 0.3f;
		swirl.SizeEnd = 2.6f;
		swirl.ColorStart = glm::vec4(0.22f, 0.22f, 0.22f, 0.85f);
		swirl.ColorEnd = glm::vec4(0.55f, 0.55f, 0.55f, 0.0f);

		// 2. Dense Core Column
		ParticleEmitterLayer dense;
		dense.Name = "DenseColumn";
		dense.Enabled = true;
		dense.MaxParticles = 260;
		dense.Lifetime = 2.8f;
		dense.EmissionRate = 38.0f;
		dense.EmitterType = ParticleEmitterType::Cone;
		dense.FacingMode = ParticleRenderMode::Billboard;
		dense.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		dense.Material.SoftParticlesEnabled = true;
		dense.Speed = 1.5f;
		dense.Gravity = glm::vec3(0.0f, 1.6f, 0.0f);
		dense.VortexStrength = 1.8f;
		dense.TurbulenceStrength = 1.2f;
		dense.LinearDrag = 0.8f;
		dense.SizeStart = 0.4f;
		dense.SizeEnd = 1.8f;
		dense.ColorStart = glm::vec4(0.12f, 0.12f, 0.12f, 0.95f);
		dense.ColorEnd = glm::vec4(0.35f, 0.35f, 0.35f, 0.0f);

		// 3. Ash Cinders
		ParticleEmitterLayer cinders;
		cinders.Name = "AshCinders";
		cinders.Enabled = true;
		cinders.MaxParticles = 160;
		cinders.Lifetime = 3.0f;
		cinders.EmissionRate = 22.0f;
		cinders.EmitterType = ParticleEmitterType::Cone;
		cinders.FacingMode = ParticleRenderMode::StretchedVelocity;
		cinders.Material.BlendMode = ParticleBlendMode::Additive;
		cinders.Material.EmissiveBoost = 2.0f;
		cinders.Speed = 3.0f;
		cinders.Gravity = glm::vec3(0.0f, 2.2f, 0.0f);
		cinders.VortexStrength = 4.2f;
		cinders.TurbulenceStrength = 2.5f;
		cinders.SizeStart = 0.06f;
		cinders.SizeEnd = 0.01f;
		cinders.ColorStart = glm::vec4(1.0f, 0.5f, 0.2f, 0.9f);
		cinders.ColorEnd = glm::vec4(0.4f, 0.1f, 0.0f, 0.0f);

		m_Asset.Emitters.push_back(swirl);
		m_Asset.Emitters.push_back(dense);
		m_Asset.Emitters.push_back(cinders);

		m_LoadedPath = "vfx/smoke_plume.wvfx";
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "✓ 已载入商业预设: 涡流浓烟柱系统 (SmokePlume)";

		EnsurePerEmitterState();
		m_EmitterGradients[0] = ColorGradient::DefaultSmoke();
		m_EmitterGradients[1] = ColorGradient::DefaultSmoke();
		SyncSimulationFromAsset();
	}

	void ParticleEditorPanel::LoadPresetBlizzard()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "Blizzard_Atmosphere";
		m_Asset.Seed = 8192;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 1.0f;
		m_TimelineDuration = 5.0f;

		// 1. Driving Snow Flurries
		ParticleEmitterLayer flurries;
		flurries.Name = "DrivingSnow";
		flurries.Enabled = true;
		flurries.MaxParticles = 750;
		flurries.Lifetime = 2.8f;
		flurries.EmissionRate = 180.0f;
		flurries.EmitterType = ParticleEmitterType::Box;
		flurries.FacingMode = ParticleRenderMode::StretchedVelocity;
		flurries.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		flurries.Speed = 8.0f;
		flurries.SpeedVariance = 3.0f;
		flurries.Gravity = glm::vec3(-6.0f, -2.5f, 1.2f);
		flurries.LinearDrag = 0.7f;
		flurries.TurbulenceStrength = 3.0f;
		flurries.SizeStart = 0.07f;
		flurries.SizeEnd = 0.04f;
		flurries.ColorStart = glm::vec4(0.88f, 0.95f, 1.0f, 0.9f);
		flurries.ColorEnd = glm::vec4(0.80f, 0.90f, 1.0f, 0.0f);

		// 2. Swirling Wind Gusts
		ParticleEmitterLayer gusts;
		gusts.Name = "WindGusts";
		gusts.Enabled = true;
		gusts.MaxParticles = 320;
		gusts.Lifetime = 2.0f;
		gusts.EmissionRate = 55.0f;
		gusts.EmitterType = ParticleEmitterType::Box;
		gusts.FacingMode = ParticleRenderMode::Billboard;
		gusts.Material.BlendMode = ParticleBlendMode::Additive;
		gusts.Material.EmissiveBoost = 1.5f;
		gusts.Speed = 11.0f;
		gusts.Gravity = glm::vec3(-8.5f, -1.2f, 1.8f);
		gusts.VortexStrength = 4.0f;
		gusts.VortexAxis = glm::vec3(0.6f, 1.0f, 0.2f);
		gusts.SizeStart = 0.15f;
		gusts.SizeEnd = 0.35f;
		gusts.ColorStart = glm::vec4(0.6f, 0.85f, 1.0f, 0.5f);
		gusts.ColorEnd = glm::vec4(0.4f, 0.7f, 1.0f, 0.0f);

		// 3. Ground Drift Sheet
		ParticleEmitterLayer drift;
		drift.Name = "GroundDrift";
		drift.Enabled = true;
		drift.MaxParticles = 220;
		drift.Lifetime = 2.2f;
		drift.EmissionRate = 45.0f;
		drift.EmitterType = ParticleEmitterType::Box;
		drift.FacingMode = ParticleRenderMode::HorizontalQuad;
		drift.Material.BlendMode = ParticleBlendMode::AlphaBlend;
		drift.Speed = 4.0f;
		drift.Gravity = glm::vec3(-4.0f, 0.2f, 0.8f);
		drift.SizeStart = 0.4f;
		drift.SizeEnd = 0.8f;
		drift.ColorStart = glm::vec4(0.9f, 0.95f, 1.0f, 0.4f);
		drift.ColorEnd = glm::vec4(0.85f, 0.92f, 1.0f, 0.0f);

		m_Asset.Emitters.push_back(flurries);
		m_Asset.Emitters.push_back(gusts);
		m_Asset.Emitters.push_back(drift);

		m_LoadedPath = "vfx/blizzard.wvfx";
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "✓ 已载入商业预设: 暴风雪大气系统 (Blizzard)";

		EnsurePerEmitterState();
		SyncSimulationFromAsset();
	}

	void ParticleEditorPanel::LoadPresetMagicPortal()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "MagicPortal_Cosmic";
		m_Asset.Seed = 10007;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.6f;
		m_TimelineDuration = 5.0f;

		// 1. Cosmic Vortex Swirl
		ParticleEmitterLayer portal;
		portal.Name = "VortexSwirl";
		portal.Enabled = true;
		portal.MaxParticles = 520;
		portal.Lifetime = 2.2f;
		portal.EmissionRate = 95.0f;
		portal.EmitterType = ParticleEmitterType::Sphere;
		portal.FacingMode = ParticleRenderMode::Billboard;
		portal.Material.BlendMode = ParticleBlendMode::Additive;
		portal.Material.EmissiveBoost = 4.5f;
		portal.Speed = 2.5f;
		portal.Gravity = glm::vec3(0.0f, 0.0f, 0.0f);
		portal.VortexStrength = 7.5f;
		portal.VortexAxis = glm::vec3(0.0f, 1.0f, 0.0f);
		portal.LinearDrag = 0.2f;
		portal.TurbulenceStrength = 1.4f;
		portal.SizeStart = 0.35f;
		portal.SizeEnd = 0.05f;
		portal.ColorStart = glm::vec4(0.7f, 0.1f, 1.0f, 1.0f);
		portal.ColorEnd = glm::vec4(0.0f, 0.9f, 1.0f, 0.0f);

		// 2. Event Horizon Disk
		ParticleEmitterLayer disk;
		disk.Name = "EventHorizon";
		disk.Enabled = true;
		disk.MaxParticles = 260;
		disk.Lifetime = 1.4f;
		disk.EmissionRate = 55.0f;
		disk.EmitterType = ParticleEmitterType::Cone;
		disk.FacingMode = ParticleRenderMode::HorizontalQuad;
		disk.Material.BlendMode = ParticleBlendMode::Additive;
		disk.Material.EmissiveBoost = 5.5f;
		disk.Speed = 4.5f;
		disk.VortexStrength = 9.0f;
		disk.VortexAxis = glm::vec3(0.0f, 1.0f, 0.0f);
		disk.SizeStart = 0.5f;
		disk.SizeEnd = 0.1f;
		disk.ColorStart = glm::vec4(1.0f, 0.15f, 0.8f, 1.0f);
		disk.ColorEnd = glm::vec4(0.4f, 0.0f, 0.9f, 0.0f);

		// 3. Mana Arc Discharge
		ParticleEmitterLayer arcs;
		arcs.Name = "ManaArcs";
		arcs.Enabled = true;
		arcs.MaxParticles = 200;
		arcs.Lifetime = 1.0f;
		arcs.EmissionRate = 45.0f;
		arcs.EmitterType = ParticleEmitterType::Point;
		arcs.FacingMode = ParticleRenderMode::StretchedVelocity;
		arcs.Material.BlendMode = ParticleBlendMode::Additive;
		arcs.Material.EmissiveBoost = 6.0f;
		arcs.Speed = 5.5f;
		arcs.VortexStrength = 5.5f;
		arcs.SizeStart = 0.15f;
		arcs.SizeEnd = 0.02f;
		arcs.ColorStart = glm::vec4(0.3f, 0.95f, 1.0f, 1.0f);
		arcs.ColorEnd = glm::vec4(0.1f, 0.4f, 1.0f, 0.0f);

		m_Asset.Emitters.push_back(portal);
		m_Asset.Emitters.push_back(disk);
		m_Asset.Emitters.push_back(arcs);

		m_LoadedPath = "vfx/magic_portal.wvfx";
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "✓ 已载入商业预设: 漩涡传送门系统 (MagicPortal)";

		EnsurePerEmitterState();
		ColorGradient plasmaGrad;
		plasmaGrad.AddColorStop(0.0f, glm::vec3(0.7f, 0.1f, 1.0f));
		plasmaGrad.AddColorStop(0.5f, glm::vec3(1.0f, 0.15f, 0.8f));
		plasmaGrad.AddColorStop(1.0f, glm::vec3(0.0f, 0.9f, 1.0f));
		plasmaGrad.AddAlphaStop(0.0f, 1.0f);
		plasmaGrad.AddAlphaStop(1.0f, 0.0f);
		plasmaGrad.PrecomputeLut(256);
		m_EmitterGradients[0] = plasmaGrad;
		m_EmitterGradients[1] = plasmaGrad;

		SyncSimulationFromAsset();
	}

	void ParticleEditorPanel::LoadPresetEnergyBeam()
	{
		m_Asset = ParticleAsset();
		m_Asset.Name = "EnergyBeam_Laser";
		m_Asset.Seed = 16384;
		m_Asset.Looping = true;
		m_Asset.WarmupTime = 0.3f;
		m_TimelineDuration = 4.0f;

		// 1. Central Laser Beam Core
		ParticleEmitterLayer core;
		core.Name = "BeamCore";
		core.Enabled = true;
		core.MaxParticles = 600;
		core.Lifetime = 0.75f;
		core.EmissionRate = 220.0f;
		core.EmitterType = ParticleEmitterType::Point;
		core.FacingMode = ParticleRenderMode::StretchedVelocity;
		core.Material.BlendMode = ParticleBlendMode::Additive;
		core.Material.EmissiveBoost = 6.5f;
		core.Speed = 18.0f;
		core.SpeedVariance = 1.5f;
		core.Gravity = glm::vec3(0.0f, 0.0f, 0.0f);
		core.LinearDrag = 0.02f;
		core.SizeStart = 0.4f;
		core.SizeEnd = 0.1f;
		core.ColorStart = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
		core.ColorEnd = glm::vec4(0.1f, 0.7f, 1.0f, 0.0f);

		// 2. Spiral Discharge Aura
		ParticleEmitterLayer spiral;
		spiral.Name = "SpiralDischarge";
		spiral.Enabled = true;
		spiral.MaxParticles = 350;
		spiral.Lifetime = 0.9f;
		spiral.EmissionRate = 100.0f;
		spiral.EmitterType = ParticleEmitterType::Cone;
		spiral.FacingMode = ParticleRenderMode::Billboard;
		spiral.Material.BlendMode = ParticleBlendMode::Additive;
		spiral.Material.EmissiveBoost = 4.0f;
		spiral.Speed = 14.0f;
		spiral.VortexStrength = 8.5f;
		spiral.VortexAxis = glm::vec3(0.0f, 0.0f, 1.0f);
		spiral.SizeStart = 0.2f;
		spiral.SizeEnd = 0.04f;
		spiral.ColorStart = glm::vec4(0.65f, 0.2f, 1.0f, 0.8f);
		spiral.ColorEnd = glm::vec4(0.1f, 0.4f, 1.0f, 0.0f);

		// 3. Impact Point Scatter
		ParticleEmitterLayer splash;
		splash.Name = "ImpactSplash";
		splash.Enabled = true;
		splash.MaxParticles = 260;
		splash.Lifetime = 1.2f;
		splash.EmissionRate = 65.0f;
		splash.EmitterType = ParticleEmitterType::Sphere;
		splash.FacingMode = ParticleRenderMode::StretchedVelocity;
		splash.Material.BlendMode = ParticleBlendMode::Additive;
		splash.Material.EmissiveBoost = 4.5f;
		splash.Speed = 6.0f;
		splash.Gravity = glm::vec3(0.0f, -3.5f, 0.0f);
		splash.Collision.Mode = ParticleCollisionMode::Plane;
		splash.Collision.Restitution = 0.5f;
		splash.SizeStart = 0.15f;
		splash.SizeEnd = 0.02f;
		splash.ColorStart = glm::vec4(0.2f, 0.6f, 1.0f, 1.0f);
		splash.ColorEnd = glm::vec4(0.05f, 0.2f, 0.8f, 0.0f);

		m_Asset.Emitters.push_back(core);
		m_Asset.Emitters.push_back(spiral);
		m_Asset.Emitters.push_back(splash);

		m_LoadedPath = "vfx/energy_beam.wvfx";
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "✓ 已载入商业预设: 粒子能量光束系统 (EnergyBeam)";

		EnsurePerEmitterState();
		SyncSimulationFromAsset();
	}

	void ParticleEditorPanel::LoadPreset(const std::string& name)
	{
		if (name == "Campfire") LoadPresetCampfire();
		else if (name == "SparksBurst") LoadPresetSparksBurst();
		else if (name == "SmokePlume") LoadPresetSmokePlume();
		else if (name == "Blizzard") LoadPresetBlizzard();
		else if (name == "MagicPortal") LoadPresetMagicPortal();
		else if (name == "EnergyBeam") LoadPresetEnergyBeam();
		else LoadPresetCampfire();
	}

	void ParticleEditorPanel::NewAsset()
	{
		LoadPresetCampfire();
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

		// 平滑相机聚焦过渡
		m_CameraFocus = glm::mix(m_CameraFocus, m_TargetCameraFocus, 0.10f);

		float curY = rect.Y + 8.0f;
		const float padX = 12.0f;
		const float contentW = rect.W - padX * 2.0f;

		// =========================================================================
		// 1. 顶部操作栏 (含预设库、新建、保存与模式切换)
		// =========================================================================
		const Wui::WuiRect topBar { rect.X + padX, curY, contentW, 36.0f };
		Wui::BarSurface(ctx, topBar, theme.PanelHeader, theme.Border);

		Wui::Label(ctx, { topBar.X + 10.0f, topBar.Y + 9.0f }, "视觉特效系统 (VFX Studio)", theme.Accent, 13.0f);

		float tbX = topBar.X + 205.0f;

		const Wui::WuiRect newBtn { tbX, topBar.Y + 6.0f, 44.0f, 24.0f };
		Wui::Tooltip(ctx, newBtn, "新建复合特效工程");
		if (Wui::Button(ctx, Wui::HashId("vfx.bar.new"), newBtn, "新建", theme))
		{
			NewAsset();
		}
		tbX += 48.0f;

		const Wui::WuiRect saveBtn { tbX, topBar.Y + 6.0f, 44.0f, 24.0f };
		Wui::Tooltip(ctx, saveBtn, "保存当前特效到 .wvfx 资产文件 (Ctrl+S)");
		if (Wui::Button(ctx, Wui::HashId("vfx.bar.save"), saveBtn, "保存", theme))
		{
			SaveAsset();
		}
		tbX += 50.0f;

		const float modeBtnW = 86.0f;
		const Wui::WuiRect visualModeBtn { tbX, topBar.Y + 6.0f, modeBtnW, 24.0f };
		Wui::Tooltip(ctx, visualModeBtn, "切换到参数可视化模块堆栈视图");
		if (Wui::Button(ctx, Wui::HashId("vfx.mode.visual"), visualModeBtn,
			m_ViewMode == ParticleEditorViewMode::Visual ? "[● 检视]" : "  检视", theme))
		{
			m_ViewMode = ParticleEditorViewMode::Visual;
		}
		tbX += modeBtnW + 4.0f;

		const Wui::WuiRect codeModeBtn { tbX, topBar.Y + 6.0f, modeBtnW, 24.0f };
		Wui::Tooltip(ctx, codeModeBtn, "切换到声明式代码/YAML 编辑视图，热重载实时生效");
		if (Wui::Button(ctx, Wui::HashId("vfx.mode.code"), codeModeBtn,
			m_ViewMode == ParticleEditorViewMode::Code ? "[● 代码]" : "  代码", theme))
		{
			m_ViewMode = ParticleEditorViewMode::Code;
			SyncCodeFromAsset();
		}
		tbX += modeBtnW + 12.0f;

		// 商业级预设库快捷按钮组 (Campfire, Sparks, Smoke, Blizzard, Portal, Beam)
		Wui::Label(ctx, { tbX, topBar.Y + 10.0f }, "预设:", theme.TextMuted, 10.5f);
		tbX += 34.0f;

		const struct PresetEntry {
			const char* Id;
			const char* Label;
			const char* Name;
			const char* Tip;
		} presets[] = {
			{ "vfx.p.camp", "篝火", "Campfire", "加载篝火预设: 核心火焰 + 上升浓烟 + 浮动余烬" },
			{ "vfx.p.spark", "火星", "SparksBurst", "加载火星预设: 地面弹性碰撞 + 散射拖尾 + 核心闪光" },
			{ "vfx.p.smoke", "浓烟", "SmokePlume", "加载浓烟预设: 涡流旋转上升 + 烟柱 + 漂浮余灰" },
			{ "vfx.p.blizz", "暴雪", "Blizzard", "加载暴风雪预设: 狂风疾雪 + 卷扬气流 + 地面贴地风雪" },
			{ "vfx.p.portal", "传送门", "MagicPortal", "加载漩涡传送门预设: 宇宙涡流 + 视界光盘 + 魔法电弧" },
			{ "vfx.p.beam", "能量束", "EnergyBeam", "加载能量光束预设: 极速激光束 + 螺旋放电 + 击中溅射" }
		};

		for (const auto& p : presets)
		{
			const float pBtnW = 46.0f;
			if (tbX + pBtnW + 10.0f < topBar.X + topBar.W - 120.0f)
			{
				const Wui::WuiRect pRect { tbX, topBar.Y + 6.0f, pBtnW, 24.0f };
				Wui::Tooltip(ctx, pRect, p.Tip);
				if (Wui::Button(ctx, Wui::HashId(p.Id), pRect, p.Label, theme))
				{
					LoadPreset(p.Name);
				}
				tbX += pBtnW + 4.0f;
			}
		}

		if (!m_StatusFeedback.empty())
		{
			Wui::WuiColor fbColor = (m_StatusFeedback.find("✓") != std::string::npos || m_StatusFeedback.find("成功") != std::string::npos)
				? Wui::WuiColor { 0.3f, 0.85f, 0.4f, 1.0f }
				: (m_StatusFeedback.find("✗") != std::string::npos || m_StatusFeedback.find("失败") != std::string::npos)
					? Wui::WuiColor { 0.95f, 0.3f, 0.3f, 1.0f }
					: theme.TextMuted;
			Wui::Label(ctx, { tbX + 8.0f, topBar.Y + 10.0f }, m_StatusFeedback, fbColor, 10.5f);
		}

		curY += topBar.H + 8.0f;

		// =========================================================================
		// 2. 双栏工业级工作台 (左侧: 3D 自由交互视口 56% | 右侧: 属性/代码面板 44%)
		// =========================================================================
		const float gap = 12.0f;
		const float availH = std::max(400.0f, rect.H - (curY - rect.Y) - 12.0f);
		const float leftW = std::floor((contentW - gap) * 0.56f);
		const float rightW = contentW - gap - leftW;

		// -------------------------------------------------------------------------
		// 左侧: 3D 粒子视口 (自由相机、Gizmo、Overdraw 透视、走带走查)
		// -------------------------------------------------------------------------
		const Wui::WuiRect vpRect { rect.X + padX, curY, leftW, availH };
		Wui::BarSurface(ctx, vpRect, Wui::WuiColor { 0.07f, 0.08f, 0.10f, 1.0f }, theme.Border);

		const bool isVpHovered = ctx.IsHovered(vpRect);
		if (isVpHovered)
		{
			if (ctx.Input().Wheel != 0.0f)
			{
				m_CameraDistance = std::clamp(m_CameraDistance - ctx.Input().Wheel * 0.5f, 1.5f, 25.0f);
			}
			if (ctx.Input().MouseClicked[0])
			{
				if (ctx.Input().MousePos.y < vpRect.Y + vpRect.H - 56.0f)
				{
					m_IsOrbiting = true;
					m_LastMousePos = ctx.Input().MousePos;
				}
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

		// 视口地面基准参考网格 (Grid)
		const Wui::WuiColor gridCol { 0.20f, 0.24f, 0.30f, 0.35f };
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
		drawProjectedSegment(glm::vec3(0.0f), glm::vec3(0.0f, 1.5f, 0.0f), axisYCol, 2.0f);
		drawProjectedSegment(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.5f), axisZCol, 2.0f);

		// 发射器形状线框 (Emitter Shape Gizmo)
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
				const float hs = 0.45f;
				drawProjectedSegment(glm::vec3(-hs, 0.0f, -hs), glm::vec3(hs, 0.0f, -hs), gizmoCol, 1.2f);
				drawProjectedSegment(glm::vec3(hs, 0.0f, -hs), glm::vec3(hs, 0.0f, hs), gizmoCol, 1.2f);
				drawProjectedSegment(glm::vec3(hs, 0.0f, hs), glm::vec3(-hs, 0.0f, hs), gizmoCol, 1.2f);
				drawProjectedSegment(glm::vec3(-hs, 0.0f, hs), glm::vec3(-hs, 0.0f, -hs), gizmoCol, 1.2f);
				drawProjectedSegment(glm::vec3(-hs, hs * 2.0f, -hs), glm::vec3(hs, hs * 2.0f, -hs), gizmoCol, 1.2f);
				drawProjectedSegment(glm::vec3(hs, hs * 2.0f, -hs), glm::vec3(hs, hs * 2.0f, hs), gizmoCol, 1.2f);
				drawProjectedSegment(glm::vec3(hs, hs * 2.0f, hs), glm::vec3(-hs, hs * 2.0f, hs), gizmoCol, 1.2f);
				drawProjectedSegment(glm::vec3(-hs, hs * 2.0f, hs), glm::vec3(-hs, hs * 2.0f, -hs), gizmoCol, 1.2f);
			}
		}

		// 视口真实粒子群推演绘制 (支持 Overdraw 热力图透视模式)
		size_t totalActiveParticles = 0;
		size_t activeEmitterCount = 0;
		for (size_t l = 0; l < m_LivePools.size(); ++l)
		{
			if (!IsEmitterActive(l)) continue;
			activeEmitterCount++;
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

					// 激活 Overdraw 热力图时改用超低 Alpha (0.15) 纯红色加法叠加渲染
					Wui::WuiColor drawCol = m_HeatmapMode
						? Wui::WuiColor { 1.0f, 0.15f, 0.15f, 0.15f }
						: Wui::WuiColor {
							std::clamp(p.Color.r, 0.0f, 1.0f),
							std::clamp(p.Color.g, 0.0f, 1.0f),
							std::clamp(p.Color.b, 0.0f, 1.0f),
							std::clamp(p.Color.a, 0.0f, 1.0f)
						};
					Wui::BarSurface(ctx, pRect, drawCol, drawCol);
				}
			}
		}

		// 视口右上角预设视角与 Overdraw 热力图切换
		const float viewBtnW = 42.0f;
		float vx = vpRect.X + vpRect.W - (viewBtnW * 4.0f + 84.0f);
		const float vy = vpRect.Y + 8.0f;
		const Wui::WuiRect vIsland { vx - 4.0f, vy - 2.0f, viewBtnW * 4.0f + 80.0f, 26.0f };
		Wui::BarSurface(ctx, vIsland, Wui::WuiColor { 0.05f, 0.07f, 0.09f, 0.75f }, theme.Border);

		const Wui::WuiRect btn3D { vx, vy, viewBtnW, 22.0f };
		Wui::Tooltip(ctx, btn3D, "3D 自由轨道透视视角 (Orbit View)");
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
		Wui::Tooltip(ctx, btnSide, "侧视图: 水平侧视 ZY 抛物线轨迹 (Side View)");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.side"), btnSide, "侧视", theme)) { m_OrbitYaw = 1.57f; m_OrbitPitch = 0.0f; }
		vx += viewBtnW + 5.0f;

		// 视口 Overdraw 热力图透视状态切换按钮
		const Wui::WuiRect heatBtn { vx, vy, 64.0f, 22.0f };
		Wui::Tooltip(ctx, heatBtn, "切换视口 Overdraw 填充率热力图透视 (超低透明度加法叠加，暴露填充率瓶颈)");
		if (Wui::Button(ctx, Wui::HashId("vfx.view.heatmap"), heatBtn, m_HeatmapMode ? "[●热力图]" : "  热力图", theme))
		{
			m_HeatmapMode = !m_HeatmapMode;
		}

		// 视口左上角状态 HUD (显示活跃粒子数、发射层数、预估 DrawCall 与 Overdraw 警报等级)
		const size_t estDrawCalls = std::max<size_t>(1, activeEmitterCount);
		std::string overdrawAlert;
		Wui::WuiColor overdrawCol;
		if (totalActiveParticles < 400)
		{
			overdrawAlert = "🟢 低 / Low (良好)";
			overdrawCol = Wui::WuiColor { 0.3f, 0.85f, 0.4f, 1.0f };
		}
		else if (totalActiveParticles < 1200)
		{
			overdrawAlert = "🟡 中 / Moderate (重叠预警)";
			overdrawCol = Wui::WuiColor { 0.95f, 0.8f, 0.2f, 1.0f };
		}
		else
		{
			overdrawAlert = "🔴 极高 / Critical (严重填充瓶颈)";
			overdrawCol = Wui::WuiColor { 0.95f, 0.3f, 0.3f, 1.0f };
		}

		const Wui::WuiRect hudRect { vpRect.X + 8.0f, vpRect.Y + 8.0f, 235.0f, 76.0f };
		Wui::BarSurface(ctx, hudRect, Wui::WuiColor { 0.05f, 0.07f, 0.09f, 0.85f }, theme.Border);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 6.0f }, "● 3D 复合特效视口 (VFX Viewport)", theme.Accent, 11.5f);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 22.0f },
			"活跃粒子: " + std::to_string(totalActiveParticles) + " | 发射层: " + std::to_string(activeEmitterCount) + "/" + std::to_string(m_Asset.Emitters.size()) + " | 预估 DC: " + std::to_string(estDrawCalls),
			theme.Text, 10.5f);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 38.0f },
			"模拟时间: " + std::to_string(m_SimTime).substr(0, 5) + "s | 热力透视: " + (m_HeatmapMode ? "[已启用]" : "[已关闭]"),
			theme.TextMuted, 10.0f);
		Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 54.0f },
			"Overdraw 警报: " + overdrawAlert, overdrawCol, 10.5f);

		// 视口底部时间轴与走带控制器 (Transport Bar & Timeline Scrubbing)
		const Wui::WuiRect transIsland { vpRect.X + 8.0f, vpRect.Y + vpRect.H - 52.0f, vpRect.W - 16.0f, 44.0f };
		Wui::BarSurface(ctx, transIsland, Wui::WuiColor { 0.06f, 0.08f, 0.10f, 0.90f }, theme.Border);

		// 第 1 行: 时间轴 Scrubbing 进度条
		const Wui::WuiRect timelineRect { transIsland.X + 8.0f, transIsland.Y + 4.0f, transIsland.W - 16.0f, 15.0f };
		Wui::BarSurface(ctx, timelineRect, Wui::WuiColor { 0.04f, 0.05f, 0.07f, 0.95f }, theme.Border);

		const float normTime = std::clamp(m_SimTime / m_TimelineDuration, 0.0f, 1.0f);
		if (normTime > 0.0f)
		{
			const Wui::WuiRect fillRect { timelineRect.X, timelineRect.Y, timelineRect.W * normTime, timelineRect.H };
			Wui::BarSurface(ctx, fillRect, Wui::WuiColor { theme.Accent.R, theme.Accent.G, theme.Accent.B, 0.35f }, theme.Accent);
		}

		// 播放指针 (Playhead needle)
		const float needleX = timelineRect.X + timelineRect.W * normTime;
		Wui::LineSegment(ctx, { needleX, timelineRect.Y - 1.0f }, { needleX, timelineRect.Y + timelineRect.H + 1.0f }, Wui::WuiColor { 1.0f, 0.88f, 0.25f, 1.0f }, 2.0f);

		// 时间刻度标识
		Wui::Label(ctx, { timelineRect.X + 4.0f, timelineRect.Y + 2.0f }, "0.0s", Wui::WuiColor { 0.5f, 0.55f, 0.6f, 0.7f }, 9.0f);
		Wui::Label(ctx, { timelineRect.X + timelineRect.W * 0.5f - 12.0f, timelineRect.Y + 2.0f }, std::to_string(m_TimelineDuration * 0.5f).substr(0, 3) + "s", Wui::WuiColor { 0.5f, 0.55f, 0.6f, 0.7f }, 9.0f);
		Wui::Label(ctx, { timelineRect.X + timelineRect.W - 24.0f, timelineRect.Y + 2.0f }, std::to_string(m_TimelineDuration).substr(0, 3) + "s", Wui::WuiColor { 0.5f, 0.55f, 0.6f, 0.7f }, 9.0f);

		// 时间轴 Scrubbing 交互判定
		if (ctx.IsHovered(timelineRect) && ctx.Input().MouseDown[0])
		{
			const float scrubFrac = std::clamp((ctx.Input().MousePos.x - timelineRect.X) / timelineRect.W, 0.0f, 1.0f);
			ScrubToTime(scrubFrac * m_TimelineDuration);
		}

		// 第 2 行: 扩展走带控制器 (重置 0s, 步进 +16ms, 后退 -16ms, 播放/暂停, 倍速切换)
		float tx = transIsland.X + 8.0f;
		const float ty = transIsland.Y + 21.0f;

		const Wui::WuiRect rBtn { tx, ty, 38.0f, 20.0f };
		Wui::Tooltip(ctx, rBtn, "重置粒子推演至 0 秒 (Reset 0s)");
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.restart"), rBtn, "0s", theme))
		{
			SyncSimulationFromAsset();
		}
		tx += 42.0f;

		const Wui::WuiRect backBtn { tx, ty, 52.0f, 20.0f };
		Wui::Tooltip(ctx, backBtn, "后退 1 帧推演 (-16ms)");
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.back"), backBtn, "-16ms", theme))
		{
			float target = std::max(0.0f, m_SimTime - 0.0166f);
			ScrubToTime(target);
		}
		tx += 56.0f;

		const Wui::WuiRect pBtn { tx, ty, 48.0f, 20.0f };
		Wui::Tooltip(ctx, pBtn, m_IsPlaying ? "暂停视口模拟 (Pause)" : "开始视口模拟 (Play)");
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.play"), pBtn, m_IsPlaying ? "暂停" : "播放", theme))
		{
			m_IsPlaying = !m_IsPlaying;
		}
		tx += 52.0f;

		const Wui::WuiRect stepBtn { tx, ty, 52.0f, 20.0f };
		Wui::Tooltip(ctx, stepBtn, "前进 1 帧推演 (+16ms)");
		if (Wui::Button(ctx, Wui::HashId("vfx.vp.step"), stepBtn, "+16ms", theme))
		{
			m_IsPlaying = false;
			StepSimulation(0.0166f);
		}
		tx += 58.0f;

		Wui::Label(ctx, { tx, ty + 3.0f }, std::to_string(m_SimTime).substr(0, 4) + "s", theme.Text, 10.5f);
		tx += 40.0f;

		// 倍速快捷切换按钮: 0.1x, 0.25x, 0.5x, 1.0x, 2.0x, 4.0x
		const float speeds[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
		const char* speedLabels[] = { "0.1", "0.25", "0.5", "1x", "2x", "4x" };
		for (size_t s = 0; s < 6; ++s)
		{
			const float sBtnW = 28.0f;
			if (tx + sBtnW < transIsland.X + transIsland.W - 6.0f)
			{
				const Wui::WuiRect sRect { tx, ty, sBtnW, 20.0f };
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

		// =========================================================================
		// 3. 右侧面板 (视觉检视器 vs 代码编辑器)
		// =========================================================================
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
			EnsurePerEmitterState();

			// 3.1 发射层精细管理条 (支持 Solo 独奏、Mute 静音、克隆、删除)
			const Wui::WuiRect layerHeader { rightX, ry, rightW, 36.0f };
			Wui::BarSurface(ctx, layerHeader, theme.PanelHeader, theme.Border);

			float lx = layerHeader.X + 8.0f;
			Wui::Label(ctx, { lx, layerHeader.Y + 10.0f }, "图层:", theme.Accent, 11.5f);
			lx += 42.0f;

			for (size_t i = 0; i < m_Asset.Emitters.size(); ++i)
			{
				const auto& em = m_Asset.Emitters[i];
				const bool isSelected = (i == m_SelectedEmitterIndex);
				const float tabW = 68.0f;
				const Wui::WuiRect tabRect { lx, layerHeader.Y + 6.0f, tabW, 24.0f };

				std::string tabLabel = em.Name;
				if (tabLabel.size() > 7) tabLabel = tabLabel.substr(0, 6) + "..";
				if (isSelected) tabLabel = "● " + tabLabel;

				if (Wui::Button(ctx, Wui::HashId(("vfx.tab." + std::to_string(i)).c_str()), tabRect, tabLabel.c_str(), theme))
				{
					m_SelectedEmitterIndex = i;
					// 平滑视口聚焦对应发射器高度
					float focusY = std::clamp(em.Speed * 0.25f + em.Gravity.y * 0.1f, 0.5f, 2.5f);
					m_TargetCameraFocus = glm::vec3(0.0f, focusY, 0.0f);
				}
				lx += tabW + 2.0f;

				// 独奏 [S] (Solo) 按钮
				const Wui::WuiRect sRect { lx, layerHeader.Y + 6.0f, 18.0f, 24.0f };
				const bool isSolo = (m_EmitterSolo.size() > i && m_EmitterSolo[i]);
				Wui::Tooltip(ctx, sRect, "独奏该发射器 (Solo) - 仅模拟与显示此图层");
				if (Wui::Button(ctx, Wui::HashId(("vfx.solo." + std::to_string(i)).c_str()), sRect, isSolo ? "[S]" : "S", theme))
				{
					m_EmitterSolo[i] = !m_EmitterSolo[i];
				}
				lx += 20.0f;

				// 静音 [M] (Mute) 按钮
				const Wui::WuiRect mRect { lx, layerHeader.Y + 6.0f, 18.0f, 24.0f };
				const bool isMute = (m_EmitterMute.size() > i && m_EmitterMute[i]);
				Wui::Tooltip(ctx, mRect, "静音该发射器 (Mute) - 隐藏并暂停此图层推演");
				if (Wui::Button(ctx, Wui::HashId(("vfx.mute." + std::to_string(i)).c_str()), mRect, isMute ? "[M]" : "M", theme))
				{
					m_EmitterMute[i] = !m_EmitterMute[i];
				}
				lx += 24.0f;
			}

			// 克隆当前发射层 [+克隆]
			const Wui::WuiRect cloneLayerBtn { lx, layerHeader.Y + 6.0f, 44.0f, 24.0f };
			Wui::Tooltip(ctx, cloneLayerBtn, "克隆当前选中的发射层及其材质与曲线参数");
			if (Wui::Button(ctx, Wui::HashId("vfx.layer.clone"), cloneLayerBtn, "+克隆", theme))
			{
				if (m_SelectedEmitterIndex < m_Asset.Emitters.size())
				{
					auto clone = m_Asset.Emitters[m_SelectedEmitterIndex];
					clone.Name = clone.Name + "_Clone";
					m_Asset.Emitters.push_back(clone);
					EnsurePerEmitterState();
					m_SelectedEmitterIndex = m_Asset.Emitters.size() - 1;
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
			}
			lx += 46.0f;

			// 删除发射层 [X]
			if (m_Asset.Emitters.size() > 1)
			{
				const Wui::WuiRect delLayerBtn { lx, layerHeader.Y + 6.0f, 28.0f, 24.0f };
				Wui::Tooltip(ctx, delLayerBtn, "删除当前选中的发射层");
				if (Wui::Button(ctx, Wui::HashId("vfx.layer.del"), delLayerBtn, "X", theme))
				{
					m_Asset.Emitters.erase(m_Asset.Emitters.begin() + m_SelectedEmitterIndex);
					if (m_SelectedEmitterIndex >= m_Asset.Emitters.size())
						m_SelectedEmitterIndex = m_Asset.Emitters.size() - 1;
					EnsurePerEmitterState();
					m_Dirty = true;
					SyncSimulationFromAsset();
				}
				lx += 30.0f;
			}

			// 添加新空白发射层 [+加层]
			const Wui::WuiRect addLayerBtn { lx, layerHeader.Y + 6.0f, 44.0f, 24.0f };
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
				EnsurePerEmitterState();
				m_SelectedEmitterIndex = m_Asset.Emitters.size() - 1;
				m_Dirty = true;
				SyncSimulationFromAsset();
			}

			ry += layerHeader.H + 8.0f;

			// 3.2 检视区滚动区域 (BeginScrollArea)
			const float inspectorAreaH = availH - (ry - curY);
			const Wui::WuiRect scrollAreaRect { rightX, ry, rightW, inspectorAreaH };

			// 计算 7 大卡片展开高度总和
			float totalCardsHeight = 0.0f;
			totalCardsHeight += (m_CardSpawningExpanded ? 76.0f : 28.0f) + 6.0f;
			totalCardsHeight += (m_CardMaterialExpanded ? 76.0f : 28.0f) + 6.0f;
			totalCardsHeight += (m_CardMotionExpanded ? 90.0f : 28.0f) + 6.0f;
			totalCardsHeight += (m_CardSizeRotationExpanded ? 116.0f : 28.0f) + 6.0f;
			totalCardsHeight += (m_CardColorAlphaExpanded ? 134.0f : 28.0f) + 6.0f;
			totalCardsHeight += (m_CardCollisionExpanded ? 68.0f : 28.0f) + 6.0f;
			totalCardsHeight += (m_CardSubEmittersExpanded ? 54.0f : 28.0f) + 6.0f;

			Wui::BeginScrollArea(ctx, scrollAreaRect, totalCardsHeight, m_InspectorScrollY, theme, Wui::HashId("vfx.inspector.scroll"));

			if (m_SelectedEmitterIndex < m_Asset.Emitters.size())
			{
				auto& em = m_Asset.Emitters[m_SelectedEmitterIndex];
				float cy = scrollAreaRect.Y - m_InspectorScrollY;

				// -------------------------------------------------------------
				// 卡片 1: [x] 发射与形状 (Emitter & Spawning)
				// -------------------------------------------------------------
				const float c1H = m_CardSpawningExpanded ? 76.0f : 28.0f;
				const Wui::WuiRect c1 { rightX, cy, rightW, c1H };
				Wui::BarSurface(ctx, c1, theme.PanelBg, theme.Border);

				const Wui::WuiRect c1Header { c1.X, c1.Y, c1.W, 26.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.card1.fold"), c1Header,
					m_CardSpawningExpanded ? "▼ [1] 发射与形状 (Emitter & Spawning)" : "▶ [1] 发射与形状 (Emitter & Spawning)", theme))
				{
					m_CardSpawningExpanded = !m_CardSpawningExpanded;
				}

				if (m_CardSpawningExpanded)
				{
					std::string emTypeStr = "Cone (圆锥)";
					if (em.EmitterType == ParticleEmitterType::Sphere) emTypeStr = "Sphere (球体)";
					else if (em.EmitterType == ParticleEmitterType::Box) emTypeStr = "Box (盒体)";
					else if (em.EmitterType == ParticleEmitterType::Point) emTypeStr = "Point (单点)";

					Wui::Label(ctx, { c1.X + 16.0f, c1.Y + 30.0f }, "形状: " + emTypeStr, theme.Text, 11.0f);
					const Wui::WuiRect toggleShape { c1.X + 140.0f, c1.Y + 28.0f, 54.0f, 18.0f };
					if (Wui::Button(ctx, Wui::HashId("vfx.shape.toggle"), toggleShape, "切换", theme))
					{
						if (em.EmitterType == ParticleEmitterType::Cone) em.EmitterType = ParticleEmitterType::Sphere;
						else if (em.EmitterType == ParticleEmitterType::Sphere) em.EmitterType = ParticleEmitterType::Box;
						else if (em.EmitterType == ParticleEmitterType::Box) em.EmitterType = ParticleEmitterType::Point;
						else em.EmitterType = ParticleEmitterType::Cone;
						m_Dirty = true;
						SyncSimulationFromAsset();
					}

					Wui::Label(ctx, { c1.X + 16.0f, c1.Y + 52.0f }, "发射率: " + std::to_string(static_cast<int>(em.EmissionRate)) + " p/s | 容量: " + std::to_string(em.MaxParticles), theme.Text, 10.5f);
					const Wui::WuiRect rSub { c1.X + 220.0f, c1.Y + 50.0f, 26.0f, 18.0f };
					if (Wui::Button(ctx, Wui::HashId("vfx.r.sub"), rSub, "-10", theme)) { em.EmissionRate = std::max(0.0f, em.EmissionRate - 10.0f); m_Dirty = true; }
					const Wui::WuiRect rAdd { c1.X + 250.0f, c1.Y + 50.0f, 26.0f, 18.0f };
					if (Wui::Button(ctx, Wui::HashId("vfx.r.add"), rAdd, "+10", theme)) { em.EmissionRate += 10.0f; m_Dirty = true; }
				}
				cy += c1H + 6.0f;

				// -------------------------------------------------------------
				// 卡片 2: [x] 材质与渲染 (Material & Rendering)
				// -------------------------------------------------------------
				const float c2H = m_CardMaterialExpanded ? 76.0f : 28.0f;
				const Wui::WuiRect c2 { rightX, cy, rightW, c2H };
				Wui::BarSurface(ctx, c2, theme.PanelBg, theme.Border);

				const Wui::WuiRect c2Header { c2.X, c2.Y, c2.W, 26.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.card2.fold"), c2Header,
					m_CardMaterialExpanded ? "▼ [2] 材质与渲染 (Material & Rendering)" : "▶ [2] 材质与渲染 (Material & Rendering)", theme))
				{
					m_CardMaterialExpanded = !m_CardMaterialExpanded;
				}

				if (m_CardMaterialExpanded)
				{
					std::string bModeStr = (em.Material.BlendMode == ParticleBlendMode::Additive) ? "叠加(Additive)" : "透明(AlphaBlend)";
					Wui::Label(ctx, { c2.X + 16.0f, c2.Y + 30.0f }, "混合: " + bModeStr, theme.Text, 11.0f);
					const Wui::WuiRect toggleBlend { c2.X + 140.0f, c2.Y + 28.0f, 48.0f, 18.0f };
					if (Wui::Button(ctx, Wui::HashId("vfx.m.blend"), toggleBlend, "切换", theme))
					{
						em.Material.BlendMode = (em.Material.BlendMode == ParticleBlendMode::Additive) ? ParticleBlendMode::AlphaBlend : ParticleBlendMode::Additive;
						m_Dirty = true;
						SyncSimulationFromAsset();
					}

					std::string fModeStr = (em.FacingMode == ParticleRenderMode::StretchedVelocity) ? "速度拉伸" : (em.FacingMode == ParticleRenderMode::HorizontalQuad ? "地面水平" : "相机对齐");
					Wui::Label(ctx, { c2.X + 16.0f, c2.Y + 52.0f }, "朝向: " + fModeStr + " | 发光: " + std::to_string(em.Material.EmissiveBoost).substr(0, 3) + "x", theme.TextMuted, 10.5f);
					const Wui::WuiRect toggleFacing { c2.X + 220.0f, c2.Y + 50.0f, 54.0f, 18.0f };
					if (Wui::Button(ctx, Wui::HashId("vfx.m.facing"), toggleFacing, "切换", theme))
					{
						if (em.FacingMode == ParticleRenderMode::Billboard) em.FacingMode = ParticleRenderMode::StretchedVelocity;
						else if (em.FacingMode == ParticleRenderMode::StretchedVelocity) em.FacingMode = ParticleRenderMode::HorizontalQuad;
						else em.FacingMode = ParticleRenderMode::Billboard;
						em.Material.FacingMode = em.FacingMode;
						m_Dirty = true;
						SyncSimulationFromAsset();
					}
				}
				cy += c2H + 6.0f;

				// -------------------------------------------------------------
				// 卡片 3: [x] 速度与力场 (Velocity & Forces)
				// -------------------------------------------------------------
				const float c3H = m_CardMotionExpanded ? 90.0f : 28.0f;
				const Wui::WuiRect c3 { rightX, cy, rightW, c3H };
				Wui::BarSurface(ctx, c3, theme.PanelBg, theme.Border);

				const Wui::WuiRect c3Header { c3.X, c3.Y, c3.W, 26.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.card3.fold"), c3Header,
					m_CardMotionExpanded ? "▼ [3] 速度与力场 (Velocity & Forces)" : "▶ [3] 速度与力场 (Velocity & Forces)", theme))
				{
					m_CardMotionExpanded = !m_CardMotionExpanded;
				}

				if (m_CardMotionExpanded)
				{
					Wui::Label(ctx, { c3.X + 16.0f, c3.Y + 30.0f }, "初速: " + std::to_string(em.Speed).substr(0, 4) + " | 重力: " + std::to_string(em.Gravity.y).substr(0, 4), theme.Text, 10.5f);
					const Wui::WuiRect sSub { c3.X + 180.0f, c3.Y + 28.0f, 24.0f, 18.0f };
					if (Wui::Button(ctx, Wui::HashId("vfx.s.sub"), sSub, "-1", theme)) { em.Speed = std::max(0.0f, em.Speed - 1.0f); m_Dirty = true; }
					const Wui::WuiRect sAdd { c3.X + 208.0f, c3.Y + 28.0f, 24.0f, 18.0f };
					if (Wui::Button(ctx, Wui::HashId("vfx.s.add"), sAdd, "+1", theme)) { em.Speed += 1.0f; m_Dirty = true; }

					Wui::Label(ctx, { c3.X + 16.0f, c3.Y + 50.0f }, "阻力: " + std::to_string(em.LinearDrag).substr(0, 4) + " | 涡流: " + std::to_string(em.VortexStrength).substr(0, 4), theme.Text, 10.5f);
					const Wui::WuiRect dAdd { c3.X + 180.0f, c3.Y + 48.0f, 42.0f, 18.0f };
					if (Wui::Button(ctx, Wui::HashId("vfx.d.tog"), dAdd, "+阻力", theme)) { em.LinearDrag = (em.LinearDrag > 1.5f ? 0.0f : em.LinearDrag + 0.3f); m_Dirty = true; }
					const Wui::WuiRect vAdd { c3.X + 226.0f, c3.Y + 48.0f, 42.0f, 18.0f };
					if (Wui::Button(ctx, Wui::HashId("vfx.v.tog"), vAdd, "+涡流", theme)) { em.VortexStrength = (em.VortexStrength > 5.0f ? 0.0f : em.VortexStrength + 1.5f); m_Dirty = true; }

					Wui::Label(ctx, { c3.X + 16.0f, c3.Y + 70.0f }, "湍流扰动: " + std::to_string(em.TurbulenceStrength).substr(0, 4), theme.TextMuted, 10.0f);
					const Wui::WuiRect tTog { c3.X + 180.0f, c3.Y + 68.0f, 42.0f, 18.0f };
					if (Wui::Button(ctx, Wui::HashId("vfx.t.tog"), tTog, "+湍流", theme)) { em.TurbulenceStrength = (em.TurbulenceStrength > 3.0f ? 0.0f : em.TurbulenceStrength + 0.8f); m_Dirty = true; }
				}
				cy += c3H + 6.0f;

				// -------------------------------------------------------------
				// 卡片 4: [x] 尺寸与贝塞尔曲线交互预览 (Size & Bezier Curve)
				// -------------------------------------------------------------
				const float c4H = m_CardSizeRotationExpanded ? 116.0f : 28.0f;
				const Wui::WuiRect c4 { rightX, cy, rightW, c4H };
				Wui::BarSurface(ctx, c4, theme.PanelBg, theme.Border);

				const Wui::WuiRect c4Header { c4.X, c4.Y, c4.W, 26.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.card4.fold"), c4Header,
					m_CardSizeRotationExpanded ? "▼ [4] 尺寸与贝塞尔曲线 (Size & Bezier Curve)" : "▶ [4] 尺寸与贝塞尔曲线 (Size & Bezier Curve)", theme))
				{
					m_CardSizeRotationExpanded = !m_CardSizeRotationExpanded;
				}

				if (m_CardSizeRotationExpanded)
				{
					Wui::Label(ctx, { c4.X + 16.0f, c4.Y + 28.0f }, "尺寸: " + std::to_string(em.SizeStart).substr(0, 4) + " → " + std::to_string(em.SizeEnd).substr(0, 4), theme.Text, 10.5f);

					// 内嵌贝塞尔曲线交互预览条 (带关键帧切线与形状形态)
					const Wui::WuiRect curveStrip { c4.X + 16.0f, c4.Y + 44.0f, rightW - 32.0f, 38.0f };
					Wui::BarSurface(ctx, curveStrip, Wui::WuiColor { 0.04f, 0.05f, 0.07f, 1.0f }, theme.Border);

					// 绘制曲线网格辅助线 (水平中线与垂直四等分线)
					const Wui::WuiColor cGuideCol { 0.18f, 0.22f, 0.28f, 0.40f };
					Wui::LineSegment(ctx, { curveStrip.X, curveStrip.Y + curveStrip.H * 0.5f }, { curveStrip.X + curveStrip.W, curveStrip.Y + curveStrip.H * 0.5f }, cGuideCol, 1.0f);
					Wui::LineSegment(ctx, { curveStrip.X + curveStrip.W * 0.25f, curveStrip.Y }, { curveStrip.X + curveStrip.W * 0.25f, curveStrip.Y + curveStrip.H }, cGuideCol, 1.0f);
					Wui::LineSegment(ctx, { curveStrip.X + curveStrip.W * 0.50f, curveStrip.Y }, { curveStrip.X + curveStrip.W * 0.50f, curveStrip.Y + curveStrip.H }, cGuideCol, 1.0f);
					Wui::LineSegment(ctx, { curveStrip.X + curveStrip.W * 0.75f, curveStrip.Y }, { curveStrip.X + curveStrip.W * 0.75f, curveStrip.Y + curveStrip.H }, cGuideCol, 1.0f);

					// 采样曲线并绘制贝塞尔连续线段
					const auto& curCurve = m_EmitterSizeCurves[m_SelectedEmitterIndex];
					const int curveSamples = 24;
					glm::vec2 prevPt { curveStrip.X, curveStrip.Y + curveStrip.H - std::clamp(curCurve.Evaluate(0.0f), 0.0f, 1.0f) * (curveStrip.H - 6.0f) - 3.0f };

					for (int cs = 1; cs <= curveSamples; ++cs)
					{
						float t = static_cast<float>(cs) / static_cast<float>(curveSamples);
						float val = std::clamp(curCurve.Evaluate(t), 0.0f, 1.0f);
						glm::vec2 curPt { curveStrip.X + t * curveStrip.W, curveStrip.Y + curveStrip.H - val * (curveStrip.H - 6.0f) - 3.0f };
						Wui::LineSegment(ctx, prevPt, curPt, theme.Accent, 1.8f);
						prevPt = curPt;
					}

					// 绘制关键帧控制手柄与切线
					const glm::vec2 k0 { curveStrip.X, curveStrip.Y + curveStrip.H - std::clamp(curCurve.Evaluate(0.0f), 0.0f, 1.0f) * (curveStrip.H - 6.0f) - 3.0f };
					const glm::vec2 k1 { curveStrip.X + curveStrip.W, curveStrip.Y + curveStrip.H - std::clamp(curCurve.Evaluate(1.0f), 0.0f, 1.0f) * (curveStrip.H - 6.0f) - 3.0f };
					Wui::BarSurface(ctx, { k0.x - 3.0f, k0.y - 3.0f, 6.0f, 6.0f }, theme.Accent, Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f });
					Wui::BarSurface(ctx, { k1.x - 3.0f, k1.y - 3.0f, 6.0f, 6.0f }, theme.Accent, Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f });

					// 贝塞尔形态快捷按钮: 线性、缓出、缓入出、弹跳
					float cbx = c4.X + 16.0f;
					const float cby = c4.Y + 88.0f;

					const struct CurvePresetDef {
						const char* Id;
						const char* Label;
						const char* Name;
					} curvePresets[] = {
						{ "vfx.c.lin", "线性", "Linear" },
						{ "vfx.c.ease", "缓出", "EaseOut" },
						{ "vfx.c.inout", "缓入出", "EaseInOut" },
						{ "vfx.c.bounce", "弹跳", "Bounce" }
					};

					for (const auto& cp : curvePresets)
					{
						const Wui::WuiRect cpRect { cbx, cby, 44.0f, 20.0f };
						const bool isCur = (m_EmitterCurveType[m_SelectedEmitterIndex] == cp.Name);
						if (Wui::Button(ctx, Wui::HashId(cp.Id), cpRect, cp.Label, theme))
						{
							if (std::string(cp.Name) == "Linear") m_EmitterSizeCurves[m_SelectedEmitterIndex] = CubicBezierCurve::Linear();
							else if (std::string(cp.Name) == "EaseOut") m_EmitterSizeCurves[m_SelectedEmitterIndex] = CubicBezierCurve::EaseOut();
							else if (std::string(cp.Name) == "EaseInOut") m_EmitterSizeCurves[m_SelectedEmitterIndex] = CubicBezierCurve::EaseInOut();
							else if (std::string(cp.Name) == "Bounce") m_EmitterSizeCurves[m_SelectedEmitterIndex] = CubicBezierCurve::Bounce();
							m_EmitterSizeCurves[m_SelectedEmitterIndex].PrecomputeLut(256);
							m_EmitterCurveType[m_SelectedEmitterIndex] = cp.Name;
							m_Dirty = true;
							SyncSimulationFromAsset();
						}
						if (isCur)
						{
							Wui::LineSegment(ctx, { cpRect.X, cpRect.Y + cpRect.H }, { cpRect.X + cpRect.W, cpRect.Y + cpRect.H }, theme.Accent, 2.0f);
						}
						cbx += 48.0f;
					}

					const Wui::WuiRect curveBtn { c4.X + c4.W - 96.0f, cby, 80.0f, 20.0f };
					Wui::Tooltip(ctx, curveBtn, "打开独立贝塞尔曲线编辑器调节细节关键帧");
					if (Wui::Button(ctx, Wui::HashId("vfx.curve.link"), curveBtn, "~ 曲线窗口", theme))
					{
						host.OpenCurveEditor("");
					}
				}
				cy += c4H + 6.0f;

				// -------------------------------------------------------------
				// 卡片 5: [x] 颜色与多色标渐变条 (Color & Alpha Gradient)
				// -------------------------------------------------------------
				const float c5H = m_CardColorAlphaExpanded ? 134.0f : 28.0f;
				const Wui::WuiRect c5 { rightX, cy, rightW, c5H };
				Wui::BarSurface(ctx, c5, theme.PanelBg, theme.Border);

				const Wui::WuiRect c5Header { c5.X, c5.Y, c5.W, 26.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.card5.fold"), c5Header,
					m_CardColorAlphaExpanded ? "▼ [5] 颜色与多色标渐变 (Color & Alpha Gradient)" : "▶ [5] 颜色与多色标渐变 (Color & Alpha Gradient)", theme))
				{
					m_CardColorAlphaExpanded = !m_CardColorAlphaExpanded;
				}

				if (m_CardColorAlphaExpanded)
				{
					// 内嵌一体化多色标渐变条 (采样 ColorGradient)
					const auto& curGrad = m_EmitterGradients[m_SelectedEmitterIndex];
					const Wui::WuiRect gradBar { c5.X + 16.0f, c5.Y + 28.0f, rightW - 32.0f, 18.0f };
					Wui::BarSurface(ctx, gradBar, Wui::WuiColor { 0.05f, 0.05f, 0.06f, 1.0f }, theme.Border);

					const int gradSlices = 16;
					const float sliceW = gradBar.W / static_cast<float>(gradSlices);
					for (int s = 0; s < gradSlices; ++s)
					{
						float t0 = static_cast<float>(s) / static_cast<float>(gradSlices);
						float t1 = static_cast<float>(s + 1) / static_cast<float>(gradSlices);
						glm::vec4 c0 = curGrad.Evaluate(t0);
						glm::vec4 c1 = curGrad.Evaluate(t1);
						const Wui::WuiRect sliceRect { gradBar.X + static_cast<float>(s) * sliceW, gradBar.Y, sliceW + 0.5f, gradBar.H };
						Wui::GradientFill(ctx, sliceRect,
							Wui::WuiColor { c0.r, c0.g, c0.b, 1.0f },
							Wui::WuiColor { c1.r, c1.g, c1.b, 1.0f },
							false);
					}

					// 绘制色标微调标记 (Color Stop Pins)
					for (const auto& stop : curGrad.GetColorStops())
					{
						const float pinX = gradBar.X + std::clamp(stop.Time, 0.0f, 1.0f) * gradBar.W;
						Wui::BarSurface(ctx, { pinX - 3.0f, gradBar.Y + gradBar.H + 1.0f, 6.0f, 5.0f },
							Wui::WuiColor { stop.Color.r, stop.Color.g, stop.Color.b, 1.0f }, theme.Border);
					}

					// 内置预设渐变按钮: 火焰 Fire, 浓烟 Smoke, 等离子 Plasma, 彩虹 Rainbow
					float gbx = c5.X + 16.0f;
					const float gby = c5.Y + 54.0f;

					const struct GradPresetDef {
						const char* Id;
						const char* Label;
						const char* Name;
					} gradPresets[] = {
						{ "vfx.g.fire", "火焰", "Fire" },
						{ "vfx.g.smoke", "浓烟", "Smoke" },
						{ "vfx.g.plasma", "等离子", "Plasma" },
						{ "vfx.g.rainbow", "彩虹", "Rainbow" }
					};

					for (const auto& gp : gradPresets)
					{
						const Wui::WuiRect gpRect { gbx, gby, 48.0f, 20.0f };
						if (Wui::Button(ctx, Wui::HashId(gp.Id), gpRect, gp.Label, theme))
						{
							ColorGradient g;
							if (std::string(gp.Name) == "Fire")
							{
								g = ColorGradient::DefaultFire();
							}
							else if (std::string(gp.Name) == "Smoke")
							{
								g = ColorGradient::DefaultSmoke();
							}
							else if (std::string(gp.Name) == "Plasma")
							{
								g.AddColorStop(0.0f, glm::vec3(0.7f, 0.1f, 1.0f));
								g.AddColorStop(0.35f, glm::vec3(1.0f, 0.15f, 0.8f));
								g.AddColorStop(0.7f, glm::vec3(0.0f, 0.9f, 1.0f));
								g.AddColorStop(1.0f, glm::vec3(1.0f, 1.0f, 1.0f));
								g.AddAlphaStop(0.0f, 0.0f);
								g.AddAlphaStop(0.2f, 1.0f);
								g.AddAlphaStop(0.8f, 0.9f);
								g.AddAlphaStop(1.0f, 0.0f);
							}
							else if (std::string(gp.Name) == "Rainbow")
							{
								g.AddColorStop(0.0f, glm::vec3(1.0f, 0.1f, 0.1f));
								g.AddColorStop(0.2f, glm::vec3(1.0f, 0.85f, 0.1f));
								g.AddColorStop(0.4f, glm::vec3(0.1f, 0.95f, 0.2f));
								g.AddColorStop(0.6f, glm::vec3(0.1f, 0.85f, 1.0f));
								g.AddColorStop(0.8f, glm::vec3(0.3f, 0.2f, 1.0f));
								g.AddColorStop(1.0f, glm::vec3(0.95f, 0.2f, 0.85f));
								g.AddAlphaStop(0.0f, 0.0f);
								g.AddAlphaStop(0.1f, 1.0f);
								g.AddAlphaStop(0.9f, 1.0f);
								g.AddAlphaStop(1.0f, 0.0f);
							}
							g.PrecomputeLut(256);
							m_EmitterGradients[m_SelectedEmitterIndex] = std::move(g);
							em.ColorStart = m_EmitterGradients[m_SelectedEmitterIndex].Evaluate(0.0f);
							em.ColorEnd = m_EmitterGradients[m_SelectedEmitterIndex].Evaluate(1.0f);
							m_Dirty = true;
							SyncSimulationFromAsset();
						}
						gbx += 52.0f;
					}

					// 起始色与终止色色块
					const Wui::WuiColor cs { em.ColorStart.r, em.ColorStart.g, em.ColorStart.b, 1.0f };
					Wui::BarSurface(ctx, { c5.X + 16.0f, c5.Y + 82.0f, 24.0f, 16.0f }, cs, theme.Border);
					Wui::Label(ctx, { c5.X + 44.0f, c5.Y + 82.0f }, "起始色", theme.Text, 10.5f);

					const Wui::WuiColor ce { em.ColorEnd.r, em.ColorEnd.g, em.ColorEnd.b, 1.0f };
					Wui::BarSurface(ctx, { c5.X + 100.0f, c5.Y + 82.0f, 24.0f, 16.0f }, ce, theme.Border);
					Wui::Label(ctx, { c5.X + 128.0f, c5.Y + 82.0f }, "终止色", theme.Text, 10.5f);

					// HDR 发光增益微调
					Wui::Label(ctx, { c5.X + 16.0f, c5.Y + 108.0f }, "HDR 发光: " + std::to_string(em.Material.EmissiveBoost).substr(0, 3) + "x", theme.TextMuted, 10.5f);
					const Wui::WuiRect hSub { c5.X + 130.0f, c5.Y + 106.0f, 32.0f, 18.0f };
					if (Wui::Button(ctx, Wui::HashId("vfx.hdr.sub"), hSub, "-0.5", theme))
					{
						em.Material.EmissiveBoost = std::max(0.1f, em.Material.EmissiveBoost - 0.5f);
						m_Dirty = true;
					}
					const Wui::WuiRect hAdd { c5.X + 166.0f, c5.Y + 106.0f, 32.0f, 18.0f };
					if (Wui::Button(ctx, Wui::HashId("vfx.hdr.add"), hAdd, "+0.5", theme))
					{
						em.Material.EmissiveBoost += 0.5f;
						m_Dirty = true;
					}
					const Wui::WuiRect hBoost { c5.X + 202.0f, c5.Y + 106.0f, 36.0f, 18.0f };
					if (Wui::Button(ctx, Wui::HashId("vfx.hdr.boost"), hBoost, "+2.0", theme))
					{
						em.Material.EmissiveBoost += 2.0f;
						m_Dirty = true;
					}
				}
				cy += c5H + 6.0f;

				// -------------------------------------------------------------
				// 卡片 6: [x] 物理碰撞与事件 (Collision & Events)
				// -------------------------------------------------------------
				const float c6H = m_CardCollisionExpanded ? 68.0f : 28.0f;
				const Wui::WuiRect c6 { rightX, cy, rightW, c6H };
				Wui::BarSurface(ctx, c6, theme.PanelBg, theme.Border);

				const Wui::WuiRect c6Header { c6.X, c6.Y, c6.W, 26.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.card6.fold"), c6Header,
					m_CardCollisionExpanded ? "▼ [6] 物理碰撞与地面反弹 (Collision & Events)" : "▶ [6] 物理碰撞与地面反弹 (Collision & Events)", theme))
				{
					m_CardCollisionExpanded = !m_CardCollisionExpanded;
				}

				if (m_CardCollisionExpanded)
				{
					std::string colModeStr = (em.Collision.Mode == ParticleCollisionMode::Plane) ? "地面反弹 (Plane)" : "无 (None)";
					Wui::Label(ctx, { c6.X + 16.0f, c6.Y + 30.0f }, "碰撞: " + colModeStr, theme.Text, 11.0f);
					const Wui::WuiRect toggleCol { c6.X + 140.0f, c6.Y + 28.0f, 48.0f, 18.0f };
					if (Wui::Button(ctx, Wui::HashId("vfx.col.toggle"), toggleCol, "切换", theme))
					{
						em.Collision.Mode = (em.Collision.Mode == ParticleCollisionMode::Plane) ? ParticleCollisionMode::None : ParticleCollisionMode::Plane;
						m_Dirty = true;
						SyncSimulationFromAsset();
					}
					Wui::Label(ctx, { c6.X + 16.0f, c6.Y + 48.0f }, "反弹系数: " + std::to_string(em.Collision.Restitution).substr(0, 4) + " | 摩擦: " + std::to_string(em.Collision.Friction).substr(0, 4), theme.TextMuted, 10.0f);
				}
				cy += c6H + 6.0f;

				// -------------------------------------------------------------
				// 卡片 7: [x] 次级发射器派生 (Sub-Emitters)
				// -------------------------------------------------------------
				const float c7H = m_CardSubEmittersExpanded ? 54.0f : 28.0f;
				const Wui::WuiRect c7 { rightX, cy, rightW, c7H };
				Wui::BarSurface(ctx, c7, theme.PanelBg, theme.Border);

				const Wui::WuiRect c7Header { c7.X, c7.Y, c7.W, 26.0f };
				if (Wui::Button(ctx, Wui::HashId("vfx.card7.fold"), c7Header,
					m_CardSubEmittersExpanded ? "▼ [7] 次级发射器派生 (Sub-Emitters)" : "▶ [7] 次级发射器派生 (Sub-Emitters)", theme))
				{
					m_CardSubEmittersExpanded = !m_CardSubEmittersExpanded;
				}

				if (m_CardSubEmittersExpanded)
				{
					Wui::Label(ctx, { c7.X + 16.0f, c7.Y + 30.0f }, "碰撞派生: OnCollision 沿法线火星飞溅", theme.TextMuted, 10.5f);
				}
				cy += c7H + 6.0f;
			}

			Wui::EndScrollArea(ctx);
		}
	}
}
