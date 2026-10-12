#include "wldpch.h"
#include "WUI/Panels/SpriteAnimationEditorPanel.h"
#include "World/Core/Log.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <filesystem>
#include <algorithm>
#include <sstream>
#include <iomanip>

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

		m_FrameRate = m_Animation->GetFrameRate();
		m_LoopMode = m_Animation->GetLoopMode();
		m_SelectedFrameIndex = m_Animation->GetFrameCount() > 0 ? 0 : -1;
		m_PlayheadTime = 0.0f;
		m_ActiveSampledFrame = 0;
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

		m_Animation->SetFrameRate(m_FrameRate);
		m_Animation->SetLoopMode(m_LoopMode);

		const auto phys = ResolvePhysicalPath();
		if (!m_Animation->SaveToFile(phys.string()))
		{
			m_StatusFeedback = "保存动画失败: " + phys.string();
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

	void SpriteAnimationEditorPanel::PrevFrame()
	{
		m_IsPlaying = false;
		if (!m_Animation || m_Animation->GetFrameCount() == 0) return;
		if (m_SelectedFrameIndex > 0)
			m_SelectedFrameIndex--;
		else
			m_SelectedFrameIndex = static_cast<int>(m_Animation->GetFrameCount()) - 1;
		m_ActiveSampledFrame = static_cast<uint32_t>(m_SelectedFrameIndex);
		SyncFromSelectedFrame();
	}

	void SpriteAnimationEditorPanel::NextFrame()
	{
		m_IsPlaying = false;
		if (!m_Animation || m_Animation->GetFrameCount() == 0) return;
		if (static_cast<size_t>(m_SelectedFrameIndex + 1) < m_Animation->GetFrameCount())
			m_SelectedFrameIndex++;
		else
			m_SelectedFrameIndex = 0;
		m_ActiveSampledFrame = static_cast<uint32_t>(m_SelectedFrameIndex);
		SyncFromSelectedFrame();
	}

	void SpriteAnimationEditorPanel::SyncFromSelectedFrame()
	{
		if (!m_Animation || m_SelectedFrameIndex < 0 ||
			static_cast<size_t>(m_SelectedFrameIndex) >= m_Animation->GetFrameCount())
		{
			m_EditFrameSub.clear();
			m_EditFrameDuration = 0.0833f;
			m_EditFrameEvent.clear();
			return;
		}

		const SpriteAnimationFrame* f = m_Animation->GetFrame(static_cast<size_t>(m_SelectedFrameIndex));
		if (!f) return;

		m_EditFrameSub = f->NameString;
		m_EditFrameDuration = f->Duration;
		m_EditFrameEvent = f->EventTag;
	}

	void SpriteAnimationEditorPanel::CommitToSelectedFrame()
	{
		if (!m_Animation || m_SelectedFrameIndex < 0 ||
			static_cast<size_t>(m_SelectedFrameIndex) >= m_Animation->GetFrameCount())
			return;

		std::vector<SpriteAnimationFrame> frames = m_Animation->GetAllFrames();
		if (static_cast<size_t>(m_SelectedFrameIndex) >= frames.size())
			return;

		frames[static_cast<size_t>(m_SelectedFrameIndex)] = SpriteAnimationFrame(
			m_EditFrameSub,
			m_EditFrameDuration,
			m_EditFrameEvent
		);

		m_Animation->ClearFrames();
		for (const auto& frame : frames)
			m_Animation->AddFrame(frame);
	}

	void SpriteAnimationEditorPanel::AddFrame(const std::string& subTexture, float duration, const std::string& eventTag)
	{
		if (!m_Animation)
			m_Animation = std::make_shared<SpriteAnimation>("anim", 12.0f);

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

	void SpriteAnimationEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg);

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
		Wui::Label(ctx, { topBar.X + 10.0f, topBar.Y + 7.0f }, "序列帧动画编辑器 (Sprite Animation Editor)", theme.Accent, 13.5f);

		std::string pathText = "资产: " + m_LogicalPath;
		Wui::Label(ctx, { topBar.X + 320.0f, topBar.Y + 8.0f }, pathText, theme.TextMuted, 11.0f);

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
		const Wui::WuiRect btnPlay { cX, ctrlBar.Y + 5.0f, 80.0f, 24.0f };
		Wui::Tooltip(ctx, btnPlay, "播放或暂停序列帧动画 (anim.btn.play)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.play"), btnPlay, m_IsPlaying ? "❚❚ 暂停" : "▶ 播放", theme))
		{
			m_IsPlaying = !m_IsPlaying;
		}
		cX += 86.0f;

		// Reset
		const Wui::WuiRect btnReset { cX, ctrlBar.Y + 5.0f, 68.0f, 24.0f };
		Wui::Tooltip(ctx, btnReset, "停止并重置动画时间指针至第 0 帧 (anim.btn.reset)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.reset"), btnReset, "■ 重置", theme))
		{
			Reset();
		}
		cX += 74.0f;

		// Prev / Next
		const Wui::WuiRect btnPrev { cX, ctrlBar.Y + 5.0f, 76.0f, 24.0f };
		Wui::Tooltip(ctx, btnPrev, "单步后退至上一帧 (anim.btn.prev_frame)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.prev_frame"), btnPrev, "◀ 上一帧", theme))
		{
			PrevFrame();
		}
		cX += 82.0f;

		const Wui::WuiRect btnNext { cX, ctrlBar.Y + 5.0f, 76.0f, 24.0f };
		Wui::Tooltip(ctx, btnNext, "单步前进至下一帧 (anim.btn.next_frame)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.next_frame"), btnNext, "▶ 下一帧", theme))
		{
			NextFrame();
		}
		cX += 90.0f;

		// FPS Input
		Wui::Label(ctx, { cX, ctrlBar.Y + 9.0f }, "FPS:", theme.TextMuted, 11.5f);
		cX += 36.0f;
		const Wui::WuiRect fpsRect { cX, ctrlBar.Y + 5.0f, 60.0f, 24.0f };
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
		cX += 70.0f;

		// Loop Mode button
		std::string loopStr = "循环: 循环 (Loop)";
		if (m_LoopMode == AnimationLoopMode::Once) loopStr = "循环: 单次 (Once)";
		else if (m_LoopMode == AnimationLoopMode::PingPong) loopStr = "循环: 往返 (PingPong)";

		const Wui::WuiRect btnLoop { cX, ctrlBar.Y + 5.0f, 130.0f, 24.0f };
		Wui::Tooltip(ctx, btnLoop, "切换循环模式: Loop / Once / PingPong (anim.btn.loop_mode)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.loop_mode"), btnLoop, loopStr, theme))
		{
			if (m_LoopMode == AnimationLoopMode::Loop) m_LoopMode = AnimationLoopMode::Once;
			else if (m_LoopMode == AnimationLoopMode::Once) m_LoopMode = AnimationLoopMode::PingPong;
			else m_LoopMode = AnimationLoopMode::Loop;
			if (m_Animation) m_Animation->SetLoopMode(m_LoopMode);
			m_Dirty = true;
		}
		cX += 140.0f;

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
		const float leftW = std::min(320.0f, rect.W * 0.4f);
		const float rightX = rect.X + 8.0f + leftW + 8.0f;
		const float rightW = rect.W - 16.0f - leftW - 8.0f;

		// 3. Left Column: Frames List & Frame Actions
		const Wui::WuiRect leftCol { rect.X + 8.0f, curY, leftW, contentH };
		Wui::PanelBackground(ctx, leftCol, theme.ContentBg, 4.0f);

		Wui::Label(ctx, { leftCol.X + 10.0f, leftCol.Y + 8.0f }, "序列帧列表 (Frames)", theme.Text, 12.5f);

		size_t fCount = m_Animation ? m_Animation->GetFrameCount() : 0;
		float totalDur = m_Animation ? m_Animation->GetTotalDuration() : 0.0f;
		std::ostringstream durInfo;
		durInfo << "总时长: " << std::fixed << std::setprecision(3) << totalDur << "s · 共 " << fCount << " 帧";
		Wui::Label(ctx, { leftCol.X + 10.0f, leftCol.Y + 28.0f }, durInfo.str(), theme.TextMuted, 11.0f);

		// Frame Toolbar: Add Frame, Delete Frame
		float fBtnY = leftCol.Y + 48.0f;
		const float fBtnHalfW = (leftCol.W - 24.0f) * 0.5f;

		const Wui::WuiRect btnAddF { leftCol.X + 8.0f, fBtnY, fBtnHalfW, 24.0f };
		Wui::Tooltip(ctx, btnAddF, "在末尾追加新序列帧 (anim.btn.add_frame)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.add_frame"), btnAddF, "＋ 添加帧", theme))
		{
			std::string defSub = m_EditFrameSub.empty() ? ("frame_" + std::to_string(fCount)) : m_EditFrameSub;
			float dur = m_FrameRate > 0.0f ? (1.0f / m_FrameRate) : 0.0833f;
			AddFrame(defSub, dur, "");
		}

		const Wui::WuiRect btnDelF { leftCol.X + 16.0f + fBtnHalfW, fBtnY, fBtnHalfW, 24.0f };
		Wui::Tooltip(ctx, btnDelF, "删除当前选中的序列帧 (anim.btn.del_frame)");
		if (Wui::Button(ctx, Wui::HashId("anim.btn.del_frame"), btnDelF, "✖ 删除帧", theme))
		{
			if (m_SelectedFrameIndex >= 0)
				DeleteFrame(static_cast<size_t>(m_SelectedFrameIndex));
		}

		// Frame List Box
		float fListY = fBtnY + 30.0f;
		float fListH = leftCol.H - (fListY - leftCol.Y) - 8.0f;
		const Wui::WuiRect fListRect { leftCol.X + 6.0f, fListY, leftCol.W - 12.0f, fListH };
		Wui::PanelBackground(ctx, fListRect, theme.PanelBg, 2.0f);

		if (m_Animation)
		{
			const auto& allFrames = m_Animation->GetAllFrames();
			float itemY = fListRect.Y + 4.0f;
			for (size_t i = 0; i < allFrames.size(); ++i)
			{
				if (itemY + 24.0f > fListRect.Y + fListRect.H)
					break;

				const Wui::WuiRect itemRect { fListRect.X + 4.0f, itemY, fListRect.W - 8.0f, 24.0f };
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
				itemY += 28.0f;
			}
		}

		// 4. Right Column: Selected Frame Inspector
		const Wui::WuiRect rightCol { rightX, curY, rightW, contentH };
		Wui::PanelBackground(ctx, rightCol, theme.ContentBg, 4.0f);

		Wui::Label(ctx, { rightCol.X + 12.0f, rightCol.Y + 10.0f }, "序列帧属性检视 (Frame Inspector)", theme.Text, 13.0f);

		float fInspY = rightCol.Y + 36.0f;
		if (m_SelectedFrameIndex >= 0 && m_Animation &&
			static_cast<size_t>(m_SelectedFrameIndex) < m_Animation->GetFrameCount())
		{
			bool changed = false;

			// SubTexture Name
			Wui::Label(ctx, { rightCol.X + 12.0f, fInspY + 4.0f }, "子图名称 (SubTexture):", theme.TextMuted, 11.5f);
			const Wui::WuiRect subRect { rightCol.X + 150.0f, fInspY, std::min(260.0f, rightCol.W - 170.0f), 24.0f };
			Wui::Tooltip(ctx, subRect, "图集中的切片名称 (anim.field.frame_sub)");
			std::string oldSub = m_EditFrameSub;
			if (Wui::TextField(ctx, Wui::HashId("anim.field.frame_sub"), subRect, m_EditFrameSub, theme) || m_EditFrameSub != oldSub)
			{
				changed = true;
			}
			fInspY += 32.0f;

			// Duration
			Wui::Label(ctx, { rightCol.X + 12.0f, fInspY + 4.0f }, "帧持续时间 (Duration, s):", theme.TextMuted, 11.5f);
			const Wui::WuiRect durRect { rightCol.X + 150.0f, fInspY, 100.0f, 24.0f };
			Wui::Tooltip(ctx, durRect, "本帧持续秒数 (anim.field.frame_duration)");
			if (Wui::DragFloat(ctx, Wui::HashId("anim.field.frame_duration"), durRect, m_EditFrameDuration, 0.005f, 0.005f, 10.0f, theme))
			{
				changed = true;
			}
			fInspY += 32.0f;

			// Event Tag
			Wui::Label(ctx, { rightCol.X + 12.0f, fInspY + 4.0f }, "事件打点 (Event Tag):", theme.TextMuted, 11.5f);
			const Wui::WuiRect evtRect { rightCol.X + 150.0f, fInspY, std::min(260.0f, rightCol.W - 170.0f), 24.0f };
			Wui::Tooltip(ctx, evtRect, "播放到达此帧时触发的事件标识 (anim.field.frame_event)");
			std::string oldEvt = m_EditFrameEvent;
			if (Wui::TextField(ctx, Wui::HashId("anim.field.frame_event"), evtRect, m_EditFrameEvent, theme) || m_EditFrameEvent != oldEvt)
			{
				changed = true;
			}
			fInspY += 32.0f;

			if (changed)
			{
				m_Dirty = true;
				CommitToSelectedFrame();
			}
		}
		else
		{
			Wui::Label(ctx, { rightCol.X + 24.0f, fInspY + 20.0f }, "请在左侧列表中选择序列帧，或点击 [＋ 添加帧] 创建新帧。", theme.TextMuted, 12.0f);
		}

		// Status Feedback
		const Wui::WuiRect statusRect { rightCol.X + 12.0f, rightCol.Y + rightCol.H - 26.0f, rightCol.W - 24.0f, 20.0f };
		Wui::Label(ctx, { statusRect.X, statusRect.Y + 3.0f }, m_StatusFeedback, theme.TextMuted, 11.0f);
	}
}



