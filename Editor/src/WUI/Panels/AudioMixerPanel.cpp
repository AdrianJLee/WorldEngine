#include "wldpch.h"
#include "WUI/Panels/AudioMixerPanel.h"
#include "World/Audio/AudioCooker.h"
#include "World/Audio/AudioSystem.h"
#include "World/Audio/AudioTypes.h"
#include "World/Audio/AudioBuffer.h"
#include "World/Audio/AudioProfiler.h"
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
	namespace
	{
		template <typename T, typename = void>
		struct HasSnapLoopMethod : std::false_type {};

		template <typename T>
		struct HasSnapLoopMethod<T, std::void_t<decltype(std::declval<T>().SnapLoopPointsToZeroCrossing(256))>> : std::true_type {};

		template <typename BufferT>
		void DoSnapLoopPoints(BufferT& buf, uint32_t searchWindow = 256)
		{
			if constexpr (HasSnapLoopMethod<BufferT>::value)
			{
				buf.SnapLoopPointsToZeroCrossing(searchWindow);
			}
			else
			{
				const auto& samples = buf.GetSamples();
				const uint32_t channels = buf.GetChannels();
				const uint32_t frameCount = buf.GetFrameCount();
				if (samples.empty() || frameCount < 2 || channels == 0) return;

				auto findZeroCrossing = [&](uint32_t target) -> uint32_t {
					uint32_t bestFrame = target;
					float bestDist = 1e9f;
					int32_t startSearch = std::max(1, static_cast<int32_t>(target) - static_cast<int32_t>(searchWindow));
					int32_t endSearch = std::min(static_cast<int32_t>(frameCount - 1), static_cast<int32_t>(target + searchWindow));

					for (int32_t f = startSearch; f <= endSearch; ++f)
					{
						float prev = samples[(f - 1) * channels];
						float curr = samples[f * channels];
						if (prev <= 0.0f && curr >= 0.0f)
						{
							float dist = static_cast<float>(std::abs(f - static_cast<int32_t>(target)));
							if (dist < bestDist)
							{
								bestDist = dist;
								bestFrame = static_cast<uint32_t>(f);
							}
						}
					}
					return bestFrame;
				};

				uint32_t start = buf.GetLoopStartFrame();
				uint32_t end = buf.HasLoopPoints() ? buf.GetLoopEndFrame() : frameCount;
				uint32_t newStart = findZeroCrossing(start);
				uint32_t newEnd = findZeroCrossing(end);
				if (newEnd > newStart)
				{
					buf.SetLoopPoints(newStart, newEnd);
				}
			}
		}
	}
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
		"UI",
		"Ambience"
	};

	static const char* kBusOptionButtons[] = {
		"Master",
		"Music",
		"SFX",
		"Voice",
		"UI",
		"Ambience"
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

	static std::shared_ptr<Audio::AudioBuffer> EnsureSliceBuffer(const std::string& path, size_t variationSeed = 0);

	std::shared_ptr<Audio::AudioBuffer> AudioMixerPanel::GetSelectedBuffer()
	{
		if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Items.size()))
			return nullptr;

		const auto& item = m_Items[m_SelectedIndex];
		if (item.kind == AudioItemKind::AudioSlice)
		{
			return EnsureSliceBuffer(item.path);
		}
		else if (item.kind == AudioItemKind::SoundEvent)
		{
			if (m_SelectedEvent.GetRootContainer() && !m_SelectedEvent.GetRootContainer()->variations.empty())
			{
				return EnsureSliceBuffer(m_SelectedEvent.GetRootContainer()->variations[0].soundAssetPath, 0);
			}
		}
		return nullptr;
	}

	static std::shared_ptr<Audio::AudioBuffer> EnsureSliceBuffer(const std::string& path, size_t variationSeed)
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

		// 2.5 Scan disk for .wmixer and .wvoice files
		const char* metaDirs[] = {
			"projects/assets/audio",
			"projects/assets/audio/voices",
			"assets/audio",
			"assets/audio/voices",
			"templates/project-example/assets/audio",
			"templates/project-example/assets/audio/voices"
		};
		for (const char* dir : metaDirs)
		{
			std::error_code ec;
			if (std::filesystem::exists(dir, ec))
			{
				for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
				{
					if (!entry.is_regular_file(ec)) continue;
					std::string ext = entry.path().extension().string();
					std::string fname = entry.path().filename().string();
					std::string stem = entry.path().stem().string();
					std::string normPath = entry.path().lexically_normal().generic_string();

					bool exists = false;
					for (const auto& it : m_Items)
					{
						if (it.path == normPath || it.displayName == fname) { exists = true; break; }
					}
					if (!exists)
					{
						if (ext == ".wmixer")
						{
							m_Items.push_back({ fname, stem, normPath, AudioItemKind::MixerConfig });
						}
						else if (ext == ".wvoice")
						{
							m_Items.push_back({ fname, stem, normPath, AudioItemKind::VoiceProfile });
						}
					}
				}
			}
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
		m_HasSnappedLoop = false;
		m_SnappedLoopStart = 0;
		m_SnappedLoopEnd = 0;

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
			Wui::Label(ctx, { rect.X + 20.0f, rect.Y + 14.0f }, "请在左侧工程音频库中选择一个声音条目以检视与编辑。", theme.TextMuted, 13.0f);
			const Wui::WuiRect profilerBox { rect.X + 8.0f, rect.Y + 38.0f, rect.W - 16.0f, 142.0f };
			RenderProfilerHud(ctx, profilerBox, host);
			return;
		}

		const auto& item = m_Items[m_SelectedIndex];
		if (item.kind == AudioItemKind::VoiceProfile)
		{
			RenderVoiceInspector(ctx, rect, host, item);
			return;
		}
		if (item.kind == AudioItemKind::MixerConfig)
		{
			RenderMixerInspector(ctx, rect, host, item);
			return;
		}
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
		const Wui::WuiRect btnPlay { transportBox.X + 12.0f, btnY, 74.0f, 26.0f };
		const Wui::WuiRect btnPause { transportBox.X + 90.0f, btnY, 74.0f, 26.0f };
		const Wui::WuiRect btnStop { transportBox.X + 168.0f, btnY, 74.0f, 26.0f };
		const Wui::WuiRect btnLoop { transportBox.X + 246.0f, btnY, 86.0f, 26.0f };

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

		// 零交叉吸附按钮 (Snap to Zero-Crossing)
		const Wui::WuiRect btnSnapLoop { transportBox.X + 338.0f, btnY, 175.0f, 26.0f };
		Wui::Tooltip(ctx, btnSnapLoop, "将循环起点与终点自动吸附到波形正斜率零交叉点，彻底根治循环接缝处的爆音 (Clicks & Pops)");
		if (Wui::Button(ctx, Wui::HashId("trans.btn.snap_zero"), btnSnapLoop, "⚡ 零交叉吸附 (Snap)", theme))
		{
			auto buf = GetSelectedBuffer();
			if (buf && buf->GetFrameCount() > 0)
			{
				if (!buf->HasLoopPoints())
				{
					buf->SetLoopPoints(0, buf->GetFrameCount());
				}
				DoSnapLoopPoints(*buf, 256);
				m_SnappedLoopStart = buf->GetLoopStartFrame();
				m_SnappedLoopEnd = buf->GetLoopEndFrame();
				m_HasSnappedLoop = true;
				m_StatusNotice = "零交叉吸附完成: 起点 " + std::to_string(m_SnappedLoopStart) + " 帧, 终点 " + std::to_string(m_SnappedLoopEnd) + " 帧 (无缝循环)";
			}
			else
			{
				m_StatusNotice = "当前条目未加载有效的音频切片波形数据";
			}
		}

		auto activeBuf = GetSelectedBuffer();
		if (activeBuf && activeBuf->GetFrameCount() > 0 && transportBox.W > 540.0f)
		{
			char loopInfoStr[80];
			uint32_t lStart = activeBuf->GetLoopStartFrame();
			uint32_t lEnd = activeBuf->HasLoopPoints() ? activeBuf->GetLoopEndFrame() : activeBuf->GetFrameCount();
			std::snprintf(loopInfoStr, sizeof(loopInfoStr), "循环帧: %u ~ %u %s",
				lStart, lEnd, m_HasSnappedLoop ? "[已吸附零点]" : "");
			Wui::Label(ctx, { transportBox.X + 520.0f, btnY + 6.0f }, loopInfoStr,
				m_HasSnappedLoop ? Wui::WuiColor{ 0.35f, 0.85f, 0.95f, 1.0f } : theme.TextMuted, 11.0f);
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
		for (int b = 0; b < 6; ++b)
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

		// 4. Audio Profiler 实时声学与频谱监视卡片
		const Wui::WuiRect profilerBox { rect.X + 8.0f, curY, rect.W - 16.0f, 142.0f };
		RenderProfilerHud(ctx, profilerBox, host);
		curY += 148.0f;

		// 5. Variations Section (if compound sound event)
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

		for (int b = 0; b < 6; ++b)
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

namespace World
{
	void AudioMixerPanel::RenderVoiceInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host, const AudioLibraryItem& item)
	{
		const Wui::WuiTheme& theme = host.Theme();
		float curY = rect.Y + 8.0f;

		Wui::Label(ctx, { rect.X + 12.0f, curY }, item.displayName, theme.Text, 16.0f);
		curY += 22.0f;

		Wui::Label(ctx, { rect.X + 12.0f, curY + 2.0f }, "类型: 角色音色配置文件 (.wvoice) | 路径: " + item.path, theme.TextMuted, 11.0f);

		const Wui::WuiRect btnOpenDir { rect.X + rect.W - 120.0f, curY - 2.0f, 108.0f, 22.0f };
		if (Wui::Button(ctx, Wui::HashId("voice.btn.open_dir"), btnOpenDir, "打开文件夹", theme))
		{
			std::filesystem::path p(item.path);
			std::string dir = p.has_parent_path() ? p.parent_path().string() : ".";
			ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOW);
		}
		curY += 28.0f;

		// Voice Properties Box
		const Wui::WuiRect vBox { rect.X + 8.0f, curY, rect.W - 16.0f, 240.0f };
		Wui::PanelBackground(ctx, vBox, theme.PanelBg, 4.0f);

		float vy = vBox.Y + 10.0f;
		Wui::Label(ctx, { vBox.X + 12.0f, vy }, "角色音色属性 (Timbre Profile Parameters):", { 0.85f, 0.45f, 0.95f, 1.0f }, 12.5f);
		vy += 26.0f;

		Wui::Label(ctx, { vBox.X + 12.0f, vy + 2.0f }, "角色标识 (CharacterId): " + m_SelectedTimbre.characterId, theme.Text, 11.5f);
		Wui::Label(ctx, { vBox.X + 320.0f, vy + 2.0f }, "基础预设 (Preset): " + m_SelectedTimbre.presetName, theme.Text, 11.5f);
		vy += 28.0f;

		// Pitch shift
		char pitchStr[32];
		std::snprintf(pitchStr, sizeof(pitchStr), "音高微调 (半音): %+.1f", m_SelectedTimbre.pitchShiftSemi);
		Wui::Label(ctx, { vBox.X + 12.0f, vy + 2.0f }, pitchStr, theme.TextMuted, 11.5f);
		const Wui::WuiRect rPitch { vBox.X + 160.0f, vy + 2.0f, 180.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("voice.slider.pitch"), rPitch, m_SelectedTimbre.pitchShiftSemi, -12.0f, 12.0f, theme);
		vy += 28.0f;

		// Speech rate
		char rateStr[32];
		std::snprintf(rateStr, sizeof(rateStr), "语速倍率 (Rate): %.2fx", m_SelectedTimbre.rateScale);
		Wui::Label(ctx, { vBox.X + 12.0f, vy + 2.0f }, rateStr, theme.TextMuted, 11.5f);
		const Wui::WuiRect rRate { vBox.X + 160.0f, vy + 2.0f, 180.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("voice.slider.rate"), rRate, m_SelectedTimbre.rateScale, 0.5f, 2.0f, theme);
		vy += 28.0f;

		// Warmth
		char warmStr[32];
		std::snprintf(warmStr, sizeof(warmStr), "音色温暖度 (Warmth): %.2f", m_SelectedTimbre.warmth);
		Wui::Label(ctx, { vBox.X + 12.0f, vy + 2.0f }, warmStr, theme.TextMuted, 11.5f);
		const Wui::WuiRect rWarm { vBox.X + 180.0f, vy + 2.0f, 160.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("voice.slider.warmth"), rWarm, m_SelectedTimbre.warmth, 0.0f, 1.0f, theme);
		vy += 28.0f;

		// Roughness
		char roughStr[32];
		std::snprintf(roughStr, sizeof(roughStr), "嗓音粗糙感 (Roughness): %.2f", m_SelectedTimbre.roughness);
		Wui::Label(ctx, { vBox.X + 12.0f, vy + 2.0f }, roughStr, theme.TextMuted, 11.5f);
		const Wui::WuiRect rRough { vBox.X + 180.0f, vy + 2.0f, 160.0f, 18.0f };
		Wui::SliderFloat(ctx, Wui::HashId("voice.slider.roughness"), rRough, m_SelectedTimbre.roughness, 0.0f, 1.0f, theme);
		vy += 38.0f;

		// Audition & Save Action bar
		curY += 250.0f;
		const Wui::WuiRect btnSynth { rect.X + 8.0f, curY, 200.0f, 28.0f };
		if (Wui::Button(ctx, Wui::HashId("voice.btn.synth"), btnSynth, "▶ 试听音色台词合成", theme))
		{
			auto& audio = Audio::AudioSystem::Get();
			auto& studio = audio.GetAiStudio();
			studio.RegisterTimbre(m_SelectedTimbre);
			std::string testLine = "你好，这是 " + m_SelectedTimbre.characterId + " 的音色台词试听效果！";
			auto result = studio.SynthesizeLine(m_SelectedTimbre.characterId, testLine, Audio::VoiceEmotion::Neutral, 0.7f);
			if (result.success)
			{
				std::string previewPath = "assets/audio/voices/preview_" + m_SelectedTimbre.characterId + ".waudio";
				audio.RegisterAudioBuffer(previewPath, std::make_shared<Audio::AudioBuffer>(result.audio));
				Audio::SoundEvent previewEvt("preview_dialogue", Audio::kBusVoice);
				previewEvt.GetRootContainer()->variations.push_back({ previewPath, 1.0f });
				audio.RegisterSoundEvent(previewEvt);
				audio.PostEvent2D("preview_dialogue");
				m_StatusNotice = "正在试听音色合成: " + m_SelectedTimbre.characterId;
			}
		}

		const Wui::WuiRect btnSaveVoice { rect.X + 220.0f, curY, 180.0f, 28.0f };
		if (Wui::Button(ctx, Wui::HashId("voice.btn.save"), btnSaveVoice, "保存音色配置 (.wvoice)", theme))
		{
			std::ofstream out(item.path);
			if (out.is_open())
			{
				out << m_SelectedTimbre.SerializeYaml();
				Audio::AudioSystem::Get().GetAiStudio().RegisterTimbre(m_SelectedTimbre);
				m_StatusNotice = "已成功保存音色配置到: " + item.path;
			}
		}
	}

	void AudioMixerPanel::RenderMixerInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host, const AudioLibraryItem& item)
	{
		const Wui::WuiTheme& theme = host.Theme();
		auto& audio = Audio::AudioSystem::Get();
		auto& mixer = audio.GetMixerGraph();

		float curY = rect.Y + 8.0f;
		Wui::Label(ctx, { rect.X + 12.0f, curY }, item.displayName, theme.Text, 16.0f);
		curY += 22.0f;

		Wui::Label(ctx, { rect.X + 12.0f, curY + 2.0f }, "类型: 混音总线拓扑配置 (.wmixer) | 路径: " + item.path, theme.TextMuted, 11.0f);

		const Wui::WuiRect btnOpenDir { rect.X + rect.W - 120.0f, curY - 2.0f, 108.0f, 22.0f };
		if (Wui::Button(ctx, Wui::HashId("mixer.btn.open_dir"), btnOpenDir, "打开文件夹", theme))
		{
			std::filesystem::path p(item.path);
			std::string dir = p.has_parent_path() ? p.parent_path().string() : ".";
			ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOW);
		}
		curY += 28.0f;

		// Mixer Layout Box
		const Wui::WuiRect mBox { rect.X + 8.0f, curY, rect.W - 16.0f, 200.0f };
		Wui::PanelBackground(ctx, mBox, theme.PanelBg, 4.0f);

		float my = mBox.Y + 10.0f;
		Wui::Label(ctx, { mBox.X + 12.0f, my }, "总线架构与默认设计电平 (Mixer Bus Hierarchy):", { 0.95f, 0.75f, 0.20f, 1.0f }, 12.5f);
		my += 24.0f;

		const char* busNames[] = { "Master (主总线)", "Music (背景音乐)", "SFX (场景音效)", "Voice (对白配音)", "UI (界面音效)", "Ambience (环境底噪)" };
		for (size_t b = 0; b < 6; ++b)
		{
			auto* bus = mixer.FindBus(static_cast<Audio::BusId>(b));
			if (!bus) continue;

			Wui::Label(ctx, { mBox.X + 12.0f, my + 2.0f }, busNames[b], theme.Text, 11.5f);
			const Wui::WuiRect sRect { mBox.X + 170.0f, my + 2.0f, 160.0f, 18.0f };
			float v = bus->GetVolume();
			Wui::SliderFloat(ctx, Wui::HashId(("mbus.s." + std::to_string(b)).c_str()), sRect, v, 0.0f, 1.5f, theme);
			if (std::abs(v - bus->GetVolume()) > 0.001f)
			{
				bus->SetVolume(v);
			}

			char volStr[16];
			std::snprintf(volStr, sizeof(volStr), "%d%%", static_cast<int>(bus->GetVolume() * 100.0f));
			Wui::Label(ctx, { mBox.X + 340.0f, my + 2.0f }, volStr, theme.TextMuted, 10.5f);

			my += 26.0f;
		}

		curY += 212.0f;

		const Wui::WuiRect btnApplyMixer { rect.X + 8.0f, curY, 210.0f, 28.0f };
		if (Wui::Button(ctx, Wui::HashId("mixer.btn.apply"), btnApplyMixer, "▶ 重新加载并应用此混音配置", theme))
		{
			std::ifstream in(item.path);
			if (in.is_open())
			{
				std::stringstream ss;
				ss << in.rdbuf();
				mixer.DeserializeYaml(ss.str());
				m_StatusNotice = "已重新加载并应用混音配置: " + item.path;
			}
		}

		const Wui::WuiRect btnSaveMixer { rect.X + 230.0f, curY, 210.0f, 28.0f };
		if (Wui::Button(ctx, Wui::HashId("mixer.btn.save_file"), btnSaveMixer, "保存混音配置 (.wmixer)", theme))
		{
			std::ofstream out(item.path);
			if (out.is_open())
			{
				out << mixer.SerializeYaml();
				m_StatusNotice = "已成功保存当前总线混音配置到: " + item.path;
			}
		}
	}

	void AudioMixerPanel::RenderProfilerHud(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg, 4.0f);

		auto& audio = Audio::AudioSystem::Get();
		const auto profilerSnap = audio.GetProfiler().GetSnapshot();
		const bool isClipped = profilerSnap.masterClipping || audio.GetProfiler().IsMasterClipping();
		const double dspTime = audio.GetDspTime();

		float curY = rect.Y + 8.0f;

		// 1. Header Row: Title + DSP Absolute Timestamp + DAC Clipping LED
		Wui::Label(ctx, { rect.X + 12.0f, curY }, "音频分析器 (Audio Profiler HUD)", { 0.35f, 0.85f, 0.95f, 1.0f }, 13.0f);

		// DSP Absolute Time
		char dspStr[64];
		std::snprintf(dspStr, sizeof(dspStr), "DSP 绝对时间: %.3f s", dspTime);
		Wui::Label(ctx, { rect.X + rect.W - 250.0f, curY + 1.0f }, dspStr, theme.TextMuted, 11.5f);

		// Clipping Indicator LED Badge
		const Wui::WuiRect ledRect { rect.X + rect.W - 96.0f, curY, 86.0f, 18.0f };
		if (isClipped)
		{
			Wui::PanelBackground(ctx, ledRect, { 0.95f, 0.20f, 0.20f, 0.95f }, 3.0f);
			Wui::Label(ctx, { ledRect.X + 8.0f, ledRect.Y + 2.0f }, "● DAC 削波", { 1.0f, 1.0f, 1.0f, 1.0f }, 10.5f);
		}
		else
		{
			Wui::PanelBackground(ctx, ledRect, { 0.15f, 0.32f, 0.20f, 0.90f }, 3.0f);
			Wui::Label(ctx, { ledRect.X + 8.0f, ledRect.Y + 2.0f }, "● 输出正常", { 0.45f, 0.85f, 0.55f, 0.95f }, 10.5f);
		}
		Wui::Tooltip(ctx, ledRect, isClipped ? "警告: 发生 DAC 输出削波失真 (Clipping)！请降低总线增益" : "DAC 输出电平正常，无削波失真");

		curY += 24.0f;

		// 2. Voice Resource Indicators (Physical & Virtual Voice Progress Bars)
		const float halfW = (rect.W - 32.0f) * 0.5f;

		// Physical Voice Bar
		char physText[48];
		std::snprintf(physText, sizeof(physText), "物理声部: %u / %u", profilerSnap.activePhysicalVoices, profilerSnap.totalPhysicalVoices);
		Wui::Label(ctx, { rect.X + 12.0f, curY }, physText, theme.Text, 11.0f);

		const float physLabelWidth = 140.0f;
		const Wui::WuiRect physBarBg { rect.X + 12.0f + physLabelWidth, curY + 2.0f, std::max(20.0f, halfW - physLabelWidth), 10.0f };
		Wui::PanelBackground(ctx, physBarBg, theme.ContentBg, 2.0f);
		const float physRatio = std::clamp(static_cast<float>(profilerSnap.activePhysicalVoices) /
			static_cast<float>(std::max(1u, profilerSnap.totalPhysicalVoices)), 0.0f, 1.0f);
		if (physRatio > 0.005f)
		{
			Wui::PanelBackground(ctx, { physBarBg.X, physBarBg.Y, physBarBg.W * physRatio, physBarBg.H }, { 0.25f, 0.80f, 0.95f, 0.95f }, 2.0f);
		}

		// Virtual Voice Bar
		const float virtX = rect.X + 20.0f + halfW;
		char virtText[48];
		std::snprintf(virtText, sizeof(virtText), "虚拟等待声部: %u / %u", profilerSnap.activeVirtualVoices, profilerSnap.totalVirtualVoices);
		Wui::Label(ctx, { virtX, curY }, virtText, theme.Text, 11.0f);

		const float virtLabelWidth = 160.0f;
		const Wui::WuiRect virtBarBg { virtX + virtLabelWidth, curY + 2.0f, std::max(20.0f, halfW - virtLabelWidth), 10.0f };
		Wui::PanelBackground(ctx, virtBarBg, theme.ContentBg, 2.0f);
		const float virtRatio = std::clamp(static_cast<float>(profilerSnap.activeVirtualVoices) /
			static_cast<float>(std::max(1u, profilerSnap.totalVirtualVoices)), 0.0f, 1.0f);
		if (virtRatio > 0.005f)
		{
			Wui::PanelBackground(ctx, { virtBarBg.X, virtBarBg.Y, virtBarBg.W * virtRatio, virtBarBg.H }, { 0.70f, 0.50f, 0.95f, 0.95f }, 2.0f);
		}

		curY += 20.0f;

		// 3. FFT Spectrum Bars (32 bands)
		audio.GetSpectrum(m_SpectrumBands, 32);
		if (m_SpectrumBands.size() < 32)
		{
			m_SpectrumBands.assign(32, 0.0f);
		}

		const float specH = 60.0f;
		const Wui::WuiRect specBox { rect.X + 12.0f, curY, rect.W - 24.0f, specH };
		Wui::PanelBackground(ctx, specBox, theme.ContentBg, 3.0f);

		const float padX = 6.0f;
		const float padY = 4.0f;
		const float innerW = specBox.W - padX * 2.0f;
		const float innerH = specBox.H - padY * 2.0f;
		const float barGap = 2.0f;
		const float barW = std::max(2.0f, (innerW - 31.0f * barGap) / 32.0f);

		for (uint32_t b = 0; b < 32; ++b)
		{
			float val = std::clamp(m_SpectrumBands[b], 0.0f, 1.0f);
			float barH = std::max(2.0f, val * innerH);
			float bx = specBox.X + padX + static_cast<float>(b) * (barW + barGap);
			float by = specBox.Y + specBox.H - padY - barH;

			// Colorful gradient across 32 frequency bands:
			// Bass (cyan/blue) -> Mid (green/emerald) -> Presence (gold/amber) -> Treble (coral/red)
			const float t = static_cast<float>(b) / 31.0f;
			Wui::WuiColor barColor;
			if (t < 0.33f)
			{
				const float u = t / 0.33f;
				barColor = { glm::mix(0.18f, 0.25f, u), glm::mix(0.75f, 0.88f, u), glm::mix(0.95f, 0.45f, u), val > 0.03f ? 0.95f : 0.40f };
			}
			else if (t < 0.66f)
			{
				const float u = (t - 0.33f) / 0.33f;
				barColor = { glm::mix(0.25f, 0.95f, u), glm::mix(0.88f, 0.78f, u), glm::mix(0.45f, 0.20f, u), val > 0.03f ? 0.95f : 0.40f };
			}
			else
			{
				const float u = (t - 0.66f) / 0.34f;
				barColor = { glm::mix(0.95f, 0.95f, u), glm::mix(0.78f, 0.30f, u), glm::mix(0.20f, 0.35f, u), val > 0.03f ? 0.95f : 0.40f };
			}

			Wui::PanelBackground(ctx, { bx, by, barW, barH }, barColor, 1.0f);
		}

		// 4. Frequency axis labels under the spectrum
		curY += specH + 3.0f;
		Wui::Label(ctx, { specBox.X + 4.0f, curY }, "20Hz", theme.TextMuted, 9.5f);
		Wui::Label(ctx, { specBox.X + specBox.W * 0.25f - 10.0f, curY }, "250Hz", theme.TextMuted, 9.5f);
		Wui::Label(ctx, { specBox.X + specBox.W * 0.50f - 10.0f, curY }, "1kHz", theme.TextMuted, 9.5f);
		Wui::Label(ctx, { specBox.X + specBox.W * 0.75f - 10.0f, curY }, "6kHz", theme.TextMuted, 9.5f);
		Wui::Label(ctx, { specBox.X + specBox.W - 28.0f, curY }, "20kHz", theme.TextMuted, 9.5f);
	}
}
