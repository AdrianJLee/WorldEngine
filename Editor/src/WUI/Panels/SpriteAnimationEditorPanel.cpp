#include "wldpch.h"
#include "WUI/Panels/SpriteAnimationEditorPanel.h"
#include "World/Core/Log.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Renderer/Texture/TextureLibrary.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/Core/StringPool.h"

#include <filesystem>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace World
{
	SpriteAnimationEditorPanel::SpriteAnimationEditorPanel(std::string logicalPath)
		: m_LogicalPath(std::move(logicalPath))
	{
		std::replace(m_LogicalPath.begin(), m_LogicalPath.end(), '\\', '/');
		m_PanelId = "anim:" + m_LogicalPath;
		m_PanelTitle = "动画: " + std::filesystem::path(m_LogicalPath).filename().string();
		Reload();
	}

	std::filesystem::path SpriteAnimationEditorPanel::ResolvePhysicalPath() const
	{
		std::filesystem::path p(m_LogicalPath);
		if (std::filesystem::exists(p))
			return std::filesystem::absolute(p);
		std::filesystem::path fromProj = Paths::ProjectDir() / p;
		if (std::filesystem::exists(fromProj))
			return std::filesystem::absolute(fromProj);
		std::filesystem::path fromAssetRoot = Paths::AssetRoot() / p;
		if (std::filesystem::exists(fromAssetRoot))
			return std::filesystem::absolute(fromAssetRoot);
		const std::string s = m_LogicalPath;
		if (s.rfind("assets/", 0) == 0 || s.rfind("assets\\", 0) == 0)
		{
			std::filesystem::path stripped = Paths::AssetRoot() / s.substr(7);
			if (std::filesystem::exists(stripped))
				return std::filesystem::absolute(stripped);
		}
		if (!p.is_absolute())
			return Paths::ProjectDir() / p;
		return p;
	}

	void SpriteAnimationEditorPanel::EnsureAnimationAtlasLoaded()
	{
		if (!m_Animation || m_EditAtlasPath.empty())
			return;

		std::string atlasPath = m_EditAtlasPath;
		if (atlasPath == m_LoadedAtlasPath && m_Atlas)
			return;

		std::string logical = atlasPath;
		if (logical.rfind("assets/", 0) == 0 || logical.rfind("assets\\", 0) == 0)
			logical = logical.substr(7);

		std::filesystem::path phys = Paths::AssetRoot() / logical;
		if (!std::filesystem::exists(phys))
			phys = Paths::ProjectDir() / atlasPath;

		m_Atlas = TextureAtlas::GetOrLoad(phys.string());
		m_LoadedAtlasPath = atlasPath;

		if (m_Atlas && !m_Atlas->GetTexturePath().empty())
		{
			std::string texPath = m_Atlas->GetTexturePath();
			if (texPath.rfind("assets/", 0) == 0 || texPath.rfind("assets\\", 0) == 0)
				texPath = texPath.substr(7);
			PathId pid = StringPool::Get().InternPath(texPath);
			auto gpu = TextureLibrary::Get().Get(pid, true);
			if (gpu)
			{
				m_GpuTexture = gpu;
				if (m_UiTextureId == 0)
					m_UiTextureId = Wui::WuiTextureRegistry::Get().Register(gpu);
				else
					Wui::WuiTextureRegistry::Get().Update(m_UiTextureId, gpu);
			}
		}
	}

	bool SpriteAnimationEditorPanel::Reload(std::string* message)
	{
		const auto phys = ResolvePhysicalPath();
		m_Animation = SpriteAnimation::LoadFromFile(phys.string());
		if (!m_Animation)
		{
			m_StatusFeedback = "无法载入动画资产: " + m_LogicalPath;
			if (message) *message = m_StatusFeedback;
			return false;
		}

		m_EditAtlasPath = m_Animation->GetAtlasPath();
		m_FrameRate = m_Animation->GetFrameRate();
		m_LoopMode = m_Animation->GetLoopMode();
		m_SelectedFrameIndex = m_Animation->GetFrameCount() > 0 ? 0 : -1;
		m_ActiveSampledFrame = 0;
		m_PlayheadTime = 0.0f;
		m_IsPlaying = false;
		SyncFromSelectedFrame();
		m_Dirty = false;
		m_StatusFeedback = "就绪: 已载入 " + std::to_string(m_Animation->GetFrameCount()) + " 帧";
		if (message) *message = m_StatusFeedback;
		return true;
	}

	bool SpriteAnimationEditorPanel::Save(std::string* message)
	{
		CommitToSelectedFrame();
		if (!m_Animation)
		{
			m_StatusFeedback = "无可用动画数据保存";
			if (message) *message = m_StatusFeedback;
			return false;
		}

		m_Animation->SetAtlasPath(m_EditAtlasPath);
		m_Animation->SetFrameRate(m_FrameRate);
		m_Animation->SetLoopMode(m_LoopMode);

		const auto phys = ResolvePhysicalPath();
		if (!m_Animation->SaveToFile(phys.string()))
		{
			m_StatusFeedback = "保存失败: " + phys.string();
			if (message) *message = m_StatusFeedback;
			return false;
		}

		SpriteAnimation::ClearCache();
		m_Dirty = false;
		m_StatusFeedback = "动画已保存: " + m_LogicalPath;
		if (message) *message = m_StatusFeedback;
		return true;
	}

	void SpriteAnimationEditorPanel::Play()
	{
		m_IsPlaying = true;
	}

	void SpriteAnimationEditorPanel::Stop()
	{
		m_IsPlaying = false;
	}

	void SpriteAnimationEditorPanel::Reset()
	{
		m_IsPlaying = false;
		m_PlayheadTime = 0.0f;
		m_ActiveSampledFrame = 0;
		if (m_Animation && m_Animation->GetFrameCount() > 0)
			m_SelectedFrameIndex = 0;
		SyncFromSelectedFrame();
	}

	void SpriteAnimationEditorPanel::NextFrame()
	{
		if (!m_Animation || m_Animation->GetFrameCount() == 0) return;
		CommitToSelectedFrame();
		m_SelectedFrameIndex = (m_SelectedFrameIndex + 1) % static_cast<int>(m_Animation->GetFrameCount());
		m_ActiveSampledFrame = static_cast<uint32_t>(m_SelectedFrameIndex);
		SyncFromSelectedFrame();
	}

	void SpriteAnimationEditorPanel::PrevFrame()
	{
		if (!m_Animation || m_Animation->GetFrameCount() == 0) return;
		CommitToSelectedFrame();
		m_SelectedFrameIndex--;
		if (m_SelectedFrameIndex < 0)
			m_SelectedFrameIndex = static_cast<int>(m_Animation->GetFrameCount()) - 1;
		m_ActiveSampledFrame = static_cast<uint32_t>(m_SelectedFrameIndex);
		SyncFromSelectedFrame();
	}

	void SpriteAnimationEditorPanel::SyncFromSelectedFrame()
	{
		if (!m_Animation || m_SelectedFrameIndex < 0 ||
			m_SelectedFrameIndex >= static_cast<int>(m_Animation->GetFrameCount()))
		{
			m_EditFrameSub.clear();
			m_EditFrameDuration = 0.0833f;
			m_EditFrameEvent.clear();
			return;
		}

		const auto* frame = m_Animation->GetFrame(static_cast<size_t>(m_SelectedFrameIndex));
		if (!frame) return;

		m_EditFrameSub = frame->NameString;
		m_EditFrameDuration = frame->Duration;
		m_EditFrameEvent = frame->EventTag;
	}

	void SpriteAnimationEditorPanel::CommitToSelectedFrame()
	{
		if (!m_Animation || m_SelectedFrameIndex < 0 ||
			m_SelectedFrameIndex >= static_cast<int>(m_Animation->GetFrameCount()))
			return;

		std::vector<SpriteAnimationFrame> frames = m_Animation->GetAllFrames();
		if (static_cast<size_t>(m_SelectedFrameIndex) >= frames.size())
			return;

		frames[static_cast<size_t>(m_SelectedFrameIndex)] = SpriteAnimationFrame(
			m_EditFrameSub,
			m_EditFrameDuration > 0.0f ? m_EditFrameDuration : 0.0833f,
			m_EditFrameEvent
		);

		m_Animation->ClearFrames();
		for (const auto& frame : frames)
			m_Animation->AddFrame(frame);
	}

	void SpriteAnimationEditorPanel::AddFrame(const std::string& subTexture, float duration, const std::string& eventTag)
	{
		if (!m_Animation)
			m_Animation = std::make_shared<SpriteAnimation>("new_anim", m_FrameRate, m_LoopMode);

		CommitToSelectedFrame();
		m_Animation->AddFrame(SpriteAnimationFrame(subTexture, duration, eventTag));
		m_SelectedFrameIndex = static_cast<int>(m_Animation->GetFrameCount()) - 1;
		m_ActiveSampledFrame = static_cast<uint32_t>(m_SelectedFrameIndex);
		SyncFromSelectedFrame();
		m_Dirty = true;
		m_StatusFeedback = "已添加帧: " + subTexture;
	}

	bool SpriteAnimationEditorPanel::DeleteFrame(size_t index)
	{
		if (!m_Animation || index >= m_Animation->GetFrameCount())
			return false;

		std::vector<SpriteAnimationFrame> frames = m_Animation->GetAllFrames();
		frames.erase(frames.begin() + static_cast<std::ptrdiff_t>(index));

		m_Animation->ClearFrames();
		for (const auto& frame : frames)
			m_Animation->AddFrame(frame);

		if (m_Animation->GetFrameCount() == 0)
		{
			m_SelectedFrameIndex = -1;
			m_ActiveSampledFrame = 0;
		}
		else if (m_SelectedFrameIndex >= static_cast<int>(m_Animation->GetFrameCount()))
		{
			m_SelectedFrameIndex = static_cast<int>(m_Animation->GetFrameCount()) - 1;
			m_ActiveSampledFrame = static_cast<uint32_t>(m_SelectedFrameIndex);
		}

		SyncFromSelectedFrame();
		m_Dirty = true;
		m_StatusFeedback = "已删除帧 #" + std::to_string(index);
		return true;
	}

	void SpriteAnimationEditorPanel::ImportAllAtlasSlices()
	{
		EnsureAnimationAtlasLoaded();
		if (!m_Atlas || m_Atlas->GetSubTextureCount() == 0)
		{
			m_StatusFeedback = "未关联有效图集或图集内无子切片";
			return;
		}
		if (!m_Animation)
			m_Animation = std::make_shared<SpriteAnimation>("new_anim", m_FrameRate, m_LoopMode);

		m_Animation->ClearFrames();
		const float dur = m_FrameRate > 0.0f ? (1.0f / m_FrameRate) : 0.0833f;
		for (const auto& sub : m_Atlas->GetAllSubTextures())
		{
			m_Animation->AddFrame(SpriteAnimationFrame(sub.Name, dur));
		}
		m_SelectedFrameIndex = m_Animation->GetFrameCount() > 0 ? 0 : -1;
		m_ActiveSampledFrame = 0;
		SyncFromSelectedFrame();
		m_Dirty = true;
		m_StatusFeedback = "已一键导入图集全部 " + std::to_string(m_Animation->GetFrameCount()) + " 个切片为序列帧";
	}

	void SpriteAnimationEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg);

		EnsureAnimationAtlasLoaded();

		// Frame timing & Playback sampling
		const auto now = std::chrono::steady_clock::now();
		float dt = 0.016f;
		if (m_LastFrameTime.time_since_epoch().count() != 0)
		{
			dt = std::chrono::duration<float>(now - m_LastFrameTime).count();
			if (dt <= 0.0f || dt > 0.1f) dt = 0.016f;
		}
		m_LastFrameTime = now;

		if (m_IsPlaying && m_Animation && m_Animation->GetFrameCount() > 0)
		{
			m_PlayheadTime += dt;
			bool finished = false;
			m_ActiveSampledFrame = m_Animation->SampleFrame(m_PlayheadTime, &finished);
			if (finished && m_LoopMode == AnimationLoopMode::Once)
			{
				m_IsPlaying = false;
			}
		}
		else if (!m_IsPlaying && m_SelectedFrameIndex >= 0 && m_Animation &&
			static_cast<size_t>(m_SelectedFrameIndex) < m_Animation->GetFrameCount())
		{
			m_ActiveSampledFrame = static_cast<uint32_t>(m_SelectedFrameIndex);
		}

		float curY = rect.Y + 6.0f;

		// 1. Top Bar: Header, Playback Transport Controls, Save button
		const Wui::WuiRect topBar { rect.X + 8.0f, curY, rect.W - 16.0f, 32.0f };
		Wui::BarSurface(ctx, topBar, theme.PanelHeader, theme.Border);
		Wui::Label(ctx, { topBar.X + 10.0f, topBar.Y + 7.0f }, "2D 序列帧动画编辑器 (Sprite Animation Workstation)", theme.Accent, 13.5f);

		std::string pathText = "资产: " + m_LogicalPath;
		Wui::Label(ctx, { topBar.X + 340.0f, topBar.Y + 8.0f }, pathText, theme.TextMuted, 11.0f);

		const Wui::WuiRect btnSave { topBar.X + topBar.W - 110.0f, topBar.Y + 4.0f, 102.0f, 24.0f };
		Wui::Tooltip(ctx, btnSave, "保存动画配置到 .wanim 文件并刷新缓存 (anim.btn.save)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.save"), btnSave, m_Dirty ? "💾 保存动画 *" : "💾 保存动画", theme))
		{
			Save();
		}

		curY += 38.0f;

		// 2. Playback Control Strip
		const Wui::WuiRect ctrlBar { rect.X + 8.0f, curY, rect.W - 16.0f, 34.0f };
		Wui::PanelBackground(ctx, ctrlBar, theme.ContentBg, 3.0f);

		float cX = ctrlBar.X + 8.0f;

		// Play / Pause
		const Wui::WuiRect btnPlay { cX, ctrlBar.Y + 5.0f, 76.0f, 24.0f };
		Wui::Tooltip(ctx, btnPlay, "播放或暂停序列帧动画 (anim.btn.play)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.play"), btnPlay, m_IsPlaying ? "❚❚ 暂停" : "▶ 播放", theme))
		{
			m_IsPlaying = !m_IsPlaying;
		}
		cX += 82.0f;

		// Reset
		const Wui::WuiRect btnReset { cX, ctrlBar.Y + 5.0f, 68.0f, 24.0f };
		Wui::Tooltip(ctx, btnReset, "停止并重置动画时间指针至第 0 帧 (anim.btn.reset)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.reset"), btnReset, "■ 重置", theme))
		{
			Reset();
		}
		cX += 74.0f;

		// Prev / Next
		const Wui::WuiRect btnPrev { cX, ctrlBar.Y + 5.0f, 72.0f, 24.0f };
		Wui::Tooltip(ctx, btnPrev, "单步后退至上一帧 (anim.btn.prev_frame)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.prev_frame"), btnPrev, "◀ 上一帧", theme))
		{
			PrevFrame();
		}
		cX += 78.0f;

		const Wui::WuiRect btnNext { cX, ctrlBar.Y + 5.0f, 72.0f, 24.0f };
		Wui::Tooltip(ctx, btnNext, "单步前进至下一帧 (anim.btn.next_frame)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.next_frame"), btnNext, "▶ 下一帧", theme))
		{
			NextFrame();
		}
		cX += 78.0f;

		// FPS Input
		Wui::Label(ctx, { cX, ctrlBar.Y + 9.0f }, "FPS:", theme.TextMuted, 11.5f);
		cX += 34.0f;
		const Wui::WuiRect fpsRect { cX, ctrlBar.Y + 5.0f, 54.0f, 24.0f };
		Wui::Tooltip(ctx, fpsRect, "动画帧率 FPS (anim.field.fps)");
		float oldFps = m_FrameRate;
		if (Wui::DragFloat(ctx, Wui::HashId("anim.field.fps"), fpsRect, m_FrameRate, 0.5f, 1.0f, 60.0f, theme))
		{
			if (m_FrameRate != oldFps && m_Animation)
			{
				m_Animation->SetFrameRate(m_FrameRate);
				m_Dirty = true;
				SyncFromSelectedFrame();
			}
		}
		cX += 62.0f;

		// Loop Mode button
		std::string loopStr = "循环 (Loop)";
		if (m_LoopMode == AnimationLoopMode::Once) loopStr = "单次 (Once)";
		else if (m_LoopMode == AnimationLoopMode::PingPong) loopStr = "往返 (PingPong)";

		const Wui::WuiRect btnLoop { cX, ctrlBar.Y + 5.0f, 100.0f, 24.0f };
		Wui::Tooltip(ctx, btnLoop, "切换循环模式: Loop / Once / PingPong (anim.btn.loop_mode)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.loop_mode"), btnLoop, loopStr, theme))
		{
			if (m_LoopMode == AnimationLoopMode::Loop) m_LoopMode = AnimationLoopMode::Once;
			else if (m_LoopMode == AnimationLoopMode::Once) m_LoopMode = AnimationLoopMode::PingPong;
			else m_LoopMode = AnimationLoopMode::Loop;
			if (m_Animation) m_Animation->SetLoopMode(m_LoopMode);
			m_Dirty = true;
		}
		cX += 108.0f;

		// Playback Status String
		std::ostringstream ss;
		if (m_IsPlaying)
			ss << "● 播放中 | ";
		else
			ss << "❚❚ 暂停 | ";
		ss << std::fixed << std::setprecision(2) << "进度: " << m_PlayheadTime << "s | 帧 "
		   << m_ActiveSampledFrame << "/" << (m_Animation ? m_Animation->GetFrameCount() : 0);
		Wui::Label(ctx, { cX, ctrlBar.Y + 9.0f }, ss.str(), m_IsPlaying ? theme.Accent : theme.TextMuted, 11.5f);

		curY += 40.0f;
		const float contentH = rect.H - (curY - rect.Y) - 8.0f;
		const float leftW = std::min(320.0f, rect.W * 0.36f);
		const float rightX = rect.X + 8.0f + leftW + 8.0f;
		const float rightW = rect.W - 16.0f - leftW - 8.0f;

		// 3. Left Column: Atlas Binding, Frames List & Actions
		const Wui::WuiRect leftCol { rect.X + 8.0f, curY, leftW, contentH };
		Wui::PanelBackground(ctx, leftCol, theme.ContentBg, 4.0f);

		float alY = leftCol.Y + 8.0f;
		Wui::Label(ctx, { leftCol.X + 10.0f, alY }, "关联图集设置 (Atlas Binding)", theme.Text, 12.0f);
		alY += 22.0f;

		// Atlas path input
		Wui::Label(ctx, { leftCol.X + 10.0f, alY + 3.0f }, "图集路径:", theme.TextMuted, 11.0f);
		const Wui::WuiRect atlasRect { leftCol.X + 70.0f, alY, leftCol.W - 78.0f, 22.0f };
		std::string oldAtlas = m_EditAtlasPath;
		if (Wui::TextField(ctx, Wui::HashId("anim.field.atlas_path"), atlasRect, m_EditAtlasPath, theme) || m_EditAtlasPath != oldAtlas)
		{
			if (m_Animation) m_Animation->SetAtlasPath(m_EditAtlasPath);
			m_Dirty = true;
			EnsureAnimationAtlasLoaded();
		}
		alY += 26.0f;

		// One-click import all slices from atlas button
		const Wui::WuiRect btnImportAll { leftCol.X + 6.0f, alY, leftCol.W - 12.0f, 22.0f };
		Wui::Tooltip(ctx, btnImportAll, "一键将关联图集内的全部子切片导入为动画序列帧 (anim.btn.import_all)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.import_all"), btnImportAll, "📥 一键导入图集全部切片为帧", theme))
		{
			ImportAllAtlasSlices();
		}
		alY += 28.0f;

		// Frames Summary
		size_t fCount = m_Animation ? m_Animation->GetFrameCount() : 0;
		float totalDur = m_Animation ? m_Animation->GetTotalDuration() : 0.0f;
		std::ostringstream durInfo;
		durInfo << "总时长: " << std::fixed << std::setprecision(3) << totalDur << "s · 共 " << fCount << " 帧";
		Wui::Label(ctx, { leftCol.X + 10.0f, alY }, durInfo.str(), theme.TextMuted, 11.0f);
		alY += 20.0f;

		// Frame Toolbar: Add Frame, Delete Frame
		const float fBtnHalfW = (leftCol.W - 20.0f) * 0.5f;

		const Wui::WuiRect btnAddF { leftCol.X + 6.0f, alY, fBtnHalfW, 22.0f };
		Wui::Tooltip(ctx, btnAddF, "在末尾追加新序列帧 (anim.btn.add_frame)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.add_frame"), btnAddF, "＋ 添加帧", theme))
		{
			std::string defSub = m_EditFrameSub.empty() ? ("frame_" + std::to_string(fCount)) : m_EditFrameSub;
			float dur = m_FrameRate > 0.0f ? (1.0f / m_FrameRate) : 0.0833f;
			AddFrame(defSub, dur, "");
		}

		const Wui::WuiRect btnDelF { leftCol.X + 10.0f + fBtnHalfW, alY, fBtnHalfW, 22.0f };
		Wui::Tooltip(ctx, btnDelF, "删除当前选中的序列帧 (anim.btn.del_frame)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.del_frame"), btnDelF, "✖ 删除帧", theme))
		{
			if (m_SelectedFrameIndex >= 0)
				DeleteFrame(static_cast<size_t>(m_SelectedFrameIndex));
		}
		alY += 26.0f;

		// Frame List Box with ScrollArea
		float fListH = leftCol.H - (alY - leftCol.Y) - 6.0f;
		const Wui::WuiRect fListRect { leftCol.X + 6.0f, alY, leftCol.W - 12.0f, fListH };
		Wui::PanelBackground(ctx, fListRect, theme.PanelBg, 2.0f);

		if (m_Animation)
		{
			const auto& allFrames = m_Animation->GetAllFrames();
			const float totalH = static_cast<float>(allFrames.size()) * 26.0f + 6.0f;
			Wui::BeginScrollArea(ctx, fListRect, totalH, m_ListScrollY, theme, Wui::HashId("anim.scroll.list"));
			float itemY = fListRect.Y - m_ListScrollY + 3.0f;
			for (size_t i = 0; i < allFrames.size(); ++i)
			{
				const Wui::WuiRect itemRect { fListRect.X + 4.0f, itemY, fListRect.W - 8.0f, 22.0f };
				bool isSelected = (static_cast<int>(i) == m_SelectedFrameIndex);
				bool isPlayingNow = (m_IsPlaying && i == m_ActiveSampledFrame);

				std::ostringstream rowLabel;
				if (isPlayingNow) rowLabel << "▶ ";
				else if (isSelected) rowLabel << "● ";
				else rowLabel << "  ";

				rowLabel << "#" << i << " " << allFrames[i].NameString
						 << " (" << std::fixed << std::setprecision(2) << allFrames[i].Duration << "s)";
				if (!allFrames[i].EventTag.empty())
					rowLabel << " [" << allFrames[i].EventTag << "]";

				const std::string fBtnId = "anim.item." + std::to_string(i);
				if (Wui::Button(ctx, Wui::HashId(fBtnId.c_str()), itemRect, rowLabel.str(), theme))
				{
					CommitToSelectedFrame();
					m_SelectedFrameIndex = static_cast<int>(i);
					m_ActiveSampledFrame = static_cast<uint32_t>(i);
					SyncFromSelectedFrame();
				}
				itemY += 26.0f;
			}
			Wui::EndScrollArea(ctx);
		}

		// 4. Right Column: Top Live Flipbook Canvas + Bottom Frame Inspector
		const Wui::WuiRect rightCol { rightX, curY, rightW, contentH };

		const float canvasH = std::min(240.0f, contentH * 0.52f);
		const Wui::WuiRect canvasBox { rightX, curY, rightW, canvasH };
		Wui::PanelBackground(ctx, canvasBox, theme.ContentBg, 4.0f);

		std::string frameInfo = "(无帧)";
		if (m_Animation && m_ActiveSampledFrame < m_Animation->GetFrameCount())
		{
			const auto* f = m_Animation->GetFrame(m_ActiveSampledFrame);
			if (f) frameInfo = "#" + std::to_string(m_ActiveSampledFrame) + " " + f->NameString;
		}
		std::string canvasTitle = "动画走带画布 (Live Flipbook Playback) · " + frameInfo;
		Wui::Label(ctx, { canvasBox.X + 12.0f, canvasBox.Y + 8.0f }, canvasTitle, m_IsPlaying ? theme.Accent : theme.Text, 12.0f);

		const Wui::WuiRect innerCanvas { canvasBox.X + 8.0f, canvasBox.Y + 28.0f, canvasBox.W - 16.0f, canvasBox.H - 36.0f };
		Wui::DrawPanelSurface(ctx, innerCanvas, theme);

		if (m_Animation && m_ActiveSampledFrame < m_Animation->GetFrameCount())
		{
			const auto* f = m_Animation->GetFrame(m_ActiveSampledFrame);
			const SubTexture2D* sub = (f && m_Atlas) ? m_Atlas->FindSubTexture(f->NameString) : nullptr;
			if (sub)
			{
				const float sliceW = sub->PixelRect.z > 0.0f ? sub->PixelRect.z : 32.0f;
				const float sliceH = sub->PixelRect.w > 0.0f ? sub->PixelRect.w : 32.0f;

				const float maxDrawW = innerCanvas.W - 24.0f;
				const float maxDrawH = innerCanvas.H - 24.0f;
				float scale = std::min(maxDrawW / sliceW, maxDrawH / sliceH);
				if (scale > 1.0f) scale = std::floor(scale);
				scale = std::max(scale, 0.1f);

				const float drawW = sliceW * scale;
				const float drawH = sliceH * scale;
				const Wui::WuiRect imgRect {
					innerCanvas.X + (innerCanvas.W - drawW) * 0.5f,
					innerCanvas.Y + (innerCanvas.H - drawH) * 0.5f,
					drawW,
					drawH
				};

				if (m_UiTextureId != 0 && m_Atlas->GetWidth() > 0 && m_Atlas->GetHeight() > 0)
				{
					const float invW = 1.0f / static_cast<float>(m_Atlas->GetWidth());
					const float invH = 1.0f / static_cast<float>(m_Atlas->GetHeight());
					const float u0 = sub->PixelRect.x * invW;
					const float v0 = sub->PixelRect.y * invH;
					const float uW = sliceW * invW;
					const float vH = sliceH * invH;

					Wui::Image(ctx, imgRect, m_UiTextureId, { u0, v0, uW, vH }, theme);
				}

				// Pivot Crosshair (cyan cross)
				const float pvScreenX = imgRect.X + sub->Pivot.x * imgRect.W;
				const float pvScreenY = imgRect.Y + (1.0f - sub->Pivot.y) * imgRect.H;
				const Wui::WuiColor pvCol { 0.15f, 0.95f, 0.95f, 1.0f };
				Wui::LineSegment(ctx, { pvScreenX - 8.0f, pvScreenY }, { pvScreenX + 8.0f, pvScreenY }, pvCol, 1.5f);
				Wui::LineSegment(ctx, { pvScreenX, pvScreenY - 8.0f }, { pvScreenX, pvScreenY + 8.0f }, pvCol, 1.5f);

				// HUD Badge
				std::ostringstream hudStr;
				hudStr << (m_IsPlaying ? "● 播放中 · " : "❚❚ 暂停 · ") << f->NameString << " (" << static_cast<int>(sliceW)
					   << "x" << static_cast<int>(sliceH) << " px) · 时长: " << std::fixed << std::setprecision(3) << f->Duration << "s";
				if (!f->EventTag.empty())
					hudStr << " · 事件: [" << f->EventTag << "]";
				Wui::Label(ctx, { innerCanvas.X + 8.0f, innerCanvas.Y + innerCanvas.H - 16.0f }, hudStr.str(), theme.TextMuted, 10.0f);
			}
		}

		// Bottom Half: Selected Frame Inspector
		float inspY = curY + canvasH + 8.0f;
		float inspH = contentH - canvasH - 8.0f;
		if (inspH > 50.0f)
		{
			const Wui::WuiRect inspBox { rightX, inspY, rightW, inspH };
			Wui::PanelBackground(ctx, inspBox, theme.ContentBg, 4.0f);

			Wui::Label(ctx, { inspBox.X + 12.0f, inspBox.Y + 8.0f }, "当前帧参数 (Frame Inspector)", theme.Text, 12.5f);

			float fieldY = inspBox.Y + 28.0f;
			if (m_SelectedFrameIndex >= 0 && m_Animation &&
				m_SelectedFrameIndex < static_cast<int>(m_Animation->GetFrameCount()))
			{
				bool fieldsChanged = false;

				// SubTexture Name input
				Wui::Label(ctx, { inspBox.X + 12.0f, fieldY + 3.0f }, "子图名称 (SubTexture):", theme.TextMuted, 11.5f);
				const Wui::WuiRect subRect { inspBox.X + 140.0f, fieldY, std::min(240.0f, inspBox.W - 160.0f), 22.0f };
				std::string oldSub = m_EditFrameSub;
				if (Wui::TextField(ctx, Wui::HashId("anim.field.sub"), subRect, m_EditFrameSub, theme) || m_EditFrameSub != oldSub)
				{
					fieldsChanged = true;
				}
				fieldY += 26.0f;

				// Quick SubTexture selector buttons from bound atlas
				if (m_Atlas && m_Atlas->GetSubTextureCount() > 0)
				{
					Wui::Label(ctx, { inspBox.X + 12.0f, fieldY + 2.0f }, "图集切片快速点选:", theme.TextMuted, 10.5f);
					float chipX = inspBox.X + 140.0f;
					const auto& allSubs = m_Atlas->GetAllSubTextures();
					for (size_t si = 0; si < std::min<size_t>(6, allSubs.size()); ++si)
					{
						const float chipW = 75.0f;
						if (chipX + chipW > inspBox.X + inspBox.W - 10.0f) break;
						const Wui::WuiRect chipRect { chipX, fieldY, chipW, 20.0f };
						const std::string chipId = "anim.chip." + std::to_string(si);
						if (Wui::Button(ctx, Wui::HashId(chipId.c_str()), chipRect, allSubs[si].Name, theme))
						{
							m_EditFrameSub = allSubs[si].Name;
							fieldsChanged = true;
						}
						chipX += chipW + 4.0f;
					}
					fieldY += 24.0f;
				}

				// Duration (s) input
				Wui::Label(ctx, { inspBox.X + 12.0f, fieldY + 3.0f }, "帧持续时长 (Duration s):", theme.TextMuted, 11.5f);
				const Wui::WuiRect durRect { inspBox.X + 140.0f, fieldY, 90.0f, 22.0f };
				if (Wui::DragFloat(ctx, Wui::HashId("anim.field.dur"), durRect, m_EditFrameDuration, 0.01f, 0.01f, 10.0f, theme))
				{
					fieldsChanged = true;
				}
				fieldY += 26.0f;

				// Event Tag input
				Wui::Label(ctx, { inspBox.X + 12.0f, fieldY + 3.0f }, "关键帧打点 (Event Tag):", theme.TextMuted, 11.5f);
				const Wui::WuiRect evtRect { inspBox.X + 140.0f, fieldY, std::min(240.0f, inspBox.W - 160.0f), 22.0f };
				std::string oldEvt = m_EditFrameEvent;
				if (Wui::TextField(ctx, Wui::HashId("anim.field.evt"), evtRect, m_EditFrameEvent, theme) || m_EditFrameEvent != oldEvt)
				{
					fieldsChanged = true;
				}

				if (fieldsChanged)
				{
					m_Dirty = true;
					CommitToSelectedFrame();
				}
			}
			else
			{
				Wui::Label(ctx, { inspBox.X + 24.0f, fieldY + 16.0f }, "请在左侧帧列表中选择一个序列帧进行编辑", theme.TextMuted, 12.0f);
			}

			// Status Feedback Bar
			const Wui::WuiRect statusRect { inspBox.X + 12.0f, inspBox.Y + inspBox.H - 22.0f, inspBox.W - 24.0f, 18.0f };
			Wui::Label(ctx, { statusRect.X, statusRect.Y + 1.0f }, m_StatusFeedback, theme.TextMuted, 10.5f);
		}
	}
}
