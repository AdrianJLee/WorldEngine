#include "wldpch.h"
#include "WUI/Panels/ParticleEditorPanel.h"
#include "World/Core/Application.h"
#include "WUI/Panels/ViewportPanel.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Renderer/ParticleAsset.h"
#include "World/Renderer/VfxModule.h"
#include "World/Renderer/VfxComputeGenerator.h"
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


	// 2D [marker] Cohen-Sutherland 
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
		RefreshProjectCustomModules();
	}


	ParticleEditorPanel::~ParticleEditorPanel()
	{
		ReleasePreviewResources();
	}


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
		defaultEmitter.Material.TexturePath = "engine/textures/particles/flame_soft.wtex";
		defaultEmitter.Material.MaterialPath = "engine/materials/particles/particle_additive.wmat";
		defaultEmitter.Material.BlendMode = ParticleBlendMode::Additive;
		defaultEmitter.Material.EmissiveBoost = 2.0f;


		m_Asset.Emitters.clear();
		defaultEmitter.EnsureDefaultModules();
		m_Asset.Emitters.push_back(defaultEmitter);


		m_LoadedPath.clear();
		m_Dirty = false;
		m_SelectedEmitterIndex = 0;
		m_StatusFeedback = "已新建特效资产";
		m_IsPlaying = true;
		EnsurePerEmitterState();
		SyncSimulationFromAsset();
	}


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
		std::error_code ec;
		std::unordered_set<std::string> seen;

		// 1. 项目资产 (Project Assets)
		std::filesystem::path assetRoot = World::Paths::AssetRoot();
		if (std::filesystem::exists(assetRoot, ec))
		{
			for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot, ec))
			{
				if (entry.is_regular_file(ec) && entry.path().extension() == ".wvfx")
				{
					std::string rel = std::filesystem::relative(entry.path(), assetRoot, ec).generic_string();
					if (!seen.count(rel))
					{
						seen.insert(rel);
						m_AvailableAssets.push_back(rel);
					}
				}
			}
		}

		// 2. 引擎全局内容库 (Engine Content)
		std::filesystem::path engineRoot = World::Paths::EngineContentRoot();
		if (!engineRoot.empty() && std::filesystem::exists(engineRoot, ec))
		{
			for (const auto& entry : std::filesystem::recursive_directory_iterator(engineRoot, ec))
			{
				if (entry.is_regular_file(ec) && entry.path().extension() == ".wvfx")
				{
					std::string rel = "engine/" + std::filesystem::relative(entry.path(), engineRoot, ec).generic_string();
					if (!seen.count(rel))
					{
						seen.insert(rel);
						m_AvailableAssets.push_back(rel);
					}
				}
			}
		}
	}


	void ParticleEditorPanel::EnsurePreviewResources()
	{
		const Rhi::Handle<Rhi::Device>& device = Renderer::GetDevice();
		if (!device) return;
		if (m_PreviewGpuDevice == device.get() && m_PreviewRenderer) return;

		ReleasePreviewResources();
		m_PreviewGpuDevice = device.get();
		m_PreviewRenderer = CreateRef<SceneRenderer>();
		m_PreviewRenderer->Init();

		m_PreviewScene = CreateRef<Scene>(Application::Get().GetContext());
		m_PreviewEntity = Entity::CreateEntity(m_PreviewScene.get(), "VfxPreviewEntity");
		m_PreviewEntity.AddComponent<TransformComponent>();
		auto& vfx = m_PreviewEntity.AddComponent<VfxComponent>();
		vfx.Effect.Path = StringPool::Get().InternPath("__vfx_editor_preview__.wvfx");
		vfx.PlaybackState = VfxPlaybackState::Playing;

		ParticleAssetRegistry::Get().RegisterAssetFromMemory("__vfx_editor_preview__.wvfx", m_Asset);
	}

	void ParticleEditorPanel::ReleasePreviewResources()
	{
		if (m_PreviewRenderer)
		{
			m_PreviewRenderer->Shutdown();
			m_PreviewRenderer.reset();
		}
		m_PreviewScene.reset();
		m_PreviewEntity = {};
		m_PreviewGpuDevice = nullptr;
		m_CurrentRenderedTexture = {};
	}
	void ParticleEditorPanel::SyncSimulationFromAsset()
	{
		ParticleAssetRegistry::Get().RegisterAssetFromMemory("__vfx_editor_preview__.wvfx", m_Asset);
		EnsurePerEmitterState();
		m_LivePools.clear();
		m_SimTime = 0.0f;

		if (m_PreviewEntity.IsValid() && m_PreviewEntity.HasComponent<VfxComponent>())
		{
			auto& vfx = m_PreviewEntity.GetComponent<VfxComponent>();
			vfx.PlaybackState = VfxPlaybackState::Playing;
			vfx.Effect.Path = StringPool::Get().InternPath("__vfx_editor_preview__.wvfx");
		}


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
		EnsurePreviewResources();

		if (m_LivePools.size() != m_Asset.Emitters.size())
		{
			SyncSimulationFromAsset();
		}

		m_SimTime += dt * m_PlaybackSpeed;
		const float maxDuration = std::max(0.5f, m_TimelineDuration);
		if (m_SimTime > maxDuration)
		{
			// 编辑器视口在到达时间轴末尾后自动平滑回绕，保持粒子始终处于动态演变中
			m_SimTime = std::fmod(m_SimTime, maxDuration);
			if (!m_Asset.Looping)
			{
				// 单次爆发特效回绕时重新激活实体播放，确保视口特效绝不静止卡死
				if (m_PreviewEntity.IsValid() && m_PreviewEntity.HasComponent<VfxComponent>())
				{
					auto& vfx = m_PreviewEntity.GetComponent<VfxComponent>();
					vfx.PlaybackState = VfxPlaybackState::Playing;
				}
			}
		}

		if (m_PreviewScene)
		{
			EditorCamera dummyCam;
			m_PreviewScene->OnUpdateEditor(Timestep(dt * m_PlaybackSpeed), dummyCam);
		}

		for (size_t i = 0; i < m_Asset.Emitters.size() && i < m_LivePools.size(); ++i)
		{
			if (!IsEmitterActive(i)) continue;

			const auto& em = m_Asset.Emitters[i];
			auto& pool = m_LivePools[i];

			ParticleEmitterConfig comp;
			comp.EmissionRate = em.EmissionRate;
			comp.Lifetime = em.Lifetime;
			comp.Speed = em.Speed;
			comp.SpeedVariance = em.SpeedVariance;
			comp.Gravity = em.Gravity;
			comp.SizeStart = em.SizeStart;
			comp.SizeEnd = em.SizeEnd;
			comp.ColorStart = PackColor(em.ColorStart);
			comp.ColorEnd = PackColor(em.ColorEnd);
			comp.MaxParticles = em.MaxParticles;
			comp.EmitterType = em.EmitterType;
			comp.RenderMode = em.FacingMode;
			comp.BlendMode = em.Material.BlendMode;
			comp.LockToXYPlane = em.LockToXY;
			comp.Looping = m_Asset.Looping;
			comp.IsPlaying = true;
			pool->SetMaterialSettings(em.Material);


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
		std::error_code ec;
		if (!fullPath.is_absolute() || !std::filesystem::exists(fullPath, ec))
		{
			fullPath = ResolveVfxDiskPath(path);
		}

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
			m_IsPlaying = true;
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
		if (m_ViewMode == ParticleEditorViewMode::SlangShader)
		{
			const Wui::WuiRect workRect { rect.X + padX, curY, contentW, availH };
			RenderSlangShaderView(ctx, workRect, theme, host);
		}
		else if (m_ViewMode == ParticleEditorViewMode::DataManifest)
		{
			const Wui::WuiRect workRect { rect.X + padX, curY, contentW, availH };
			RenderDataView(ctx, workRect, theme, host);
		}
		else if (m_ViewMode == ParticleEditorViewMode::CustomModule)
		{
			const Wui::WuiRect workRect { rect.X + padX, curY, contentW, availH };
			RenderCustomModuleWorkspace(ctx, workRect, theme, host);
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


		// 右键上下文菜单与下拉菜单 (最顶层绘制，绝不被任何视口或分区遮挡)
		RenderContextMenu(ctx, theme);
		RenderCustomModuleMenu(ctx, theme);
	}


	void ParticleEditorPanel::RenderTopBar(Wui::WuiContext& ctx, const Wui::WuiRect& barRect, const Wui::WuiTheme& theme, PanelHost& host)
	{
		(void)host;
		Wui::BarSurface(ctx, barRect, theme.PanelHeader, theme.Border);

		Wui::Label(ctx, { barRect.X + 10.0f, barRect.Y + 9.0f }, "视觉特效系统 (WorldVFX Studio 2.0)", theme.Accent, 12.5f);

		float tbX = barRect.X + 225.0f;

		const Wui::WuiRect newBtn { tbX, barRect.Y + 6.0f, 44.0f, 24.0f };
		Wui::Tooltip(ctx, newBtn, "新建纯净默认特效资产 (New VFX Asset)");
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
		tbX += 56.0f;

		// -------------------------------------------------------------------------
		// 项目全局代码自定义模块菜单栏入口 (VFX Editor Menu Bar - Custom Slang Modules)
		// -------------------------------------------------------------------------
		const float modMenuBtnW = 152.0f;
		const Wui::WuiRect modMenuBtn { tbX, barRect.Y + 6.0f, modMenuBtnW, 24.0f };
		Wui::Tooltip(ctx, modMenuBtn, "项目全局代码自定义模块菜单，管理和编写全局 Slang 动力学模块 (Scratch Pad)");
		if (Wui::Button(ctx, Wui::HashId("vfx.bar.mod_menu"), modMenuBtn, "代码模块 (Scratch Pad) ▼", theme))
		{
			m_CustomModuleMenuOpen = !m_CustomModuleMenuOpen;
			if (m_CustomModuleMenuOpen)
			{
				m_CustomModuleMenuPos = { modMenuBtn.X, barRect.Y + barRect.H + 2.0f };
				RefreshProjectCustomModules();
			}
		}
		tbX += modMenuBtnW + 8.0f;

		// -------------------------------------------------------------------------
		// 工作区视图模式自由切换 (Visual Stack vs Scratch Pad vs Full Shader vs YAML)
		// -------------------------------------------------------------------------
		const float visualBtnW = 100.0f;
		const Wui::WuiRect visualModeBtn { tbX, barRect.Y + 6.0f, visualBtnW, 24.0f };
		Wui::Tooltip(ctx, visualModeBtn, "切换到 3D 自由视口与 Niagara 级模块化参数堆栈 (Visual Modules Stack)");
		if (Wui::Button(ctx, Wui::HashId("vfx.mode.visual"), visualModeBtn,
			m_ViewMode == ParticleEditorViewMode::Visual ? "[视口与堆栈]" : "  视口与堆栈", theme))
		{
			m_ViewMode = ParticleEditorViewMode::Visual;
		}
		tbX += visualBtnW + 4.0f;

		const float scratchBtnW = 118.0f;
		const Wui::WuiRect scratchModeBtn { tbX, barRect.Y + 6.0f, scratchBtnW, 24.0f };
		Wui::Tooltip(ctx, scratchModeBtn, "打开项目全局代码自定义模块工作区 (Scratch Pad Code Studio)");
		if (Wui::Button(ctx, Wui::HashId("vfx.mode.scratch"), scratchModeBtn,
			m_ViewMode == ParticleEditorViewMode::CustomModule ? "[代码模块编写器]" : "  代码模块编写器", theme))
		{
			m_ViewMode = ParticleEditorViewMode::CustomModule;
			RefreshProjectCustomModules();
			if (m_EditingCustomModule.SlangSource.empty())
			{
				if (!m_ProjectCustomModules.empty())
					OpenExistingCustomModule(0);
				else
					OpenCustomModuleEditor();
			}
		}
		tbX += scratchBtnW + 4.0f;

		const float shaderBtnW = 112.0f;
		const Wui::WuiRect shaderModeBtn { tbX, barRect.Y + 6.0f, shaderBtnW, 24.0f };
		Wui::Tooltip(ctx, shaderModeBtn, "切换到统一 Slang 计算着色器源码视图，支持全屏编码与即时热重载 (Ctrl+Enter)");
		if (Wui::Button(ctx, Wui::HashId("vfx.mode.shader"), shaderModeBtn,
			m_ViewMode == ParticleEditorViewMode::SlangShader ? "[统一 Slang]" : "  统一 Slang", theme))
		{
			m_ViewMode = ParticleEditorViewMode::SlangShader;
			if (m_SlangShaderBuffer.empty())
			{
				SyncSlangShaderFromModules();
			}
		}
		tbX += shaderBtnW + 4.0f;

		const float dataBtnW = 96.0f;
		const Wui::WuiRect dataModeBtn { tbX, barRect.Y + 6.0f, dataBtnW, 24.0f };
		Wui::Tooltip(ctx, dataModeBtn, "切换到底层资产声明式清单 (YAML Data Manifest)");
		if (Wui::Button(ctx, Wui::HashId("vfx.mode.data"), dataModeBtn,
			m_ViewMode == ParticleEditorViewMode::DataManifest ? "[资产(YAML)]" : "  资产(YAML)", theme))
		{
			m_ViewMode = ParticleEditorViewMode::DataManifest;
			SyncCodeFromAsset();
		}
		tbX += dataBtnW + 12.0f;

		// 资产名称与状态反馈
		if (!m_StatusFeedback.empty())
		{
			Wui::WuiColor fbColor = (m_StatusFeedback.find("成功") != std::string::npos || m_StatusFeedback.find("已") != std::string::npos)
				? Wui::WuiColor { 0.3f, 0.85f, 0.4f, 1.0f }
				: (m_StatusFeedback.find("失败") != std::string::npos || m_StatusFeedback.find("错误") != std::string::npos)
					? Wui::WuiColor { 0.95f, 0.3f, 0.3f, 1.0f }
					: theme.TextMuted;
			Wui::Label(ctx, { tbX + 8.0f, barRect.Y + 10.0f }, m_StatusFeedback, fbColor, 10.5f);
		}


	}

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
		std::string durStr = std::to_string(m_TimelineDuration).substr(0, 3) + "s" + (m_Asset.Looping ? " [循环]" : "");
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

	void ParticleEditorPanel::RenderCustomModuleMenu(Wui::WuiContext& ctx, const Wui::WuiTheme& theme)
	{
		if (!m_CustomModuleMenuOpen) return;

		const float menuW = 270.0f;
		const float itemH = 24.0f;
		const float menuH = 68.0f + itemH * (std::max<size_t>(1, m_ProjectCustomModules.size()) + 1);
		const Wui::WuiRect menuRect { m_CustomModuleMenuPos.x, m_CustomModuleMenuPos.y, menuW, menuH };
		Wui::BarSurface(ctx, menuRect, Wui::WuiColor { 0.12f, 0.14f, 0.18f, 0.98f }, theme.Accent);

		float my = menuRect.Y + 6.0f;

		const Wui::WuiRect newModItem { menuRect.X + 4.0f, my, menuRect.W - 8.0f, 22.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.bar.mod.new"), newModItem, "+ 新建项目代码模块 (Scratch Pad)", theme))
		{
			m_CustomModuleMenuOpen = false;
			OpenCustomModuleEditor("新自定义动力学块 (Custom Slang)");
			m_ViewMode = ParticleEditorViewMode::CustomModule;
		}
		my += 26.0f;

		const Wui::WuiRect manageItem { menuRect.X + 4.0f, my, menuRect.W - 8.0f, 22.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.bar.mod.manage"), manageItem, "管理全部项目代码模块...", theme))
		{
			m_CustomModuleMenuOpen = false;
			m_ViewMode = ParticleEditorViewMode::CustomModule;
		}
		my += 28.0f;

		std::string modLabel = "--- 项目全局模块 (" + std::to_string(m_ProjectCustomModules.size()) + ") ---";
		Wui::Label(ctx, { menuRect.X + 8.0f, my + 3.0f }, modLabel, theme.Accent, 9.5f);
		my += 20.0f;

		if (m_ProjectCustomModules.empty())
		{
			Wui::Label(ctx, { menuRect.X + 12.0f, my + 3.0f }, "(暂无项目模块，点击上方新建)", theme.TextMuted, 9.5f);
		}
		else
		{
			for (size_t i = 0; i < m_ProjectCustomModules.size(); ++i)
			{
				const auto& mod = m_ProjectCustomModules[i];
				const Wui::WuiRect modItem { menuRect.X + 4.0f, my, menuRect.W - 8.0f, 22.0f };
				std::string btnText = "[★] " + mod.Name;
				if (Wui::Button(ctx, Wui::HashId(("vfx.bar.mod_sel_" + std::to_string(i)).c_str()), modItem, btnText.c_str(), theme))
				{
					m_CustomModuleMenuOpen = false;
					OpenExistingCustomModule(i);
					m_ViewMode = ParticleEditorViewMode::CustomModule;
				}
				my += 24.0f;
			}
		}

		// 点击菜单外部以及触发按钮外部时关闭
		const Wui::WuiRect triggerBtnRect { m_CustomModuleMenuPos.x, m_CustomModuleMenuPos.y - 30.0f, 160.0f, 28.0f };
		if (ctx.Input().MouseClicked[0] && !ctx.IsHovered(menuRect) && !ctx.IsHovered(triggerBtnRect))
		{
			m_CustomModuleMenuOpen = false;
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
		const glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 1000.0f);
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

			const VfxModule* activeSpawnMod = nullptr;
			for (const auto& mod : curEm.Modules)
			{
				if (mod.Enabled && (mod.Id == "mod.spawn.cone" || mod.Id == "mod.spawn.sphere" || mod.Id == "mod.spawn.box" || mod.Id == "mod.spawn.point"))
				{
					activeSpawnMod = &mod;
					break;
				}
			}

			if (activeSpawnMod)
			{
				if (activeSpawnMod->Id == "mod.spawn.cone")
				{
					float cRadius = activeSpawnMod->GetFloat("ConeRadius", 0.5f);
					float cAngle = activeSpawnMod->GetFloat("ConeAngle", 30.0f);
					const float rBase = std::max(0.01f, cRadius);
					const float h = 1.5f;
					const float halfAngleRad = glm::radians(std::clamp(cAngle * 0.5f, 0.0f, 85.0f));
					const float rTop = rBase + h * std::tan(halfAngleRad);

					char buf[64];
					std::snprintf(buf, sizeof(buf), "圆锥底面半径: %.2fm (拖拽调整)", rBase);
					handles.push_back({
						static_cast<int>(ParticleGizmoHandleId::ConeBaseRadius),
						glm::vec3(rBase, 0.0f, 0.0f),
						glm::vec3(1.0f, 0.0f, 0.0f),
						rBase,
						Wui::WuiColor { 1.0f, 0.85f, 0.20f, 1.0f },
						buf,
						"R",
						0.01f, 20.0f, false
					});

					std::snprintf(buf, sizeof(buf), "圆锥张角: %.1f度 (拖拽调整)", cAngle);
					handles.push_back({
						static_cast<int>(ParticleGizmoHandleId::ConeTopAngle),
						glm::vec3(rTop, h, 0.0f),
						glm::vec3(1.0f, 0.0f, 0.0f),
						cAngle,
						Wui::WuiColor { 0.25f, 0.85f, 1.0f, 1.0f },
						buf,
						"A",
						0.0f, 120.0f, true
					});
				}
				else if (activeSpawnMod->Id == "mod.spawn.sphere")
				{
					float spR = std::max(0.05f, activeSpawnMod->GetFloat("SphereRadius", 1.0f));
					char buf[64];
					std::snprintf(buf, sizeof(buf), "球体生成半径: %.2fm (拖拽调整)", spR);
					handles.push_back({
						static_cast<int>(ParticleGizmoHandleId::SphereRadiusX),
						glm::vec3(spR, 0.0f, 0.0f),
						glm::vec3(1.0f, 0.0f, 0.0f),
						spR,
						Wui::WuiColor { 0.25f, 0.85f, 1.0f, 1.0f },
						buf,
						"R",
						0.05f, 30.0f, false
					});
				}
				else if (activeSpawnMod->Id == "mod.spawn.box")
				{
					glm::vec3 e(1.0f);
					const auto* pB = activeSpawnMod->FindParam("BoxExtents");
					if (pB) e = glm::max(glm::vec3(pB->VecVal), glm::vec3(0.05f));
					handles.push_back({
						static_cast<int>(ParticleGizmoHandleId::BoxExtentPosX),
						glm::vec3(e.x, 0.0f, 0.0f),
						glm::vec3(1.0f, 0.0f, 0.0f),
						e.x,
						Wui::WuiColor { 1.0f, 0.35f, 0.35f, 1.0f },
						"包围盒宽度 X (拖拽调整)",
						"X",
						0.05f, 30.0f, false
					});
				}
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

				if (activeItem && m_SelectedEmitterIndex < m_Asset.Emitters.size())
				{
					auto& curEm = m_Asset.Emitters[m_SelectedEmitterIndex];
					VfxModule* activeSpawnMod = nullptr;
					for (auto& mod : curEm.Modules)
					{
						if (mod.Enabled && (mod.Id == "mod.spawn.cone" || mod.Id == "mod.spawn.sphere" || mod.Id == "mod.spawn.box"))
						{
							activeSpawnMod = &mod;
							break;
						}
					}

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

					if (activeSpawnMod)
					{
						if (activeItem->IsAngle)
						{
							const float deltaAngle = proj * 0.35f;
							float newAng = std::clamp(m_GizmoDragInitialParam + deltaAngle, activeItem->MinVal, activeItem->MaxVal);
							activeSpawnMod->SetFloat("ConeAngle", newAng);
						}
						else
						{
							const float deltaDist = proj * (m_CameraDistance * 0.0025f);
							const float newVal = std::clamp(m_GizmoDragInitialParam + deltaDist, activeItem->MinVal, activeItem->MaxVal);
							switch (static_cast<ParticleGizmoHandleId>(activeItem->Id))
							{
							case ParticleGizmoHandleId::ConeBaseRadius:
								activeSpawnMod->SetFloat("ConeRadius", newVal);
								break;
							case ParticleGizmoHandleId::SphereRadiusX:
							case ParticleGizmoHandleId::SphereRadiusY:
							case ParticleGizmoHandleId::SphereRadiusZ:
								activeSpawnMod->SetFloat("SphereRadius", newVal);
								break;
							case ParticleGizmoHandleId::BoxExtentPosX:
								if (auto* pB = activeSpawnMod->FindParam("BoxExtents"))
									pB->VecVal.x = newVal;
								break;
							default:
								break;
							}
						}
						m_Dirty = true;
						SyncSimulationFromAsset();
					}
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
		// =========================================================================
		// 6. 3D 原点坐标指示 (地面参考网格已由 GPU SceneRenderer 原生渲染)
		// =========================================================================
		const Wui::WuiColor axisXCol { 0.95f, 0.22f, 0.22f, 0.95f };
		const Wui::WuiColor axisYCol { 0.22f, 0.90f, 0.32f, 0.95f };
		const Wui::WuiColor axisZCol { 0.22f, 0.55f, 0.98f, 0.95f };

		drawProjectedSegment(glm::vec3(0.0f), glm::vec3(1.2f, 0.0f, 0.0f), axisXCol, 2.0f);
		drawProjectedSegment(glm::vec3(0.0f), glm::vec3(0.0f, 1.2f, 0.0f), axisYCol, 2.0f);
		drawProjectedSegment(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.2f), axisZCol, 2.0f);
		
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

		// 3. GPU 原生 3D SceneRenderer 渲染视口画面 (True Hardware Scene Rendering)
		EnsurePreviewResources();
		if (m_PreviewRenderer && m_PreviewScene)
		{
			const uint32_t targetW = std::max(1u, static_cast<uint32_t>(vpRect.W));
			const uint32_t targetH = std::max(1u, static_cast<uint32_t>(vpRect.H));
			m_PreviewRenderer->OnResize(targetW, targetH);

			SceneRendererOptions options;
			options.ShowGrid = true;
			Camera previewCam(proj);
			m_PreviewRenderer->BeginScene(m_PreviewScene.get(), options);
			m_PreviewRenderer->SubmitScene(previewCam, glm::inverse(view));
			m_PreviewRenderer->EndScene();

			const Rhi::Handle<Rhi::Texture>& color = m_PreviewRenderer->GetColorTexture();
			if (color)
			{
				Wui::WuiTextureRegistry& registry = Wui::WuiTextureRegistry::Get();
				if (m_PreviewTextureId == 0)
				{
					m_PreviewTextureId = registry.Register(color);
					m_CurrentRenderedTexture = color;
				}
				else if (m_CurrentRenderedTexture != color)
				{
					registry.Update(m_PreviewTextureId, color);
					m_CurrentRenderedTexture = color;
				}
			}

			if (m_PreviewTextureId != 0)
			{
				Wui::Image(ctx, vpRect, m_PreviewTextureId, ViewChrome::kPreviewImageUv, theme);
			}
		}

		// 热力图分析模式调试覆盖层
		if (m_HeatmapMode)
		{
			for (const auto& item : drawItems)
			{
				glm::vec2 pScreen;
				if (projectPoint(item.Position, pScreen))
				{
					const glm::vec4 clip = vpMat * glm::vec4(item.Position, 1.0f);
					const float pSize = std::clamp(item.Size.x * 320.0f / clip.w, 3.0f, 32.0f);
					const Wui::WuiRect pRect { pScreen.x - pSize * 0.5f, pScreen.y - pSize * 0.5f, pSize, pSize };
					Wui::BarSurface(ctx, pRect, { 1.0f, 0.15f, 0.15f, 0.22f }, { 1.0f, 0.15f, 0.15f, 0.22f });
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

			const VfxModule* activeSpawnMod = nullptr;
			for (const auto& mod : curEm.Modules)
			{
				if (mod.Enabled && (mod.Id == "mod.spawn.cone" || mod.Id == "mod.spawn.sphere" || mod.Id == "mod.spawn.box" || mod.Id == "mod.spawn.point"))
				{
					activeSpawnMod = &mod;
					break;
				}
			}

			if (activeSpawnMod)
			{
				if (activeSpawnMod->Id == "mod.spawn.cone")
				{
					const float rBase = std::max(0.01f, activeSpawnMod->GetFloat("ConeRadius", 0.5f));
					const float cAngle = activeSpawnMod->GetFloat("ConeAngle", 30.0f);
					const float h = 1.5f;
					const float halfAngleRad = glm::radians(std::clamp(cAngle * 0.5f, 0.0f, 85.0f));
					const float rTop = rBase + h * std::tan(halfAngleRad);

					drawProjectedCircle(glm::vec3(0.0f, 0.0f, 0.0f), rBase, glm::vec3(0.0f, 1.0f, 0.0f), gizmoWireCol, 1.5f, 24);
					drawProjectedCircle(glm::vec3(0.0f, h, 0.0f), rTop, glm::vec3(0.0f, 1.0f, 0.0f), gizmoWireCol, 1.5f, 24);

					drawProjectedSegment(glm::vec3(rBase, 0.0f, 0.0f), glm::vec3(rTop, h, 0.0f), gizmoWireCol, 1.2f);
					drawProjectedSegment(glm::vec3(-rBase, 0.0f, 0.0f), glm::vec3(-rTop, h, 0.0f), gizmoWireCol, 1.2f);
					drawProjectedSegment(glm::vec3(0.0f, 0.0f, rBase), glm::vec3(0.0f, h, rTop), gizmoWireCol, 1.2f);
					drawProjectedSegment(glm::vec3(0.0f, 0.0f, -rBase), glm::vec3(0.0f, h, -rTop), gizmoWireCol, 1.2f);
				}
				else if (activeSpawnMod->Id == "mod.spawn.sphere")
				{
					const float spR = std::max(0.05f, activeSpawnMod->GetFloat("SphereRadius", 1.0f));
					drawProjectedCircle(glm::vec3(0.0f), spR, glm::vec3(0.0f, 1.0f, 0.0f), gizmoWireCol, 1.4f, 24);
					drawProjectedCircle(glm::vec3(0.0f), spR, glm::vec3(0.0f, 0.0f, 1.0f), gizmoWireCol, 1.4f, 24);
					drawProjectedCircle(glm::vec3(0.0f), spR, glm::vec3(1.0f, 0.0f, 0.0f), gizmoWireCol, 1.4f, 24);
				}
				else if (activeSpawnMod->Id == "mod.spawn.box")
				{
					glm::vec3 e(1.0f);
					const auto* pB = activeSpawnMod->FindParam("BoxExtents");
					if (pB) e = glm::max(glm::vec3(pB->VecVal), glm::vec3(0.05f));

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
				else if (activeSpawnMod->Id == "mod.spawn.point")
				{
					const float pSize = 0.25f;
					drawProjectedSegment(glm::vec3(-pSize, 0.0f, 0.0f), glm::vec3(pSize, 0.0f, 0.0f), gizmoWireCol, 1.5f);
					drawProjectedSegment(glm::vec3(0.0f, -pSize, 0.0f), glm::vec3(0.0f, pSize, 0.0f), gizmoWireCol, 1.5f);
					drawProjectedSegment(glm::vec3(0.0f, 0.0f, -pSize), glm::vec3(0.0f, 0.0f, pSize), gizmoWireCol, 1.5f);
				}
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


		// =========================================================================
	// 项目全局自定义 Slang 模块与设计器实现 (Niagara Scratch Pad Studio)
	// =========================================================================

	void ParticleEditorPanel::RefreshProjectCustomModules()
	{
		m_ProjectCustomModules = VfxModuleLibrary::LoadProjectCustomModules();
	}

	void ParticleEditorPanel::OpenExistingCustomModule(size_t index)
	{
		if (index >= m_ProjectCustomModules.size()) return;
		m_SelectedProjectModuleIndex = static_cast<int>(index);
		m_EditingCustomModule = m_ProjectCustomModules[index];
		m_CustomModuleCompileError.clear();
		m_CustomModuleCompileSuccess = false;
		m_NewParamNameBuffer = "MyParam";
		m_NewParamTypeIndex = 0;
		m_ViewMode = ParticleEditorViewMode::CustomModule;
	}

	void ParticleEditorPanel::OpenCustomModuleEditor(const std::string& defaultName)
	{
		m_SelectedProjectModuleIndex = -1;
		m_CustomModuleCompileError.clear();
		m_CustomModuleCompileSuccess = false;
		m_EditingCustomModule = VfxModule();
		m_EditingCustomModule.Id = "mod.custom." + std::to_string(rand() % 1000000);
		m_EditingCustomModule.Name = defaultName.empty() ? "新自定义动力学块 (Custom Slang)" : defaultName;
		m_EditingCustomModule.Description = "用户编写的项目全局 Slang 计算代码块，可在全项目任意特效中组装";
		m_EditingCustomModule.Stage = VfxModuleStage::Update;
		m_EditingCustomModule.Enabled = true;
		m_EditingCustomModule.IsCustom = true;

		m_EditingCustomModule.Parameters = {
			{ "SpeedGain", VfxParamType::Float, 2.0f, {}, 0, false, 0.0f, 50.0f, 0.1f, "自定义增益乘子", true, "SpeedGain" },
			{ "WaveFrequency", VfxParamType::Float, 1.5f, {}, 0, false, 0.1f, 20.0f, 0.1f, "波动频率", true, "WaveFrequency" }
		};

		m_EditingCustomModule.SlangSource = "// 自定义 Slang 计算代码 (Project-Global Custom Module)\n// 可用内置字段: p.Position, p.Velocity, p.Color, p.Size, p.Life, dt\nfloat dist = length(p.Position);\np.Velocity += cross(float3(0, 1, 0), p.Position) * (u_SpeedGain * dt);\np.Position.y += sin(p.Life * u_WaveFrequency) * (0.5f * dt);\n";
		m_NewParamNameBuffer = "Param" + std::to_string(m_EditingCustomModule.Parameters.size() + 1);
		m_NewParamTypeIndex = 0;
		m_ViewMode = ParticleEditorViewMode::CustomModule;
	}

	void ParticleEditorPanel::RenderCustomModuleWorkspace(Wui::WuiContext& ctx, const Wui::WuiRect& workRect, const Wui::WuiTheme& theme, PanelHost& host)
	{
		(void)host;
		Wui::BarSurface(ctx, workRect, theme.PanelBg, theme.Border);

		// =========================================================================
		// 1. 左侧栏: 项目全局模块库列表 (260px)
		// =========================================================================
		const float leftW = 260.0f;
		const Wui::WuiRect leftRect { workRect.X, workRect.Y, leftW, workRect.H };
		Wui::BarSurface(ctx, leftRect, theme.PanelBg, theme.Border);

		// 标头
		const Wui::WuiRect lHead { leftRect.X, leftRect.Y, leftRect.W, 28.0f };
		Wui::BarSurface(ctx, lHead, theme.PanelHeader, theme.Border);
		Wui::Label(ctx, { lHead.X + 8.0f, lHead.Y + 7.0f }, "项目全局模块库 (Project Modules)", theme.Accent, 11.0f);

		// 操作工具栏
		float lty = leftRect.Y + 34.0f;
		const Wui::WuiRect newModBtn { leftRect.X + 8.0f, lty, 110.0f, 22.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.sc.l_new"), newModBtn, "+ 新建代码模块", theme))
		{
			OpenCustomModuleEditor("新自定义动力学块 (Custom Slang)");
		}

		const Wui::WuiRect refreshBtn { leftRect.X + 124.0f, lty, 78.0f, 22.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.sc.l_refresh"), refreshBtn, "刷新磁盘", theme))
		{
			RefreshProjectCustomModules();
		}
		lty += 28.0f;

		// 列表滚动区
		const Wui::WuiRect listRect { leftRect.X + 6.0f, lty, leftRect.W - 12.0f, leftRect.H - (lty - leftRect.Y) - 8.0f };
		float totalListH = std::max(listRect.H, static_cast<float>(m_ProjectCustomModules.size() * 36.0f + 20.0f));
		Wui::BeginScrollArea(ctx, listRect, totalListH, m_CustomModuleListScrollY, theme, Wui::HashId("vfx.sc.list_scroll"));
		float ly = listRect.Y - m_CustomModuleListScrollY;

		if (m_ProjectCustomModules.empty())
		{
			Wui::Label(ctx, { listRect.X + 8.0f, ly + 14.0f }, "当前项目无自定义模块\n点击上方 [+ 新建代码模块] 创建", theme.TextMuted, 10.0f);
		}
		else
		{
			for (size_t i = 0; i < m_ProjectCustomModules.size(); ++i)
			{
				const auto& mod = m_ProjectCustomModules[i];
				const Wui::WuiRect itemRect { listRect.X, ly, listRect.W, 30.0f };
				bool isSel = (m_SelectedProjectModuleIndex == static_cast<int>(i));
				Wui::BarSurface(ctx, itemRect, isSel ? theme.Selection : Wui::WuiColor { 0.12f, 0.14f, 0.18f, 0.8f }, isSel ? theme.Accent : theme.Border);

				bool isSpawn = (mod.Stage == VfxModuleStage::Spawn);
				Wui::Label(ctx, { itemRect.X + 4.0f, itemRect.Y + 7.0f }, isSpawn ? "[Spawn]" : "[Update]", isSpawn ? Wui::WuiColor { 0.95f, 0.72f, 0.25f, 1.0f } : theme.Accent, 9.5f);

				const Wui::WuiRect selBtn { itemRect.X + 54.0f, itemRect.Y + 4.0f, itemRect.W - 80.0f, 22.0f };
				if (Wui::Button(ctx, Wui::HashId(("vfx.sc.mod_item_" + std::to_string(i)).c_str()), selBtn, mod.Name.c_str(), theme))
				{
					OpenExistingCustomModule(i);
				}

				const Wui::WuiRect delBtn { itemRect.X + itemRect.W - 22.0f, itemRect.Y + 5.0f, 18.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(("vfx.sc.mod_del_" + std::to_string(i)).c_str()), delBtn, "X", theme))
				{
					VfxModuleLibrary::DeleteProjectCustomModule(mod.Name);
					RefreshProjectCustomModules();
					if (m_SelectedProjectModuleIndex == static_cast<int>(i))
						m_SelectedProjectModuleIndex = -1;
					break;
				}
				ly += 34.0f;
			}
		}
		Wui::EndScrollArea(ctx);

		// =========================================================================
		// 2. 右侧工作区: 模块源码设计器与即时编译器 (Scratch Pad Studio)
		// =========================================================================
		const float rightX = leftRect.X + leftW + 4.0f;
		const float rightW = workRect.W - leftW - 4.0f;
		const Wui::WuiRect rightRect { rightX, workRect.Y, rightW, workRect.H };
		Wui::BarSurface(ctx, rightRect, theme.PanelBg, theme.Border);

		// 右侧顶部操作栏
		const Wui::WuiRect rHead { rightRect.X, rightRect.Y, rightRect.W, 36.0f };
		Wui::BarSurface(ctx, rHead, theme.PanelHeader, theme.Border);
		std::string titleStr = "Slang 模块设计器 (Scratch Pad) - " + m_EditingCustomModule.Name;
		Wui::Label(ctx, { rHead.X + 12.0f, rHead.Y + 10.0f }, titleStr, theme.Accent, 12.0f);

		float rbx = rHead.X + rHead.W - 12.0f;

		// 返回按钮
		rbx -= 100.0f;
		const Wui::WuiRect backBtn { rbx, rHead.Y + 6.0f, 96.0f, 24.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.sc.r_back"), backBtn, "返回视口与堆栈", theme))
		{
			m_ViewMode = ParticleEditorViewMode::Visual;
		}

		// 装配到发射器按钮
		rbx -= 150.0f;
		const Wui::WuiRect addEmBtn { rbx, rHead.Y + 6.0f, 144.0f, 24.0f };
		Wui::Tooltip(ctx, addEmBtn, "将当前自定义 Slang 模块装配至选中的特效发射器");
		if (Wui::Button(ctx, Wui::HashId("vfx.sc.r_add_em"), addEmBtn, "[+ 装配至当前发射器]", theme))
		{
			AddModuleToSelectedEmitter(m_EditingCustomModule);
			SyncSimulationFromAsset();
			m_ViewMode = ParticleEditorViewMode::Visual;
			m_StatusFeedback = "模块 [" + m_EditingCustomModule.Name + "] 已成功装配至发射器！";
		}

		// 编译并保存为项目全局模块按钮
		rbx -= 208.0f;
		const Wui::WuiRect saveModBtn { rbx, rHead.Y + 6.0f, 202.0f, 24.0f };
		Wui::Tooltip(ctx, saveModBtn, "验证并编译 Slang 源码，通过后将模块持久化保存至 assets/vfx/modules/ 供全项目任何特效使用");
		if (Wui::Button(ctx, Wui::HashId("vfx.sc.r_save_mod"), saveModBtn, "[编译并保存为项目全局模块]", theme))
		{
			std::string testShader = VfxComputeGenerator::GenerateComputeShaderSource(
				"CustomTest", { m_EditingCustomModule }, {}, {}
			);
			std::string err;
			bool ok = VfxComputeGenerator::ValidateAndCompile(testShader, &err);
			if (ok)
			{
				m_CustomModuleCompileSuccess = true;
				m_CustomModuleCompileError.clear();
				VfxModuleLibrary::SaveProjectCustomModule(m_EditingCustomModule);
				RefreshProjectCustomModules();
				m_StatusFeedback = "项目模块 [" + m_EditingCustomModule.Name + "] 已成功编译并持久化至 assets/vfx/modules/！";
			}
			else
			{
				m_CustomModuleCompileSuccess = false;
				m_CustomModuleCompileError = err.empty() ? "语法校验未通过" : err;
			}
		}

		// 模块详细设计编辑区
		const Wui::WuiRect editorContentRect { rightRect.X + 12.0f, rightRect.Y + 44.0f, rightRect.W - 24.0f, rightRect.H - 52.0f };
		float totalEditH = 760.0f;
		Wui::BeginScrollArea(ctx, editorContentRect, totalEditH, m_CustomModuleEditorScrollY, theme, Wui::HashId("vfx.sc.editor_scroll"));
		float ry = editorContentRect.Y - m_CustomModuleEditorScrollY;

		const float labelW = 80.0f;
		const float rowValX = editorContentRect.X + labelW + 10.0f;
		const float rowValW = std::min(500.0f, editorContentRect.W - labelW - 20.0f);

		// 模块名称
		Wui::Label(ctx, { editorContentRect.X, ry + 3.0f }, "模块名称:", theme.TextMuted, 10.5f);
		std::string modName = m_EditingCustomModule.Name;
		if (Wui::TextField(ctx, Wui::HashId("vfx.sc.name"), { rowValX, ry, rowValW, 20.0f }, modName, theme))
		{
			m_EditingCustomModule.Name = modName;
		}
		ry += 26.0f;

		// 执行阶段
		Wui::Label(ctx, { editorContentRect.X, ry + 3.0f }, "执行阶段:", theme.TextMuted, 10.5f);
		static const std::vector<std::string> s_StageOpts = { "Update (粒子动力学更新阶段)", "Spawn (粒子出生初始化阶段)" };
		int stageIdx = (m_EditingCustomModule.Stage == VfxModuleStage::Spawn) ? 1 : 0;
		if (Wui::Combo(ctx, Wui::HashId("vfx.sc.stage"), { rowValX, ry, rowValW, 20.0f }, "", s_StageOpts, stageIdx, theme))
		{
			m_EditingCustomModule.Stage = (stageIdx == 1) ? VfxModuleStage::Spawn : VfxModuleStage::Update;
		}
		ry += 26.0f;

		// 模块描述
		Wui::Label(ctx, { editorContentRect.X, ry + 3.0f }, "模块描述:", theme.TextMuted, 10.5f);
		std::string modDesc = m_EditingCustomModule.Description;
		if (Wui::TextField(ctx, Wui::HashId("vfx.sc.desc"), { rowValX, ry, rowValW, 20.0f }, modDesc, theme))
		{
			m_EditingCustomModule.Description = modDesc;
		}
		ry += 32.0f;

		// 参数列表
		Wui::Label(ctx, { editorContentRect.X, ry + 3.0f }, "输入参数列表 (Dynamic Uniform Parameters):", theme.Accent, 11.0f);
		ry += 24.0f;

		for (size_t pIdx = 0; pIdx < m_EditingCustomModule.Parameters.size(); ++pIdx)
		{
			auto& p = m_EditingCustomModule.Parameters[pIdx];
			const Wui::WuiRect pRow { editorContentRect.X, ry, editorContentRect.W, 24.0f };
			Wui::BarSurface(ctx, pRow, Wui::WuiColor { 0.10f, 0.12f, 0.16f, 0.9f }, theme.Border);

			std::string typeStr = (p.Type == VfxParamType::Float) ? "[Float]" : ((p.Type == VfxParamType::Vec3) ? "[Vec3]" : "[Color]");
			Wui::Label(ctx, { pRow.X + 8.0f, pRow.Y + 4.0f }, typeStr + "  " + p.Name + " (绑定: " + p.GetUniformName() + ")", theme.Text, 10.0f);

			const Wui::WuiRect delPBtn { pRow.X + pRow.W - 24.0f, pRow.Y + 2.0f, 20.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId((std::string("vfx.sc.delp_") + std::to_string(pIdx)).c_str()), delPBtn, "X", theme))
			{
				m_EditingCustomModule.Parameters.erase(m_EditingCustomModule.Parameters.begin() + pIdx);
				break;
			}
			ry += 28.0f;
		}

		// 添加新参数行
		const Wui::WuiRect addParamBox { editorContentRect.X, ry, editorContentRect.W, 28.0f };
		Wui::BarSurface(ctx, addParamBox, Wui::WuiColor { 0.08f, 0.09f, 0.12f, 0.9f }, theme.Border);
		float aX = addParamBox.X + 6.0f;
		Wui::Label(ctx, { aX, addParamBox.Y + 6.0f }, "+ 新参数:", theme.TextMuted, 10.0f);
		aX += 60.0f;

		const Wui::WuiRect pNameRect { aX, addParamBox.Y + 4.0f, 130.0f, 20.0f };
		Wui::TextField(ctx, Wui::HashId("vfx.sc.newp_name"), pNameRect, m_NewParamNameBuffer, theme);
		aX += 136.0f;

		static const std::vector<std::string> s_ParamTypeOpts = { "Float", "Vec3", "Color" };
		const Wui::WuiRect pTypeRect { aX, addParamBox.Y + 4.0f, 90.0f, 20.0f };
		Wui::Combo(ctx, Wui::HashId("vfx.sc.newp_type"), pTypeRect, "", s_ParamTypeOpts, m_NewParamTypeIndex, theme);
		aX += 96.0f;

		const Wui::WuiRect addPBtn { aX, addParamBox.Y + 4.0f, 74.0f, 20.0f };
		if (Wui::Button(ctx, Wui::HashId("vfx.sc.addp_btn"), addPBtn, "+ 添加参数", theme))
		{
			if (!m_NewParamNameBuffer.empty())
			{
				VfxModuleParam newP;
				newP.Name = m_NewParamNameBuffer;
				newP.Type = (m_NewParamTypeIndex == 0) ? VfxParamType::Float : ((m_NewParamTypeIndex == 1) ? VfxParamType::Vec3 : VfxParamType::Vec4);
				newP.FloatVal = 1.0f;
				newP.VecVal = glm::vec4(1.0f);
				newP.IsExposed = true;
				newP.ExposedName = newP.Name;
				m_EditingCustomModule.Parameters.push_back(newP);
				m_NewParamNameBuffer = "Param" + std::to_string(m_EditingCustomModule.Parameters.size() + 1);
			}
		}
		ry += 38.0f;

		// Slang 源码编辑区
		Wui::Label(ctx, { editorContentRect.X, ry + 3.0f }, "Slang 计算着色器源码 (Compute Code):", theme.Accent, 11.0f);
		ry += 22.0f;

		const Wui::WuiRect hintRect { editorContentRect.X, ry, editorContentRect.W, 20.0f };
		Wui::Label(ctx, { hintRect.X + 4.0f, hintRect.Y + 3.0f }, "可用内置粒子字段: p.Position, p.Velocity, p.Color, p.Size, p.Life, dt | 辅助函数: Hash1D, Noise3D, CurlNoise3D | 参数: u_<参数名>", theme.TextMuted, 9.5f);
		ry += 24.0f;

		const Wui::WuiRect codeBox { editorContentRect.X, ry, editorContentRect.W, 240.0f };
		Wui::BarSurface(ctx, codeBox, Wui::WuiColor { 0.05f, 0.06f, 0.08f, 1.0f }, theme.Border);
		Wui::TextField(ctx, Wui::HashId("vfx.sc.code_editor"), codeBox, m_EditingCustomModule.SlangSource, theme);
		ry += 246.0f;

		// 编译状态提示
		if (!m_CustomModuleCompileError.empty())
		{
			const Wui::WuiRect errRect { editorContentRect.X, ry, editorContentRect.W, 28.0f };
			Wui::BarSurface(ctx, errRect, Wui::WuiColor { 0.35f, 0.12f, 0.12f, 0.95f }, theme.Warning);
			Wui::Label(ctx, { errRect.X + 8.0f, errRect.Y + 6.0f }, "[Slang 语法/编译错误] " + m_CustomModuleCompileError.substr(0, 100), theme.Warning, 9.5f);
			ry += 34.0f;
		}
		else if (m_CustomModuleCompileSuccess)
		{
			const Wui::WuiRect okRect { editorContentRect.X, ry, editorContentRect.W, 28.0f };
			Wui::BarSurface(ctx, okRect, Wui::WuiColor { 0.12f, 0.30f, 0.18f, 0.95f }, theme.Accent);
			Wui::Label(ctx, { okRect.X + 8.0f, okRect.Y + 6.0f }, "[编译成功] 模块已通过 Slang 编译器校验，保存为项目全局模块并在全项目可用！", theme.Accent, 10.0f);
			ry += 34.0f;
		}

		Wui::EndScrollArea(ctx);
	}

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
		
		em.EnsureDefaultModules();

		// -------------------------------------------------------------
		// 1. 发射器基础配置 (Base Emitter Properties)
		// -------------------------------------------------------------
		Wui::Label(ctx, { inspectorRect.X + 12.0f, cy + 4.0f }, "发射器基础属性 (Emitter Base)", theme.Accent, 11.0f);
		cy += 24.0f;

		// 最大粒子数
		Wui::Label(ctx, { inspectorRect.X + 12.0f, cy + 3.0f }, "最大粒子数:", theme.TextMuted, 10.0f);
		float maxP = static_cast<float>(em.MaxParticles);
		const Wui::WuiRect mpRect { inspectorRect.X + 110.0f, cy, inspectorRect.W - 130.0f, 20.0f };
		if (Wui::DragFloat(ctx, Wui::HashId("vfx.em.maxp"), mpRect, maxP, 10.0f, 10.0f, 100000.0f, theme))
		{
			em.MaxParticles = static_cast<uint32_t>(maxP);
			m_Dirty = true;
			SyncSimulationFromAsset();
		}
		cy += 24.0f;

		// 生命周期
		Wui::Label(ctx, { inspectorRect.X + 12.0f, cy + 3.0f }, "基准寿命 (秒):", theme.TextMuted, 10.0f);
		const Wui::WuiRect lifeRect { inspectorRect.X + 110.0f, cy, inspectorRect.W - 130.0f, 20.0f };
		if (Wui::DragFloat(ctx, Wui::HashId("vfx.em.life"), lifeRect, em.Lifetime, 0.05f, 0.05f, 60.0f, theme))
		{
			m_Dirty = true;
			SyncSimulationFromAsset();
		}
		cy += 24.0f;

		// 发射速率
		Wui::Label(ctx, { inspectorRect.X + 12.0f, cy + 3.0f }, "发射速率 (/s):", theme.TextMuted, 10.0f);
		const Wui::WuiRect rateRect { inspectorRect.X + 110.0f, cy, inspectorRect.W - 130.0f, 20.0f };
		if (Wui::DragFloat(ctx, Wui::HashId("vfx.em.rate"), rateRect, em.EmissionRate, 1.0f, 0.0f, 20000.0f, theme))
		{
			m_Dirty = true;
			SyncSimulationFromAsset();
		}
		cy += 24.0f;

		// 混合模式 (需求 1: 全量采用下拉列表枚举)
		Wui::Label(ctx, { inspectorRect.X + 12.0f, cy + 3.0f }, "混合模式:", theme.TextMuted, 10.0f);
		static const std::vector<std::string> s_BaseBlendOptions = {
			"半透 (AlphaBlend)",
			"加法 (Additive)",
			"正片叠底 (Multiply)",
			"不透明 (Opaque)"
		};
		int curBlendIdx = (em.Material.BlendMode == ParticleBlendMode::Additive) ? 1 : ((em.Material.BlendMode == ParticleBlendMode::Multiply) ? 2 : ((em.Material.BlendMode == ParticleBlendMode::Opaque) ? 3 : 0));
		const Wui::WuiRect blendRect { inspectorRect.X + 110.0f, cy, inspectorRect.W - 130.0f, 20.0f };
		if (Wui::Combo(ctx, Wui::HashId("vfx.em.blend.combo"), blendRect, "", s_BaseBlendOptions, curBlendIdx, theme))
		{
			PushUndoSnapshot();
			if (curBlendIdx == 1) em.Material.BlendMode = ParticleBlendMode::Additive;
			else if (curBlendIdx == 2) em.Material.BlendMode = ParticleBlendMode::Multiply;
			else if (curBlendIdx == 3) em.Material.BlendMode = ParticleBlendMode::Opaque;
			else em.Material.BlendMode = ParticleBlendMode::AlphaBlend;
			m_Dirty = true;
			SyncSimulationFromAsset();
		}
		cy += 30.0f;

		// -------------------------------------------------------------
		// 2. Niagara 级模块堆栈 (Modules Stack - All Native Slang Code)
		// -------------------------------------------------------------
		const Wui::WuiRect stackHeaderRect { inspectorRect.X + 8.0f, cy, inspectorRect.W - 16.0f, 28.0f };
		Wui::BarSurface(ctx, stackHeaderRect, theme.PanelHeader, theme.Border);
		Wui::Label(ctx, { stackHeaderRect.X + 8.0f, stackHeaderRect.Y + 7.0f }, "模块堆栈 (Modules Stack - Slang 代码)", theme.Accent, 11.5f);

		// 右侧添加模块操作按钮 (仅保留标准/全局模块添加按钮，代码自定义模块移入菜单栏)
		const Wui::WuiRect addPresetBtn { stackHeaderRect.X + stackHeaderRect.W - 88.0f, stackHeaderRect.Y + 4.0f, 84.0f, 20.0f };
		Wui::Tooltip(ctx, addPresetBtn, "添加标准或项目全局 Slang 动力学模块 (代码编写请点击菜单栏 [代码模块])");
		if (Wui::Button(ctx, Wui::HashId("vfx.stk.add_preset"), addPresetBtn, "+ 添加模块", theme))
		{
			m_AddModuleMenuOpen = !m_AddModuleMenuOpen;
			if (m_AddModuleMenuOpen)
			{
				RefreshProjectCustomModules();
			}
		}
		cy += 34.0f;

		// 添加模块弹出菜单 (支持内建标准 Slang 模块 + 项目全局代码自定义模块)
		if (m_AddModuleMenuOpen)
		{
			const float itemH = 22.0f;
			const float menuH = 194.0f + (m_ProjectCustomModules.empty() ? 24.0f : static_cast<float>(m_ProjectCustomModules.size() * 24.0f));
			const Wui::WuiRect menuRect { inspectorRect.X + 12.0f, cy, inspectorRect.W - 24.0f, menuH };
			Wui::BarSurface(ctx, menuRect, Wui::WuiColor { 0.14f, 0.16f, 0.20f, 0.98f }, theme.Accent);

			float my = menuRect.Y + 4.0f;
			const auto renderMenuItem = [&](const char* id, const char* label, const VfxModule& mod) {
				const Wui::WuiRect itemRect { menuRect.X + 4.0f, my, menuRect.W - 8.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(id), itemRect, label, theme))
				{
					AddModuleToSelectedEmitter(mod);
					m_AddModuleMenuOpen = false;
				}
				my += itemH;
			};

			Wui::Label(ctx, { menuRect.X + 8.0f, my + 3.0f }, "--- 内建标准 Slang 模块 ---", theme.Accent, 9.5f);
			my += 18.0f;

			renderMenuItem("vfx.add.g", "+ 重力场 (Gravity Force)", VfxModuleLibrary::CreateGravityModule());
			renderMenuItem("vfx.add.d", "+ 空气阻力 (Linear Drag)", VfxModuleLibrary::CreateLinearDragModule());
			renderMenuItem("vfx.add.c", "+ 3D 旋度流体噪声 (Curl Noise)", VfxModuleLibrary::CreateCurlNoiseModule());
			renderMenuItem("vfx.add.a", "+ 点引力/黑洞吸引子 (Point Attractor)", VfxModuleLibrary::CreatePointAttractorModule());
			renderMenuItem("vfx.add.v", "+ 旋转涡流场 (Vortex Field)", VfxModuleLibrary::CreateVortexModule());
			renderMenuItem("vfx.add.s", "+ 物理欧拉积分与消亡 (Solve Velocity)", VfxModuleLibrary::CreateSolveVelocityModule());

			my += 4.0f;
			std::string projHeader = "--- 项目全局代码模块 (" + std::to_string(m_ProjectCustomModules.size()) + ") ---";
			Wui::Label(ctx, { menuRect.X + 8.0f, my + 3.0f }, projHeader, theme.Accent, 9.5f);
			my += 18.0f;

			if (m_ProjectCustomModules.empty())
			{
				Wui::Label(ctx, { menuRect.X + 12.0f, my + 3.0f }, "(暂无项目模块 - 前往顶部菜单栏编写)", theme.TextMuted, 9.0f);
				my += 22.0f;
			}
			else
			{
				for (size_t pModIdx = 0; pModIdx < m_ProjectCustomModules.size(); ++pModIdx)
				{
					const auto& pMod = m_ProjectCustomModules[pModIdx];
					std::string pId = "vfx.add.proj_" + std::to_string(pModIdx);
					std::string pLabel = "[★] " + pMod.Name;
					renderMenuItem(pId.c_str(), pLabel.c_str(), pMod);
				}
			}

			// 底部快捷通道
			my += 2.0f;
			const Wui::WuiRect scratchPadLink { menuRect.X + 4.0f, my, menuRect.W - 8.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId("vfx.add.goto_scratch"), scratchPadLink, "→ 在菜单栏编写新代码模块 (Scratch Pad)", theme))
			{
				m_AddModuleMenuOpen = false;
				OpenCustomModuleEditor();
			}

			cy += menuRect.H + 8.0f;
		}

		// 遍历渲染模块卡片
		const float cardX = inspectorRect.X + 12.0f;
		const float cardW = inspectorRect.W - 24.0f;
		for (size_t mIdx = 0; mIdx < em.Modules.size(); ++mIdx)
		{
			RenderModuleCard(ctx, em.Modules[mIdx], mIdx, cardX, cy, cardW, theme);
		}

		cy += 20.0f;
		Wui::EndScrollArea(ctx);

		// 【优化 1: 独立滚动条】当卡片内容超出视口时，在右侧清晰显示并支持鼠标直接拖拽
		const float scrollbarW = 6.0f;
		const Wui::WuiRect scrollbarRect { inspectorRect.X + inspectorRect.W - scrollbarW - 2.0f, curY, scrollbarW, scrollAreaH };
		Wui::ScrollBar(ctx, Wui::HashId("vfx.insp.scrollbar"), scrollbarRect, totalCardsHeight, scrollAreaH, m_InspectorScrollY, theme, false);
	}



	// =========================================================================
	// 纯 Slang 模块化着色器堆栈与热重载实现 (Slang Modular Pipeline)
	// =========================================================================

	void ParticleEditorPanel::AddModuleToSelectedEmitter(const VfxModule& module)
	{
		if (m_SelectedEmitterIndex >= m_Asset.Emitters.size()) return;
		PushUndoSnapshot();
		auto& em = m_Asset.Emitters[m_SelectedEmitterIndex];
		em.EnsureDefaultModules();
		em.Modules.push_back(module);
		m_Dirty = true;
		SyncSlangShaderFromModules();
		SyncSimulationFromAsset();
		m_StatusFeedback = "已添加模块: " + module.Name;
	}

	void ParticleEditorPanel::RemoveModuleFromSelectedEmitter(size_t moduleIndex)
	{
		if (m_SelectedEmitterIndex >= m_Asset.Emitters.size()) return;
		auto& em = m_Asset.Emitters[m_SelectedEmitterIndex];
		if (moduleIndex >= em.Modules.size()) return;
		PushUndoSnapshot();
		const std::string name = em.Modules[moduleIndex].Name;
		em.Modules.erase(em.Modules.begin() + moduleIndex);
		m_Dirty = true;
		SyncSlangShaderFromModules();
		SyncSimulationFromAsset();
		m_StatusFeedback = "已移除模块: " + name;
	}

	void ParticleEditorPanel::MoveModuleUp(size_t moduleIndex)
	{
		if (m_SelectedEmitterIndex >= m_Asset.Emitters.size() || moduleIndex == 0) return;
		auto& em = m_Asset.Emitters[m_SelectedEmitterIndex];
		if (moduleIndex >= em.Modules.size()) return;
		PushUndoSnapshot();
		std::swap(em.Modules[moduleIndex], em.Modules[moduleIndex - 1]);
		m_Dirty = true;
		SyncSlangShaderFromModules();
		SyncSimulationFromAsset();
	}

	void ParticleEditorPanel::MoveModuleDown(size_t moduleIndex)
	{
		if (m_SelectedEmitterIndex >= m_Asset.Emitters.size()) return;
		auto& em = m_Asset.Emitters[m_SelectedEmitterIndex];
		if (moduleIndex + 1 >= em.Modules.size()) return;
		PushUndoSnapshot();
		std::swap(em.Modules[moduleIndex], em.Modules[moduleIndex + 1]);
		m_Dirty = true;
		SyncSlangShaderFromModules();
		SyncSimulationFromAsset();
	}

	void ParticleEditorPanel::SyncSlangShaderFromModules()
	{
		if (m_SelectedEmitterIndex >= m_Asset.Emitters.size()) return;
		auto& em = m_Asset.Emitters[m_SelectedEmitterIndex];
		em.EnsureDefaultModules();

		m_SlangShaderBuffer = VfxComputeGenerator::GenerateComputeShaderSource(
			em.Name,
			em.Modules,
			m_Asset.UserParamsFloat,
			m_Asset.UserParamsVec4
		);
		m_SlangCompileError.clear();
		m_SlangCompileSuccess = true;
		m_StatusFeedback = "已从模块堆栈生成最新统一 Slang 计算着色器";
	}

	void ParticleEditorPanel::CompileAndApplySlangShader()
	{
		if (m_SelectedEmitterIndex >= m_Asset.Emitters.size()) return;
		auto& em = m_Asset.Emitters[m_SelectedEmitterIndex];

		std::string err;
		const bool ok = VfxComputeGenerator::ValidateAndCompile(m_SlangShaderBuffer, &err);
		if (ok)
		{
			PushUndoSnapshot();
			em.UseCustomShaderOverride = true;
			em.CustomSlangShaderOverride = m_SlangShaderBuffer;
			m_SlangCompileSuccess = true;
			m_SlangCompileError.clear();
			m_Dirty = true;
			SyncSimulationFromAsset();
			m_StatusFeedback = "[成功] Slang 计算着色器已即时热重载生效！";
		}
		else
		{
			m_SlangCompileSuccess = false;
			m_SlangCompileError = err;
			m_StatusFeedback = "[编译错误] Slang 语法诊断失败，请检查错误提示";
		}
	}

	void ParticleEditorPanel::RenderModuleCard(Wui::WuiContext& ctx, VfxModule& mod, size_t modIdx, float cardX, float& curY, float cardW, const Wui::WuiTheme& theme)
	{
		const float cardPad = 8.0f;
		const float headerH = 26.0f;
		float cardContentH = headerH + 6.0f;

		const std::string modKey = mod.Id + "_" + std::to_string(modIdx);
		const bool isSourceExpanded = m_ModuleSourceExpanded[modKey];

		if (isSourceExpanded) cardContentH += 130.0f;
		if (mod.Enabled) cardContentH += mod.Parameters.size() * 26.0f;

		const Wui::WuiRect cardRect { cardX, curY, cardW, cardContentH };
		Wui::BarSurface(ctx, cardRect, Wui::WuiColor { 0.12f, 0.14f, 0.18f, 0.95f }, theme.Border);

		// 1. 头部标题行
		float hx = cardRect.X + 6.0f;
		const float hy = cardRect.Y + 3.0f;

		// 启用复选框
		const Wui::WuiRect enCheck { hx, hy + 2.0f, 16.0f, 16.0f };
		if (Wui::Checkbox(ctx, Wui::HashId((std::string("vfx.mod.en_") + std::to_string(modIdx)).c_str()), enCheck, "", mod.Enabled, theme))
		{
			PushUndoSnapshot();
			SyncSlangShaderFromModules();
			SyncSimulationFromAsset();
		}
		hx += 20.0f;

		// 阶段徽章 (Emitter:深蓝, Spawn:青色, Update:橙色, Render:紫色)
		Wui::WuiColor badgeCol { 0.9f, 0.55f, 0.2f, 1.0f };
		const char* badgeText = "[Update]";
		if (mod.Stage == VfxModuleStage::Emitter)
		{
			badgeCol = Wui::WuiColor { 0.25f, 0.55f, 0.95f, 1.0f };
			badgeText = "[Emitter]";
		}
		else if (mod.Stage == VfxModuleStage::Spawn)
		{
			badgeCol = Wui::WuiColor { 0.15f, 0.80f, 0.85f, 1.0f };
			badgeText = "[Spawn]";
		}
		else if (mod.Stage == VfxModuleStage::Render)
		{
			badgeCol = Wui::WuiColor { 0.75f, 0.35f, 0.95f, 1.0f };
			badgeText = "[Render]";
		}
		Wui::Label(ctx, { hx, hy + 3.0f }, badgeText, badgeCol, 9.5f);
		hx += 52.0f;

		// 模块名称行与悬浮提示
		const Wui::WuiRect modTitleRect { hx, hy, cardRect.W - 190.0f, 22.0f };
		Wui::Tooltip(ctx, modTitleRect, mod.Description.empty() ? mod.Name : mod.Description);
		Wui::Label(ctx, { hx, hy + 3.0f }, mod.Name, mod.Enabled ? theme.Text : theme.TextMuted, 10.5f);

		// 右侧操作工具栏 (上移, 下移, 源码, 删除)
		float rx = cardRect.X + cardRect.W - 22.0f;

		// 删除按钮
		const Wui::WuiRect delBtn { rx, hy, 18.0f, 20.0f };
		Wui::Tooltip(ctx, delBtn, "从堆栈中移除此模块");
		if (Wui::Button(ctx, Wui::HashId((std::string("vfx.mod.del_") + std::to_string(modIdx)).c_str()), delBtn, "X", theme))
		{
			RemoveModuleFromSelectedEmitter(modIdx);
			return;
		}
		rx -= 22.0f;

		// 源码展开按钮
		const Wui::WuiRect srcBtn { rx - 20.0f, hy, 38.0f, 20.0f };
		Wui::Tooltip(ctx, srcBtn, isSourceExpanded ? "折叠此模块的 Slang 计算源码" : "就地展开编辑此模块的 Slang 计算源码");
		if (Wui::Button(ctx, Wui::HashId((std::string("vfx.mod.src_") + std::to_string(modIdx)).c_str()), srcBtn, isSourceExpanded ? "收起" : "源码", theme))
		{
			m_ModuleSourceExpanded[modKey] = !isSourceExpanded;
		}
		rx -= 44.0f;

		// 【需求 1: 下移按钮使用标准符号 ↓】
		const Wui::WuiRect dnBtn { rx, hy, 20.0f, 20.0f };
		Wui::Tooltip(ctx, dnBtn, "下移执行顺序 (在当前阶段向后执行)");
		if (Wui::Button(ctx, Wui::HashId((std::string("vfx.mod.dn_") + std::to_string(modIdx)).c_str()), dnBtn, "↓", theme))
		{
			MoveModuleDown(modIdx);
			return;
		}
		rx -= 22.0f;

		// 【需求 1: 上移按钮使用标准符号 ↑】
		const Wui::WuiRect upBtn { rx, hy, 20.0f, 20.0f };
		Wui::Tooltip(ctx, upBtn, "上移执行顺序 (在当前阶段优先执行)");
		if (Wui::Button(ctx, Wui::HashId((std::string("vfx.mod.up_") + std::to_string(modIdx)).c_str()), upBtn, "↑", theme))
		{
			MoveModuleUp(modIdx);
			return;
		}

		float fieldY = cardRect.Y + headerH + 4.0f;

		// 2. 参数调节列表 (带悬浮提示、通用调色板与下拉枚举)
		if (mod.Enabled)
		{
			for (size_t pIdx = 0; pIdx < mod.Parameters.size(); ++pIdx)
			{
				auto& param = mod.Parameters[pIdx];
				const float rowH = 24.0f;

				// 参数标签与悬浮提示
				const Wui::WuiRect labelRect { cardRect.X + 10.0f, fieldY, 95.0f, 20.0f };
				Wui::Tooltip(ctx, labelRect, param.Tooltip.empty() ? param.Name : param.Tooltip);
				Wui::Label(ctx, { cardRect.X + 12.0f, fieldY + 3.0f }, param.Name + ":", theme.TextMuted, 10.0f);

				// 暴露开关符号按钮 [●] 与 [○]
				const Wui::WuiRect expBtn { cardRect.X + cardRect.W - 28.0f, fieldY, 22.0f, 20.0f };
				Wui::Tooltip(ctx, expBtn, param.IsExposed ? "已暴露为系统外部公开参数 (可在外部 Inspector / 脚本驱动)" : "点击将此参数暴露给系统 (公开展现在外部 Inspector 中)");
				const char* expLabel = param.IsExposed ? "●" : "○";
				if (param.IsExposed) Wui::BarSurface(ctx, expBtn, Wui::WuiColor { 0.28f, 0.24f, 0.12f, 0.95f }, theme.Accent);
				if (Wui::Button(ctx, Wui::HashId((std::string("vfx.p.exp_") + std::to_string(modIdx * 100 + pIdx)).c_str()), expBtn, expLabel, theme))
				{
					PushUndoSnapshot();
					param.IsExposed = !param.IsExposed;
					if (param.IsExposed)
					{
						if (param.ExposedName.empty()) param.ExposedName = param.Name;
						m_Asset.UserParamsFloat[param.ExposedName] = param.FloatVal;
					}
					else
					{
						m_Asset.UserParamsFloat.erase(param.ExposedName);
					}
					SyncSlangShaderFromModules();
				}

				// 参数输入控件区域
				const float valW = cardRect.W - 150.0f;
				const Wui::WuiRect valRect { cardRect.X + 110.0f, fieldY, valW, 20.0f };

				// 【需求 3: 所有 Mode 类型的变量必须使用下拉列表枚举 Combo】
				if (param.Name.find("Mode") != std::string::npos || param.Name == "FacingMode" || param.Name == "BlendMode")
				{
					std::vector<std::string> comboOpts;
					if (param.Name == "BlendMode")
					{
						comboOpts = { "半透 (AlphaBlend)", "加法 (Additive)", "正片叠底 (Multiply)", "不透明 (Opaque)" };
					}
					else if (param.Name == "FacingMode")
					{
						comboOpts = { "相机对齐 (Billboard)", "2D四边形 (Quad2D)", "拉伸速度 (StretchedVelocity)", "地面水平法阵 (HorizontalQuad)" };
					}
					else
					{
						comboOpts = { "模式 0", "模式 1", "模式 2" };
					}

					int selected = static_cast<int>(param.FloatVal);
					if (selected < 0 || selected >= static_cast<int>(comboOpts.size())) selected = 0;
					Wui::Tooltip(ctx, valRect, param.Tooltip.empty() ? (param.Name + " 下拉枚举选择") : param.Tooltip);
					if (Wui::Combo(ctx, Wui::HashId((std::string("vfx.combo_") + std::to_string(modIdx * 100 + pIdx)).c_str()), valRect, "", comboOpts, selected, theme))
					{
						param.FloatVal = static_cast<float>(selected);
						m_Dirty = true;
						SyncSimulationFromAsset();
					}
				}
				// 【需求 2: Color 变量使用通用调色板 ColorField】
				else if (param.Type == VfxParamType::Vec4 && param.Name.find("Color") != std::string::npos)
				{
					Wui::Tooltip(ctx, valRect, param.Tooltip.empty() ? (param.Name + ": 点击打开通用调色板 (支持色轮/HEX/透明度)") : param.Tooltip);
					if (Wui::ColorField(ctx, Wui::HashId((std::string("vfx.cf_") + std::to_string(modIdx * 100 + pIdx)).c_str()), valRect, param.VecVal, theme))
					{
						m_Dirty = true;
						SyncSimulationFromAsset();
					}
				}
				else if (param.Type == VfxParamType::Float)
				{
					Wui::Tooltip(ctx, valRect, param.Tooltip.empty() ? param.Name : param.Tooltip);
					if (Wui::DragFloat(ctx, Wui::HashId((std::string("vfx.mod.val_") + std::to_string(modIdx * 100 + pIdx)).c_str()), valRect, param.FloatVal, param.Step, param.Min, param.Max, theme))
					{
						if (param.IsExposed)
						{
							m_Asset.UserParamsFloat[param.ExposedName.empty() ? param.Name : param.ExposedName] = param.FloatVal;
						}
						m_Dirty = true;
						SyncSimulationFromAsset();
					}
				}
				else if (param.Type == VfxParamType::Vec3)
				{
					const float subW = (valW - 8.0f) / 3.0f;
					if (Wui::DragFloat(ctx, Wui::HashId((std::string("vfx.mod.v3x_") + std::to_string(modIdx * 100 + pIdx)).c_str()), { valRect.X, valRect.Y, subW, valRect.H }, param.VecVal.x, param.Step, param.Min, param.Max, theme))
					{
						m_Dirty = true;
						SyncSimulationFromAsset();
					}
					if (Wui::DragFloat(ctx, Wui::HashId((std::string("vfx.mod.v3y_") + std::to_string(modIdx * 100 + pIdx)).c_str()), { valRect.X + subW + 4.0f, valRect.Y, subW, valRect.H }, param.VecVal.y, param.Step, param.Min, param.Max, theme))
					{
						m_Dirty = true;
						SyncSimulationFromAsset();
					}
					if (Wui::DragFloat(ctx, Wui::HashId((std::string("vfx.mod.v3z_") + std::to_string(modIdx * 100 + pIdx)).c_str()), { valRect.X + (subW + 4.0f) * 2.0f, valRect.Y, subW, valRect.H }, param.VecVal.z, param.Step, param.Min, param.Max, theme))
					{
						m_Dirty = true;
						SyncSimulationFromAsset();
					}
				}

				fieldY += rowH + 2.0f;
			}
		}

		// 3. 就地展开的 Slang 源码编辑器
		if (isSourceExpanded)
		{
			const Wui::WuiRect srcBox { cardRect.X + 8.0f, fieldY, cardRect.W - 16.0f, 120.0f };
			Wui::BarSurface(ctx, srcBox, Wui::WuiColor { 0.06f, 0.07f, 0.09f, 1.0f }, theme.Border);

			std::istringstream stream(mod.SlangSource);
			std::string line;
			float lineY = srcBox.Y + 4.0f;
			while (std::getline(stream, line) && lineY < srcBox.Y + srcBox.H - 14.0f)
			{
				Wui::WuiColor col = theme.Text;
				if (line.find("//") != std::string::npos) col = Wui::WuiColor { 0.45f, 0.65f, 0.45f, 0.85f };
				else if (line.find("Velocity") != std::string::npos || line.find("Position") != std::string::npos)
					col = theme.Accent;
				Wui::Label(ctx, { srcBox.X + 6.0f, lineY }, line, col, 9.5f);
				lineY += 14.0f;
			}
			fieldY += 126.0f;
		}

		curY += cardContentH + 6.0f;
	}

	void ParticleEditorPanel::RenderSlangShaderView(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme, PanelHost& host)
	{
		(void)host;
		const Wui::WuiRect toolbar { rect.X, rect.Y, rect.W, 36.0f };
		Wui::BarSurface(ctx, toolbar, theme.PanelHeader, theme.Border);

		float cx = toolbar.X + 10.0f;
		Wui::Label(ctx, { cx, toolbar.Y + 10.0f }, "统一 Slang 计算着色器 (Unified Slang Compute Shader)", theme.Accent, 11.5f);
		cx += 360.0f;

		// 编译并即时热重载按钮
		const Wui::WuiRect applyBtn { cx, toolbar.Y + 6.0f, 160.0f, 24.0f };
		Wui::Tooltip(ctx, applyBtn, "即时编译 Slang 源码并在 GPU 视口与实体上热重载生效 (Ctrl+Enter)");
		if (Wui::Button(ctx, Wui::HashId("vfx.slang.apply"), applyBtn, "[运行] 编译并热重载 (Ctrl+Enter)", theme))
		{
			CompileAndApplySlangShader();
		}
		cx += 166.0f;

		// 从模块重新生成
		const Wui::WuiRect syncBtn { cx, toolbar.Y + 6.0f, 120.0f, 24.0f };
		Wui::Tooltip(ctx, syncBtn, "丢弃手写覆盖，从当前模块堆栈重新生成统一着色器");
		if (Wui::Button(ctx, Wui::HashId("vfx.slang.sync"), syncBtn, "[循环] 从模块堆栈生成", theme))
		{
			SyncSlangShaderFromModules();
		}
		cx += 128.0f;

		// 编译状态与诊断显示
		if (!m_SlangCompileError.empty())
		{
			Wui::Label(ctx, { cx, toolbar.Y + 10.0f }, "[编译错误] " + m_SlangCompileError.substr(0, 60), Wui::WuiColor { 0.95f, 0.3f, 0.3f, 1.0f }, 10.0f);
		}
		else if (m_SlangCompileSuccess)
		{
			Wui::Label(ctx, { cx, toolbar.Y + 10.0f }, "[就绪] Slang 模块化计算着色器已热重载生效 (SPIR-V 1.3)", Wui::WuiColor { 0.3f, 0.85f, 0.4f, 1.0f }, 10.0f);
		}

		// 源码文本查看编辑框
		const Wui::WuiRect codeBox { rect.X, rect.Y + 40.0f, rect.W, rect.H - 40.0f };
		Wui::BarSurface(ctx, codeBox, Wui::WuiColor { 0.05f, 0.06f, 0.08f, 1.0f }, theme.Border);

		if (m_SlangShaderBuffer.empty())
		{
			SyncSlangShaderFromModules();
		}

		std::istringstream stream(m_SlangShaderBuffer);
		std::string line;
		int lineIdx = 0;
		float lineY = codeBox.Y + 8.0f - m_SlangScrollY;
		const float lineHeight = 18.0f;

		const glm::vec2 mousePos = ctx.Input().MousePos;
		const bool hovered = (mousePos.x >= codeBox.X && mousePos.x <= codeBox.X + codeBox.W &&
		                      mousePos.y >= codeBox.Y && mousePos.y <= codeBox.Y + codeBox.H);
		if (hovered && std::abs(ctx.Input().Wheel) > 0.01f)
		{
			m_SlangScrollY = std::max(0.0f, m_SlangScrollY - ctx.Input().Wheel * 36.0f);
		}

		while (std::getline(stream, line))
		{
			lineIdx++;
			if (lineY >= codeBox.Y && lineY <= codeBox.Y + codeBox.H - lineHeight)
			{
				Wui::Label(ctx, { codeBox.X + 8.0f, lineY }, std::to_string(lineIdx), Wui::WuiColor { 0.35f, 0.40f, 0.48f, 0.8f }, 9.5f);

				Wui::WuiColor textCol = theme.Text;
				if (line.find("//") != std::string::npos)
				{
					textCol = Wui::WuiColor { 0.40f, 0.65f, 0.45f, 0.85f };
				}
				else if (line.find("struct") != std::string::npos || line.find("float") != std::string::npos ||
				         line.find("uint") != std::string::npos || line.find("void") != std::string::npos ||
				         line.find("cbuffer") != std::string::npos)
				{
					textCol = Wui::WuiColor { 0.35f, 0.65f, 0.95f, 1.0f };
				}
				else if (line.find("[numthreads") != std::string::npos || line.find("[[vk") != std::string::npos)
				{
					textCol = Wui::WuiColor { 0.95f, 0.60f, 0.20f, 1.0f };
				}
				else if (line.find("CS_Spawn") != std::string::npos || line.find("CS_Update") != std::string::npos)
				{
					textCol = theme.Accent;
				}

				Wui::Label(ctx, { codeBox.X + 42.0f, lineY }, line, textCol, 10.5f);
			}
			lineY += lineHeight;
		}
	}

	void ParticleEditorPanel::RenderDataView(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme, PanelHost& host)
	{
		(void)host;
		const Wui::WuiRect codeToolbar { rect.X, rect.Y, rect.W, 36.0f };
		Wui::BarSurface(ctx, codeToolbar, theme.PanelHeader, theme.Border);

		float cx = codeToolbar.X + 8.0f;
		const Wui::WuiRect applyBtn { cx, codeToolbar.Y + 6.0f, 150.0f, 24.0f };
		Wui::Tooltip(ctx, applyBtn, "解析并应用当前底层 YAML 声明式配置到视口与特效系统 (Ctrl+Enter)");
		if (Wui::Button(ctx, Wui::HashId("vfx.data.apply"), applyBtn, "[应用] 应用数据 (Ctrl+Enter)", theme))
		{
			ApplyCodeToAsset();
		}
		cx += 156.0f;

		const Wui::WuiRect syncBtn { cx, codeToolbar.Y + 6.0f, 120.0f, 24.0f };
		Wui::Tooltip(ctx, syncBtn, "从当前视口与模块参数重新生成 YAML 数据清单");
		if (Wui::Button(ctx, Wui::HashId("vfx.data.sync"), syncBtn, "[循环] 从视口重新生成", theme))
		{
			SyncCodeFromAsset();
		}
		cx += 128.0f;

		Wui::Label(ctx, { cx, codeToolbar.Y + 10.0f }, "说明: 此处为底层序列化清单 (Data Manifest)，代码控制请切换至 [统一 Slang 着色器]", theme.TextMuted, 10.0f);

		const Wui::WuiRect codeBox { rect.X, rect.Y + 40.0f, rect.W, rect.H - 40.0f };
		Wui::BarSurface(ctx, codeBox, Wui::WuiColor { 0.05f, 0.06f, 0.08f, 1.0f }, theme.Border);

		std::istringstream stream(m_CodeBuffer);
		std::string line;
		int lineIdx = 0;
		float lineY = codeBox.Y + 8.0f - m_CodeScrollY;
		const float lineHeight = 18.0f;

		const glm::vec2 mousePos = ctx.Input().MousePos;
		const bool hovered = (mousePos.x >= codeBox.X && mousePos.x <= codeBox.X + codeBox.W &&
		                      mousePos.y >= codeBox.Y && mousePos.y <= codeBox.Y + codeBox.H);
		if (hovered && std::abs(ctx.Input().Wheel) > 0.01f)
		{
			m_CodeScrollY = std::max(0.0f, m_CodeScrollY - ctx.Input().Wheel * 36.0f);
		}

		while (std::getline(stream, line))
		{
			lineIdx++;
			if (lineY >= codeBox.Y && lineY <= codeBox.Y + codeBox.H - lineHeight)
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

} // namespace World
