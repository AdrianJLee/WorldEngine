#include "wldpch.h"
#include "WUI/Panels/SoundEventEditorPanel.h"
#include "World/Audio/AudioCooker.h"
#include "World/Audio/AudioSystem.h"
#include "World/Audio/AudioTypes.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

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
			const std::wstring parameters = L"\"" + dir.wstring() + L"\"";
			ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
		}
#endif
	}

	static std::shared_ptr<Audio::AudioBuffer> EnsureSliceBuffer(const std::string& path, size_t variationSeed = 0)
	{
		auto& audio = Audio::AudioSystem::Get();
		auto buf = audio.FindAudioBuffer(path);
		if (buf) return buf;

		std::error_code ec;
		if (std::filesystem::exists(path, ec))
		{
			if (path.rfind(".waudio") != std::string::npos)
			{
				buf = Audio::AudioCooker::LoadCookedFile(path);
			}
			else if (path.rfind(".wav") != std::string::npos)
			{
				Audio::AudioBuffer tmp;
				if (Audio::AudioCooker::LoadWavFile(path, tmp))
				{
					buf = std::make_shared<Audio::AudioBuffer>(std::move(tmp));
				}
			}
		}

		if (!buf)
		{
			float freq = 440.0f + static_cast<float>(variationSeed) * 80.0f;
			buf = std::make_shared<Audio::AudioBuffer>(
				Audio::AudioBuffer::GenerateSineWave(freq, 0.22f, 0.45f, 48000));
		}

		audio.RegisterAudioBuffer(path, buf);
		return buf;
	}

	void SoundEventEditorPanel::RefreshAvailableFiles()
	{
		m_AvailableSoundFiles.clear();
		m_AvailableSliceAssets.clear();

		const char* searchDirs[] = {
			"projects/assets/audio/events",
			"projects/assets/audio",
			"assets/audio/events",
			"assets/audio",
			"templates/project-example/assets/audio/events",
			"templates/project-example/assets/audio"
		};

		for (const char* dir : searchDirs)
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

		if (m_AvailableSoundFiles.empty())
		{
			std::error_code ec;
			if (std::filesystem::exists("templates/project-example/assets/audio/events/footstep_player.wsound", ec))
			{
				m_AvailableSoundFiles.push_back("templates/project-example/assets/audio/events/footstep_player.wsound");
				m_AvailableSoundFiles.push_back("templates/project-example/assets/audio/events/ui_click.wsound");
			}
			else
			{
				m_AvailableSoundFiles.push_back("projects/assets/audio/events/footstep_player.wsound");
				m_AvailableSoundFiles.push_back("projects/assets/audio/events/ui_click.wsound");
			}
		}

		// Fallback slice assets
		auto addSlice = [this](const std::string& p) {
			if (std::find(m_AvailableSliceAssets.begin(), m_AvailableSliceAssets.end(), p) == m_AvailableSliceAssets.end())
			{
				m_AvailableSliceAssets.push_back(p);
			}
		};
		addSlice("assets/audio/sfx/footstep_grass.waudio");
		addSlice("assets/audio/sfx/ui_click.waudio");
		addSlice("assets/audio/sfx/impact_metal.waudio");
		addSlice("assets/audio/sfx/footstep_stone.waudio");
		addSlice("assets/audio/sfx/footstep_wood.waudio");

		if (m_SelectedSliceAssetIndex < 0 || m_SelectedSliceAssetIndex >= static_cast<int>(m_AvailableSliceAssets.size()))
		{
			m_SelectedSliceAssetIndex = 0;
		}
	}

	void SoundEventEditorPanel::LoadEvent(const std::string& path)
	{
		m_LoadedPath = path;
		std::ifstream in(path);
		if (in.is_open())
		{
			std::stringstream ss;
			ss << in.rdbuf();
			m_Event.DeserializeYaml(ss.str());
		}
		else
		{
			m_Event = Audio::SoundEvent("New_SoundEvent", Audio::kBusSfx);
		}

		if (!m_Event.GetRootContainer())
		{
			m_Event.GetRootContainer() = std::make_shared<Audio::SoundContainerNode>();
			m_Event.GetRootContainer()->type = Audio::ContainerType::Random;
		}

		m_SelectedBusIndex = static_cast<int>(m_Event.GetBusId());
		if (m_SelectedBusIndex < 0 || m_SelectedBusIndex >= 5) m_SelectedBusIndex = 2;

		m_Is3D = m_Event.GetSpatial().is3D;
		m_MinDist = m_Event.GetSpatial().minDistance;
		m_MaxDist = m_Event.GetSpatial().maxDistance;
		m_MaxInstances = static_cast<int>(m_Event.GetMaxInstances());
		m_CooldownSeconds = m_Event.GetCooldownSeconds();
		m_StealPolicyIndex = static_cast<int>(m_Event.GetStealPolicy());
		if (m_Event.GetSpatial().mode == Audio::SpatialMode::Spatial2D) m_SpatialModeIndex = 0; else if (m_Event.GetSpatial().is3D || m_Event.GetSpatial().mode == Audio::SpatialMode::Spatial3D) m_SpatialModeIndex = 1; else m_SpatialModeIndex = 2;
		if (m_Event.GetRootContainer()) m_SelectedContainerType = static_cast<int>(m_Event.GetRootContainer()->type);
		m_StatusFeedback = "已载入声音事件资产: " + path;

		EnsureBuffersForEvent();
	}

	void SoundEventEditorPanel::EnsureBuffersForEvent()
	{
		if (!m_Event.GetRootContainer()) return;
		auto& vars = m_Event.GetRootContainer()->variations;
		for (size_t i = 0; i < vars.size(); ++i)
		{
			EnsureSliceBuffer(vars[i].soundAssetPath, i);
		}
	}

	void SoundEventEditorPanel::SaveEvent()
	{
		if (m_LoadedPath.empty())
		{
			m_LoadedPath = "projects/assets/audio/events/" + m_Event.GetId() + ".wsound";
		}

		m_Event.SetBusId(static_cast<Audio::BusId>(m_SelectedBusIndex));
		m_Event.SetMaxInstances(static_cast<uint32_t>(std::max(1, m_MaxInstances)));
		m_Event.SetCooldownSeconds(std::max(0.0f, m_CooldownSeconds));
		m_Event.SetStealPolicy(static_cast<Audio::StealPolicy>(m_StealPolicyIndex));
		if (m_SpatialModeIndex == 0) { m_Event.GetSpatial().mode = Audio::SpatialMode::Spatial2D; m_Event.GetSpatial().is3D = false; } else if (m_SpatialModeIndex == 1) { m_Event.GetSpatial().mode = Audio::SpatialMode::Spatial3D; m_Event.GetSpatial().is3D = true; } else { m_Event.GetSpatial().mode = Audio::SpatialMode::None; m_Event.GetSpatial().is3D = false; }
		m_Event.GetSpatial().minDistance = m_MinDist;
		m_Event.GetSpatial().maxDistance = m_MaxDist;
		m_Event.SetVolumeRange(m_MinVolume, m_MaxVolume);
		m_Event.SetPitchRangeSemitones(m_MinPitchSemi, m_MaxPitchSemi);
		if (m_Event.GetRootContainer()) m_Event.GetRootContainer()->type = static_cast<Audio::ContainerType>(m_SelectedContainerType);

		std::filesystem::path p(m_LoadedPath);
		std::error_code ec;
		if (p.has_parent_path())
		{
			std::filesystem::create_directories(p.parent_path(), ec);
		}

		std::ofstream out(m_LoadedPath);
		if (out.is_open())
		{
			out << m_Event.SerializeYaml();
			Audio::AudioSystem::Get().RegisterSoundEvent(m_Event);
			m_StatusFeedback = "已成功保存修改到资产文件: " + m_LoadedPath;
		}
		else
		{
			m_StatusFeedback = "保存失败: 无法写入文件 " + m_LoadedPath;
		}
	}

	void SoundEventEditorPanel::PlayAudition()
	{
		SaveEvent();
		EnsureBuffersForEvent();

		auto& audio = Audio::AudioSystem::Get();
		if (m_CurrentAuditionVoice != Audio::kInvalidVoiceHandle)
		{
			audio.StopVoice(m_CurrentAuditionVoice, 0.02f);
			m_CurrentAuditionVoice = Audio::kInvalidVoiceHandle;
		}

		if (m_SpatialModeIndex == 1)
		{
			m_CurrentAuditionVoice = audio.PostEvent(m_Event.GetId(), glm::vec3(0.0f));
		}
		else if (m_SpatialModeIndex == 0)
		{
			m_CurrentAuditionVoice = audio.PostEvent2D(m_Event.GetId(), glm::vec2(0.0f));
		}
		else
		{
			m_CurrentAuditionVoice = audio.PostEvent2D(m_Event.GetId());
		}
		m_StatusFeedback = "正在试听事件随机回放: " + m_Event.GetId();
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

		float curY = rect.Y + 6.0f;

		// 1. Top Bar: Title & Real Path with Open Folder Button
		const Wui::WuiRect topBar { rect.X + 8.0f, curY, rect.W - 16.0f, 32.0f };
		Wui::BarSurface(ctx, topBar, theme.PanelHeader, theme.Border);
		Wui::Label(ctx, { topBar.X + 10.0f, topBar.Y + 7.0f }, "声效事件编辑器 (Sound Event Editor)", theme.Accent, 13.5f);

		std::string realPath = m_LoadedPath.empty() ? ("projects/assets/audio/events/" + m_Event.GetId() + ".wsound") : m_LoadedPath;
		std::string pathText = "资产文件: " + realPath;
		Wui::Label(ctx, { topBar.X + 280.0f, topBar.Y + 8.0f }, pathText, theme.TextMuted, 11.0f);

		const Wui::WuiRect btnOpenDir { topBar.X + topBar.W - 120.0f, topBar.Y + 4.0f, 112.0f, 24.0f };
		Wui::Tooltip(ctx, btnOpenDir, "在 Windows 资源管理器中打开此文件所在目录并高亮定位");
		if (Wui::Button(ctx, Wui::HashId("se.btn.open_explorer"), btnOpenDir, "打开文件夹", theme))
		{
			OpenInExplorer(realPath);
			m_StatusFeedback = "已在资源管理器中定位: " + realPath;
		}

		curY += 38.0f;

		// 2. Asset Selector Row
		const Wui::WuiRect fileBar { rect.X + 8.0f, curY, rect.W - 16.0f, 32.0f };
		Wui::PanelBackground(ctx, fileBar, theme.ContentBg, 3.0f);

		Wui::Label(ctx, { fileBar.X + 8.0f, fileBar.Y + 8.0f }, "切换资产:", theme.TextMuted, 11.5f);
		float fX = fileBar.X + 80.0f;

		for (size_t i = 0; i < m_AvailableSoundFiles.size() && i < 4; ++i)
		{
			std::filesystem::path p(m_AvailableSoundFiles[i]);
			std::string stem = p.stem().string();
			bool isActive = (m_LoadedPath == m_AvailableSoundFiles[i]);
			const Wui::WuiRect btnFile { fX, fileBar.Y + 4.0f, 130.0f, 24.0f };
			std::string label = isActive ? ("* " + stem + ".wsound") : (stem + ".wsound");
			if (Wui::Button(ctx, Wui::HashId(("se.f." + std::to_string(i)).c_str()), btnFile, label, theme))
			{
				LoadEvent(m_AvailableSoundFiles[i]);
			}
			fX += 136.0f;
		}

		const Wui::WuiRect btnRescan { fX, fileBar.Y + 4.0f, 96.0f, 24.0f };
		if (Wui::Button(ctx, Wui::HashId("se.btn.rescan"), btnRescan, "↺ 刷新扫描", theme))
		{
			RefreshAvailableFiles();
			m_StatusFeedback = "已刷新工程声效事件列表。";
		}
		fX += 102.0f;

		curY += 38.0f;

				// 3. Section 1: Properties Box
		const Wui::WuiRect propBox { rect.X + 8.0f, curY, rect.W - 16.0f, 172.0f };
		Wui::PanelBackground(ctx, propBox, theme.ContentBg, 4.0f);
		float pY = propBox.Y + 6.0f;
		Wui::Label(ctx, { propBox.X + 12.0f, pY }, "Properties", { 0.95f, 0.75f, 0.20f, 1.0f }, 12.0f);
		pY += 22.0f;
		// Row 1: Bus Selector Buttons
		Wui::Label(ctx, { propBox.X + 12.0f, pY + 2.0f }, "Bus:", theme.TextMuted, 11.0f);
		const char* buses[] = { "Master", "Music", "SFX", "Voice", "UI", "Ambience" };
		float bX = propBox.X + 90.0f;
		for (int b = 0; b < 6; ++b) { bool active = (m_SelectedBusIndex == b); const Wui::WuiRect bBtn { bX, pY, 70.0f, 20.0f }; std::string bLabel = active ? ("* " + std::string(buses[b])) : buses[b]; if (Wui::Button(ctx, Wui::HashId(("se.bus." + std::to_string(b)).c_str()), bBtn, bLabel, theme)) m_SelectedBusIndex = b; bX += 74.0f; }
		pY += 25.0f;
		// Row 2: Container Type Buttons
		Wui::Label(ctx, { propBox.X + 12.0f, pY + 2.0f }, "Container:", theme.TextMuted, 11.0f);
		const char* cTypes[] = { "Single", "Random", "Seq", "Switch", "Blend", "Layer" };
		float cX = propBox.X + 90.0f;
		for (int c = 0; c < 6; ++c) { bool active = (m_SelectedContainerType == c); const Wui::WuiRect cBtn { cX, pY, 76.0f, 20.0f }; std::string cLabel = active ? ("* " + std::string(cTypes[c])) : cTypes[c]; if (Wui::Button(ctx, Wui::HashId(("se.cnt." + std::to_string(c)).c_str()), cBtn, cLabel, theme)) { m_SelectedContainerType = c; if (m_Event.GetRootContainer()) m_Event.GetRootContainer()->type = static_cast<Audio::ContainerType>(c); } cX += 80.0f; }
		pY += 25.0f;
		// Row 3: Spatial Mode & Distance
		Wui::Label(ctx, { propBox.X + 12.0f, pY + 2.0f }, "Spatial:", theme.TextMuted, 11.0f);
		const char* spatials[] = { "2D Pan", "3D Spatial", "Center 2D" };
		float sX = propBox.X + 90.0f;
		for (int s = 0; s < 3; ++s) { bool active = (m_SpatialModeIndex == s); const Wui::WuiRect sBtn { sX, pY, 72.0f, 20.0f }; std::string sLabel = active ? ("* " + std::string(spatials[s])) : spatials[s]; if (Wui::Button(ctx, Wui::HashId(("se.spat." + std::to_string(s)).c_str()), sBtn, sLabel, theme)) m_SpatialModeIndex = s; sX += 76.0f; }
		char dMinStr[32], dMaxStr[32];
		std::snprintf(dMinStr, sizeof(dMinStr), "Min: %.1fm", m_MinDist);
		Wui::Label(ctx, { sX + 10.0f, pY + 2.0f }, dMinStr, theme.TextMuted, 10.5f);
		const Wui::WuiRect rMinD { sX + 75.0f, pY + 2.0f, 75.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.slider.mind"), rMinD, m_MinDist, 0.1f, 10.0f, theme);
		std::snprintf(dMaxStr, sizeof(dMaxStr), "Max: %.1fm", m_MaxDist);
		Wui::Label(ctx, { sX + 160.0f, pY + 2.0f }, dMaxStr, theme.TextMuted, 10.5f);
		const Wui::WuiRect rMaxD { sX + 225.0f, pY + 2.0f, 75.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.slider.maxd"), rMaxD, m_MaxDist, 2.0f, 100.0f, theme);
		pY += 25.0f;
		// Row 4: MaxInstances, Cooldown, Stealing
		char instStr[32], cdStr[32];
		std::snprintf(instStr, sizeof(instStr), "MaxInst: %d", m_MaxInstances);
		Wui::Label(ctx, { propBox.X + 12.0f, pY + 2.0f }, instStr, theme.TextMuted, 10.5f);
		float fInst = static_cast<float>(m_MaxInstances);
		const Wui::WuiRect sInst { propBox.X + 105.0f, pY + 2.0f, 65.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.slider.maxinst"), sInst, fInst, 1.0f, 32.0f, theme);
		m_MaxInstances = static_cast<int>(fInst);
		std::snprintf(cdStr, sizeof(cdStr), "CD: %.2fs", m_CooldownSeconds);
		Wui::Label(ctx, { propBox.X + 175.0f, pY + 2.0f }, cdStr, theme.TextMuted, 10.5f);
		const Wui::WuiRect sCd { propBox.X + 245.0f, pY + 2.0f, 65.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.slider.cd"), sCd, m_CooldownSeconds, 0.0f, 1.0f, theme);
		Wui::Label(ctx, { propBox.X + 315.0f, pY + 2.0f }, "Steal:", theme.TextMuted, 10.5f);
		const char* steals[] = { "Oldest", "Quiet", "Reject" };
		float stX = propBox.X + 360.0f;
		for (int st = 0; st < 3; ++st) { bool active = (m_StealPolicyIndex == st); const Wui::WuiRect stBtn { stX, pY, 68.0f, 20.0f }; std::string stLabel = active ? ("* " + std::string(steals[st])) : steals[st]; if (Wui::Button(ctx, Wui::HashId(("se.steal." + std::to_string(st)).c_str()), stBtn, stLabel, theme)) m_StealPolicyIndex = st; stX += 72.0f; }
		pY += 25.0f;
		// Row 5: Volume Range & Pitch Range
		char vRangeStr[32];
		std::snprintf(vRangeStr, sizeof(vRangeStr), "Vol: %.2f~%.2f", m_MinVolume, m_MaxVolume);
		Wui::Label(ctx, { propBox.X + 12.0f, pY + 2.0f }, vRangeStr, theme.TextMuted, 10.5f);
		const Wui::WuiRect sMinV { propBox.X + 110.0f, pY + 2.0f, 65.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.slider.minv"), sMinV, m_MinVolume, 0.1f, 1.0f, theme);
		const Wui::WuiRect sMaxV { propBox.X + 180.0f, pY + 2.0f, 65.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.slider.vmax"), sMaxV, m_MaxVolume, 0.5f, 2.0f, theme);
		char pRangeStr[32];
		std::snprintf(pRangeStr, sizeof(pRangeStr), "Pitch: +-%.1fst", m_MaxPitchSemi);
		Wui::Label(ctx, { propBox.X + 250.0f, pY + 2.0f }, pRangeStr, theme.TextMuted, 10.5f);
		const Wui::WuiRect sMaxP { propBox.X + 345.0f, pY + 2.0f, 85.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("se.slider.maxp"), sMaxP, m_MaxPitchSemi, 0.0f, 12.0f, theme);
		m_MinPitchSemi = -m_MaxPitchSemi;
		curY += 180.0f;

		// 4. Section 2: Variations Box
		float bottomBarHeight = 44.0f;
		float varBoxH = rect.H - (curY - rect.Y) - bottomBarHeight - 12.0f;
		if (varBoxH < 100.0f) varBoxH = 100.0f;

		const Wui::WuiRect varBox { rect.X + 8.0f, curY, rect.W - 16.0f, varBoxH };
		Wui::PanelBackground(ctx, varBox, theme.ContentBg, 4.0f);

		float vY = varBox.Y + 8.0f;
		Wui::Label(ctx, { varBox.X + 12.0f, vY }, "变体音频切片列表 (Variations - 每次触发随机挑一个播放)", { 0.35f, 0.75f, 0.95f, 1.0f }, 12.5f);
		vY += 26.0f;

		auto& vars = m_Event.GetRootContainer()->variations;
		int removeIdx = -1;
		for (size_t vi = 0; vi < vars.size(); ++vi)
		{
			if (vY + 24.0f > varBox.Y + varBox.H - 34.0f) break;

			const Wui::WuiRect rowVar { varBox.X + 8.0f, vY, varBox.W - 16.0f, 24.0f };
			Wui::PanelBackground(ctx, rowVar, theme.PanelBg, 3.0f);

			Wui::Label(ctx, { rowVar.X + 8.0f, rowVar.Y + 4.0f }, vars[vi].soundAssetPath, theme.Text, 11.5f);

			char wStr[32];
			std::snprintf(wStr, sizeof(wStr), "权重: %.1f", vars[vi].weight);
			Wui::Label(ctx, { rowVar.X + rowVar.W - 270.0f, rowVar.Y + 4.0f }, wStr, theme.TextMuted, 10.5f);

			float oldWeight = vars[vi].weight;
			const Wui::WuiRect wSlider { rowVar.X + rowVar.W - 200.0f, rowVar.Y + 3.0f, 80.0f, 18.0f };
			Wui::SliderFloat(ctx, Wui::HashId(("se.v.w." + std::to_string(vi)).c_str()), wSlider, vars[vi].weight, 0.1f, 5.0f, theme);
			if (std::abs(oldWeight - vars[vi].weight) > 0.001f)
			{
				Audio::AudioSystem::Get().RegisterSoundEvent(m_Event);
			}

			const Wui::WuiRect btnPreview { rowVar.X + rowVar.W - 110.0f, rowVar.Y + 2.0f, 52.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId(("se.v.play." + std::to_string(vi)).c_str()), btnPreview, "▶ 试听", theme))
			{
				StopAudition();
				auto& audio = Audio::AudioSystem::Get();
				auto buf = EnsureSliceBuffer(vars[vi].soundAssetPath, vi);
				m_CurrentAuditionVoice = audio.GetVoicePool().AllocateVoice(
					buf, static_cast<Audio::BusId>(m_SelectedBusIndex), 1.0f, 1.0f, false);
				m_StatusFeedback = "正在试听切片: " + vars[vi].soundAssetPath;
			}

			const Wui::WuiRect btnDelete { rowVar.X + rowVar.W - 52.0f, rowVar.Y + 2.0f, 44.0f, 20.0f };
			if (Wui::Button(ctx, Wui::HashId(("se.v.del." + std::to_string(vi)).c_str()), btnDelete, "✖ 删除", theme))
			{
				removeIdx = static_cast<int>(vi);
			}

			vY += 26.0f;
		}

		if (removeIdx >= 0 && removeIdx < static_cast<int>(vars.size()))
		{
			vars.erase(vars.begin() + removeIdx);
			Audio::AudioSystem::Get().RegisterSoundEvent(m_Event);
			m_StatusFeedback = "已删除变体切片，变体实时已生效。";
		}

		// Add variation row via asset selector (no manual path text input!)
		if (vY + 26.0f <= varBox.Y + varBox.H)
		{
			const Wui::WuiRect comboField { varBox.X + 8.0f, vY + 2.0f, varBox.W - 150.0f, 22.0f };
			Wui::SearchableCombo(ctx, Wui::HashId("se.add.combo"), comboField, "", m_AvailableSliceAssets, m_SelectedSliceAssetIndex, theme);

			const Wui::WuiRect btnAdd { varBox.X + varBox.W - 136.0f, vY + 2.0f, 128.0f, 22.0f };
			if (Wui::Button(ctx, Wui::HashId("se.btn.add"), btnAdd, "＋ 添加所选切片", theme))
			{
				if (m_SelectedSliceAssetIndex >= 0 && m_SelectedSliceAssetIndex < static_cast<int>(m_AvailableSliceAssets.size()))
				{
					const std::string& chosen = m_AvailableSliceAssets[m_SelectedSliceAssetIndex];
					vars.push_back({ chosen, 1.0f });
					Audio::AudioSystem::Get().RegisterSoundEvent(m_Event);
					m_StatusFeedback = "已添加新切片: " + chosen + "，变体实时已生效。";
				}
			}
		}

		curY += varBoxH + 8.0f;

		// 5. Bottom Action Bar
		const Wui::WuiRect bottomBar { rect.X + 8.0f, curY, rect.W - 16.0f, 32.0f };
		Wui::PanelBackground(ctx, bottomBar, theme.ContentBg, 3.0f);

		const Wui::WuiRect btnPlayAll { bottomBar.X + 8.0f, bottomBar.Y + 4.0f, 136.0f, 24.0f };
		if (Wui::Button(ctx, Wui::HashId("se.btn.play_event"), btnPlayAll, "▶ 试听随机回放", theme))
		{
			PlayAudition();
		}

		const Wui::WuiRect btnStopAll { bottomBar.X + 150.0f, bottomBar.Y + 4.0f, 80.0f, 24.0f };
		if (Wui::Button(ctx, Wui::HashId("se.btn.stop_event"), btnStopAll, "■ 停止", theme))
		{
			StopAudition();
		}

		const Wui::WuiRect btnSaveAll { bottomBar.X + 236.0f, bottomBar.Y + 4.0f, 110.0f, 24.0f };
		if (Wui::Button(ctx, Wui::HashId("se.btn.save_event"), btnSaveAll, "保存修改", theme))
		{
			SaveEvent();
		}

		if (!m_StatusFeedback.empty())
		{
			Wui::Label(ctx, { bottomBar.X + 360.0f, bottomBar.Y + 8.0f }, m_StatusFeedback, theme.Accent, 11.0f);
		}
	}
}
