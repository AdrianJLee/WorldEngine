#include "wldpch.h"
#include <glm/vec2.hpp>
#include "WUI/Panels/SoundEventEditorPanel.h"
#include "World/Audio/AudioCooker.h"
#include "World/Audio/AudioSystem.h"
#include "World/Audio/AudioTypes.h"
#include "World/Audio/AudioBuffer.h"
#include "World/Utils/Paths.h"
#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <iomanip>

namespace World
{
	std::string SoundEventEditorPanel::s_PendingOpenPath;

	void SoundEventEditorPanel::RequestOpenEvent(const std::string& path)
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
			std::filesystem::path dir = target;
			if (!std::filesystem::is_directory(target, ec))
			{
				dir = target.parent_path();
			}
			if (!std::filesystem::exists(dir, ec))
			{
				std::filesystem::create_directories(dir, ec);
			}
			const std::wstring parameters = L"\"" + dir.wstring() + L"\"" ;
			ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
		}
#endif
	}

	static std::shared_ptr<Audio::AudioBuffer> EnsureSliceBuffer(const std::string& path, size_t variationSeed = 0)
	{
		auto& audio = Audio::AudioSystem::Get();
		auto buf = audio.FindAudioBuffer(path);
		if (buf) return buf;

		std::vector<std::filesystem::path> candidates = {
			std::filesystem::path(path),
			Paths::AssetRoot() / path,
			Paths::ProjectDir() / path,
			std::filesystem::current_path() / "templates/project-example" / path,
			std::filesystem::current_path() / path
		};

		std::error_code ec;
		for (const auto& c : candidates)
		{
			if (std::filesystem::exists(c, ec) && !std::filesystem::is_directory(c, ec))
			{
				std::string pStr = c.generic_string();
				if (pStr.rfind(".waudio") != std::string::npos)
				{
					buf = Audio::AudioCooker::LoadCookedFile(pStr);
				}
				else
				{
					Audio::AudioBuffer tmp;
					if (Audio::AudioCooker::LoadAudioFile(pStr, tmp))
					{
						buf = std::make_shared<Audio::AudioBuffer>(std::move(tmp));
					}
					else
					{
						buf = Audio::AudioCooker::LoadCookedFile(pStr);
					}
				}
				if (buf) break;
			}
		}

		if (!buf)
		{
			float freq = 440.0f + static_cast<float>(variationSeed) * 60.0f;
			buf = std::make_shared<Audio::AudioBuffer>(
				Audio::AudioBuffer::GenerateSineWave(freq, 0.22f, 0.35f, 48000));
		}

		audio.RegisterAudioBuffer(path, buf);
		return buf;
	}

	bool SoundEventEditorPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		(void)shift;
		(void)alt;
		if (ctrl && keyCode == KeyCodes::S)
		{
			SaveEvent();
			return true;
		}
		if (ctrl && keyCode == KeyCodes::Enter && m_ViewMode == SoundEventViewMode::Code)
		{
			ApplyCodeToEvent();
			return true;
		}
		return false;
	}

	void SoundEventEditorPanel::RefreshAvailableFiles()
	{
		m_AvailableSoundFiles.clear();
		m_AvailableSliceAssets.clear();

		const std::vector<std::string> searchDirs = {
			(Paths::AssetRoot() / "audio").generic_string(),
			(Paths::ProjectDir() / "assets/audio").generic_string(),
			"assets/audio/events",
			"assets/audio",
			"templates/project-example/assets/audio/events",
			"templates/project-example/assets/audio"
		};

		for (const auto& dir : searchDirs)
		{
			std::error_code ec;
			if (std::filesystem::exists(dir, ec))
			{
				for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
				{
					if (!entry.is_regular_file(ec)) continue;
					auto ext = entry.path().extension();
					std::string p = entry.path().lexically_normal().generic_string();

					if (ext == ".wsound")
					{
						if (std::find(m_AvailableSoundFiles.begin(), m_AvailableSoundFiles.end(), p) == m_AvailableSoundFiles.end())
						{
							m_AvailableSoundFiles.push_back(p);
						}
					}
					else if (ext == ".waudio" || ext == ".wav" || ext == ".ogg" || ext == ".mp3")
					{
						if (std::find(m_AvailableSliceAssets.begin(), m_AvailableSliceAssets.end(), p) == m_AvailableSliceAssets.end())
						{
							m_AvailableSliceAssets.push_back(p);
						}
					}
				}
			}
		}

		std::sort(m_AvailableSoundFiles.begin(), m_AvailableSoundFiles.end());
		std::sort(m_AvailableSliceAssets.begin(), m_AvailableSliceAssets.end());

		if (m_AvailableSoundFiles.empty())
		{
			m_AvailableSoundFiles.push_back("assets/audio/events/footstep_player.wsound");
			m_AvailableSoundFiles.push_back("assets/audio/events/ui_click.wsound");
		}

		auto addClip = [this](const std::string& p) {
			if (std::find(m_AvailableSliceAssets.begin(), m_AvailableSliceAssets.end(), p) == m_AvailableSliceAssets.end())
			{
				m_AvailableSliceAssets.push_back(p);
			}
		};
		addClip("assets/audio/sfx/footstep_grass.waudio");
		addClip("assets/audio/sfx/impact_metal.waudio");
		addClip("assets/audio/sfx/ui_click.waudio");

		if (m_SelectedSliceAssetIndex < 0 || m_SelectedSliceAssetIndex >= static_cast<int>(m_AvailableSliceAssets.size()))
		{
			m_SelectedSliceAssetIndex = 0;
		}
	}

	void SoundEventEditorPanel::LoadEvent(const std::string& path)
	{
		if (path.empty()) return;
		m_LoadedPath = path;
		std::replace(m_LoadedPath.begin(), m_LoadedPath.end(), '\\', '/');

		std::filesystem::path p(m_LoadedPath);
		std::vector<std::filesystem::path> candidates = {
			p,
			Paths::AssetRoot() / p,
			Paths::ProjectDir() / p,
			std::filesystem::current_path() / "templates/project-example" / p,
			std::filesystem::current_path() / p
		};

		std::filesystem::path actualPath;
		std::error_code ec;
		for (const auto& c : candidates)
		{
			if (std::filesystem::exists(c, ec) && !std::filesystem::is_directory(c, ec))
			{
				actualPath = c;
				break;
			}
		}

		std::ifstream in(actualPath.empty() ? p : actualPath);
		if (in.is_open())
		{
			std::stringstream ss;
			ss << in.rdbuf();
			m_Event.DeserializeYaml(ss.str());
			m_StatusFeedback = "已载入声音事件: " + m_LoadedPath;
			m_StatusIsError = false;
		}
		else
		{
			std::string stem = p.stem().string();
			m_Event = Audio::SoundEvent(stem.empty() ? "New_SoundEvent" : stem, Audio::kBusSfx);
			m_StatusFeedback = "新建声音事件: " + m_LoadedPath;
			m_StatusIsError = false;
		}

		m_EventIdBuffer = m_Event.GetId();

		if (!m_Event.GetRootContainer())
		{
			m_Event.GetRootContainer() = std::make_shared<Audio::SoundContainerNode>();
			m_Event.GetRootContainer()->type = Audio::ContainerType::Random;
		}

		m_SelectedBusIndex = static_cast<int>(m_Event.GetBusId());
		if (m_SelectedBusIndex < 0 || m_SelectedBusIndex >= 6) m_SelectedBusIndex = 2;

		m_MinDist = m_Event.GetSpatial().minDistance;
		m_MaxDist = m_Event.GetSpatial().maxDistance;
		m_AttenuationModelIndex = static_cast<int>(m_Event.GetSpatial().model);
		if (m_AttenuationModelIndex < 0 || m_AttenuationModelIndex >= 3) m_AttenuationModelIndex = 0;

		m_MaxInstances = static_cast<int>(m_Event.GetMaxInstances());
		m_CooldownSeconds = m_Event.GetCooldownSeconds();
		m_StealPolicyIndex = static_cast<int>(m_Event.GetStealPolicy());
		if (m_StealPolicyIndex < 0 || m_StealPolicyIndex >= 3) m_StealPolicyIndex = 0;

		if (m_Event.GetSpatial().mode == Audio::SpatialMode::Spatial2D)
			m_SpatialModeIndex = 0;
		else if (m_Event.GetSpatial().is3D || m_Event.GetSpatial().mode == Audio::SpatialMode::Spatial3D)
			m_SpatialModeIndex = 1;
		else
			m_SpatialModeIndex = 2;

		m_MinVolume = m_Event.GetMinVolume();
		m_MaxVolume = m_Event.GetMaxVolume();
		m_MinPitchSemi = m_Event.GetMinPitchSemitones();
		m_MaxPitchSemi = m_Event.GetMaxPitchSemitones();

		if (m_Event.GetRootContainer())
		{
			m_SelectedContainerType = static_cast<int>(m_Event.GetRootContainer()->type);
			m_ShuffleNoRepeat = m_Event.GetRootContainer()->shuffleNoRepeat;
		}

		m_Dirty = false;
		SyncCodeFromEvent();
		EnsureBuffersForEvent();
	}

	void SoundEventEditorPanel::EnsureBuffersForEvent()
	{
		if (!m_Event.GetRootContainer()) return;
		auto root = m_Event.GetRootContainer();
		for (size_t i = 0; i < root->variations.size(); ++i)
		{
			EnsureSliceBuffer(root->variations[i].soundAssetPath, i);
		}
		for (const auto& [bName, bNode] : root->switchBranches)
		{
			if (bNode && !bNode->variations.empty())
				EnsureSliceBuffer(bNode->variations[0].soundAssetPath, 0);
		}
		for (const auto& pt : root->blendPoints)
		{
			if (pt.second && !pt.second->variations.empty())
				EnsureSliceBuffer(pt.second->variations[0].soundAssetPath, 0);
		}
		for (const auto& lay : root->layerChildren)
		{
			if (lay.second && !lay.second->variations.empty())
				EnsureSliceBuffer(lay.second->variations[0].soundAssetPath, 0);
		}
	}

	void SoundEventEditorPanel::SaveEvent()
	{
		if (m_ViewMode == SoundEventViewMode::Code)
		{
			ApplyCodeToEvent();
		}

		if (m_LoadedPath.empty())
		{
			m_LoadedPath = "assets/audio/events/" + m_Event.GetId() + ".wsound";
		}

		if (!m_EventIdBuffer.empty() && m_EventIdBuffer != m_Event.GetId())
		{
			m_Event.SetId(m_EventIdBuffer);
		}

		m_Event.SetBusId(static_cast<Audio::BusId>(m_SelectedBusIndex));
		m_Event.SetMaxInstances(static_cast<uint32_t>(std::max(1, m_MaxInstances)));
		m_Event.SetCooldownSeconds(std::max(0.0f, m_CooldownSeconds));
		m_Event.SetStealPolicy(static_cast<Audio::StealPolicy>(m_StealPolicyIndex));

		if (m_SpatialModeIndex == 0)
		{
			m_Event.GetSpatial().mode = Audio::SpatialMode::Spatial2D;
			m_Event.GetSpatial().is3D = false;
		}
		else if (m_SpatialModeIndex == 1)
		{
			m_Event.GetSpatial().mode = Audio::SpatialMode::Spatial3D;
			m_Event.GetSpatial().is3D = true;
		}
		else
		{
			m_Event.GetSpatial().mode = Audio::SpatialMode::None;
			m_Event.GetSpatial().is3D = false;
		}

		m_Event.GetSpatial().minDistance = m_MinDist;
		m_Event.GetSpatial().maxDistance = m_MaxDist;
		m_Event.GetSpatial().model = static_cast<Audio::AttenuationModel>(m_AttenuationModelIndex);
		m_Event.SetVolumeRange(m_MinVolume, m_MaxVolume);
		m_Event.SetPitchRangeSemitones(m_MinPitchSemi, m_MaxPitchSemi);

		if (m_Event.GetRootContainer())
		{
			m_Event.GetRootContainer()->type = static_cast<Audio::ContainerType>(m_SelectedContainerType);
			m_Event.GetRootContainer()->shuffleNoRepeat = m_ShuffleNoRepeat;
		}

		std::filesystem::path p(m_LoadedPath);
		std::filesystem::path outPath = p;
		if (!std::filesystem::exists(p) && !p.is_absolute())
		{
			if (std::filesystem::exists(Paths::AssetRoot()))
				outPath = Paths::AssetRoot() / p;
			else if (std::filesystem::exists(Paths::ProjectDir()))
				outPath = Paths::ProjectDir() / p;
		}

		std::error_code ec;
		if (outPath.has_parent_path())
		{
			std::filesystem::create_directories(outPath.parent_path(), ec);
		}

		std::ofstream out(outPath);
		if (out.is_open())
		{
			out << m_Event.SerializeYaml();
			Audio::AudioSystem::Get().RegisterSoundEvent(m_Event);
			m_StatusFeedback = "✓ 已成功保存声效资产: " + outPath.generic_string();
			m_StatusIsError = false;
			m_Dirty = false;
			SyncCodeFromEvent();
		}
		else
		{
			m_StatusFeedback = "保存失败: 无法写入文件 " + outPath.generic_string();
			m_StatusIsError = true;
		}
	}

	void SoundEventEditorPanel::NewEvent()
	{
		std::string newId = "sound_event_" + std::to_string(m_AvailableSoundFiles.size() + 1);
		std::string newPath = "assets/audio/events/" + newId + ".wsound";
		m_Event = Audio::SoundEvent(newId, Audio::kBusSfx);
		m_LoadedPath = newPath;
		m_EventIdBuffer = newId;
		m_Event.GetRootContainer()->variations.push_back({ "assets/audio/sfx/ui_click.waudio", 1.0f });
		SaveEvent();
		RefreshAvailableFiles();
	}

	void SoundEventEditorPanel::ApplyCodeToEvent()
	{
		std::string codeText = m_CodeBuffer.Text();
		Audio::SoundEvent parsedEvent;
		if (parsedEvent.DeserializeYaml(codeText))
		{
			m_Event = parsedEvent;
			m_EventIdBuffer = m_Event.GetId();
			m_SelectedBusIndex = static_cast<int>(m_Event.GetBusId());
			m_MinVolume = m_Event.GetMinVolume();
			m_MaxVolume = m_Event.GetMaxVolume();
			m_MinPitchSemi = m_Event.GetMinPitchSemitones();
			m_MaxPitchSemi = m_Event.GetMaxPitchSemitones();
			m_MaxInstances = static_cast<int>(m_Event.GetMaxInstances());
			m_CooldownSeconds = m_Event.GetCooldownSeconds();
			m_StealPolicyIndex = static_cast<int>(m_Event.GetStealPolicy());
			m_MinDist = m_Event.GetSpatial().minDistance;
			m_MaxDist = m_Event.GetSpatial().maxDistance;
			m_AttenuationModelIndex = static_cast<int>(m_Event.GetSpatial().model);
			if (m_Event.GetSpatial().mode == Audio::SpatialMode::Spatial2D) m_SpatialModeIndex = 0;
			else if (m_Event.GetSpatial().is3D) m_SpatialModeIndex = 1;
			else m_SpatialModeIndex = 2;
			if (m_Event.GetRootContainer())
			{
				m_SelectedContainerType = static_cast<int>(m_Event.GetRootContainer()->type);
				m_ShuffleNoRepeat = m_Event.GetRootContainer()->shuffleNoRepeat;
			}
			Audio::AudioSystem::Get().RegisterSoundEvent(m_Event);
			EnsureBuffersForEvent();
			m_CodeStatusFeedback = "✓ YAML 语法验证通过，运行态已热更生效";
			m_CodeStatusIsError = false;
			m_StatusFeedback = "✓ 代码已同步到事件运行态";
			m_StatusIsError = false;
			m_Dirty = true;
		}
		else
		{
			m_CodeStatusFeedback = "✕ YAML 解析失败，请检查语法结构与缩进";
			m_CodeStatusIsError = true;
		}
	}

	void SoundEventEditorPanel::SyncCodeFromEvent()
	{
		std::string yaml = m_Event.SerializeYaml();
		m_CodeBuffer.SetText(yaml);
		m_CodeStatusFeedback = "已同步最新事件参数至 YAML 声明";
		m_CodeStatusIsError = false;
	}

	void SoundEventEditorPanel::PlayAudition()
	{
		if (m_ViewMode == SoundEventViewMode::Code)
		{
			ApplyCodeToEvent();
		}
		else
		{
			SaveEvent();
		}
		EnsureBuffersForEvent();

		auto& audio = Audio::AudioSystem::Get();
		if (m_CurrentAuditionVoice != Audio::kInvalidVoiceHandle)
		{
			audio.StopVoice(m_CurrentAuditionVoice, 0.02f);
			m_CurrentAuditionVoice = Audio::kInvalidVoiceHandle;
		}

		if (m_SpatialModeIndex == 1)
		{
			m_CurrentAuditionVoice = audio.PostEvent(m_Event.GetId(), glm::vec3(0.0f, 0.0f, 0.0f));
		}
		else if (m_SpatialModeIndex == 0)
		{
			m_CurrentAuditionVoice = audio.PostEvent2D(m_Event.GetId(), glm::vec2(0.0f, 0.0f));
		}
		else
		{
			m_CurrentAuditionVoice = audio.PostEvent2D(m_Event.GetId());
		}
		m_StatusFeedback = "正在试听事件回放: " + m_Event.GetId();
		m_StatusIsError = false;
	}

	void SoundEventEditorPanel::StopAudition()
	{
		auto& audio = Audio::AudioSystem::Get();
		if (m_CurrentAuditionVoice != Audio::kInvalidVoiceHandle)
		{
			audio.StopVoice(m_CurrentAuditionVoice, 0.05f);
			m_CurrentAuditionVoice = Audio::kInvalidVoiceHandle;
		}
		m_StatusFeedback = "试听已停止。";
		m_StatusIsError = false;
	}

	void SoundEventEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg);

		if (!s_PendingOpenPath.empty())
		{
			std::string p = s_PendingOpenPath;
			s_PendingOpenPath.clear();
			LoadEvent(p);
		}

		if (m_AvailableSoundFiles.empty() || m_AvailableSliceAssets.empty())
		{
			RefreshAvailableFiles();
			if (m_LoadedPath.empty() && !m_AvailableSoundFiles.empty())
			{
				LoadEvent(m_AvailableSoundFiles[0]);
			}
		}

		if (!m_Event.GetRootContainer())
		{
			m_Event.GetRootContainer() = std::make_shared<Audio::SoundContainerNode>();
			m_Event.GetRootContainer()->type = Audio::ContainerType::Random;
		}

		const float topBarH = 40.0f;
		const float statusBarH = 26.0f;
		const float pad = 6.0f;

		// 1. Top Bar
		const Wui::WuiRect topBarRect { rect.X + pad, rect.Y + pad, rect.W - pad * 2.0f, topBarH };
		RenderTopBar(ctx, topBarRect, theme, host);

		// 2. Middle Content with Sidebar
		const float workY = topBarRect.Y + topBarH + 4.0f;
		const float workH = rect.H - (topBarH + statusBarH + pad * 3.0f);
		m_SidebarWidth = std::clamp(m_SidebarWidth, 180.0f, 400.0f);

		const Wui::WuiRect sideRect { rect.X + pad, workY, m_SidebarWidth, workH };
		const float splitterW = 5.0f;
		const Wui::WuiRect splitterRect { sideRect.X + sideRect.W, workY, splitterW, workH };
		const Wui::WuiRect mainRect { splitterRect.X + splitterW, workY, rect.W - (m_SidebarWidth + splitterW + pad * 2.0f), workH };

		RenderSidebar(ctx, sideRect, theme, host);
		Wui::Splitter(ctx, Wui::HashId("se.split.sidebar"), splitterRect, true, m_SidebarWidth, 180.0f, 400.0f, theme);

		if (m_ViewMode == SoundEventViewMode::Code)
		{
			RenderCodeView(ctx, mainRect, theme, host);
		}
		else
		{
			RenderVisualInspector(ctx, mainRect, theme, host);
		}

		// 3. Bottom Status Bar
		const Wui::WuiRect statusRect { rect.X + pad, rect.Y + rect.H - statusBarH - pad, rect.W - pad * 2.0f, statusBarH };
		Wui::BarSurface(ctx, statusRect, theme.PanelHeader, theme.Border);
		if (!m_StatusFeedback.empty())
		{
			Wui::Label(ctx, { statusRect.X + 10.0f, statusRect.Y + 6.0f }, m_StatusFeedback,
				m_StatusIsError ? theme.Danger : theme.Accent, 11.5f);
		}
		else
		{
			Wui::Label(ctx, { statusRect.X + 10.0f, statusRect.Y + 6.0f }, "就绪。支持在属性检视与 YAML 源码模式之间无缝切换。",
				theme.TextMuted, 11.0f);
		}
	}

	void SoundEventEditorPanel::RenderTopBar(Wui::WuiContext& ctx, const Wui::WuiRect& barRect, const Wui::WuiTheme& theme, PanelHost& host)
	{
		(void)host;
		Wui::BarSurface(ctx, barRect, theme.PanelHeader, theme.Border);

		float cx = barRect.X + 12.0f;
		Wui::Label(ctx, { cx, barRect.Y + 11.0f }, "♫ 声效事件工作台", theme.Accent, 14.0f);
		cx += 140.0f;

		std::string displayPath = m_LoadedPath.empty() ? "(未选择事件)" : m_LoadedPath;
		if (m_Dirty) displayPath += " *";
		Wui::Label(ctx, { cx, barRect.Y + 12.0f }, displayPath, m_Dirty ? theme.Warning : theme.TextMuted, 11.5f);

		float rx = barRect.X + barRect.W - 8.0f;

		rx -= 94.0f;
		const Wui::WuiRect btnExp { rx, barRect.Y + 7.0f, 90.0f, 26.0f };
		Wui::Tooltip(ctx, btnExp, "在 Windows 资源管理器中打开此文件目录并高亮定位");
		if (Wui::Button(ctx, Wui::HashId("se.top.explorer"), btnExp, "定位文件夹", theme))
		{
			std::filesystem::path actual = m_LoadedPath;
			if (!actual.is_absolute()) actual = Paths::AssetRoot() / actual;
			OpenInExplorer(actual);
		}

		rx -= 94.0f;
		const Wui::WuiRect btnSave { rx, barRect.Y + 7.0f, 90.0f, 26.0f };
		Wui::Tooltip(ctx, btnSave, "保存修改到物理磁盘并同步运行态 (Ctrl+S)");
		if (Wui::Button(ctx, Wui::HashId("se.top.save"), btnSave, m_Dirty ? "保存资产 *" : "保存资产", theme))
		{
			SaveEvent();
		}

		rx -= 74.0f;
		const Wui::WuiRect btnStop { rx, barRect.Y + 7.0f, 70.0f, 26.0f };
		Wui::Tooltip(ctx, btnStop, "停止正在试听回放的声音");
		if (Wui::Button(ctx, Wui::HashId("se.top.stop"), btnStop, "■ 停止", theme))
		{
			StopAudition();
		}

		rx -= 126.0f;
		const Wui::WuiRect btnPlay { rx, barRect.Y + 7.0f, 122.0f, 26.0f };
		Wui::Tooltip(ctx, btnPlay, "按当前参数评估音量、音高与空间定位并触发回放试听");
		if (Wui::Button(ctx, Wui::HashId("se.top.play"), btnPlay, "▶ 试听随机回放", theme))
		{
			PlayAudition();
		}

		rx -= 220.0f;
		const Wui::WuiRect segMode { rx, barRect.Y + 7.0f, 212.0f, 26.0f };
		const std::vector<std::string> viewModes = { "属性检视 (Visual)", "YAML 源码 (Code)" };
		int oldMode = m_SelectedViewModeIndex;
		if (Wui::Segmented(ctx, Wui::HashId("se.top.mode_seg"), segMode, viewModes, m_SelectedViewModeIndex, theme))
		{
			if (m_SelectedViewModeIndex != oldMode)
			{
				m_ViewMode = static_cast<SoundEventViewMode>(m_SelectedViewModeIndex);
				if (m_ViewMode == SoundEventViewMode::Code)
				{
					SyncCodeFromEvent();
				}
				else
				{
					ApplyCodeToEvent();
				}
			}
		}
	}

	void SoundEventEditorPanel::RenderSidebar(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme, PanelHost& host)
	{
		(void)host;
		Wui::BarSurface(ctx, rect, theme.ContentBg, theme.Border);

		float curY = rect.Y + 8.0f;
		const float padX = 8.0f;
		const float w = rect.W - padX * 2.0f;

		std::string headerText = "声音事件 (" + std::to_string(m_AvailableSoundFiles.size()) + ")";
		Wui::Label(ctx, { rect.X + padX + 2.0f, curY + 2.0f }, headerText, theme.Accent, 12.0f);
		curY += 24.0f;

		const Wui::WuiRect searchRect { rect.X + padX, curY, w, 24.0f };
		Wui::TextField(ctx, Wui::HashId("se.side.search"), searchRect, m_SearchFilter, theme);
		curY += 28.0f;

		const float listH = rect.H - (curY - rect.Y) - 38.0f;
		const Wui::WuiRect listRect { rect.X + padX, curY, w, std::max(60.0f, listH) };
		Wui::BarSurface(ctx, listRect, theme.PanelBg, theme.Border);

		float itemY = listRect.Y + 4.0f;
		const float itemH = 26.0f;

		std::string lowerFilter = m_SearchFilter;
		std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);

		for (size_t i = 0; i < m_AvailableSoundFiles.size(); ++i)
		{
			if (itemY + itemH > listRect.Y + listRect.H) break;

			const std::string& path = m_AvailableSoundFiles[i];
			std::filesystem::path fp(path);
			std::string stem = fp.stem().string();

			if (!lowerFilter.empty())
			{
				std::string lowerStem = stem;
				std::transform(lowerStem.begin(), lowerStem.end(), lowerStem.begin(), ::tolower);
				if (lowerStem.find(lowerFilter) == std::string::npos) continue;
			}

			bool isSelected = (m_LoadedPath == path || m_Event.GetId() == stem);
			const Wui::WuiRect itemRect { listRect.X + 3.0f, itemY, listRect.W - 6.0f, itemH };

			if (isSelected)
			{
				Wui::PanelBackground(ctx, itemRect, theme.Selection, 3.0f);
			}

			std::string label = "♫ " + stem;
			if (Wui::Button(ctx, Wui::HashId(("se.side.item." + std::to_string(i)).c_str()), itemRect, label, theme))
			{
				LoadEvent(path);
			}

			itemY += itemH + 2.0f;
		}

		const float botY = rect.Y + rect.H - 32.0f;
		const Wui::WuiRect btnNew { rect.X + padX, botY, w - 42.0f, 24.0f };
		Wui::Tooltip(ctx, btnNew, "在 assets/audio/events/ 中新建一个声效事件定义");
		if (Wui::Button(ctx, Wui::HashId("se.side.new"), btnNew, "＋ 新建声效事件", theme))
		{
			NewEvent();
		}

		const Wui::WuiRect btnRef { rect.X + rect.W - padX - 38.0f, botY, 38.0f, 24.0f };
		Wui::Tooltip(ctx, btnRef, "重新扫描工程目录下的所有声音资产");
		if (Wui::Button(ctx, Wui::HashId("se.side.refresh"), btnRef, "↺", theme))
		{
			RefreshAvailableFiles();
			m_StatusFeedback = "已刷新工程声效资产列表。";
		}
	}

	void SoundEventEditorPanel::RenderVisualInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme, PanelHost& host)
	{
		Wui::PanelBackground(ctx, rect, theme.PanelBg);

		const float padX = 14.0f;
		const float cardW = rect.W - padX * 2.0f;
		float curY = rect.Y + 8.0f;

		// Card 1: 基础属性与总线路由 (General & Bus Routing)
		const float c1H = 158.0f;
		const Wui::WuiRect c1Rect { rect.X + padX, curY, cardW, c1H };
		Wui::BarSurface(ctx, c1Rect, theme.ContentBg, theme.Border);

		float c1Y = c1Rect.Y + 8.0f;
		Wui::Label(ctx, { c1Rect.X + 12.0f, c1Y }, "基础属性与音频总线路由 (General & Bus Routing)", theme.Accent, 12.5f);
		c1Y += 24.0f;

		Wui::Label(ctx, { c1Rect.X + 14.0f, c1Y + 3.0f }, "事件 ID:", theme.TextMuted, 11.5f);
		const Wui::WuiRect idFieldRect { c1Rect.X + 80.0f, c1Y, 220.0f, 22.0f };
		if (Wui::TextField(ctx, Wui::HashId("se.prop.id"), idFieldRect, m_EventIdBuffer, theme))
		{
			m_Dirty = true;
		}
		c1Y += 28.0f;

		Wui::Label(ctx, { c1Rect.X + 14.0f, c1Y + 3.0f }, "输出总线:", theme.TextMuted, 11.5f);
		const std::vector<std::string> busOptions = { "Master", "Music", "SFX", "Voice", "UI", "Ambience" };
		const Wui::WuiRect busSegRect { c1Rect.X + 80.0f, c1Y, std::min(480.0f, cardW - 100.0f), 24.0f };
		if (Wui::Segmented(ctx, Wui::HashId("se.prop.bus"), busSegRect, busOptions, m_SelectedBusIndex, theme))
		{
			m_Dirty = true;
		}
		c1Y += 30.0f;

		char vBuf[64];
		std::snprintf(vBuf, sizeof(vBuf), "音量范围 [%.2f ~ %.2f]:", m_MinVolume, m_MaxVolume);
		Wui::Label(ctx, { c1Rect.X + 14.0f, c1Y + 2.0f }, vBuf, theme.TextMuted, 11.0f);
		const float halfSliderW = std::min(180.0f, (cardW - 250.0f) * 0.5f);
		const Wui::WuiRect vMinR { c1Rect.X + 170.0f, c1Y, halfSliderW, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.prop.vmin"), vMinR, m_MinVolume, 0.0f, 1.5f, theme);
		const Wui::WuiRect vMaxR { c1Rect.X + 175.0f + halfSliderW, c1Y, halfSliderW, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.prop.vmax"), vMaxR, m_MaxVolume, 0.0f, 1.5f, theme);
		c1Y += 26.0f;

		char pBuf[64];
		std::snprintf(pBuf, sizeof(pBuf), "音高微调 [%.1fst ~ %.1fst]:", m_MinPitchSemi, m_MaxPitchSemi);
		Wui::Label(ctx, { c1Rect.X + 14.0f, c1Y + 2.0f }, pBuf, theme.TextMuted, 11.0f);
		const Wui::WuiRect pMinR { c1Rect.X + 170.0f, c1Y, halfSliderW, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.prop.pmin"), pMinR, m_MinPitchSemi, -12.0f, 12.0f, theme);
		const Wui::WuiRect pMaxR { c1Rect.X + 175.0f + halfSliderW, c1Y, halfSliderW, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.prop.pmax"), pMaxR, m_MaxPitchSemi, -12.0f, 12.0f, theme);

		curY += c1H + 8.0f;

		// Card 2: 并发控制与抢占策略 (Concurrency & Steal Policy)
		const float c2H = 92.0f;
		const Wui::WuiRect c2Rect { rect.X + padX, curY, cardW, c2H };
		Wui::BarSurface(ctx, c2Rect, theme.ContentBg, theme.Border);

		float c2Y = c2Rect.Y + 8.0f;
		Wui::Label(ctx, { c2Rect.X + 12.0f, c2Y }, "并发限制与声音抢占 (Concurrency & Steal Policy)", theme.Accent, 12.5f);
		c2Y += 24.0f;

		char instStr[32], cdStr[32];
		std::snprintf(instStr, sizeof(instStr), "最大发声数: %d", m_MaxInstances);
		Wui::Label(ctx, { c2Rect.X + 14.0f, c2Y + 2.0f }, instStr, theme.TextMuted, 11.0f);
		float fInst = static_cast<float>(m_MaxInstances);
		const Wui::WuiRect sInst { c2Rect.X + 110.0f, c2Y, 120.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.prop.maxinst"), sInst, fInst, 1.0f, 32.0f, theme);
		m_MaxInstances = static_cast<int>(fInst);

		std::snprintf(cdStr, sizeof(cdStr), "冷却时间: %.2fs", m_CooldownSeconds);
		Wui::Label(ctx, { c2Rect.X + 250.0f, c2Y + 2.0f }, cdStr, theme.TextMuted, 11.0f);
		const Wui::WuiRect sCd { c2Rect.X + 340.0f, c2Y, 120.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.prop.cd"), sCd, m_CooldownSeconds, 0.0f, 1.0f, theme);
		c2Y += 26.0f;

		Wui::Label(ctx, { c2Rect.X + 14.0f, c2Y + 3.0f }, "抢占策略:", theme.TextMuted, 11.0f);
		const std::vector<std::string> stealOptions = { "抢占最老 (Steal Oldest)", "抢占最弱 (Steal Quiet)", "拒绝触发 (Reject)" };
		const Wui::WuiRect stealSegRect { c2Rect.X + 110.0f, c2Y, std::min(450.0f, cardW - 130.0f), 24.0f };
		Wui::Segmented(ctx, Wui::HashId("se.prop.steal"), stealSegRect, stealOptions, m_StealPolicyIndex, theme);

		curY += c2H + 8.0f;

		// Card 3: 3D 空间化与距离衰减 (3D Spatialization & Attenuation)
		const float c3H = 118.0f;
		const Wui::WuiRect c3Rect { rect.X + padX, curY, cardW, c3H };
		Wui::BarSurface(ctx, c3Rect, theme.ContentBg, theme.Border);

		float c3Y = c3Rect.Y + 8.0f;
		Wui::Label(ctx, { c3Rect.X + 12.0f, c3Y }, "空间化与距离衰减 (3D Spatialization & Attenuation)", theme.Accent, 12.5f);
		c3Y += 24.0f;

		Wui::Label(ctx, { c3Rect.X + 14.0f, c3Y + 3.0f }, "空间模式:", theme.TextMuted, 11.0f);
		const std::vector<std::string> spatialOptions = { "2D 声像平移 (2D Pan)", "3D 物理空间定位 (3D Spatial)", "居中立体声 (Center 2D)" };
		const Wui::WuiRect spatSegRect { c3Rect.X + 80.0f, c3Y, std::min(480.0f, cardW - 100.0f), 24.0f };
		Wui::Segmented(ctx, Wui::HashId("se.prop.spatial"), spatSegRect, spatialOptions, m_SpatialModeIndex, theme);
		c3Y += 28.0f;

		Wui::Label(ctx, { c3Rect.X + 14.0f, c3Y + 3.0f }, "衰减曲线:", theme.TextMuted, 11.0f);
		const std::vector<std::string> attenOptions = { "平方反比 (Inverse Square)", "线性衰减 (Linear)", "对数衰减 (Logarithmic)" };
		const Wui::WuiRect attenSegRect { c3Rect.X + 80.0f, c3Y, std::min(480.0f, cardW - 100.0f), 24.0f };
		Wui::Segmented(ctx, Wui::HashId("se.prop.atten"), attenSegRect, attenOptions, m_AttenuationModelIndex, theme);
		c3Y += 28.0f;

		char dMinStr[32], dMaxStr[32];
		std::snprintf(dMinStr, sizeof(dMinStr), "近距: %.1fm", m_MinDist);
		Wui::Label(ctx, { c3Rect.X + 14.0f, c3Y + 2.0f }, dMinStr, theme.TextMuted, 10.5f);
		const Wui::WuiRect rMinD { c3Rect.X + 80.0f, c3Y, 130.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.prop.mind"), rMinD, m_MinDist, 0.1f, 15.0f, theme);

		std::snprintf(dMaxStr, sizeof(dMaxStr), "远距: %.1fm", m_MaxDist);
		Wui::Label(ctx, { c3Rect.X + 230.0f, c3Y + 2.0f }, dMaxStr, theme.TextMuted, 10.5f);
		const Wui::WuiRect rMaxD { c3Rect.X + 300.0f, c3Y, 150.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.prop.maxd"), rMaxD, m_MaxDist, 1.0f, 100.0f, theme);

		curY += c3H + 8.0f;

		// Card 4: 动态容器与音轨 (Dynamic Container & Tracks)
		const float c4H = std::max(220.0f, rect.H - (curY - rect.Y) - 10.0f);
		const Wui::WuiRect c4Rect { rect.X + padX, curY, cardW, c4H };
		Wui::BarSurface(ctx, c4Rect, theme.ContentBg, theme.Border);

		float c4Y = c4Rect.Y + 8.0f;
		Wui::Label(ctx, { c4Rect.X + 12.0f, c4Y }, "动态容器逻辑架构与切片 (Container Architecture & Slices)", { 0.35f, 0.75f, 0.95f, 1.0f }, 12.5f);
		c4Y += 24.0f;

		Wui::Label(ctx, { c4Rect.X + 14.0f, c4Y + 3.0f }, "容器模式:", theme.TextMuted, 11.0f);
		const std::vector<std::string> containerTypes = { "Single (单曲)", "Random (随机)", "Sequence (顺序)", "Switch (分支)", "Blend (混合)", "Layer (分层)" };
		const Wui::WuiRect cTypeSeg { c4Rect.X + 80.0f, c4Y, std::min(600.0f, cardW - 100.0f), 24.0f };
		if (Wui::Segmented(ctx, Wui::HashId("se.prop.cnt_type"), cTypeSeg, containerTypes, m_SelectedContainerType, theme))
		{
			if (m_Event.GetRootContainer())
				m_Event.GetRootContainer()->type = static_cast<Audio::ContainerType>(m_SelectedContainerType);
			m_Dirty = true;
		}
		c4Y += 32.0f;

		auto root = m_Event.GetRootContainer();

		// Sub-View: Random / Single / Sequence
		if (m_SelectedContainerType == 1 || m_SelectedContainerType == 0 || m_SelectedContainerType == 2)
		{
			if (m_SelectedContainerType == 1)
			{
				const Wui::WuiRect chkShuffle { c4Rect.X + 14.0f, c4Y, 140.0f, 20.0f };
				Wui::Checkbox(ctx, Wui::HashId("se.prop.shuffle"), chkShuffle, "随机不重复 (Shuffle)", m_ShuffleNoRepeat, theme);
				root->shuffleNoRepeat = m_ShuffleNoRepeat;
				c4Y += 24.0f;
			}

			auto& vars = root->variations;
			float totalWeight = 0.0f;
			for (const auto& v : vars) totalWeight += v.weight;
			if (totalWeight < 1e-4f) totalWeight = 1.0f;

			int removeIdx = -1;
			for (size_t vi = 0; vi < vars.size(); ++vi)
			{
				if (c4Y + 28.0f > c4Rect.Y + c4Rect.H - 40.0f) break;

				const Wui::WuiRect rowRect { c4Rect.X + 12.0f, c4Y, c4Rect.W - 24.0f, 26.0f };
				Wui::PanelBackground(ctx, rowRect, theme.PanelBg, 3.0f);

				Wui::Label(ctx, { rowRect.X + 8.0f, rowRect.Y + 5.0f }, vars[vi].soundAssetPath, theme.Text, 11.5f);

				float pct = (vars[vi].weight / totalWeight) * 100.0f;
				char wStr[32];
				std::snprintf(wStr, sizeof(wStr), "权重 %.1f (%.0f%%)", vars[vi].weight, pct);
				Wui::Label(ctx, { rowRect.X + rowRect.W - 310.0f, rowRect.Y + 5.0f }, wStr, theme.TextMuted, 10.5f);

				const Wui::WuiRect wSlider { rowRect.X + rowRect.W - 220.0f, rowRect.Y + 4.0f, 75.0f, 18.0f };
				Wui::SliderFloat(ctx, Wui::HashId(("se.v.w." + std::to_string(vi)).c_str()), wSlider, vars[vi].weight, 0.1f, 5.0f, theme);

				const Wui::WuiRect btnPick { rowRect.X + rowRect.W - 138.0f, rowRect.Y + 3.0f, 32.0f, 20.0f };
				Wui::Tooltip(ctx, btnPick, "在工程资产库中选择音频切片替换此项");
				if (Wui::Button(ctx, Wui::HashId(("se.v.pick." + std::to_string(vi)).c_str()), btnPick, "...", theme))
				{
					host.OpenAssetPicker("选择音频切片资产 (WAV / OGG / MP3 / WAUDIO)", AssetCategory::Unknown, [this, vi](const std::string& picked) {
						if (m_Event.GetRootContainer() && vi < m_Event.GetRootContainer()->variations.size())
						{
							m_Event.GetRootContainer()->variations[vi].soundAssetPath = picked;
							m_Dirty = true;
						}
					});
				}

				const Wui::WuiRect btnPreview { rowRect.X + rowRect.W - 100.0f, rowRect.Y + 3.0f, 50.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(("se.v.play." + std::to_string(vi)).c_str()), btnPreview, "▶ 试听", theme))
				{
					StopAudition();
					auto buf = EnsureSliceBuffer(vars[vi].soundAssetPath, vi);
					m_CurrentAuditionVoice = Audio::AudioSystem::Get().GetVoicePool().AllocateVoice(
						buf, static_cast<Audio::BusId>(m_SelectedBusIndex), 1.0f, 1.0f, false);
					m_StatusFeedback = "单切片试听: " + vars[vi].soundAssetPath;
				}

				const Wui::WuiRect btnDelete { rowRect.X + rowRect.W - 44.0f, rowRect.Y + 3.0f, 38.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(("se.v.del." + std::to_string(vi)).c_str()), btnDelete, "✖", theme))
				{
					removeIdx = static_cast<int>(vi);
				}

				c4Y += 28.0f;
			}

			if (removeIdx >= 0 && removeIdx < static_cast<int>(vars.size()))
			{
				vars.erase(vars.begin() + removeIdx);
				m_Dirty = true;
			}

			// Add variation toolbar
			if (c4Y + 28.0f <= c4Rect.Y + c4Rect.H)
			{
				const Wui::WuiRect comboField { c4Rect.X + 12.0f, c4Y + 2.0f, std::max(200.0f, cardW - 270.0f), 24.0f };
				Wui::SearchableCombo(ctx, Wui::HashId("se.add.combo"), comboField, "", m_AvailableSliceAssets, m_SelectedSliceAssetIndex, theme);

				const Wui::WuiRect btnAdd { c4Rect.X + cardW - 246.0f, c4Y + 2.0f, 120.0f, 24.0f };
				if (Wui::Button(ctx, Wui::HashId("se.btn.add"), btnAdd, "＋ 添加所选切片", theme))
				{
					if (m_SelectedSliceAssetIndex >= 0 && m_SelectedSliceAssetIndex < static_cast<int>(m_AvailableSliceAssets.size()))
					{
						vars.push_back({ m_AvailableSliceAssets[m_SelectedSliceAssetIndex], 1.0f });
						m_Dirty = true;
					}
				}

				const Wui::WuiRect btnBrowse { c4Rect.X + cardW - 118.0f, c4Y + 2.0f, 106.0f, 24.0f };
				if (Wui::Button(ctx, Wui::HashId("se.btn.browse"), btnBrowse, "浏览资产库...", theme))
				{
					host.OpenAssetPicker("选择音频切片资产", AssetCategory::Unknown, [this](const std::string& picked) {
						if (m_Event.GetRootContainer())
						{
							m_Event.GetRootContainer()->variations.push_back({ picked, 1.0f });
							m_Dirty = true;
						}
					});
				}
			}
		}
		// Sub-View: Switch Container (按参数分支切换)
		else if (m_SelectedContainerType == 3)
		{
			Wui::Label(ctx, { c4Rect.X + 14.0f, c4Y + 3.0f }, "切换参数名 (Switch Param):", theme.TextMuted, 11.5f);
			const Wui::WuiRect paramField { c4Rect.X + 190.0f, c4Y, 200.0f, 22.0f };
			Wui::TextField(ctx, Wui::HashId("se.switch.param"), paramField, root->switchParamName, theme);
			c4Y += 28.0f;

			std::string removeBranch;
			int bIdx = 0;
			for (auto& [bName, bNode] : root->switchBranches)
			{
				if (c4Y + 28.0f > c4Rect.Y + c4Rect.H - 40.0f) break;

				const Wui::WuiRect rowRect { c4Rect.X + 12.0f, c4Y, c4Rect.W - 24.0f, 26.0f };
				Wui::PanelBackground(ctx, rowRect, theme.PanelBg, 3.0f);

				Wui::Label(ctx, { rowRect.X + 8.0f, rowRect.Y + 5.0f }, "分支 [" + bName + "]:", theme.Accent, 11.5f);

				std::string p = (bNode && !bNode->variations.empty()) ? bNode->variations[0].soundAssetPath : "(未指定声音)";
				Wui::Label(ctx, { rowRect.X + 160.0f, rowRect.Y + 5.0f }, p, theme.Text, 11.0f);

				const Wui::WuiRect btnPick { rowRect.X + rowRect.W - 138.0f, rowRect.Y + 3.0f, 32.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(("se.sw.pick." + std::to_string(bIdx)).c_str()), btnPick, "...", theme))
				{
					std::string capturedName = bName;
					host.OpenAssetPicker("选择分支音频资产", AssetCategory::Unknown, [this, capturedName](const std::string& picked) {
						if (m_Event.GetRootContainer() && m_Event.GetRootContainer()->switchBranches.count(capturedName))
						{
							auto node = m_Event.GetRootContainer()->switchBranches[capturedName];
							if (node)
							{
								node->variations.clear();
								node->variations.push_back({ picked, 1.0f });
								m_Dirty = true;
							}
						}
					});
				}

				const Wui::WuiRect btnPreview { rowRect.X + rowRect.W - 100.0f, rowRect.Y + 3.0f, 50.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(("se.sw.play." + std::to_string(bIdx)).c_str()), btnPreview, "▶ 试听", theme))
				{
					if (!p.empty() && p != "(未指定声音)")
					{
						StopAudition();
						auto buf = EnsureSliceBuffer(p, bIdx);
						m_CurrentAuditionVoice = Audio::AudioSystem::Get().GetVoicePool().AllocateVoice(
							buf, static_cast<Audio::BusId>(m_SelectedBusIndex), 1.0f, 1.0f, false);
					}
				}

				const Wui::WuiRect btnDel { rowRect.X + rowRect.W - 44.0f, rowRect.Y + 3.0f, 38.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(("se.sw.del." + std::to_string(bIdx)).c_str()), btnDel, "✖", theme))
				{
					removeBranch = bName;
				}

				c4Y += 28.0f;
				bIdx++;
			}

			if (!removeBranch.empty())
			{
				root->switchBranches.erase(removeBranch);
				m_Dirty = true;
			}

			if (c4Y + 28.0f <= c4Rect.Y + c4Rect.H)
			{
				Wui::Label(ctx, { c4Rect.X + 14.0f, c4Y + 3.0f }, "新分支名:", theme.TextMuted, 11.0f);
				const Wui::WuiRect branchField { c4Rect.X + 75.0f, c4Y + 2.0f, 110.0f, 22.0f };
				Wui::TextField(ctx, Wui::HashId("se.sw.new_name"), branchField, m_NewBranchName, theme);

				const Wui::WuiRect comboField { c4Rect.X + 195.0f, c4Y + 2.0f, std::max(160.0f, cardW - 350.0f), 22.0f };
				Wui::SearchableCombo(ctx, Wui::HashId("se.sw.new_combo"), comboField, "", m_AvailableSliceAssets, m_SelectedSliceAssetIndex, theme);

				const Wui::WuiRect btnAdd { c4Rect.X + cardW - 140.0f, c4Y + 2.0f, 128.0f, 22.0f };
				if (Wui::Button(ctx, Wui::HashId("se.sw.btn_add"), btnAdd, "＋ 添加参数分支", theme))
				{
					if (!m_NewBranchName.empty() && m_SelectedSliceAssetIndex < static_cast<int>(m_AvailableSliceAssets.size()))
					{
						auto sub = std::make_shared<Audio::SoundContainerNode>();
						sub->type = Audio::ContainerType::Single;
						sub->variations.push_back({ m_AvailableSliceAssets[m_SelectedSliceAssetIndex], 1.0f });
						root->switchBranches[m_NewBranchName] = sub;
						m_Dirty = true;
					}
				}
			}
		}
		// Sub-View: Blend Container (连续数值参数平滑混合)
		else if (m_SelectedContainerType == 4)
		{
			Wui::Label(ctx, { c4Rect.X + 14.0f, c4Y + 3.0f }, "混合参数名 (Blend Param):", theme.TextMuted, 11.5f);
			const Wui::WuiRect paramField { c4Rect.X + 190.0f, c4Y, 200.0f, 22.0f };
			Wui::TextField(ctx, Wui::HashId("se.blend.param"), paramField, root->blendParamName, theme);
			c4Y += 28.0f;

			int removePtIdx = -1;
			for (size_t pi = 0; pi < root->blendPoints.size(); ++pi)
			{
				if (c4Y + 28.0f > c4Rect.Y + c4Rect.H - 40.0f) break;

				const Wui::WuiRect rowRect { c4Rect.X + 12.0f, c4Y, c4Rect.W - 24.0f, 26.0f };
				Wui::PanelBackground(ctx, rowRect, theme.PanelBg, 3.0f);

				char thStr[32];
				std::snprintf(thStr, sizeof(thStr), "阈值: %.1f", root->blendPoints[pi].first);
				Wui::Label(ctx, { rowRect.X + 8.0f, rowRect.Y + 5.0f }, thStr, theme.Accent, 11.5f);

				auto subNode = root->blendPoints[pi].second;
				std::string p = (subNode && !subNode->variations.empty()) ? subNode->variations[0].soundAssetPath : "(未指定声音)";
				Wui::Label(ctx, { rowRect.X + 140.0f, rowRect.Y + 5.0f }, p, theme.Text, 11.0f);

				const Wui::WuiRect btnPick { rowRect.X + rowRect.W - 138.0f, rowRect.Y + 3.0f, 32.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(("se.bl.pick." + std::to_string(pi)).c_str()), btnPick, "...", theme))
				{
					host.OpenAssetPicker("选择混合点音频", AssetCategory::Unknown, [this, pi](const std::string& picked) {
						if (m_Event.GetRootContainer() && pi < m_Event.GetRootContainer()->blendPoints.size())
						{
							auto node = m_Event.GetRootContainer()->blendPoints[pi].second;
							if (node)
							{
								node->variations.clear();
								node->variations.push_back({ picked, 1.0f });
								m_Dirty = true;
							}
						}
					});
				}

				const Wui::WuiRect btnPreview { rowRect.X + rowRect.W - 100.0f, rowRect.Y + 3.0f, 50.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(("se.bl.play." + std::to_string(pi)).c_str()), btnPreview, "▶ 试听", theme))
				{
					if (!p.empty() && p != "(未指定声音)")
					{
						StopAudition();
						auto buf = EnsureSliceBuffer(p, pi);
						m_CurrentAuditionVoice = Audio::AudioSystem::Get().GetVoicePool().AllocateVoice(
							buf, static_cast<Audio::BusId>(m_SelectedBusIndex), 1.0f, 1.0f, false);
					}
				}

				const Wui::WuiRect btnDel { rowRect.X + rowRect.W - 44.0f, rowRect.Y + 3.0f, 38.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(("se.bl.del." + std::to_string(pi)).c_str()), btnDel, "✖", theme))
				{
					removePtIdx = static_cast<int>(pi);
				}

				c4Y += 28.0f;
			}

			if (removePtIdx >= 0 && removePtIdx < static_cast<int>(root->blendPoints.size()))
			{
				root->blendPoints.erase(root->blendPoints.begin() + removePtIdx);
				m_Dirty = true;
			}

			if (c4Y + 28.0f <= c4Rect.Y + c4Rect.H)
			{
				char thEditStr[32];
				std::snprintf(thEditStr, sizeof(thEditStr), "阈值: %.1f", m_NewBlendThreshold);
				Wui::Label(ctx, { c4Rect.X + 14.0f, c4Y + 3.0f }, thEditStr, theme.TextMuted, 10.5f);
				const Wui::WuiRect sTh { c4Rect.X + 85.0f, c4Y + 2.0f, 110.0f, 18.0f };
				Wui::SliderFloat(ctx, Wui::HashId("se.bl.new_th"), sTh, m_NewBlendThreshold, 0.0f, 8000.0f, theme);

				const Wui::WuiRect comboField { c4Rect.X + 205.0f, c4Y + 2.0f, std::max(160.0f, cardW - 355.0f), 22.0f };
				Wui::SearchableCombo(ctx, Wui::HashId("se.bl.new_combo"), comboField, "", m_AvailableSliceAssets, m_SelectedSliceAssetIndex, theme);

				const Wui::WuiRect btnAdd { c4Rect.X + cardW - 138.0f, c4Y + 2.0f, 126.0f, 22.0f };
				if (Wui::Button(ctx, Wui::HashId("se.bl.btn_add"), btnAdd, "＋ 添加混合点", theme))
				{
					if (m_SelectedSliceAssetIndex < static_cast<int>(m_AvailableSliceAssets.size()))
					{
						auto sub = std::make_shared<Audio::SoundContainerNode>();
						sub->type = Audio::ContainerType::Single;
						sub->variations.push_back({ m_AvailableSliceAssets[m_SelectedSliceAssetIndex], 1.0f });
						root->blendPoints.push_back({ m_NewBlendThreshold, sub });
						std::sort(root->blendPoints.begin(), root->blendPoints.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
						m_Dirty = true;
					}
				}
			}
		}
		// Sub-View: Layer Container (多音轨多层同时并行发声)
		else if (m_SelectedContainerType == 5)
		{
			Wui::Label(ctx, { c4Rect.X + 14.0f, c4Y }, "并行多层音轨 (Simultaneous Layers - 每次触发同时播放所有子层):", theme.TextMuted, 11.5f);
			c4Y += 24.0f;

			int removeLayerIdx = -1;
			for (size_t li = 0; li < root->layerChildren.size(); ++li)
			{
				if (c4Y + 28.0f > c4Rect.Y + c4Rect.H - 40.0f) break;

				const Wui::WuiRect rowRect { c4Rect.X + 12.0f, c4Y, c4Rect.W - 24.0f, 26.0f };
				Wui::PanelBackground(ctx, rowRect, theme.PanelBg, 3.0f);

				char lNum[32];
				std::snprintf(lNum, sizeof(lNum), "分层 #%d", static_cast<int>(li + 1));
				Wui::Label(ctx, { rowRect.X + 8.0f, rowRect.Y + 5.0f }, lNum, theme.Accent, 11.5f);

				auto subNode = root->layerChildren[li].second;
				std::string p = (subNode && !subNode->variations.empty()) ? subNode->variations[0].soundAssetPath : "(未指定声音)";
				Wui::Label(ctx, { rowRect.X + 90.0f, rowRect.Y + 5.0f }, p, theme.Text, 11.0f);

				char volStr[32];
				std::snprintf(volStr, sizeof(volStr), "音量: %.2f", root->layerChildren[li].first);
				Wui::Label(ctx, { rowRect.X + rowRect.W - 270.0f, rowRect.Y + 5.0f }, volStr, theme.TextMuted, 10.5f);

				const Wui::WuiRect sVol { rowRect.X + rowRect.W - 200.0f, rowRect.Y + 4.0f, 58.0f, 18.0f };
				Wui::SliderFloat(ctx, Wui::HashId(("se.lay.v." + std::to_string(li)).c_str()), sVol, root->layerChildren[li].first, 0.0f, 2.0f, theme);

				const Wui::WuiRect btnPick { rowRect.X + rowRect.W - 138.0f, rowRect.Y + 3.0f, 32.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(("se.lay.pick." + std::to_string(li)).c_str()), btnPick, "...", theme))
				{
					host.OpenAssetPicker("选择音轨切片", AssetCategory::Unknown, [this, li](const std::string& picked) {
						if (m_Event.GetRootContainer() && li < m_Event.GetRootContainer()->layerChildren.size())
						{
							auto node = m_Event.GetRootContainer()->layerChildren[li].second;
							if (node)
							{
								node->variations.clear();
								node->variations.push_back({ picked, 1.0f });
								m_Dirty = true;
							}
						}
					});
				}

				const Wui::WuiRect btnPreview { rowRect.X + rowRect.W - 100.0f, rowRect.Y + 3.0f, 50.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(("se.lay.play." + std::to_string(li)).c_str()), btnPreview, "▶ 试听", theme))
				{
					if (!p.empty() && p != "(未指定声音)")
					{
						StopAudition();
						auto buf = EnsureSliceBuffer(p, li);
						m_CurrentAuditionVoice = Audio::AudioSystem::Get().GetVoicePool().AllocateVoice(
							buf, static_cast<Audio::BusId>(m_SelectedBusIndex), root->layerChildren[li].first, 1.0f, false);
					}
				}

				const Wui::WuiRect btnDel { rowRect.X + rowRect.W - 44.0f, rowRect.Y + 3.0f, 38.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId(("se.lay.del." + std::to_string(li)).c_str()), btnDel, "✖", theme))
				{
					removeLayerIdx = static_cast<int>(li);
				}

				c4Y += 28.0f;
			}

			if (removeLayerIdx >= 0 && removeLayerIdx < static_cast<int>(root->layerChildren.size()))
			{
				root->layerChildren.erase(root->layerChildren.begin() + removeLayerIdx);
				m_Dirty = true;
			}

			if (c4Y + 28.0f <= c4Rect.Y + c4Rect.H)
			{
				const Wui::WuiRect comboField { c4Rect.X + 12.0f, c4Y + 2.0f, std::max(200.0f, cardW - 200.0f), 22.0f };
				Wui::SearchableCombo(ctx, Wui::HashId("se.lay.new_combo"), comboField, "", m_AvailableSliceAssets, m_SelectedSliceAssetIndex, theme);

				const Wui::WuiRect btnAdd { c4Rect.X + cardW - 176.0f, c4Y + 2.0f, 164.0f, 22.0f };
				if (Wui::Button(ctx, Wui::HashId("se.lay.btn_add"), btnAdd, "＋ 添加并行层音轨", theme))
				{
					if (m_SelectedSliceAssetIndex < static_cast<int>(m_AvailableSliceAssets.size()))
					{
						auto sub = std::make_shared<Audio::SoundContainerNode>();
						sub->type = Audio::ContainerType::Single;
						sub->variations.push_back({ m_AvailableSliceAssets[m_SelectedSliceAssetIndex], 1.0f });
						root->layerChildren.push_back({ 1.0f, sub });
						m_Dirty = true;
					}
				}
			}
		}
	}

	void SoundEventEditorPanel::RenderCodeView(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme, PanelHost& host)
	{
		(void)host;
		const float subBarH = 34.0f;
		const Wui::WuiRect subBar { rect.X, rect.Y, rect.W, subBarH };
		Wui::BarSurface(ctx, subBar, theme.PanelHeader, theme.Border);

		float cx = subBar.X + 8.0f;
		const Wui::WuiRect btnApply { cx, subBar.Y + 5.0f, 170.0f, 24.0f };
		Wui::Tooltip(ctx, btnApply, "解析当前代码声明并热更新至声音引擎 (Ctrl+Enter)");
		if (Wui::Button(ctx, Wui::HashId("se.code.apply"), btnApply, "✓ 应用修改 (Ctrl+Enter)", theme))
		{
			ApplyCodeToEvent();
		}
		cx += 176.0f;

		const Wui::WuiRect btnRevert { cx, subBar.Y + 5.0f, 140.0f, 24.0f };
		Wui::Tooltip(ctx, btnRevert, "丢弃未应用的文本修改，重新从事件属性生成 YAML");
		if (Wui::Button(ctx, Wui::HashId("se.code.revert"), btnRevert, "↺ 从检视重新生成", theme))
		{
			SyncCodeFromEvent();
		}
		cx += 148.0f;

		if (!m_CodeStatusFeedback.empty())
		{
			Wui::Label(ctx, { cx, subBar.Y + 9.0f }, m_CodeStatusFeedback,
				m_CodeStatusIsError ? theme.Danger : theme.Accent, 11.5f);
		}
		else
		{
			Wui::Label(ctx, { cx, subBar.Y + 9.0f }, "提示: 可直接手写/粘贴完整声音事件 YAML 代码，满足任意复杂自定义需求",
				theme.TextMuted, 10.5f);
		}

		// Interactive Code Editor
		const Wui::WuiRect codeBox { rect.X, rect.Y + subBarH, rect.W, rect.H - subBarH };
		Wui::WuiCodeEditorOptions options;
		options.FontSize = 13.5f;
		options.LineHeight = 20.0f;

		// Resilient YAML syntax highlighter
		options.Highlight = [](std::string_view line, std::vector<Wui::WuiCodeToken>& out) {
			size_t i = 0;
			while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
			if (i < line.size() && line[i] == '#')
			{
				out.push_back({ static_cast<uint32_t>(i), static_cast<uint32_t>(line.size()), Wui::WuiCodeTokenKind::Comment });
				return;
			}
			auto colonPos = line.find(':');
			if (colonPos != std::string_view::npos)
			{
				out.push_back({ static_cast<uint32_t>(i), static_cast<uint32_t>(colonPos), Wui::WuiCodeTokenKind::Keyword });
				out.push_back({ static_cast<uint32_t>(colonPos), static_cast<uint32_t>(colonPos + 1), Wui::WuiCodeTokenKind::Operator });
				size_t valStart = colonPos + 1;
				while (valStart < line.size() && (line[valStart] == ' ' || line[valStart] == '\t')) valStart++;
				if (valStart < line.size())
				{
					if (line[valStart] == '"')
						out.push_back({ static_cast<uint32_t>(valStart), static_cast<uint32_t>(line.size()), Wui::WuiCodeTokenKind::String });
					else if (std::isdigit(static_cast<unsigned char>(line[valStart])) || line[valStart] == '-' || line[valStart] == '[')
						out.push_back({ static_cast<uint32_t>(valStart), static_cast<uint32_t>(line.size()), Wui::WuiCodeTokenKind::Number });
					else
						out.push_back({ static_cast<uint32_t>(valStart), static_cast<uint32_t>(line.size()), Wui::WuiCodeTokenKind::Default });
				}
			}
		};

		const Wui::WuiId editorStateId = Wui::HashId("se.code.editor_state");
		Wui::CodeEditor(ctx, editorStateId, codeBox, m_CodeBuffer, options);
	}
}
