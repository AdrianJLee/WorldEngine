#include "WidgetGalleryPanel_Internal.h"

namespace World
{

using namespace WidgetGalleryPanelDetail;


	// ---- WUI-P1.6b:Play 模式(真实输入直通)+ 交互契约 runner ----
	//
	// 口径(与派工单/plan §P1.6 一致):
	//   · Play = `draw.RouteRealInput=true` + `draw.State="default"`(不套伪状态),
	//     hover/pressed/focus 由真实输入产生;工作台只**观测**、不强制外观;
	//   · 注入写的是 `WuiInputState`(屏幕坐标系就是窗口客户区;按下/抬起/按键**沿**在正确的
	//     帧上产生)—— 与 `ui.invoke`/`ui.key` 的 `WuiScriptedInput` 是同一条输入结构,
	//     控件侧没有任何"测试专用分支";
	//   · 注入只作用于 showcase 绘制段(保存/恢复 ctx.Input(),与 Edit 的 PseudoState 同一挂点):
	//     同一帧里工作台自己的控件看到的是真实指针,不被脚本化的"手"污染。
	//   · 阶段用**墙钟**定时,`_live.json` 在阶段切换时落盘 —— 探针据此在 press-hold 段里
	//     用 `capture.float` 抓 pressed 帧(按下沿只存在一帧,抓不到就变成猜)。
void WidgetGalleryPanel::SetPlayMode(Wui::WuiContext& ctx, bool play){
		if (m_PlayMode == play)
			return;
		m_PlayMode = play;
		if (m_Run.Active)
		{
			// 运行中切模式:不写半份证据(证据文件只在一条契约跑完时落盘)。
			m_Run.Active = false;
			m_Run.Result = "cancelled";
			ctx.RecordOp("gallery", "mode", "run-cancelled", m_Run.ComponentId);
		}
		if (!play)
		{
			// 回到 Edit:清观测,免得上一轮的 Play 计数被当成 Edit 的观测。
			m_PlayState = "default";
			m_PlayObservation.clear();
			m_PlayObservationPrev.clear();
			m_PlayFocusName.clear();
			m_PlayLastEvent.clear();
			m_PlayLastEventFrame = 0;
			m_PlayEvents.clear();
			m_PlayHoverSeen = false;
			m_PlayPressCount = m_PlayClickCount = m_PlayReleaseCount = m_PlayKeyCount = 0;
			m_PlayA11yJson.clear();
			m_PlayA11yPrevJson.clear();
		}
		m_RunStatus = play
			? Wui::Tr("workbench.play.ready",
				"Play: real input routed into the showcase (no forced state). "
				"Run interactions writes evidence per contract.")
			: Wui::Tr("workbench.play.edit",
				"Edit: forced states + property overrides (baselines unchanged).");
		ctx.RecordOp("gallery", "mode", "Workbench", play ? "play" : "edit");
	}


std::string WidgetGalleryPanel::PlayObservationText() const{
		std::string out = std::string("play state=") + m_PlayState
			+ " focus=" + (m_PlayFocusName.empty() ? std::string("none") : m_PlayFocusName)
			+ " press=" + std::to_string(m_PlayPressCount)
			+ " click=" + std::to_string(m_PlayClickCount)
			+ " release=" + std::to_string(m_PlayReleaseCount)
			+ " key=" + std::to_string(m_PlayKeyCount)
			+ " events=" + std::to_string(m_PlayEvents.size());
		if (!m_PlayLastEvent.empty())
			out += " last=" + m_PlayLastEvent + "@" + std::to_string(m_PlayLastEventFrame);
		return out;
	}


void WidgetGalleryPanel::StartInteractionRun(Wui::WuiContext& ctx, const Wui::WuiComponentDesc& desc){
		if (m_PlayMode == false || desc.Interactions.empty())
		{
			m_RunStatus = Wui::Tr("workbench.play.static",
				"Static component: no interaction contract declared (nothing to run).");
			return;
		}
		m_Run = InteractionRun {};
		m_Run.Active = true;
		m_Run.Id = "play-" + desc.Id + "-" + std::to_string(NowMs());
		m_Run.ComponentId = desc.Id;
		m_Run.WindowKey = ctx.WindowKey();
		m_Run.Index = 0;
		m_Run.StartFrame = ctx.Frame();
		m_Run.StartedWallMs = NowMs();
		BeginRunInteraction(ctx, desc);
		ctx.RecordOp("gallery", "interaction-run", desc.Id,
			std::to_string(desc.Interactions.size()) + " contracts");
	}


void WidgetGalleryPanel::BeginRunInteraction(Wui::WuiContext& ctx, const Wui::WuiComponentDesc& desc){
		const Wui::WuiComponentInteraction& item = desc.Interactions[m_Run.Index];
		m_Run.Kind = InteractionKindId(item.Kind);
		m_Run.Target = item.TargetId;
		m_Run.TargetId = Wui::HashId(item.TargetId.c_str());
		m_Run.Expect = InteractionExpectId(item.Expect);
		m_Run.Steps = item.Steps;
		m_Run.Note = item.Note;
		m_Run.TargetRect = ResolveInteractionTarget(item.TargetId, m_CanvasSlot,
			m_LastTargetId, m_LastTargetRect);
		m_Run.Result.clear();
		m_Run.GapReason.clear();
		m_Run.PressSeen = false;
		m_Run.ReleaseSeen = false;
		m_Run.ClickCompleted = false;
		m_Run.KeyEnterSeen = false;
		m_Run.KeySpaceSeen = false;
		m_Run.PressFrame = 0;
		m_Run.ReleaseFrame = 0;
		m_Run.TabsQueued = 0;
		m_Run.FocusReached = false;
		m_Run.FocusMatch.clear();
		m_Run.NextTabMs = 0;
		m_Run.TypeCharsInjected = 0;
		m_Run.PhaseEdgeSent = false;
		m_Run.PressHeld = false;
		m_Run.Phases.clear();
		m_Run.Timeline.clear();
		m_Run.Events.clear();
		m_Run.StartFrame = ctx.Frame();
		m_Run.StartedWallMs = NowMs();
		// before 快照取**上一帧**的画布 a11y(本帧的节点要等 showcase 画完才登记)。
		m_Run.A11yBefore = m_PlayA11yPrevJson;
		m_Run.ValueBefore = AccessNodeValue(m_Run.TargetId);
		const int start = StartRunPhase(m_Run.Kind);
		EnterRunPhase(start, RunPhaseDurationMs(start), RunPhaseNote(start, m_Run.Kind));
		m_RunStatus = std::string("interaction ") + std::to_string(m_Run.Index + 1) + "/"
			+ std::to_string(desc.Interactions.size()) + "  " + m_Run.Kind;
	}


void WidgetGalleryPanel::EnterRunPhase(int phase, uint64_t durationMs, const std::string& note){
		m_Run.Phase = phase;
		m_Run.PhaseEdgeSent = false;
		if (phase == kRunIdle || phase == kRunFinished || phase == kRunSettle)
			m_Run.PressHeld = false;   // 收起按住存续位(别把按下状态带出这条契约)
		const uint64_t now = NowMs();
		m_Run.PhaseStartMs = now;
		m_Run.PhaseDeadlineMs = now + durationMs;
		if (phase == kRunIdle || phase == kRunFinished)
			return;
		PlayPhaseRecord record;
		record.Name = RunPhaseName(phase);
		record.Note = note;
		record.WallMs = durationMs;
		m_Run.Phases.push_back(record);
		if (phase == kRunFocusScan)
			m_Run.NextTabMs = now;   // 第一帧就发第一次 Tab
		WriteInteractionLive(record.Name);
	}


bool WidgetGalleryPanel::AdvanceInteractionRun(Wui::WuiContext& ctx, const Wui::WuiRect& slot, const Wui::WuiComponentDesc& desc){
		if (!m_Run.Active)
			return false;
		if (m_Run.ComponentId != desc.Id)
		{
			m_Run.Active = false;
			m_Run.Result = "cancelled";
			m_RunStatus = Wui::Tr("workbench.play.run_cancelled",
				"Interaction run cancelled: the selected component changed");
			return false;
		}
		const uint64_t now = NowMs();
		const Wui::WuiRect target = m_Run.TargetRect.W > 0.0f && m_Run.TargetRect.H > 0.0f
			? m_Run.TargetRect : slot;
		const glm::vec2 center { target.X + target.W * 0.5f, target.Y + target.H * 0.5f };
		Wui::WuiInputState& input = ctx.Input();

		// ---- Key 契约:焦点扫描(阶段内每 NextTabMs 发一次 Tab;注入走 WuiScriptedInput,
		// 因为焦点导航发生在 BeginFrame —— 面板内直接改 KeyPressed 已经晚了)----
		if (m_Run.Phase == kRunFocusScan && !m_Run.FocusReached)
		{
			const Wui::WuiId focus = ctx.Focus();
			bool focusOnTarget = focus != 0 && focus == m_Run.TargetId;
			bool focusOnCanvas = false;
			if (!focusOnTarget && focus != 0 && m_CanvasRect.W > 0.0f)
			{
				if (const Wui::WuiAccessNode* node = Wui::WuiAccessibility::Get().Find(focus))
				{
					const Wui::WuiRect& r = node->Rect;
					focusOnCanvas = r.X + r.W > m_CanvasRect.X && r.X < m_CanvasRect.X + m_CanvasRect.W
						&& r.Y + r.H > m_CanvasRect.Y && r.Y < m_CanvasRect.Y + m_CanvasRect.H;
				}
			}
			if (focusOnTarget || focusOnCanvas)
			{
				m_Run.FocusReached = true;
				m_Run.FocusMatch = focusOnTarget ? "id" : "canvas";
				EnterRunPhase(kRunKeyEnter, RunPhaseDurationMs(kRunKeyEnter),
					RunPhaseNote(kRunKeyEnter, m_Run.Kind));
				return false;
			}
			if (now >= m_Run.PhaseDeadlineMs || m_Run.TabsQueued >= 90)
			{
				// 焦点到不了就如实记 gap(不 SetFocus —— 那等于伪造键盘可达性)。
				FinishCurrentInteraction(ctx, desc, "gap", "focus-unreachable");
				return false;
			}
			if (now >= m_Run.NextTabMs)
			{
				Wui::WuiScriptedInput::Get().QueueKey(ctx.WindowKey(), KeyCodes::Tab);
				++m_Run.TabsQueued;
				m_Run.NextTabMs = now + 130;
			}
			return false;
		}

		// ---- 阶段到点:下一段(或在最后一段结算)----
		if (now >= m_Run.PhaseDeadlineMs)
		{
			const int next = NextRunPhase(m_Run.Kind, m_Run.Phase);
			if (next != kRunFinished)
			{
				EnterRunPhase(next, RunPhaseDurationMs(next), RunPhaseNote(next, m_Run.Kind));
				return false;
			}
			const bool hoverSeen = std::any_of(m_Run.Phases.begin(), m_Run.Phases.end(),
				[](const PlayPhaseRecord& phase) { return phase.Hover; });
			const bool pressedSeen = std::any_of(m_Run.Phases.begin(), m_Run.Phases.end(),
				[](const PlayPhaseRecord& phase) { return phase.Pressed; });
			std::string gap;
			if (m_Run.Kind == "hover" && !hoverSeen)
				gap = "no-hover";
			else if (m_Run.Kind == "click" && !(m_Run.PressSeen && m_Run.ReleaseSeen))
				gap = "no-click";
			else if (m_Run.Kind == "key"
				&& !(m_Run.FocusReached && m_Run.KeyEnterSeen && m_Run.KeySpaceSeen))
				gap = m_Run.FocusReached ? "no-key-activation" : "focus-unreachable";
			else if (m_Run.Kind == "drag" && !pressedSeen)
				gap = "no-press";
			else if ((m_Run.Kind == "type" || m_Run.Kind == "scroll") && !hoverSeen)
				gap = "target-unreachable";
			else if (m_Run.Expect == "value-change" && !m_Run.ValueBefore.empty()
				&& m_Run.ValueBefore == AccessNodeValue(m_Run.TargetId))
				gap = "no-value-change";
			FinishCurrentInteraction(ctx, desc, gap.empty() ? "ok" : "gap", gap);
			return false;
		}

		// ---- 本阶段注入(只写 ctx.Input();调用方会保存/恢复,作用域 = showcase 段)----
		switch (m_Run.Phase)
		{
		case kRunApproach:
			input.MousePos = center;
			return true;
		case kRunClickSend:
			// 点击的**按下/抬起沿**走 WuiScriptedInput(与 `ui.invoke` 完全同一条队列,
			// 在 BeginFrame 之前注入)= 引擎的点击归属生命周期能正常跨帧存续;
			// 面板这一帧不注入,等 press 帧被观测到再进"按住段"。
			if (!m_Run.PhaseEdgeSent)
			{
				m_Run.PhaseEdgeSent = true;
				Wui::WuiScriptedInput::Get().QueueClick(ctx.WindowKey(), center);
			}
			else if (m_Run.PressSeen)
			{
				EnterRunPhase(kRunPressHold, RunPhaseDurationMs(kRunPressHold),
					RunPhaseNote(kRunPressHold, m_Run.Kind));
			}
			return false;
		case kRunPressHold:
			input.MousePos = center;
			input.MouseDown[0] = true;
			// click 的按下沿已经由脚本化点击给出;按住段只把 MouseDown 保持住(像素证据窗口)。
			if (m_Run.Kind != "click")
				input.MouseClicked[0] = !m_Run.PhaseEdgeSent;   // 首帧 = 按下沿
			m_Run.PressHeld = true;
			m_Run.PhaseEdgeSent = true;
			return true;
		case kRunRelease:
			input.MousePos = center;
			input.MouseDown[0] = false;
			input.MouseReleased[0] = !m_Run.PhaseEdgeSent;  // 首帧 = 抬起沿
			m_Run.PressHeld = false;                        // 抬起沿之后不再保持
			m_Run.PhaseEdgeSent = true;
			return true;
		case kRunLeave:
			input.MousePos = LeavePoint(target, m_CanvasInner);
			return true;
		case kRunKeyEnter:
		case kRunKeySpace:
		{
			const uint32_t key = m_Run.Phase == kRunKeyEnter ? KeyCodes::Enter : KeyCodes::Space;
			input.MousePos = center;
			input.MouseDown[0] = false;
			input.KeyDown.clear();
			input.KeyPressed.clear();
			if (!m_Run.PhaseEdgeSent)
			{
				input.KeyDown.push_back(key);
				input.KeyPressed.push_back(key);   // 边沿:控件按 WasKeyPressed 消费
				m_Run.PhaseEdgeSent = true;
			}
			return true;
		}
		case kRunDragMove:
		{
			const float progress = std::min(1.0f,
				static_cast<float>(now - m_Run.PhaseStartMs) / static_cast<float>(
					RunPhaseDurationMs(kRunDragMove)));
			input.MousePos = { center.x + 60.0f * progress, center.y + 24.0f * progress };
			input.MouseDown[0] = true;
			m_Run.PressHeld = true;
			return true;
		}
		case kRunTypeText:
		{
			static const char* const kScript = "play";
			input.MousePos = center;
			const int step = static_cast<int>((now - m_Run.PhaseStartMs) / 150ULL);
			const int length = static_cast<int>(std::strlen(kScript));
			if (step >= 0 && step < length && m_Run.TypeCharsInjected <= step)
			{
				input.TextInput.push_back(static_cast<uint32_t>(kScript[step]));
				m_Run.TypeCharsInjected = step + 1;
			}
			return true;
		}
		case kRunScroll:
			input.MousePos = center;
			input.Wheel = -1.0f;   // 与真实滚轮同向(GLFW yoffset 向下为负)
			return true;
		default:
			return false;          // settle / idle / finished:不注入
		}
	}


void WidgetGalleryPanel::ObservePlayInput(Wui::WuiContext& ctx, const Wui::WuiRect& slot, const Wui::WuiComponentDesc& desc){
		const Wui::WuiRect target = (m_Run.Active && m_Run.TargetRect.W > 0.0f
			&& m_Run.TargetRect.H > 0.0f) ? m_Run.TargetRect : slot;
		const Wui::WuiInputState& input = ctx.Input();
		const bool hover = ctx.IsHovered(target);
		const bool pressed = hover && input.MouseDown[0];
		const bool pressedEdge = hover && input.MouseClicked[0];
		const bool releasedEdge = hover && input.MouseReleased[0];
		const Wui::WuiId focus = ctx.Focus();
		const Wui::WuiId fallbackTarget = Wui::HashId(("showcase." + desc.Id).c_str());
		const Wui::WuiId targetId = m_Run.Active && m_Run.TargetId != 0 ? m_Run.TargetId
			: fallbackTarget;
		// 目标矩形以 a11y 实测为准(此刻 showcase 已画完,本帧节点已登记):回填运行中的目标,
		// 并存成"上一帧缓存"给下一次契约切换用 —— 槽位 ≠ 控件,点空是实测踩过的坑。
		if (m_Run.Active && m_Run.TargetId != 0)
		{
			if (const Wui::WuiAccessNode* node = Wui::WuiAccessibility::Get().Find(m_Run.TargetId))
			{
				if (node->Rect.W > 0.0f && node->Rect.H > 0.0f)
				{
					m_Run.TargetRect = node->Rect;
					m_LastTargetId = m_Run.Target;
					m_LastTargetRect = node->Rect;
				}
			}
		}
		bool focusOnTarget = focus != 0 && focus == targetId;
		if (!focusOnTarget && focus != 0 && m_CanvasRect.W > 0.0f)
		{
			if (const Wui::WuiAccessNode* node = Wui::WuiAccessibility::Get().Find(focus))
			{
				const Wui::WuiRect& r = node->Rect;
				focusOnTarget = r.X + r.W > m_CanvasRect.X && r.X < m_CanvasRect.X + m_CanvasRect.W
					&& r.Y + r.H > m_CanvasRect.Y && r.Y < m_CanvasRect.Y + m_CanvasRect.H;
			}
		}

		std::string state = "default";
		if (pressed)
			state = "pressed";
		else if (hover)
			state = "hover";
		else if (focusOnTarget)
			state = "focus";
		m_PlayState = state;
		if (focusOnTarget || focus != 0)
			m_PlayFocusName = PlayFocusName(focus);
		else
			m_PlayFocusName.clear();

		const auto record = [&](const std::string& name, const std::string& detail) {
			m_PlayLastEvent = name;
			m_PlayLastEventFrame = ctx.Frame();
			const std::string line = detail + " frame=" + std::to_string(ctx.Frame());
			m_PlayEvents.emplace_back(name, line);
			if (m_PlayEvents.size() > 60)
				m_PlayEvents.erase(m_PlayEvents.begin());
			if (m_Run.Active)
				m_Run.Events.emplace_back(name, line);
		};
		if (hover && !m_PlayHoverSeen)
		{
			m_PlayHoverSeen = true;
			record("hover.enter", "hovered=1");
		}
		else if (!hover && m_PlayHoverSeen)
		{
			m_PlayHoverSeen = false;
			record("hover.leave", "hovered=0");
		}
		if (pressedEdge)
		{
			++m_PlayPressCount;
			++m_PlayClickCount;
			record("press.edge",
				"hovered=1 MouseClicked[0]=1 -> widget predicate (hovered && MouseClicked[0]) true");
			if (m_Run.Active)
			{
				m_Run.PressSeen = true;
				m_Run.PressFrame = ctx.Frame();
			}
		}
		if (releasedEdge)
		{
			++m_PlayReleaseCount;
			// 引擎口径的"这一次点击落在同一目标上":press 帧登记归属、release 帧核对。
			const bool completed = ctx.IsClickCompleted(0, target);
			record("release.edge", std::string("hovered=1 MouseReleased[0]=1 clickCompleted=")
				+ (completed ? "1" : "0"));
			if (m_Run.Active)
			{
				m_Run.ReleaseSeen = true;
				m_Run.ReleaseFrame = ctx.Frame();
				m_Run.ClickCompleted = completed;
			}
		}
		else if (pressed && m_Run.Active)
		{
			// 按住期间持续登记归属,保证抬起帧的 IsClickCompleted 有归属可核对。
			ctx.IsClickCompleted(0, target);
		}
		if (focusOnTarget && ctx.WasKeyPressed(KeyCodes::Enter))
		{
			++m_PlayKeyCount;
			record("key.enter", "focused=1 KeyPressed(Enter)=1 -> keyActivated true");
			if (m_Run.Active)
				m_Run.KeyEnterSeen = true;
		}
		if (focusOnTarget && ctx.WasKeyPressed(KeyCodes::Space))
		{
			++m_PlayKeyCount;
			record("key.space", "focused=1 KeyPressed(Space)=1 -> keyActivated true");
			if (m_Run.Active)
				m_Run.KeySpaceSeen = true;
		}

		if (m_Run.Active && !m_Run.Phases.empty())
		{
			PlayPhaseRecord& phase = m_Run.Phases.back();
			if (phase.FirstFrame == 0)
				phase.FirstFrame = ctx.Frame();
			phase.LastFrame = ctx.Frame();
			++phase.Frames;
			phase.Hover = phase.Hover || hover;
			phase.Pressed = phase.Pressed || pressed;
			phase.FocusOnTarget = phase.FocusOnTarget || focusOnTarget;
			if (m_Run.Timeline.size() < 700)
			{
				PlayTimelineEntry entry;
				entry.Frame = ctx.Frame();
				entry.Phase = phase.Name;
				entry.State = state;
				entry.MouseX = input.MousePos.x;
				entry.MouseY = input.MousePos.y;
				entry.Hover = hover;
				entry.Pressed = pressed;
				entry.Clicked = input.MouseClicked[0];
				entry.Released = input.MouseReleased[0];
				entry.Focus = focus;
				entry.FocusOnTarget = focusOnTarget;
				m_Run.Timeline.push_back(entry);
			}
		}

		m_PlayObservation = PlayObservationText();
		m_PlayA11yPrevJson = m_PlayA11yJson;
		m_PlayA11yJson = CanvasA11yJson(ctx);
	}


std::string WidgetGalleryPanel::CanvasA11yJson(const Wui::WuiContext& ctx) const{
		std::ostringstream out;
		out << "[";
		bool first = true;
		for (const Wui::WuiAccessNode& node : Wui::WuiAccessibility::Get().Nodes())
		{
			if (node.Window != ctx.WindowKey())
				continue;
			const Wui::WuiRect& r = node.Rect;
			if (r.X + r.W <= m_CanvasRect.X || r.X >= m_CanvasRect.X + m_CanvasRect.W
				|| r.Y + r.H <= m_CanvasRect.Y || r.Y >= m_CanvasRect.Y + m_CanvasRect.H)
				continue;
			out << (first ? "\n" : ",\n");
			first = false;
			out << "    {\"id\": " << node.Id << ", \"kind\": \"" << JsonEscape(node.Kind)
				<< "\", \"label\": \"" << JsonEscape(node.Label) << "\", \"value\": \""
				<< JsonEscape(node.Value) << "\", \"enabled\": " << (node.Enabled ? "true" : "false")
				<< ", \"focused\": " << (node.Focused ? "true" : "false")
				<< ", \"interactive\": " << (node.Interactive ? "true" : "false")
				<< ", \"rect\": [" << r.X << ", " << r.Y << ", " << r.W << ", " << r.H << "]}";
		}
		out << (first ? "]" : "\n  ]");
		return out.str();
	}


void WidgetGalleryPanel::FinishCurrentInteraction(Wui::WuiContext& ctx, const Wui::WuiComponentDesc& desc, const char* result, const std::string& gap){
		if (!m_Run.Active)
			return;
		m_Run.Result = result;
		m_Run.GapReason = gap;
		m_Run.PressHeld = false;
		m_Run.ValueAfter = AccessNodeValue(m_Run.TargetId);
		m_Run.EndFrame = ctx.Frame();
		m_Run.FinishedWallMs = NowMs();
		WriteInteractionEvidence();
		m_Run.WrittenKinds.push_back(m_Run.Kind);
		const std::string summary = m_Run.Kind + std::string(": ") + m_Run.Result
			+ (m_Run.GapReason.empty() ? std::string() : std::string(" (") + m_Run.GapReason + ")");
		ctx.RecordOp("gallery", "interaction", m_Run.ComponentId, summary);
		++m_Run.Index;
		if (m_Run.Index < desc.Interactions.size())
		{
			BeginRunInteraction(ctx, desc);
			return;
		}
		WriteInteractionSummary();
		m_RunStatus = m_Run.Kind.empty()
			? summary
			: std::string("interactions done (") + std::to_string(m_Run.WrittenKinds.size())
				+ "): " + summary;
		m_Run.Active = false;
		m_Run.Phase = kRunIdle;
	}


void WidgetGalleryPanel::WriteInteractionLive(const std::string& phaseName) const{
		if (m_Run.ComponentId.empty())
			return;
		const std::filesystem::path directory =
			WorkbenchDir() / m_Run.ComponentId / "interaction";
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		std::ofstream file(directory / "_live.json", std::ios::binary | std::ios::trunc);
		if (!file)
			return;
		std::ostringstream out;
		out << "{\n";
		out << "  \"schema\": \"wui-workbench-interaction-live/1\",\n";
		out << "  \"run\": \"" << JsonEscape(m_Run.Id) << "\",\n";
		out << "  \"component\": \"" << JsonEscape(m_Run.ComponentId) << "\",\n";
		out << "  \"kind\": \"" << JsonEscape(m_Run.Kind) << "\",\n";
		out << "  \"index\": " << m_Run.Index << ",\n";
		out << "  \"phase\": \"" << JsonEscape(phaseName) << "\",\n";
		out << "  \"target\": \"" << JsonEscape(m_Run.Target) << "\",\n";
		out << "  \"active\": " << (m_Run.Active ? "true" : "false") << ",\n";
		out << "  \"tabs\": " << m_Run.TabsQueued << ",\n";
		out << "  \"focusReached\": " << (m_Run.FocusReached ? "true" : "false") << ",\n";
		out << "  \"uiScale\": " << (Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f) << ",\n";
		out << "  \"canvas\": {\"x\": " << m_CanvasRect.X << ", \"y\": " << m_CanvasRect.Y
			<< ", \"w\": " << m_CanvasRect.W << ", \"h\": " << m_CanvasRect.H << "},\n";
		out << "  \"slot\": {\"x\": " << m_CanvasSlot.X << ", \"y\": " << m_CanvasSlot.Y
			<< ", \"w\": " << m_CanvasSlot.W << ", \"h\": " << m_CanvasSlot.H << "},\n";
		out << "  \"targetRect\": {\"x\": " << m_Run.TargetRect.X << ", \"y\": " << m_Run.TargetRect.Y
			<< ", \"w\": " << m_Run.TargetRect.W << ", \"h\": " << m_Run.TargetRect.H << "},\n";
		out << "  \"updatedAt\": \"" << WallStamp() << "\"\n";
		out << "}\n";
		file << out.str();
	}


void WidgetGalleryPanel::WriteInteractionEvidence() const{
		if (m_Run.ComponentId.empty() || m_Run.Kind.empty())
			return;
		const std::filesystem::path directory =
			WorkbenchDir() / m_Run.ComponentId / "interaction";
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		std::ofstream file(directory / (m_Run.Kind + ".json"), std::ios::binary | std::ios::trunc);
		if (!file)
			return;
		file << InteractionEvidenceJson();
	}


std::string WidgetGalleryPanel::InteractionEvidenceJson() const{
		std::ostringstream out;
		out << "{\n";
		out << "  \"schema\": \"wui-workbench-interaction/1\",\n";
		out << "  \"component\": \"" << JsonEscape(m_Run.ComponentId) << "\",\n";
		out << "  \"kind\": \"" << JsonEscape(m_Run.Kind) << "\",\n";
		out << "  \"target\": \"" << JsonEscape(m_Run.Target) << "\",\n";
		out << "  \"targetId\": " << m_Run.TargetId << ",\n";
		out << "  \"expect\": \"" << JsonEscape(m_Run.Expect) << "\",\n";
		out << "  \"steps\": \"" << JsonEscape(m_Run.Steps) << "\",\n";
		out << "  \"note\": \"" << JsonEscape(m_Run.Note) << "\",\n";
		out << "  \"mode\": \"play\",\n";
		out << "  \"route\": \"WuiInputState(draw.RouteRealInput=true;与 WuiScriptedInput/ui.invoke "
			"同一输入结构,不做测试专用分支)\",\n";
		out << "  \"run\": \"" << JsonEscape(m_Run.Id) << "\",\n";
		out << "  \"window\": \"" << JsonEscape(m_Run.WindowKey) << "\",\n";
		out << "  \"frames\": [" << m_Run.StartFrame << ", " << m_Run.EndFrame << "],\n";
		out << "  \"wallMs\": " << (m_Run.FinishedWallMs >= m_Run.StartedWallMs
			? m_Run.FinishedWallMs - m_Run.StartedWallMs : 0) << ",\n";
		out << "  \"uiScale\": " << (Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f) << ",\n";
		out << "  \"canvas\": {\"x\": " << m_CanvasRect.X << ", \"y\": " << m_CanvasRect.Y
			<< ", \"w\": " << m_CanvasRect.W << ", \"h\": " << m_CanvasRect.H << "},\n";
		out << "  \"slot\": {\"x\": " << m_CanvasSlot.X << ", \"y\": " << m_CanvasSlot.Y
			<< ", \"w\": " << m_CanvasSlot.W << ", \"h\": " << m_CanvasSlot.H << "},\n";
		out << "  \"targetRect\": {\"x\": " << m_Run.TargetRect.X << ", \"y\": " << m_Run.TargetRect.Y
			<< ", \"w\": " << m_Run.TargetRect.W << ", \"h\": " << m_Run.TargetRect.H << "},\n";
		out << "  \"pixelHint\": \"" << InteractionPixelPhase(m_Run.Kind) << "\",\n";
		out << "  \"phases\": [";
		for (size_t index = 0; index < m_Run.Phases.size(); ++index)
		{
			const PlayPhaseRecord& phase = m_Run.Phases[index];
			out << (index == 0 ? "\n" : ",\n");
			out << "    {\"name\": \"" << JsonEscape(phase.Name) << "\", \"note\": \""
				<< JsonEscape(phase.Note) << "\", \"frames\": [" << phase.FirstFrame << ", "
				<< phase.LastFrame << "], \"sampledFrames\": " << phase.Frames
				<< ", \"budgetMs\": " << phase.WallMs << ", \"hover\": "
				<< (phase.Hover ? "true" : "false") << ", \"pressed\": "
				<< (phase.Pressed ? "true" : "false") << ", \"focusOnTarget\": "
				<< (phase.FocusOnTarget ? "true" : "false") << "}";
		}
		out << (m_Run.Phases.empty() ? "]," : "\n  ],") << "\n";
		out << "  \"timeline\": [";
		for (size_t index = 0; index < m_Run.Timeline.size(); ++index)
		{
			const PlayTimelineEntry& entry = m_Run.Timeline[index];
			out << (index == 0 ? "\n" : ",\n");
			out << "    {\"frame\": " << entry.Frame << ", \"phase\": \"" << JsonEscape(entry.Phase)
				<< "\", \"state\": \"" << JsonEscape(entry.State) << "\", \"mouse\": ["
				<< entry.MouseX << ", " << entry.MouseY << "], \"hover\": "
				<< (entry.Hover ? "true" : "false") << ", \"pressed\": "
				<< (entry.Pressed ? "true" : "false") << ", \"clicked\": "
				<< (entry.Clicked ? "true" : "false") << ", \"released\": "
				<< (entry.Released ? "true" : "false") << ", \"focus\": " << entry.Focus
				<< ", \"focusOnTarget\": " << (entry.FocusOnTarget ? "true" : "false") << "}";
		}
		out << (m_Run.Timeline.empty() ? "]," : "\n  ],") << "\n";
		out << "  \"events\": [";
		for (size_t index = 0; index < m_Run.Events.size(); ++index)
		{
			out << (index == 0 ? "\n" : ",\n");
			out << "    {\"event\": \"" << JsonEscape(m_Run.Events[index].first) << "\", \"detail\": \""
				<< JsonEscape(m_Run.Events[index].second) << "\"}";
		}
		out << (m_Run.Events.empty() ? "]," : "\n  ],") << "\n";
		out << "  \"activation\": {\n";
		out << "    \"witness\": \"widget-predicate+engine-click-completed\",\n";
		out << "    \"predicate\": \"hovered && ctx.Input().MouseClicked[0]\",\n";
		out << "    \"value\": " << (m_Run.PressFrame != 0 ? "true" : "false") << ",\n";
		out << "    \"frame\": " << m_Run.PressFrame << ",\n";
		out << "    \"clickCompleted\": " << (m_Run.ClickCompleted ? "true" : "false") << ",\n";
		out << "    \"clickCompletedWitness\": \"WuiContext::IsClickCompleted(0, targetRect)\",\n";
		out << "    \"releaseFrame\": " << m_Run.ReleaseFrame << ",\n";
		out << "    \"limitation\": \"showcase 丢弃 Button 的返回值(Engine 侧 ShowButton 未接);"
			"字面返回值需要 Engine 改动,超出本单边界 —— 这里记的是同帧判据求值 + 引擎的点击完成口径\"\n";
		out << "  },\n";
		out << "  \"keyboard\": {\"focusReached\": " << (m_Run.FocusReached ? "true" : "false")
			<< ", \"match\": \"" << JsonEscape(m_Run.FocusMatch) << "\", \"tabs\": " << m_Run.TabsQueued
			<< ", \"enter\": " << (m_Run.KeyEnterSeen ? "true" : "false") << ", \"space\": "
			<< (m_Run.KeySpaceSeen ? "true" : "false") << "},\n";
		out << "  \"observation\": {\"valueBefore\": \"" << JsonEscape(m_Run.ValueBefore)
			<< "\", \"valueAfter\": \"" << JsonEscape(m_Run.ValueAfter) << "\", \"hoverSeen\": "
			<< (std::any_of(m_Run.Phases.begin(), m_Run.Phases.end(),
				[](const PlayPhaseRecord& phase) { return phase.Hover; }) ? "true" : "false")
			<< ", \"pressSeen\": " << (m_Run.PressSeen ? "true" : "false")
			<< ", \"releaseSeen\": " << (m_Run.ReleaseSeen ? "true" : "false") << "},\n";
		out << "  \"a11y\": {\"before\": " << (m_Run.A11yBefore.empty() ? "[]" : m_Run.A11yBefore)
			<< ",\n          \"after\": " << (m_PlayA11yJson.empty() ? "[]" : m_PlayA11yJson) << "},\n";
		out << "  \"result\": \"" << JsonEscape(m_Run.Result) << "\",\n";
		out << "  \"gap\": \"" << JsonEscape(m_Run.GapReason) << "\",\n";
		out << "  \"pixels\": {\"source\": \"probe:capture.float\", \"filledBy\": \"probe\", "
			"\"phase\": \"" << InteractionPixelPhase(m_Run.Kind)
			<< "\", \"sha256\": \"\", \"captures\": {}}\n";
		out << "}\n";
		return out.str();
	}


void WidgetGalleryPanel::WriteInteractionSummary() const{
		if (m_Run.ComponentId.empty())
			return;
		const std::filesystem::path directory =
			WorkbenchDir() / m_Run.ComponentId / "interaction";
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		std::ofstream file(directory / "summary.json", std::ios::binary | std::ios::trunc);
		if (!file)
			return;
		std::ostringstream out;
		out << "{\n";
		out << "  \"schema\": \"wui-workbench-interaction-summary/1\",\n";
		out << "  \"run\": \"" << JsonEscape(m_Run.Id) << "\",\n";
		out << "  \"component\": \"" << JsonEscape(m_Run.ComponentId) << "\",\n";
		out << "  \"mode\": \"play\",\n";
		out << "  \"route\": \"WuiInputState(RouteRealInput=true)\",\n";
		out << "  \"startedAt\": \"" << WallStamp() << "\",\n";
		out << "  \"frames\": [" << m_Run.StartFrame << ", " << m_Run.EndFrame << "],\n";
		out << "  \"wallMs\": " << (m_Run.FinishedWallMs >= m_Run.StartedWallMs
			? m_Run.FinishedWallMs - m_Run.StartedWallMs : 0) << ",\n";
		out << "  \"interactions\": [";
		for (size_t index = 0; index < m_Run.WrittenKinds.size(); ++index)
			out << (index == 0 ? "\n" : ",\n") << "    {\"kind\": \""
				<< JsonEscape(m_Run.WrittenKinds[index]) << "\", \"file\": \""
				<< JsonEscape(m_Run.WrittenKinds[index]) << ".json\"}";
		out << (m_Run.WrittenKinds.empty() ? "]," : "\n  ],") << "\n";
		out << "  \"observation\": {\"state\": \"" << JsonEscape(m_PlayState) << "\", \"focus\": \""
			<< JsonEscape(m_PlayFocusName) << "\", \"press\": " << m_PlayPressCount
			<< ", \"click\": " << m_PlayClickCount << ", \"release\": " << m_PlayReleaseCount
			<< ", \"key\": " << m_PlayKeyCount << ", \"lastEvent\": \""
			<< JsonEscape(m_PlayLastEvent) << "\"},\n";
		out << "  \"events\": [";
		for (size_t index = 0; index < m_PlayEvents.size(); ++index)
			out << (index == 0 ? "\n" : ",\n") << "    {\"event\": \""
				<< JsonEscape(m_PlayEvents[index].first) << "\", \"detail\": \""
				<< JsonEscape(m_PlayEvents[index].second) << "\"}";
		out << (m_PlayEvents.empty() ? "]\n" : "\n  ]\n");
		out << "}\n";
		file << out.str();
	}


void WidgetGalleryPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host){
		Wui::WuiTheme& theme = host.Theme();
		const WbLayout layout = ComputeLayout(rect, theme);
		const float density = m_DensityIndex == 1 ? 0.85f : 1.0f;
		const float uiScale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		m_PanelRect = rect;
		m_PopupOpenNow = false;
		// 信息条画在画布之前:它显示上一帧的 Play 观测(同一帧的观测在动作条上)。
		m_PlayObservationPrev = m_PlayObservation;

		// "专用舞台"顺序(覆盖层组件):画布先画,其余内容画到更深的分层 —— 否则模态遮罩登记的
		// 全窗遮挡区会把工作台自己整块面板的命中打死(实测:选中 modal 之后连 Capture 都点不动,
		// 逐件遍历从 scrollarea 起全部卡死)。
		const std::vector<const Wui::WuiComponentDesc*> visible = FilteredComponents();
		const Wui::WuiComponentDesc* ahead = ResolveSelection(visible);
		const bool overlayStage = ahead != nullptr && m_OverlayStageComponents.count(ahead->Id) != 0;
		m_CanvasOverlayStage = overlayStage;

		if (overlayStage)
		{
			// 画布底(背景/网格/槽位)先画 —— 它在正常命令层,遮罩盖在它上面才是"画布里的模态"。
			DrawCanvas(ctx, layout, theme, ahead, density, uiScale, true);
			// 内容层比 showcase 深两层:showcase 里的模态自己在 depth+1 上登记遮挡区,
			// 内容画在 depth ≥ 该深度才不会被它挡住(见 DrawCanvas 的注释)。
			ctx.PushOverlay();
			ctx.PushOverlay();
			DrawTopBar(ctx, layout, theme, uiScale);
			const Wui::WuiComponentDesc* selected = nullptr;
			if (!layout.Stacked)
			{
				selected = DrawTree(ctx, layout, theme, uiScale);
				DrawInfoBar(ctx, layout, theme, selected, uiScale);
				DrawProperties(ctx, layout, theme, selected, density, uiScale);
			}
			else
			{
				// 窄窗:树/属性仍然走 body 滚动(画布已单独画在它的位置上)。
				const float contentHeight = layout.Props.Y + layout.Props.H + 8.0f - layout.Body.Y;
				Wui::BeginScrollArea(ctx, layout.Body, contentHeight, m_BodyScroll, theme);
				selected = DrawTree(ctx, layout, theme, uiScale);
				DrawProperties(ctx, layout, theme, selected, density, uiScale);
				Wui::EndScrollArea(ctx);
				DrawInfoBar(ctx, layout, theme, selected, uiScale);
			}
			DrawActions(ctx, layout, theme, selected, uiScale);
			ctx.PopOverlay();
			ctx.PopOverlay();
			// showcase 最后画(仍在 overlay 层、裁剪到画布矩形):模态打开会 ConsumePointerClick,
			// 放在内容之后才吞不到本帧落在树/属性/按钮上的点击;裁剪则保证整窗遮罩只盖画布。
			if (ahead != nullptr)
				DrawCanvasShowcase(ctx, theme, *ahead, density, uiScale, true, layout.Canvas);
		}
		else
		{
			DrawTopBar(ctx, layout, theme, uiScale);
			const Wui::WuiComponentDesc* selected = nullptr;

			if (!layout.Stacked)
			{
				selected = DrawTree(ctx, layout, theme, uiScale);
				DrawInfoBar(ctx, layout, theme, selected, uiScale);
				DrawCanvas(ctx, layout, theme, selected, density, uiScale, false);
				DrawProperties(ctx, layout, theme, selected, density, uiScale);
			}
			else
			{
				// 窄窗:三段纵向排布,整个 body 一起滚动(窗口够小时仍能看到全部区域)。
				const float contentHeight = layout.Props.Y + layout.Props.H + 8.0f - layout.Body.Y;
				Wui::BeginScrollArea(ctx, layout.Body, contentHeight, m_BodyScroll, theme);
				selected = DrawTree(ctx, layout, theme, uiScale);
				DrawCanvas(ctx, layout, theme, selected, density, uiScale, false);
				DrawProperties(ctx, layout, theme, selected, density, uiScale);
				Wui::EndScrollArea(ctx);
				DrawInfoBar(ctx, layout, theme, selected, uiScale);
			}

			DrawActions(ctx, layout, theme, selected, uiScale);
		}

		// 下一帧左树画在本帧之前,所以"弹层开着 → ↑/↓ 归弹层"只能用上一帧的结果。
		m_PopupOpenPrev = m_PopupOpenNow;
	}

}
