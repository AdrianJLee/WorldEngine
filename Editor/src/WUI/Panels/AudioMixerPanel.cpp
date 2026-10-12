#include "wldpch.h"
#include "WUI/Panels/AudioMixerPanel.h"
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
	std::string AudioMixerPanel::s_PendingOpenAsset;

	void AudioMixerPanel::RequestOpenAsset(const std::string& path)
	{
		s_PendingOpenAsset = path;
	}

	static const char* kBusDisplayNames[] = {
		"Master",
		"Music",
		"SFX",
		"Voice",
		"UI"
	};

	static const char* kBusOptionButtons[] = {
		"Master",
		"Music",
		"SFX",
		"Voice",
		"UI"
	};

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

	void AudioMixerPanel::RefreshLibrary()
	{
		m_Items.clear();
		m_AvailableSliceAssets.clear();
		auto& audio = Audio::AudioSystem::Get();

		// 1. Scan disk for .wsound asset files
		const char* soundDirs[] = {
			"projects/assets/audio/events",
			"projects/assets/audio",
			"assets/audio/events",
			"assets/audio",
			"templates/project-example/assets/audio/events",
			"templates/project-example/assets/audio"
		};
		for (const char* dir : soundDirs)
		{
			std::error_code ec;
			if (std::filesystem::exists(dir, ec))
			{
				for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
				{
					if (!entry.is_regular_file(ec)) continue;
					if (entry.path().extension() == ".wsound")
					{
						std::string stem = entry.path().stem().string();
						std::string fname = entry.path().filename().string();
						std::string normPath = entry.path().lexically_normal().generic_string();
						bool exists = false;
						for (const auto& it : m_Items)
						{
							if (it.id == stem || it.path == normPath) { exists = true; break; }
						}
						if (!exists)
						{
							m_Items.push_back({ fname, stem, normPath, AudioItemKind::SoundEvent });
						}
					}
				}
			}
		}

		// 2. Registered Sound Events in AudioSystem
		std::vector<std::string> regIds = audio.GetRegisteredSoundEventIds();
		for (const auto& id : regIds)
		{
			bool exists = false;
			for (const auto& it : m_Items)
			{
				if (it.id == id) { exists = true; break; }
			}
			if (!exists)
			{
				std::string resolvedPath = "projects/assets/audio/events/" + id + ".wsound";
				std::error_code ec;
				if (std::filesystem::exists("templates/project-example/assets/audio/events/" + id + ".wsound", ec))
				{
					resolvedPath = "templates/project-example/assets/audio/events/" + id + ".wsound";
				}
				m_Items.push_back({ id + ".wsound", id, resolvedPath, AudioItemKind::SoundEvent });
			}
		}

		// Fallbacks for standard sound events
		bool hasFootstep = false;
		bool hasUiClick = false;
		for (const auto& it : m_Items)
		{
			if (it.id == "footstep_player") hasFootstep = true;
			if (it.id == "ui_click") hasUiClick = true;
		}
		if (!hasFootstep)
		{
			std::string p = "projects/assets/audio/events/footstep_player.wsound";
			std::error_code ec;
			if (std::filesystem::exists("templates/project-example/assets/audio/events/footstep_player.wsound", ec))
			{
				p = "templates/project-example/assets/audio/events/footstep_player.wsound";
			}
			m_Items.push_back({ "footstep_player.wsound", "footstep_player", p, AudioItemKind::SoundEvent });
		}
		if (!hasUiClick)
		{
			std::string p = "projects/assets/audio/events/ui_click.wsound";
			std::error_code ec;
			if (std::filesystem::exists("templates/project-example/assets/audio/events/ui_click.wsound", ec))
			{
				p = "templates/project-example/assets/audio/events/ui_click.wsound";
			}
			m_Items.push_back({ "ui_click.wsound", "ui_click", p, AudioItemKind::SoundEvent });
		}

		// 3. Scan disk for audio slices (.waudio, .wav, .ogg, .mp3)
		const char* sliceDirs[] = {
			"projects/assets/audio/sfx",
			"projects/assets/audio",
			"assets/audio/sfx",
			"assets/audio",
			"templates/project-example/assets/audio/sfx",
			"templates/project-example/assets/audio"
		};
		for (const char* dir : sliceDirs)
		{
			std::error_code ec;
			if (std::filesystem::exists(dir, ec))
			{
				for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
				{
					if (!entry.is_regular_file(ec)) continue;
					auto ext = entry.path().extension();
					if (ext == ".waudio" || ext == ".wav" || ext == ".ogg" || ext == ".mp3")
					{
						std::string fname = entry.path().filename().string();
						std::string normPath = entry.path().lexically_normal().generic_string();
						bool exists = false;
						for (const auto& it : m_Items)
						{
							if (it.path == normPath) { exists = true; break; }
						}
						if (!exists)
						{
							m_Items.push_back({ fname, fname, normPath, AudioItemKind::AudioSlice });
						}

						if (std::find(m_AvailableSliceAssets.begin(), m_AvailableSliceAssets.end(), normPath) == m_AvailableSliceAssets.end())
						{
							m_AvailableSliceAssets.push_back(normPath);
						}
					}
				}
			}
		}

		// Fallbacks for audio slices in library & variation selector
		auto addSliceIfMissing = [this](const std::string& name, const std::string& path)
		{
			bool inItems = false;
			for (const auto& it : m_Items)
			{
				if (it.path == path || it.displayName == name) { inItems = true; break; }
			}
			if (!inItems)
			{
				m_Items.push_back({ name, name, path, AudioItemKind::AudioSlice });
			}

			if (std::find(m_AvailableSliceAssets.begin(), m_AvailableSliceAssets.end(), path) == m_AvailableSliceAssets.end())
			{
				m_AvailableSliceAssets.push_back(path);
			}
		};

		addSliceIfMissing("footstep_grass.waudio", "assets/audio/sfx/footstep_grass.waudio");
		addSliceIfMissing("ui_click.waudio", "assets/audio/sfx/ui_click.waudio");
		addSliceIfMissing("impact_metal.waudio", "assets/audio/sfx/impact_metal.waudio");
		addSliceIfMissing("footstep_stone.waudio", "assets/audio/sfx/footstep_stone.waudio");
		addSliceIfMissing("footstep_wood.waudio", "assets/audio/sfx/footstep_wood.waudio");

		if (m_SelectedSliceAssetIndex < 0 || m_SelectedSliceAssetIndex >= static_cast<int>(m_AvailableSliceAssets.size()))
		{
			m_SelectedSliceAssetIndex = 0;
		}

		if (m_SelectedIndex < 0 && !m_Items.empty())
		{
			SelectItem(0);
		}
	}

	void AudioMixerPanel::SelectItem(int index)
	{
		if (index < 0 || index >= static_cast<int>(m_Items.size()))
			return;

		m_SelectedIndex = index;
		auto& item = m_Items[index];
		m_IsEventLoaded = false;
		m_LoadedEventPath.clear();
		m_PlaybackTime = 0.0f;
		m_Duration = 1.8f;

		if (item.kind == AudioItemKind::SoundEvent)
		{
			auto& audio = Audio::AudioSystem::Get();
			std::error_code ec;
			if (std::filesystem::exists(item.path, ec))
			{
				std::ifstream in(item.path);
				if (in.is_open())
				{
					std::stringstream ss;
					ss << in.rdbuf();
					m_SelectedEvent.DeserializeYaml(ss.str());
					m_IsEventLoaded = true;
					m_LoadedEventPath = item.path;
				}
			}
			else
			{
				const Audio::SoundEvent* existing = audio.FindSoundEvent(item.id);
				if (existing)
				{
					m_SelectedEvent = *existing;
					m_IsEventLoaded = true;
					m_LoadedEventPath = item.path;
				}
				else
				{
					m_SelectedEvent = Audio::SoundEvent(item.id, Audio::kBusSfx);
					m_IsEventLoaded = true;
					m_LoadedEventPath = item.path;
				}
			}

			if (!m_SelectedEvent.GetRootContainer())
			{
				m_SelectedEvent.GetRootContainer() = std::make_shared<Audio::SoundContainerNode>();
				m_SelectedEvent.GetRootContainer()->type = Audio::ContainerType::Random;
			}

			// Preload variations buffers
			for (size_t i = 0; i < m_SelectedEvent.GetRootContainer()->variations.size(); ++i)
			{
				EnsureSliceBuffer(m_SelectedEvent.GetRootContainer()->variations[i].soundAssetPath, i);
			}

			m_SelectedBus = static_cast<int>(m_SelectedEvent.GetBusId());
			if (m_SelectedBus < 0 || m_SelectedBus >= 5) m_SelectedBus = 2;

			m_Spatial3D = m_SelectedEvent.GetSpatial().is3D;
			m_MinDistance = m_SelectedEvent.GetSpatial().minDistance;
			m_MaxDistance = m_SelectedEvent.GetSpatial().maxDistance;
			m_Volume = 1.0f;
			m_Pitch = 1.0f;
			m_PitchRandomSemi = 1.5f;
		}
		else
		{
			m_SelectedBus = 2; // SFX default
			m_Volume = 1.0f;
			m_Pitch = 1.0f;
			m_Spatial3D = false;
			auto buf = EnsureSliceBuffer(item.path);
			if (buf && buf->GetDurationSeconds() > 0.05f)
			{
				m_Duration = buf->GetDurationSeconds();
			}
		}

		m_StatusNotice = "已切换选择声音: " + item.displayName;
	}

	void AudioMixerPanel::PlayCurrent()
	{
		if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Items.size()))
			return;

		auto& audio = Audio::AudioSystem::Get();
		const auto& item = m_Items[m_SelectedIndex];

		if (m_IsPaused && m_CurrentVoice != Audio::kInvalidVoiceHandle)
		{
			audio.ResumeVoice(m_CurrentVoice);
			m_IsPaused = false;
			m_IsPlaying = true;
			m_StatusNotice = "恢复播放: " + item.displayName;
			return;
		}

		if (m_CurrentVoice != Audio::kInvalidVoiceHandle)
		{
			audio.StopVoice(m_CurrentVoice, 0.02f);
			m_CurrentVoice = Audio::kInvalidVoiceHandle;
		}

		if (item.kind == AudioItemKind::SoundEvent)
		{
			m_SelectedEvent.SetBusId(static_cast<Audio::BusId>(m_SelectedBus));
			m_SelectedEvent.GetSpatial().is3D = m_Spatial3D;
			m_SelectedEvent.GetSpatial().minDistance = m_MinDistance;
			m_SelectedEvent.GetSpatial().maxDistance = m_MaxDistance;
			m_SelectedEvent.SetVolumeRange(m_Volume, m_Volume);
			m_SelectedEvent.SetPitchRangeSemitones(-m_PitchRandomSemi, m_PitchRandomSemi);

			audio.RegisterSoundEvent(m_SelectedEvent);

			if (m_SelectedEvent.GetRootContainer())
			{
				for (size_t i = 0; i < m_SelectedEvent.GetRootContainer()->variations.size(); ++i)
				{
					EnsureSliceBuffer(m_SelectedEvent.GetRootContainer()->variations[i].soundAssetPath, i);
				}
			}

			if (m_Spatial3D)
			{
				m_CurrentVoice = audio.PostEvent(m_SelectedEvent.GetId(), glm::vec3(0.0f), m_Volume, m_Pitch, m_Looping);
			}
			else
			{
				m_CurrentVoice = audio.PostEvent2D(m_SelectedEvent.GetId(), m_Volume, m_Pitch, m_Looping);
			}
		}
		else
		{
			auto buf = EnsureSliceBuffer(item.path);
			m_CurrentVoice = audio.GetVoicePool().AllocateVoice(
				buf, static_cast<Audio::BusId>(m_SelectedBus), m_Volume, m_Pitch, m_Looping);
			if (buf && buf->GetDurationSeconds() > 0.05f)
			{
				m_Duration = buf->GetDurationSeconds();
			}
		}

		m_IsPlaying = true;
		m_IsPaused = false;
		m_PlaybackTime = 0.0f;
		m_StatusNotice = "正在播放: " + item.displayName;
	}

	void AudioMixerPanel::PauseCurrent()
	{
		auto& audio = Audio::AudioSystem::Get();
		if (m_CurrentVoice != Audio::kInvalidVoiceHandle)
		{
			audio.PauseVoice(m_CurrentVoice);
		}
		m_IsPaused = true;
		m_StatusNotice = "已暂停播放。";
	}

	void AudioMixerPanel::StopCurrent()
	{
		auto& audio = Audio::AudioSystem::Get();
		if (m_CurrentVoice != Audio::kInvalidVoiceHandle)
		{
			audio.StopVoice(m_CurrentVoice, 0.05f);
			m_CurrentVoice = Audio::kInvalidVoiceHandle;
		}
		m_IsPlaying = false;
		m_IsPaused = false;
		m_PlaybackTime = 0.0f;
		m_StatusNotice = "播放已停止。";
	}

	void AudioMixerPanel::ImportWavAsset(const std::string& wavPath)
	{
		std::filesystem::path p(wavPath);
		std::error_code ec;
		if (!std::filesystem::exists(p, ec) || std::filesystem::is_directory(p, ec))
		{
			m_StatusNotice = "未找到指定源 WAV 文件: " + wavPath;
			return;
		}

		std::filesystem::path outDir = "projects/assets/audio/sfx";
		std::filesystem::create_directories(outDir, ec);

		std::string stem = p.stem().generic_string();
		std::filesystem::path outCooked = outDir / (stem + ".waudio");

		Audio::AudioCookSettings settings;
		settings.format = Audio::AudioFormat::PcmFloat32;
		if (Audio::AudioCooker::CookFile(wavPath, outCooked.generic_string(), settings))
		{
			m_StatusNotice = "成功导入 WAV 并烘焙为: " + outCooked.generic_string();
			RefreshLibrary();
			for (size_t i = 0; i < m_Items.size(); ++i)
			{
				if (m_Items[i].id == stem)
				{
					SelectItem(static_cast<int>(i));
					break;
				}
			}
		}
		else
		{
			m_StatusNotice = "导入失败: 无法解析 " + wavPath;
		}
	}

	void AudioMixerPanel::SaveCurrentEvent()
	{
		if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Items.size()))
		{
			m_StatusNotice = "未选择有效声音条目。";
			return;
		}

		auto& item = m_Items[m_SelectedIndex];
		std::string targetPath = item.path;
		if (targetPath.rfind("Registered Event:", 0) == 0 || targetPath.empty())
		{
			targetPath = "projects/assets/audio/events/" + item.id + ".wsound";
			item.path = targetPath;
			m_LoadedEventPath = targetPath;
		}

		m_SelectedEvent.SetBusId(static_cast<Audio::BusId>(m_SelectedBus));
		m_SelectedEvent.GetSpatial().is3D = m_Spatial3D;
		m_SelectedEvent.GetSpatial().minDistance = m_MinDistance;
		m_SelectedEvent.GetSpatial().maxDistance = m_MaxDistance;
		m_SelectedEvent.SetVolumeRange(m_Volume, m_Volume);
		m_SelectedEvent.SetPitchRangeSemitones(-m_PitchRandomSemi, m_PitchRandomSemi);

		std::filesystem::path p(targetPath);
		std::error_code ec;
		if (p.has_parent_path())
		{
			std::filesystem::create_directories(p.parent_path(), ec);
		}

		std::ofstream out(targetPath);
		if (out.is_open())
		{
			out << m_SelectedEvent.SerializeYaml();
			Audio::AudioSystem::Get().RegisterSoundEvent(m_SelectedEvent);
			m_StatusNotice = "已成功保存声效文件到物理磁盘: " + targetPath;
		}
		else
		{
			m_StatusNotice = "保存失败: 无法写入 " + targetPath;
		}
	}

	void AudioMixerPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg);

		if (!s_PendingOpenAsset.empty())
		{
			std::string p = s_PendingOpenAsset;
			s_PendingOpenAsset.clear();
			RefreshLibrary();
			for (size_t i = 0; i < m_Items.size(); ++i)
			{
				if (m_Items[i].path == p || m_Items[i].displayName == p)
				{
					SelectItem(static_cast<int>(i));
					break;
				}
			}
		}

		if (m_Items.empty())
		{
			RefreshLibrary();
		}

		// Advance playback timer smoothly
		if (m_IsPlaying && !m_IsPaused)
		{
			m_PlaybackTime += 0.016f;
			if (m_PlaybackTime >= m_Duration)
			{
				if (m_Looping)
				{
					m_PlaybackTime = std::fmod(m_PlaybackTime, m_Duration);
				}
				else
				{
					m_PlaybackTime = m_Duration;
					m_IsPlaying = false;
				}
			}
		}

		const float leftWidth = 260.0f;
		const Wui::WuiRect leftRect { rect.X + 4.0f, rect.Y + 4.0f, leftWidth, rect.H - 8.0f };
		const Wui::WuiRect rightRect { rect.X + leftWidth + 8.0f, rect.Y + 4.0f, rect.W - leftWidth - 12.0f, rect.H - 8.0f };

		RenderLeftLibrary(ctx, leftRect, host);

		const float globalVolumeHeight = 172.0f;
		const Wui::WuiRect inspectorRect { rightRect.X, rightRect.Y, rightRect.W, rightRect.H - globalVolumeHeight - 4.0f };
		const Wui::WuiRect globalRect { rightRect.X, rightRect.Y + rightRect.H - globalVolumeHeight, rightRect.W, globalVolumeHeight };

		RenderRightInspector(ctx, inspectorRect, host);
		RenderGlobalVolume(ctx, globalRect, host);
	}

	void AudioMixerPanel::RenderLeftLibrary(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.ContentBg, 4.0f);

		// Header Bar
		const Wui::WuiRect headerRect { rect.X, rect.Y, rect.W, 28.0f };
		Wui::BarSurface(ctx, headerRect, theme.PanelHeader, theme.Border);
		Wui::Label(ctx, { rect.X + 8.0f, rect.Y + 6.0f }, "工程音频库 (Audio Library)", theme.Text, 13.0f);

		// Search box
		const Wui::WuiRect searchRect { rect.X + 6.0f, rect.Y + 34.0f, rect.W - 12.0f, 24.0f };
		Wui::TextFieldA11y a11y { "搜索音频", "搜索声音名称..." };
		Wui::TextField(ctx, Wui::HashId("lib.search"), searchRect, m_SearchFilter, theme, nullptr, &a11y);

		// Filter items
		std::vector<int> matchedIndices;
		for (int i = 0; i < static_cast<int>(m_Items.size()); ++i)
		{
			if (m_SearchFilter.empty())
			{
				matchedIndices.push_back(i);
			}
			else
			{
				std::string lowerFilter = m_SearchFilter;
				std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
				std::string lowerDisp = m_Items[i].displayName;
				std::transform(lowerDisp.begin(), lowerDisp.end(), lowerDisp.begin(), ::tolower);
				std::string lowerId = m_Items[i].id;
				std::transform(lowerId.begin(), lowerId.end(), lowerId.begin(), ::tolower);
				if (lowerDisp.find(lowerFilter) != std::string::npos || lowerId.find(lowerFilter) != std::string::npos)
				{
					matchedIndices.push_back(i);
				}
			}
		}

		// Scrollable Item List
		const Wui::WuiRect listRect { rect.X + 4.0f, rect.Y + 64.0f, rect.W - 8.0f, rect.H - 70.0f };
		float rowHeight = 28.0f;
		float totalContentH = static_cast<float>(matchedIndices.size()) * (rowHeight + 2.0f);

		Wui::BeginScrollArea(ctx, listRect, totalContentH, m_LibraryScrollY, theme, Wui::HashId("lib.scroll"));

		float curY = listRect.Y;
		for (size_t mi = 0; mi < matchedIndices.size(); ++mi)
		{
			int origIdx = matchedIndices[mi];
			const auto& item = m_Items[origIdx];
			bool isSelected = (m_SelectedIndex == origIdx);

			const Wui::WuiRect rowRect { listRect.X + 2.0f, curY, listRect.W - 4.0f, rowHeight };
			if (isSelected)
			{
				Wui::PanelBackground(ctx, rowRect, theme.Selection, 3.0f);
			}

			if (Wui::Button(ctx, Wui::HashId(("lib.item." + std::to_string(origIdx)).c_str()), rowRect, "", theme))
			{
				SelectItem(origIdx);
			}

			// Label with explicit extension (.wsound / .waudio) & Badge
			Wui::WuiColor textColor = isSelected ? theme.Accent : theme.Text;
			Wui::Label(ctx, { rowRect.X + 6.0f, rowRect.Y + 6.0f }, item.displayName, textColor, 12.0f);

			const Wui::WuiRect badgeRect { rowRect.X + rowRect.W - 68.0f, rowRect.Y + 4.0f, 62.0f, 20.0f };
			if (item.kind == AudioItemKind::SoundEvent)
			{
				Wui::Badge(ctx, badgeRect, "音效事件", { 0.15f, 0.45f, 0.75f, 0.40f }, theme.Accent, theme, 10.5f, true, 3.0f);
			}
			else
			{
				Wui::Badge(ctx, badgeRect, "音频切片", { 0.75f, 0.45f, 0.15f, 0.40f }, { 0.95f, 0.75f, 0.35f, 1.0f }, theme, 10.5f, true, 3.0f);
			}

			curY += rowHeight + 2.0f;
		}

		Wui::EndScrollArea(ctx);
	}

	void AudioMixerPanel::RenderRightInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.ContentBg, 4.0f);

		if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Items.size()))
		{
			Wui::Label(ctx, { rect.X + 20.0f, rect.Y + 20.0f }, "请在左侧工程音频库中选择一个声音条目。", theme.TextMuted, 13.0f);
			return;
		}

		const auto& item = m_Items[m_SelectedIndex];
		float curY = rect.Y + 8.0f;

		// 1. Top Title & Real Path Header with Open In Explorer Button
		Wui::Label(ctx, { rect.X + 12.0f, curY }, item.displayName, theme.Text, 16.0f);
		curY += 22.0f;

		std::string realPath = item.path;
		if (realPath.rfind("Registered Event:", 0) == 0 || realPath.empty())
		{
			realPath = "projects/assets/audio/events/" + item.id + ".wsound";
		}

		std::string infoStr = (item.kind == AudioItemKind::SoundEvent ? "类型: 复合声效事件 | 真实路径: " : "类型: 独立音频切片 | 真实路径: ") + realPath;
		Wui::Label(ctx, { rect.X + 12.0f, curY + 2.0f }, infoStr, theme.TextMuted, 11.0f);

		const Wui::WuiRect btnOpenDir { rect.X + rect.W - 126.0f, curY - 2.0f, 114.0f, 22.0f };
		Wui::Tooltip(ctx, btnOpenDir, "在 Windows 资源管理器中打开此文件所在目录并高亮定位");
		if (Wui::Button(ctx, Wui::HashId("mgr.btn.open_explorer"), btnOpenDir, "打开文件夹", theme))
		{
			OpenInExplorer(realPath);
			m_StatusNotice = "已在资源管理器中定位: " + realPath;
		}
		curY += 26.0f;

		// 2. Transport Player Controls (Big intuitive buttons)
		const Wui::WuiRect transportBox { rect.X + 8.0f, curY, rect.W - 16.0f, 68.0f };
		Wui::PanelBackground(ctx, transportBox, theme.PanelBg, 4.0f);

		float btnY = transportBox.Y + 8.0f;
		const Wui::WuiRect btnPlay { transportBox.X + 12.0f, btnY, 78.0f, 26.0f };
		const Wui::WuiRect btnPause { transportBox.X + 96.0f, btnY, 78.0f, 26.0f };
		const Wui::WuiRect btnStop { transportBox.X + 180.0f, btnY, 78.0f, 26.0f };
		const Wui::WuiRect btnLoop { transportBox.X + 264.0f, btnY, 92.0f, 26.0f };

		Wui::Tooltip(ctx, btnPlay, "开始回放选中的声音 (多切片事件将按变体权重随机触发)");
		if (Wui::Button(ctx, Wui::HashId("trans.play"), btnPlay, "▶ 播放", theme))
		{
			PlayCurrent();
		}

		Wui::Tooltip(ctx, btnPause, "暂停回放声音");
		if (Wui::Button(ctx, Wui::HashId("trans.pause"), btnPause, "|| 暂停", theme))
		{
			PauseCurrent();
		}

		Wui::Tooltip(ctx, btnStop, "停止回放声音并重置进度");
		if (Wui::Button(ctx, Wui::HashId("trans.stop"), btnStop, "■ 停止", theme))
		{
			StopCurrent();
		}

		std::string loopLabel = m_Looping ? "循环: 开" : "循环: 关";
		Wui::Tooltip(ctx, btnLoop, "切换循环回放模式");
		if (Wui::Button(ctx, Wui::HashId("trans.loop"), btnLoop, loopLabel, theme))
		{
			m_Looping = !m_Looping;
			if (m_CurrentVoice != Audio::kInvalidVoiceHandle)
			{
				Audio::AudioSystem::Get().SetVoiceLooping(m_CurrentVoice, m_Looping);
			}
		}

		// Progress bar & playback time
		float barY = btnY + 34.0f;
		const Wui::WuiRect barRect { transportBox.X + 12.0f, barY, transportBox.W - 120.0f, 10.0f };
		Wui::PanelBackground(ctx, barRect, theme.ContentBg, 3.0f);

		float progress = std::clamp(m_PlaybackTime / m_Duration, 0.0f, 1.0f);
		if (progress > 0.005f)
		{
			const Wui::WuiRect fillRect { barRect.X, barRect.Y, barRect.W * progress, barRect.H };
			Wui::PanelBackground(ctx, fillRect, theme.Accent, 3.0f);
		}

		char timeText[32];
		std::snprintf(timeText, sizeof(timeText), "%.1fs / %.1fs", m_PlaybackTime, m_Duration);
		Wui::Label(ctx, { transportBox.X + transportBox.W - 96.0f, barY - 2.0f }, timeText, theme.Text, 11.5f);

		curY += 76.0f;

		// 3. Sound Properties Adjustment
		const Wui::WuiRect propBox { rect.X + 8.0f, curY, rect.W - 16.0f, 126.0f };
		Wui::PanelBackground(ctx, propBox, theme.PanelBg, 4.0f);

		float pY = propBox.Y + 8.0f;
		// Row 1: Bus Category Options
		Wui::Label(ctx, { propBox.X + 12.0f, pY + 2.0f }, "所属分类 (Bus):", theme.TextMuted, 11.5f);
		float bX = propBox.X + 130.0f;
		for (int b = 0; b < 5; ++b)
		{
			bool active = (m_SelectedBus == b);
			const Wui::WuiRect bBtn { bX, pY, 78.0f, 22.0f };
			std::string bLabel = active ? ("* " + std::string(kBusOptionButtons[b])) : kBusOptionButtons[b];
			if (Wui::Button(ctx, Wui::HashId(("prop.bus." + std::to_string(b)).c_str()), bBtn, bLabel, theme))
			{
				m_SelectedBus = b;
			}
			bX += 82.0f;
		}
		pY += 28.0f;

		// Row 2: Volume & Pitch Sliders
		char volStr[32];
		std::snprintf(volStr, sizeof(volStr), "音量: %d%%", static_cast<int>(m_Volume * 100.0f));
		Wui::Label(ctx, { propBox.X + 12.0f, pY + 2.0f }, volStr, theme.TextMuted, 11.5f);
		const Wui::WuiRect rectVol { propBox.X + 130.0f, pY + 2.0f, 150.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("prop.slider.vol"), rectVol, m_Volume, 0.0f, 1.5f, theme);

		const Wui::WuiRect btnResetVol { propBox.X + 286.0f, pY, 60.0f, 20.0f };
		Wui::Tooltip(ctx, btnResetVol, "快速重置音量为 100%");
		if (Wui::Button(ctx, Wui::HashId("prop.btn.reset_vol"), btnResetVol, "100%", theme))
		{
			m_Volume = 1.0f;
		}

		char pitchStr[32];
		std::snprintf(pitchStr, sizeof(pitchStr), "音高: %.2fx", m_Pitch);
		Wui::Label(ctx, { propBox.X + 360.0f, pY + 2.0f }, pitchStr, theme.TextMuted, 11.5f);
		const Wui::WuiRect rectPitch { propBox.X + 440.0f, pY + 2.0f, 140.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("prop.slider.pitch"), rectPitch, m_Pitch, 0.5f, 2.0f, theme);

		const Wui::WuiRect btnResetPitch { propBox.X + 586.0f, pY, 60.0f, 20.0f };
		Wui::Tooltip(ctx, btnResetPitch, "快速重置音高为 1.0x 标准速度");
		if (Wui::Button(ctx, Wui::HashId("prop.btn.reset_pitch"), btnResetPitch, "1.0x", theme))
		{
			m_Pitch = 1.0f;
		}
		pY += 28.0f;

		// Row 3: 3D Spatial Attenuation Toggle & Distance Sliders
		const Wui::WuiRect btnSpatial { propBox.X + 12.0f, pY, 156.0f, 22.0f };
		std::string spatialLabel = m_Spatial3D ? "✓ 启用 3D 空间衰减" : "  启用 3D 空间衰减";
		Wui::Tooltip(ctx, btnSpatial, "开启 3D 空间声像与距离衰减衰减计算");
		if (Wui::Button(ctx, Wui::HashId("prop.btn.spatial"), btnSpatial, spatialLabel, theme))
		{
			m_Spatial3D = !m_Spatial3D;
		}

		if (m_Spatial3D)
		{
			char minDistStr[32];
			std::snprintf(minDistStr, sizeof(minDistStr), "近场全响: %.1fm", m_MinDistance);
			Wui::Label(ctx, { propBox.X + 180.0f, pY + 2.0f }, minDistStr, theme.TextMuted, 11.0f);
			const Wui::WuiRect rectMinD { propBox.X + 280.0f, pY + 2.0f, 100.0f, 18.0f };
			Wui::SliderFloat(ctx, Wui::HashId("prop.slider.mind"), rectMinD, m_MinDistance, 0.1f, 10.0f, theme);

			char maxDistStr[32];
			std::snprintf(maxDistStr, sizeof(maxDistStr), "远场消失: %.1fm", m_MaxDistance);
			Wui::Label(ctx, { propBox.X + 400.0f, pY + 2.0f }, maxDistStr, theme.TextMuted, 11.0f);
			const Wui::WuiRect rectMaxD { propBox.X + 500.0f, pY + 2.0f, 120.0f, 18.0f };
			Wui::SliderFloat(ctx, Wui::HashId("prop.slider.maxd"), rectMaxD, m_MaxDistance, 5.0f, 100.0f, theme);
		}

		curY += 134.0f;

		// 4. Variations Section (if compound sound event)
		if (item.kind == AudioItemKind::SoundEvent && m_SelectedEvent.GetRootContainer())
		{
			float varBoxH = rect.H - (curY - rect.Y) - 10.0f;
			if (varBoxH > 80.0f)
			{
				const Wui::WuiRect varBox { rect.X + 8.0f, curY, rect.W - 16.0f, varBoxH };
				Wui::PanelBackground(ctx, varBox, theme.PanelBg, 4.0f);

				float vY = varBox.Y + 8.0f;
				Wui::Label(ctx, { varBox.X + 12.0f, vY }, "变体音频切片列表 (Variations - 每次触发随机挑一个播放)", { 0.35f, 0.75f, 0.95f, 1.0f }, 12.0f);

				// Save button on the right
				const Wui::WuiRect btnSave { varBox.X + varBox.W - 140.0f, vY - 2.0f, 128.0f, 22.0f };
				Wui::Tooltip(ctx, btnSave, "保存当前声效变体与属性配置到 .wsound 资产文件");
				if (Wui::Button(ctx, Wui::HashId("var.btn.save"), btnSave, "保存声效文件", theme))
				{
					SaveCurrentEvent();
				}
				vY += 26.0f;

				auto& vars = m_SelectedEvent.GetRootContainer()->variations;
				int removeIdx = -1;
				for (size_t vi = 0; vi < vars.size(); ++vi)
				{
					if (vY + 24.0f > varBox.Y + varBox.H - 32.0f) break;

					const Wui::WuiRect rowVar { varBox.X + 8.0f, vY, varBox.W - 16.0f, 24.0f };
					Wui::PanelBackground(ctx, rowVar, theme.ContentBg, 3.0f);

					Wui::Label(ctx, { rowVar.X + 8.0f, rowVar.Y + 4.0f }, vars[vi].soundAssetPath, theme.Text, 11.5f);

					char wStr[32];
					std::snprintf(wStr, sizeof(wStr), "权重: %.1f", vars[vi].weight);
					Wui::Label(ctx, { rowVar.X + rowVar.W - 270.0f, rowVar.Y + 4.0f }, wStr, theme.TextMuted, 10.5f);

					float oldWeight = vars[vi].weight;
					const Wui::WuiRect wSlider { rowVar.X + rowVar.W - 200.0f, rowVar.Y + 3.0f, 80.0f, 18.0f };
					Wui::SliderFloat(ctx, Wui::HashId(("var.w." + std::to_string(vi)).c_str()), wSlider, vars[vi].weight, 0.1f, 5.0f, theme);
					if (std::abs(oldWeight - vars[vi].weight) > 0.001f)
					{
						Audio::AudioSystem::Get().RegisterSoundEvent(m_SelectedEvent);
					}

					const Wui::WuiRect btnPreview { rowVar.X + rowVar.W - 110.0f, rowVar.Y + 2.0f, 52.0f, 20.0f };
					if (Wui::Button(ctx, Wui::HashId(("var.play." + std::to_string(vi)).c_str()), btnPreview, "▶ 试听", theme))
					{
						auto& audio = Audio::AudioSystem::Get();
						if (m_CurrentVoice != Audio::kInvalidVoiceHandle)
						{
							audio.StopVoice(m_CurrentVoice, 0.02f);
							m_CurrentVoice = Audio::kInvalidVoiceHandle;
						}
						auto buf = EnsureSliceBuffer(vars[vi].soundAssetPath, vi);
						m_CurrentVoice = audio.GetVoicePool().AllocateVoice(
							buf, static_cast<Audio::BusId>(m_SelectedBus), m_Volume, m_Pitch, false);
						m_PlaybackTime = 0.0f;
						m_Duration = (buf && buf->GetDurationSeconds() > 0.05f) ? buf->GetDurationSeconds() : 1.8f;
						m_IsPlaying = true;
						m_IsPaused = false;
						m_StatusNotice = "正在试听切片: " + vars[vi].soundAssetPath;
					}

					const Wui::WuiRect btnDelete { rowVar.X + rowVar.W - 52.0f, rowVar.Y + 2.0f, 44.0f, 20.0f };
					if (Wui::Button(ctx, Wui::HashId(("var.del." + std::to_string(vi)).c_str()), btnDelete, "✖ 删除", theme))
					{
						removeIdx = static_cast<int>(vi);
					}

					vY += 26.0f;
				}

				if (removeIdx >= 0 && removeIdx < static_cast<int>(vars.size()))
				{
					vars.erase(vars.begin() + removeIdx);
					Audio::AudioSystem::Get().RegisterSoundEvent(m_SelectedEvent);
					m_StatusNotice = "已移除变体音频切片，变体实时已生效。";
				}

				// Asset Selector Dropdown & Add Slice button (No manual path text input!)
				if (vY + 26.0f <= varBox.Y + varBox.H)
				{
					const Wui::WuiRect comboField { varBox.X + 8.0f, vY + 2.0f, varBox.W - 150.0f, 22.0f };
					Wui::SearchableCombo(ctx, Wui::HashId("var.add.combo"), comboField, "", m_AvailableSliceAssets, m_SelectedSliceAssetIndex, theme);

					const Wui::WuiRect btnAdd { varBox.X + varBox.W - 136.0f, vY + 2.0f, 128.0f, 22.0f };
					if (Wui::Button(ctx, Wui::HashId("var.btn.add"), btnAdd, "＋ 添加所选切片", theme))
					{
						if (m_SelectedSliceAssetIndex >= 0 && m_SelectedSliceAssetIndex < static_cast<int>(m_AvailableSliceAssets.size()))
						{
							const std::string& chosenSlice = m_AvailableSliceAssets[m_SelectedSliceAssetIndex];
							vars.push_back({ chosenSlice, 1.0f });
							Audio::AudioSystem::Get().RegisterSoundEvent(m_SelectedEvent);
							m_StatusNotice = "已添加新切片: " + chosenSlice + "，变体实时已生效。";
						}
					}
				}
			}
		}
	}

	void AudioMixerPanel::RenderGlobalVolume(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.ContentBg, 4.0f);

		auto& audio = Audio::AudioSystem::Get();

		// Sync initial volumes with audio graph
		if (!m_GlobalBusesSynced)
		{
			for (uint32_t b = 0; b < 5; ++b)
			{
				const auto* bus = audio.GetMixerGraph().FindBus(b);
				if (bus)
				{
					m_BusVolumes[b] = bus->GetVolume();
					m_BusMuted[b] = bus->IsMute();
				}
			}
			m_GlobalBusesSynced = true;
		}

		float gY = rect.Y + 8.0f;

		// Section Header & Stop All Emergency Button
		Wui::Label(ctx, { rect.X + 12.0f, gY + 3.0f }, "游戏全局音量控制 (Global Game Volume)", { 0.95f, 0.75f, 0.20f, 1.0f }, 13.0f);

		const Wui::WuiRect btnStopAll { rect.X + rect.W - 148.0f, gY, 136.0f, 24.0f };
		Wui::Tooltip(ctx, btnStopAll, "紧急停止: 立即停止游戏与试听中的所有声音");
		if (Wui::Button(ctx, Wui::HashId("global.btn.stop_all"), btnStopAll, "■ 停止全部声音", theme))
		{
			audio.StopAll(0.05f);
			m_IsPlaying = false;
			m_IsPaused = false;
			m_CurrentVoice = Audio::kInvalidVoiceHandle;
			m_StatusNotice = "已执行全局停止，所有通道声音已切断。";
		}

		gY += 30.0f;

		// 5 Global Volume Sliders
		const char* busIcons[] = {
			"主音量 (Master)",
			"背景音乐 (Music)",
			"游戏音效 (SFX)",
			"角色语音 (Voice)",
			"界面音效 (UI)"
		};

		for (int b = 0; b < 5; ++b)
		{
			const Wui::WuiRect rowRect { rect.X + 8.0f, gY, rect.W - 16.0f, 22.0f };

			Wui::Label(ctx, { rowRect.X + 6.0f, rowRect.Y + 2.0f }, busIcons[b], theme.Text, 11.5f);

			const Wui::WuiRect sliderRect { rowRect.X + 160.0f, rowRect.Y + 2.0f, rowRect.W - 290.0f, 18.0f };
			float oldVol = m_BusVolumes[b];
			Wui::SliderFloat(ctx, Wui::HashId(("global.slider." + std::to_string(b)).c_str()), sliderRect, m_BusVolumes[b], 0.0f, 1.0f, theme);
			if (std::abs(oldVol - m_BusVolumes[b]) > 0.001f)
			{
				audio.SetBusVolume(static_cast<Audio::BusId>(b), m_BusVolumes[b]);
			}

			char volPercent[16];
			std::snprintf(volPercent, sizeof(volPercent), "%d%%", static_cast<int>(m_BusVolumes[b] * 100.0f));
			Wui::Label(ctx, { rowRect.X + rowRect.W - 120.0f, rowRect.Y + 2.0f }, volPercent, theme.TextMuted, 11.0f);

			const Wui::WuiRect btnMute { rowRect.X + rowRect.W - 80.0f, rowRect.Y, 74.0f, 20.0f };
			std::string muteLabel = m_BusMuted[b] ? "[已静音]" : "[静音 Mute]";
			if (Wui::Button(ctx, Wui::HashId(("global.mute." + std::to_string(b)).c_str()), btnMute, muteLabel, theme))
			{
				m_BusMuted[b] = !m_BusMuted[b];
				audio.SetBusMute(static_cast<Audio::BusId>(b), m_BusMuted[b]);
				m_StatusNotice = std::string(kBusDisplayNames[b]) + (m_BusMuted[b] ? " 已静音" : " 已恢复");
			}

			gY += 24.0f;
		}

		if (!m_StatusNotice.empty())
		{
			Wui::Label(ctx, { rect.X + 12.0f, rect.Y + rect.H - 16.0f }, m_StatusNotice, theme.Accent, 10.5f);
		}
	}
}
