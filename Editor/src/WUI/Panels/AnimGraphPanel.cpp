#include "WUI/Panels/AnimGraphPanel.h"

#include "World/Core/KeyCodes.h"
#include "World/Core/StringPool.h"
#include "World/Scene/Components.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

#if defined(__has_include)
#	if __has_include("World/Animation/AnimGraphSystem.h")
#		include "World/Animation/AnimGraphSystem.h"
#		define WE_HAS_ANIMGRAPH_SYSTEM 1
#	endif
#endif
#ifndef WE_HAS_ANIMGRAPH_SYSTEM
#	define WE_HAS_ANIMGRAPH_SYSTEM 0
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string_view>
#include <cstdlib>
#include <iterator>
#include <system_error>
#include <utility>

namespace World
{
	namespace
	{
		// ---- 布局常量(4px 栅格;行高 24,与 WuiTheme.ControlHeight 一致) ----
		constexpr float kToolbarH = 40.0f;
		constexpr float kStatusH = 24.0f;
		constexpr float kRowH = 24.0f;
		constexpr float kStateCardH = 176.0f;
		constexpr float kBlendCardH = 356.0f;
		constexpr float kLayerCardH = 520.0f;
		// 姿态预览卡：放在中栏（可滚动），窄窗口/右栏折叠时也能看到。
		constexpr float kPoseCardH = 196.0f;
		constexpr float kStateGraphH = 180.0f;
		constexpr float kGap = 8.0f;
		constexpr float kSampleRowH = 28.0f;
		constexpr float kTransitionRowH = 56.0f;
		constexpr size_t kHistoryLimit = 64;

		std::string NormalizeSlashes(std::string text)
		{
			std::replace(text.begin(), text.end(), '\\', '/');
			return text;
		}

		bool StartsWith(const std::string& text, const char* prefix)
		{
			return text.rfind(prefix, 0) == 0;
		}

		std::string FormatFloat(float value, int decimals = 2)
		{
			char buffer[64] = {};
			std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, static_cast<double>(value));
			return buffer;
		}

		std::string AGTr(const char* key, const char* fallback)
		{
			return Wui::Tr(std::string("panel.animgraph.") + key, fallback);
		}

		std::string AGFmt(const char* key, const char* fallback,
			std::initializer_list<std::pair<std::string_view, std::string_view>> args = {})
		{
			return Wui::TrFormat(std::string("panel.animgraph.") + key, fallback,
				std::vector<std::pair<std::string_view, std::string_view>>(args));
		}

		std::string KindLabel(AnimStateKind kind)
		{
			switch (kind)
			{
				case AnimStateKind::BlendSpace1D: return AGTr("kind.blend1d", "Blend 1D");
				case AnimStateKind::BlendSpace2D: return AGTr("kind.blend2d", "Blend 2D");
				default: return AGTr("kind.single", "Single Clip");
			}
		}

		std::string KindBadge(AnimStateKind kind)
		{
			switch (kind)
			{
				case AnimStateKind::BlendSpace1D: return "1D";
				case AnimStateKind::BlendSpace2D: return "2D";
				default: return AGTr("badge.clip", "CLIP");
			}
		}

		Wui::WuiColor WithAlpha(Wui::WuiColor color, float alpha)
		{
			color.A = alpha;
			return color;
		}

		std::string UniqueStateName(const AnimGraph& graph, const std::string& base)
		{
			if (graph.FindState(base) == nullptr)
				return base;
			for (int i = 2; i < 1000; ++i)
			{
				const std::string candidate = base + " " + std::to_string(i);
				if (graph.FindState(candidate) == nullptr)
					return candidate;
			}
			return base + " Copy";
		}

		std::string UniqueLayerName(const AnimGraph& graph, const std::string& base)
		{
			const auto& layers = graph.GetLayers();
			auto taken = [&layers](const std::string& name)
			{
				return std::any_of(layers.begin(), layers.end(),
					[&name](const AnimLayer& layer) { return layer.Name == name; });
			};
			if (!taken(base))
				return base;
			for (int i = 2; i < 1000; ++i)
			{
				const std::string candidate = base + " " + std::to_string(i);
				if (!taken(candidate))
					return candidate;
			}
			return base + " Copy";
		}

		AnimGraph HistorySnapshot(const AnimGraph& graph, const std::string& initialStateHint)
		{
			AnimGraph snapshot = graph;
			if (!initialStateHint.empty() && snapshot.FindState(initialStateHint) != nullptr)
				snapshot.SetInitialState(initialStateHint);
			return snapshot;
		}

		std::string& PendingOpenPath()
		{
			static std::string pending;
			return pending;
		}

		std::string EntityTag(const entt::registry& registry, entt::entity entity)
		{
			if (const auto* tag = registry.try_get<TagComponent>(entity))
				return StringPool::Get().NameOf(tag->Tag);
			return std::string();
		}

		struct ModelClipInfo
		{
			const Asset::WModelAnimation* Clip = nullptr;
			float Duration = 0.0f;
			int Frames = 0;
		};

		std::string LowerAscii(std::string text)
		{
			for (char& ch : text)
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			return text;
		}

		bool ContainsTokenNoCase(const std::string& name, const char* token)
		{
			return LowerAscii(name).find(LowerAscii(token)) != std::string::npos;
		}

		ModelClipInfo FindModelClip(const Asset::WModelData& model, const std::string& clipName)
		{
			ModelClipInfo info;
			for (const Asset::WModelAnimation& animation : model.Animations)
			{
				if (animation.Name != clipName)
					continue;
				info.Clip = &animation;
				info.Duration = std::max(0.0f, animation.Duration);
				std::vector<float> times;
				for (const Asset::WModelAnimationChannel& channel : animation.Channels)
				{
					for (const Asset::WModelAnimationKey& key : channel.Keys)
					{
						info.Duration = std::max(info.Duration, key.Time);
						times.push_back(key.Time);
					}
				}
				std::sort(times.begin(), times.end());
				times.erase(std::unique(times.begin(), times.end(),
					[](float a, float b) { return std::fabs(a - b) < 0.0005f; }), times.end());
				if (!times.empty())
				{
					info.Frames = static_cast<int>(times.size());
				}
				else if (info.Duration > 0.0f)
				{
					const float rate = model.Meta.Settings.AnimationSampleRate > 0.0f
						? model.Meta.Settings.AnimationSampleRate : 30.0f;
					info.Frames = std::max(1, static_cast<int>(std::round(info.Duration * rate)));
				}
				break;
			}
			return info;
		}

		std::string SerializeBoneMask(const std::vector<BoneMaskEntry>& mask)
		{
			std::string text;
			for (size_t i = 0; i < mask.size(); ++i)
			{
				if (i > 0)
					text += ", ";
				text += mask[i].BoneName + " " + FormatFloat(mask[i].Weight, 2);
			}
			return text;
		}

		std::vector<BoneMaskEntry> ParseBoneMaskText(const std::string& text)
		{
			std::vector<BoneMaskEntry> mask;
			size_t start = 0;
			while (start < text.size())
			{
				size_t comma = text.find(',', start);
				if (comma == std::string::npos)
					comma = text.size();
				const std::string entry = text.substr(start, comma - start);
				const size_t firstNonSpace = entry.find_first_not_of(" \t");
				const size_t lastSpace = entry.find_last_of(' ');
				if (firstNonSpace != std::string::npos && lastSpace != std::string::npos
					&& lastSpace > firstNonSpace)
				{
					BoneMaskEntry bone;
					bone.BoneName = entry.substr(firstNonSpace, lastSpace - firstNonSpace);
					bone.Weight = std::clamp(
						static_cast<float>(std::atof(entry.substr(lastSpace + 1).c_str())), 0.0f, 1.0f);
					if (!bone.BoneName.empty())
						mask.push_back(bone);
				}
				start = comma + 1;
			}
			return mask;
		}

		// 把 "engine/foo" 解析到引擎内容库,其余解析到当前项目内容根(与内容浏览器同口径)。
		std::filesystem::path ResolveLogical(const std::string& logicalPath)
		{
			std::string logical = NormalizeSlashes(logicalPath);
			if (logical.empty())
				return {};

			const std::filesystem::path direct(logical);
			std::error_code ec;
			if (direct.is_absolute())
				return direct;

			if (StartsWith(logical, "engine/"))
			{
				const std::filesystem::path engine = Paths::EngineContentRoot() / logical.substr(7);
				if (std::filesystem::exists(engine, ec))
					return engine;
			}
			if (StartsWith(logical, "assets/"))
				logical = logical.substr(7);

			const std::filesystem::path fromAsset = Paths::AssetRoot() / logical;
			if (std::filesystem::exists(fromAsset, ec))
				return fromAsset;
			const std::filesystem::path fromProject = Paths::ProjectDir() / logical;
			if (std::filesystem::exists(fromProject, ec))
				return fromProject;
			return fromAsset;
		}
	}

	// ---------------------------------------------------------------------------
	// 文档生命周期
	// ---------------------------------------------------------------------------

	AnimGraphPanel::AnimGraphPanel()
	{
		MakeDefaultGraph();
		m_LoadedPath.clear();
		m_Dirty = false;
		SetStatus(AGTr("status.new_graph", "New animation graph (not saved yet)"), false);
	}

	void AnimGraphPanel::RequestOpenPath(const std::string& logicalPath)
	{
		PendingOpenPath() = NormalizeSlashes(logicalPath);
	}

	void AnimGraphPanel::MakeDefaultGraph()
	{
		m_UndoStack.clear();
		m_RedoStack.clear();
		m_UndoInitialState.clear();
		m_EditTransactionKey.clear();

		m_Graph.Clear();
		m_Graph.SetGraphName(AGTr("graph.default_name", "New Animation Graph"));
		m_Graph.SetModelHint("engine/models/characters/mannequin.wmodel");
		m_Graph.SetParameterDefault("Speed", 0.0f);
		m_Graph.SetParameterDefault("Direction", 0.0f);
		m_Graph.SetParameterDefault("IsFalling", 0.0f);
		m_Graph.SetParameterDefault("AimWeight", 0.0f);

		AnimState idle;
		idle.Name = "Idle";
		idle.Kind = AnimStateKind::SingleClip;
		idle.ClipName = "Idle";
		idle.Loop = true;
		{
			AnimTransition toLocomotion;
			toLocomotion.TargetState = "Locomotion";
			toLocomotion.Duration = 0.20f;
			toLocomotion.ConditionExpression = "Speed > 0.10";
			idle.Transitions.push_back(toLocomotion);
		}
		m_Graph.AddState(idle);

		AnimState locomotion;
		locomotion.Name = "Locomotion";
		locomotion.Kind = AnimStateKind::BlendSpace2D;
		locomotion.ParameterX = "Speed";
		locomotion.ParameterY = "Direction";
		locomotion.Blend2D.AddSample({ 0.0f, 0.0f }, "Idle");
		locomotion.Blend2D.AddSample({ 1.6f, 0.0f }, "Walk");
		locomotion.Blend2D.AddSample({ 1.6f, 90.0f }, "StrafeRight");
		locomotion.Blend2D.AddSample({ 1.6f, -90.0f }, "StrafeLeft");
		locomotion.Blend2D.AddSample({ 1.6f, 180.0f }, "WalkBack");
		locomotion.Blend2D.AddSample({ 4.5f, 0.0f }, "Run");
		locomotion.Blend2D.AddSample({ 4.5f, 90.0f }, "StrafeRight");
		locomotion.Blend2D.AddSample({ 4.5f, -90.0f }, "StrafeLeft");
		locomotion.Blend2D.AddSample({ 4.5f, 180.0f }, "WalkBack");
		{
			AnimTransition toIdle;
			toIdle.TargetState = "Idle";
			toIdle.Duration = 0.18f;
			toIdle.ConditionExpression = "Speed < 0.10 && IsFalling < 0.50";
			locomotion.Transitions.push_back(toIdle);
		}
		m_Graph.AddState(locomotion);
		m_Graph.SetInitialState("Idle");
		m_InitialStateHint = "Idle";

		m_SelectedState = "Idle";
		m_SelectedSample = -1;
		m_DragSample = -1;
		m_DragPending = -1;
		m_DragChanged = false;
		m_HoverSample = -1;
		m_SelectedLayer.clear();
		m_SelectedTransition = -1;
		m_StateScroll = 0.0f;
		m_LayerScroll = 0.0f;
		m_CenterScroll = 0.0f;
		m_TrianglesDirty = true;
		m_ParamsInitialized = false;
		m_SimGraphReady = false;
		m_SimCycleTime = 0.0f;
		m_StateGraphPositions.clear();
		m_StateGraphDragPending.clear();
		m_StateGraphDragNode.clear();
		m_NewParameterRowVisible = false;
		m_NewParameterBuffer.clear();
		m_LayerMaskAdvancedOpen = false;
		m_LayerMaskScroll = 0.0f;
		m_LayerMaskTextKey.clear();
		m_LayerMaskTextBuffer.clear();
	}

	std::filesystem::path AnimGraphPanel::ResolvePhysicalPath(const std::string& logicalPath) const
	{
		std::string logical = NormalizeSlashes(logicalPath);
		if (logical.empty())
			logical = "animation/new_graph.wanimgraph";
		if (StartsWith(logical, "assets/"))
			logical = logical.substr(7);
		return ResolveLogical(logical);
	}

	std::string AnimGraphPanel::ToLogicalPath(const std::filesystem::path& physical) const
	{
		std::error_code ec;
		const std::filesystem::path fromAsset = std::filesystem::relative(physical, Paths::AssetRoot(), ec);
		if (!ec && !fromAsset.empty() && fromAsset.generic_string().rfind("..", 0) != 0)
			return fromAsset.generic_string();
		const std::filesystem::path fromEngine = std::filesystem::relative(physical, Paths::EngineContentRoot(), ec);
		if (!ec && !fromEngine.empty() && fromEngine.generic_string().rfind("..", 0) != 0)
			return "engine/" + fromEngine.generic_string();
		return NormalizeSlashes(physical.generic_string());
	}

	void AnimGraphPanel::ConsumeOpenRequest()
	{
		std::string pending = PendingOpenPath();
		if (pending.empty())
			return;
		PendingOpenPath().clear();
		LoadAsset(pending);
	}

	void AnimGraphPanel::LoadAsset(const std::string& logicalPath)
	{
		const std::filesystem::path physical = ResolvePhysicalPath(logicalPath);
		if (physical.empty())
		{
			SetStatus(AGFmt("status.cannot_resolve", "Cannot resolve asset path: {path}",
				{ { "path", logicalPath } }), true);
			return;
		}

		AnimGraph loaded;
		std::string error;
		if (!loaded.LoadFromFile(physical.string(), &error))
		{
			// 失败保留上次数据(派工 §3.3),只把错误说清楚。
			SetStatus(AGFmt("status.load_failed", "Failed to load {file}: {error}",
				{ { "file", physical.filename().generic_string() },
				  { "error", error.empty() ? AGTr("status.unknown_error", "unknown error") : std::string(error) } }), true);
			return;
		}

		m_Graph = std::move(loaded);
		// LoadFromFile 末尾 SetInitialState(initial:) ⇒ 此刻的 CurrentState 就是资产初始状态。
		m_InitialStateHint = m_Graph.GetCurrentStateName();
		m_UndoStack.clear();
		m_RedoStack.clear();
		m_UndoInitialState.clear();
		m_EditTransactionKey.clear();
		m_LoadedPath = ToLogicalPath(physical);
		m_Dirty = false;
		m_SelectedState.clear();
		if (!m_Graph.GetStates().empty())
			m_SelectedState = m_Graph.GetStates().begin()->first;
		m_SelectedSample = -1;
		m_DragSample = -1;
		m_DragPending = -1;
		m_DragChanged = false;
		m_HoverSample = -1;
		m_SelectedLayer.clear();
		m_SelectedTransition = -1;
		m_StateScroll = 0.0f;
		m_LayerScroll = 0.0f;
		m_CenterScroll = 0.0f;
		m_TrianglesDirty = true;
		m_ParamsInitialized = false;
		m_NameBufferKey.clear();
		m_ConditionKey.clear();
		m_SimGraphReady = false;
		m_SimCycleTime = 0.0f;
		m_StateGraphPositions.clear();
		m_StateGraphDragPending.clear();
		m_StateGraphDragNode.clear();
		m_NewParameterRowVisible = false;
		m_NewParameterBuffer.clear();
		m_LayerMaskAdvancedOpen = false;
		m_LayerMaskScroll = 0.0f;
		m_LayerMaskTextKey.clear();
		m_LayerMaskTextBuffer.clear();
		SetStatus(AGTr("status.loaded", "Loaded ") + m_LoadedPath, false);
	}

	bool AnimGraphPanel::SaveAsset()
	{
		if (m_LoadedPath.empty())
			m_LoadedPath = "animation/new_graph.wanimgraph";

		const std::filesystem::path physical = ResolvePhysicalPath(m_LoadedPath);
		std::error_code ec;
		if (!physical.parent_path().empty())
			std::filesystem::create_directories(physical.parent_path(), ec);

		std::string error;
		if (!m_Graph.SaveToFile(physical.string(), &error))
		{
			SetStatus(AGFmt("status.save_failed", "Save failed: {error}",
				{ { "error", error.empty() ? physical.string() : error } }), true);
			return false;
		}

		m_LoadedPath = ToLogicalPath(physical);
		m_Dirty = false;
		SetStatus(AGTr("status.saved", "Saved ") + m_LoadedPath, false);
		return true;
	}

	bool AnimGraphPanel::ReloadAsset()
	{
		if (m_LoadedPath.empty())
		{
			SetStatus(AGTr("status.nothing_to_reload", "Nothing to reload: this graph has not been saved yet"), true);
			return false;
		}
		const std::string path = m_LoadedPath;
		LoadAsset(path);
		return m_StatusError == false;
	}

	bool AnimGraphPanel::ApplyToSelected(PanelHost& host)
	{
		Entity entity = host.GetSelectedEntity();
		const Ref<Scene> sceneRef = host.GetActiveScene();
		Scene* scene = sceneRef.get();
		if (scene == nullptr || !entity.IsValid() || entity.GetScene() != scene)
		{
			SetStatus(AGTr("status.select_entity_first", "Select a character entity in the scene first"), true);
			return false;
		}
		if (m_LoadedPath.empty())
		{
			SetStatus(AGTr("status.save_before_apply", "Save the graph before applying it to an entity"), true);
			return false;
		}

		const entt::entity handle = entity;
		const entt::registry& readRegistry = static_cast<const Scene&>(*scene).GetRegistry();
		const bool hasComponent = readRegistry.try_get<AnimGraphComponent>(handle) != nullptr;
		AnimGraphComponent* component = nullptr;
		if (!hasComponent)
		{
			std::string reason;
			if (!entity.CanAddComponent(entt::type_id<AnimGraphComponent>().hash(), &reason))
			{
				SetStatus(AGTr("status.cannot_add_component", "Cannot add AnimGraphComponent: ")
					+ (reason.empty() ? AGTr("status.scene_running", "scene is running") : reason), true);
				return false;
			}

			// Active scenes reject structural writes even when CanAddComponent() only checked
			// static dependencies. Reject before touching the mutable registry: GetRegistry()
			// itself throws on the read-only editor path during Play/Simulate.
			if (scene->IsActive())
			{
				SetStatus(AGTr("status.runtime_add_blocked", "运行时不能给实体添加组件，请先停止播放"), true);
				return false;
			}
			component = &entity.AddOrReplaceComponent<AnimGraphComponent>();
		}
		else
		{
			// Existing component: mutate the component itself, never take a mutable registry.
			component = static_cast<AnimGraphComponent*>(
				entity.GetComponent(entt::type_id<AnimGraphComponent>().hash()));
			if (component == nullptr)
			{
				SetStatus(AGTr("status.cannot_add_component", "Cannot add AnimGraphComponent: ")
					+ AGTr("status.scene_running", "scene is running"), true);
				return false;
			}
		}

		component->AnimGraphAsset = AssetRef { StringPool::Get().InternPath(m_LoadedPath), AssetId {} };
#if WE_HAS_ANIMGRAPH_SYSTEM
		AnimGraphSystem::ReloadAsset(handle);
#endif
		host.MarkDocumentDirty();

		const bool hasSkin = readRegistry.try_get<SkinnedMeshRendererComponent>(handle) != nullptr;
		SetStatus(hasSkin
			? AGTr("status.applied", "Applied ") + m_LoadedPath + AGTr("status.applied_suffix", " to the selected entity")
			: AGTr("status.applied_no_skin", "Applied to the selected entity (no SkinnedMeshRendererComponent yet: nothing will draw)"),
			!hasSkin);
		return true;
	}

	void AnimGraphPanel::MarkDirty()
	{
		m_Dirty = true;
		m_TrianglesDirty = true;
	}

	void AnimGraphPanel::PushUndo()
	{
		m_UndoStack.push_back(HistorySnapshot(m_Graph, m_InitialStateHint));
		if (m_UndoStack.size() > kHistoryLimit)
			m_UndoStack.erase(m_UndoStack.begin());
		m_UndoInitialState = m_InitialStateHint;
		m_RedoStack.clear();
	}

	bool AnimGraphPanel::BeginContinuousEdit(const std::string& key, bool continuous)
	{
		if (continuous && m_EditTransactionKey == key)
			return false;
		PushUndo();
		if (continuous)
			m_EditTransactionKey = key;
		return true;
	}

	void AnimGraphPanel::RestoreHistorySnapshot(AnimGraph snapshot, const std::string& initialStateHint)
	{
		const std::string previousState = m_SelectedState;
		const int previousSample = m_SelectedSample;
		const std::string previousLayer = m_SelectedLayer;
		const int previousTransition = m_SelectedTransition;

		m_Graph = std::move(snapshot);
		m_InitialStateHint = initialStateHint;
		if (!m_InitialStateHint.empty() && m_Graph.FindState(m_InitialStateHint) != nullptr)
			m_Graph.SetInitialState(m_InitialStateHint);

		const auto& states = m_Graph.GetStates();
		m_SelectedState = (!previousState.empty() && m_Graph.FindState(previousState) != nullptr)
			? previousState
			: (states.empty() ? std::string() : states.begin()->first);

		const AnimState* state = SelectedState();
		size_t sampleCount = 0;
		if (state != nullptr)
		{
			sampleCount = state->Kind == AnimStateKind::BlendSpace1D ? state->Blend1D.GetSampleCount()
				: (state->Kind == AnimStateKind::BlendSpace2D ? state->Blend2D.GetSampleCount() : 0u);
		}
		m_SelectedSample = (previousSample >= 0 && static_cast<size_t>(previousSample) < sampleCount)
			? previousSample : -1;
		m_SelectedTransition = (state != nullptr && previousTransition >= 0
			&& static_cast<size_t>(previousTransition) < state->Transitions.size())
			? previousTransition : -1;

		const auto& layers = m_Graph.GetLayers();
		m_SelectedLayer = std::any_of(layers.begin(), layers.end(),
			[&previousLayer](const AnimLayer& layer) { return layer.Name == previousLayer; })
			? previousLayer : std::string();

		m_HoverSample = -1;
		m_DragSample = -1;
		m_DragPending = -1;
		m_DragChanged = false;
		m_DeleteArmState.clear();
		m_DeleteArmAge = 0.0f;
		m_NameBufferKey.clear();
		m_ConditionKey.clear();
		m_NameFieldWasFocused = false;
		m_ConditionFocusKey.clear();
		m_EditTransactionKey.clear();
		MarkDirty();
	}

	bool AnimGraphPanel::Undo()
	{
		if (m_UndoStack.empty())
		{
			SetStatus(AGTr("status.nothing_to_undo", "Nothing to undo"), false);
			return false;
		}

		m_RedoStack.push_back(HistorySnapshot(m_Graph, m_InitialStateHint));
		if (m_RedoStack.size() > kHistoryLimit)
			m_RedoStack.erase(m_RedoStack.begin());

		const std::string restoreHint = m_UndoInitialState;
		AnimGraph restored = std::move(m_UndoStack.back());
		m_UndoStack.pop_back();
		m_UndoInitialState = m_UndoStack.empty() ? std::string() : m_UndoStack.back().GetCurrentStateName();
		RestoreHistorySnapshot(std::move(restored), restoreHint);
		SetStatus(AGTr("status.undone", "Undone"), false);
		return true;
	}

	bool AnimGraphPanel::Redo()
	{
		if (m_RedoStack.empty())
		{
			SetStatus(AGTr("status.nothing_to_redo", "Nothing to redo"), false);
			return false;
		}

		m_UndoStack.push_back(HistorySnapshot(m_Graph, m_InitialStateHint));
		if (m_UndoStack.size() > kHistoryLimit)
			m_UndoStack.erase(m_UndoStack.begin());
		m_UndoInitialState = m_InitialStateHint;

		AnimGraph restored = std::move(m_RedoStack.back());
		m_RedoStack.pop_back();
		const std::string restoreHint = restored.GetCurrentStateName();
		RestoreHistorySnapshot(std::move(restored), restoreHint);
		SetStatus(AGTr("status.redone", "Redone"), false);
		return true;
	}

	void AnimGraphPanel::SetStatus(const std::string& text, bool error)
	{
		m_Status = text;
		m_StatusError = error;
		m_StatusAge = 3.0f; // 3 秒淡出(派工 §3.2)
	}

	// ---------------------------------------------------------------------------
	// 状态编辑
	// ---------------------------------------------------------------------------

	const AnimState* AnimGraphPanel::SelectedState() const
	{
		if (m_SelectedState.empty())
			return nullptr;
		return m_Graph.FindState(m_SelectedState);
	}

	bool AnimGraphPanel::ReplaceState(const std::string& originalName, const AnimState& state, bool recordUndo)
	{
		if (state.Name.empty())
		{
			SetStatus(AGTr("status.state_name_required", "A state needs a non-empty name"), true);
			return false;
		}
		if (m_Graph.FindState(originalName) == nullptr)
			return false;
		if (originalName != state.Name)
		{
			if (m_Graph.FindState(state.Name) != nullptr)
			{
				SetStatus(AGFmt("status.state_exists", "A state named '{name}' already exists",
				{ { "name", state.Name } }), true);
				return false;
			}
		}

		if (recordUndo)
			PushUndo();

		if (originalName != state.Name)
		{
			if (!m_Graph.RemoveState(originalName))
				return false;

			// 改名:把其它状态里指向旧名字的过渡同步改到新名字(避免运行时指向不存在的状态)。
			std::vector<AnimState> rewritten;
			for (const auto& entry : m_Graph.GetStates())
				rewritten.push_back(entry.second);
			for (AnimState& other : rewritten)
			{
				bool changed = false;
				for (AnimTransition& transition : other.Transitions)
				{
					if (transition.TargetState == originalName)
					{
						transition.TargetState = state.Name;
						changed = true;
					}
				}
				if (changed)
					m_Graph.AddState(other);
			}
		}
		else
		{
			m_Graph.RemoveState(originalName);
		}

		m_Graph.AddState(state);
		if (m_InitialStateHint == originalName)
		{
			m_InitialStateHint = state.Name;
			m_Graph.SetInitialState(state.Name);
		}
		m_SelectedState = state.Name;
		MarkDirty();
		return true;
	}

	void AnimGraphPanel::AddStateOfKind(AnimStateKind kind)
	{
		AnimState state;
		state.Kind = kind;
		state.Name = UniqueStateName(m_Graph, kind == AnimStateKind::SingleClip
			? AGTr("state.new", "New State") : AGTr("state.new_blend", "New Blend"));
		state.ClipName = "Idle";
		state.Loop = true;
		state.Speed = 1.0f;
		state.ParameterX = "Speed";
		state.ParameterY = "Direction";

		if (kind == AnimStateKind::BlendSpace1D)
		{
			state.Blend1D.AddSample(0.0f, "Idle");
			state.Blend1D.AddSample(1.6f, "Walk");
			state.Blend1D.AddSample(4.5f, "Run");
		}
		else if (kind == AnimStateKind::BlendSpace2D)
		{
			state.Blend2D.AddSample({ 0.0f, 0.0f }, "Idle");
			state.Blend2D.AddSample({ 1.6f, 0.0f }, "Walk");
			state.Blend2D.AddSample({ 4.5f, 0.0f }, "Run");
			state.Blend2D.AddSample({ 1.6f, 90.0f }, "StrafeRight");
			state.Blend2D.AddSample({ 1.6f, -90.0f }, "StrafeLeft");
		}

		m_Graph.AddState(state);
		if (m_InitialStateHint.empty())
			m_InitialStateHint = state.Name;
		m_SelectedState = state.Name;
		m_SelectedSample = -1;
		m_SelectedTransition = -1;
		m_TrianglesDirty = true;
		MarkDirty();
		SetStatus(AGFmt("status.state_added", "Added {kind} state '{name}'",
				{ { "kind", KindLabel(kind) }, { "name", state.Name } }), false);
	}

	void AnimGraphPanel::DeleteSelectedState()
	{
		if (m_SelectedState.empty())
			return;
		const std::string removed = m_SelectedState;
		if (m_Graph.FindState(removed) == nullptr)
			return;
		PushUndo();
		if (!m_Graph.RemoveState(removed))
			return;

		// 清理指向被删状态的过渡 + 初始状态兜底。
		std::vector<AnimState> rewritten;
		for (const auto& entry : m_Graph.GetStates())
			rewritten.push_back(entry.second);
		for (AnimState& other : rewritten)
		{
			const size_t before = other.Transitions.size();
			other.Transitions.erase(
				std::remove_if(other.Transitions.begin(), other.Transitions.end(),
					[&removed](const AnimTransition& t) { return t.TargetState == removed; }),
				other.Transitions.end());
			if (other.Transitions.size() != before)
				m_Graph.AddState(other);
		}

		m_SelectedState = m_Graph.GetStates().empty() ? std::string() : m_Graph.GetStates().begin()->first;
		// 删掉的若正好是初始状态(面板跟踪的 hint),把 initial 落到第一个剩余状态;删空则清空。
		if (m_InitialStateHint == removed)
		{
			m_InitialStateHint = m_SelectedState;
			m_Graph.SetInitialState(m_InitialStateHint);
		}

		m_SelectedSample = -1;
		m_SelectedTransition = -1;
		m_DeleteArmState.clear();
		m_TrianglesDirty = true;
		MarkDirty();
		SetStatus(AGFmt("status.state_deleted", "Deleted state '{name}'",
				{ { "name", removed } }), false);
	}

	// ---------------------------------------------------------------------------
	// 模型 / 实体绑定 / 参数
	// ---------------------------------------------------------------------------

	std::string AnimGraphPanel::ModelLogicalPathForSelected(PanelHost& host) const
	{
		Entity entity = host.GetSelectedEntity();
		const Ref<Scene> sceneRef = host.GetActiveScene();
		Scene* scene = sceneRef.get();
		if (scene == nullptr || !entity.IsValid() || entity.GetScene() != scene)
			return {};
		const auto& registry = static_cast<const Scene&>(*scene).GetRegistry();
		if (const auto* skinned = registry.try_get<SkinnedMeshRendererComponent>(entity))
		{
			if (skinned->Mesh.HasPath())
				return StringPool::Get().PathOf(skinned->Mesh.Path);
		}
		return {};
	}

	void AnimGraphPanel::RefreshModel(PanelHost& host)
	{
		const std::string path = ModelLogicalPathForSelected(host);
		if (path == m_ModelKey)
			return;
		m_ModelKey = path;
		m_ModelValid = false;
		m_ModelError.clear();
		m_Model = Asset::WModelData {};

		if (path.empty())
			return;

		const std::filesystem::path physical = ResolveLogical(path);
		std::string error;
		if (!Asset::WModelIO::ReadFile(physical.string(), m_Model, &error))
		{
			m_ModelError = error.empty() ? (AGTr("state.cannot_read", "cannot read ") + physical.generic_string()) : error;
			return;
		}
		m_ModelValid = true;
	}

	std::vector<std::string> AnimGraphPanel::ClipOptionsForCombo(const std::string& current) const
	{
		std::vector<std::string> options;
		if (m_ModelValid)
		{
			options.reserve(m_Model.Animations.size() + 1);
			for (const Asset::WModelAnimation& animation : m_Model.Animations)
				options.push_back(animation.Name);
		}
		if (!current.empty() && std::find(options.begin(), options.end(), current) == options.end())
			options.insert(options.begin(), current);
		if (options.empty())
			options.push_back("(no clips)");
		return options;
	}

	void AnimGraphPanel::RefreshLiveBinding(PanelHost& host)
	{
		Entity entity = host.GetSelectedEntity();
		const Ref<Scene> sceneRef = host.GetActiveScene();
		Scene* scene = sceneRef.get();
		if (scene == nullptr || !entity.IsValid() || entity.GetScene() != scene)
		{
			m_BoundEntity = entt::null;
			m_BoundScene = nullptr;
			m_BoundName.clear();
		}
		else
		{
			m_BoundEntity = entity;
			m_BoundScene = scene;
			m_BoundName = EntityTag(static_cast<const Scene&>(*scene).GetRegistry(), entity);
			if (m_BoundName.empty())
				m_BoundName = AGTr("live.entity", "Entity");
			// 名称 + 实体 ID:同名实体在层级里很常见,只给名字无法确认绑到了哪一个。
			m_BoundName += "  #" + std::to_string(static_cast<uint32_t>(entity));
		}
		RefreshModel(host);

		if (!m_ParamsInitialized)
		{
			const auto& defaults = m_Graph.GetParameterDefaults();
			const bool hasParameters = !defaults.empty();
			auto readDefault = [&defaults](const char* name)
			{
				const auto found = defaults.find(name);
				return found == defaults.end() ? 0.0f : found->second;
			};
			m_ParamSpeed = readDefault("Speed");
			m_ParamDirection = readDefault("Direction");
			m_ParamFalling = readDefault("IsFalling");
			m_ParamAimWeight = readDefault("AimWeight");
			m_ParamsInitialized = true;
		}
	}

	void AnimGraphPanel::PushParameters(PanelHost& host)
	{
		(void)host;
#if WE_HAS_ANIMGRAPH_SYSTEM
		if (m_BoundEntity == entt::null || m_BoundScene == nullptr)
			return;
		const auto& registry = static_cast<const Scene&>(*m_BoundScene).GetRegistry();
		if (registry.try_get<AnimGraphComponent>(m_BoundEntity) == nullptr)
			return;
		AnimGraphSystem::SetParameter(m_BoundEntity, "Speed", m_ParamSpeed);
		AnimGraphSystem::SetParameter(m_BoundEntity, "Direction", m_ParamDirection);
		AnimGraphSystem::SetParameter(m_BoundEntity, "IsFalling", m_ParamFalling);
		AnimGraphSystem::SetParameter(m_BoundEntity, "AimWeight", m_ParamAimWeight);
#endif
	}

	void AnimGraphPanel::ResetSimulation()
	{
		m_SimGraphReady = false;
		m_SimCycleTime = 0.0f;
	}

	void AnimGraphPanel::EnsureSimulationGraph()
	{
		if (m_SimGraphReady)
			return;
		m_SimGraph = m_Graph;
		if (!m_InitialStateHint.empty() && m_SimGraph.FindState(m_InitialStateHint) != nullptr)
			m_SimGraph.SetInitialState(m_InitialStateHint);
		m_SimGraphReady = true;
		m_SimCycleTime = 0.0f;
	}

	void AnimGraphPanel::UpdateSimulation(float dt)
	{
		if (!m_Simulate)
			return;
		EnsureSimulationGraph();

		constexpr float stageSeconds = 2.5f;
		constexpr int stageCount = 7;
		m_SimCycleTime = std::fmod(m_SimCycleTime + std::max(0.0f, dt), stageSeconds * stageCount);
		const int stage = static_cast<int>(m_SimCycleTime / stageSeconds) % stageCount;
		const float stageTime = std::fmod(m_SimCycleTime, stageSeconds);

		float speed = 0.0f;
		float direction = 0.0f;
		float falling = 0.0f;
		float verticalSpeed = 0.0f;
		switch (stage)
		{
			case 1: speed = 1.6f; break;
			case 2: speed = 4.5f; break;
			case 3: speed = 1.6f; direction = 90.0f; break;
			case 4: speed = 1.6f; direction = -90.0f; break;
			case 5: speed = 1.6f; direction = 180.0f; break;
			case 6:
				falling = 1.0f;
				verticalSpeed = stageTime < stageSeconds * 0.5f ? 4.5f : -7.0f;
				break;
			default: break;
		}

		m_SimGraph.SetFloat("Speed", speed);
		m_SimGraph.SetFloat("Direction", direction);
		m_SimGraph.SetFloat("IsFalling", falling);
		m_SimGraph.SetFloat("VerticalSpeed", verticalSpeed);
		m_SimGraph.SetFloat("AimWeight", std::clamp(m_ParamAimWeight, 0.0f, 1.0f));
		m_SimGraph.Update(dt);

		// 模拟模式仍保持既有 Live Preview 语义：绑定实体时把循环参数继续推给运行时。
		m_ParamSpeed = speed;
		m_ParamDirection = direction;
		m_ParamFalling = falling;
		m_ParamsInitialized = true;
	}

	// ---------------------------------------------------------------------------
	// 面板主入口
	// ---------------------------------------------------------------------------

	bool AnimGraphPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		if (!ctrl || alt)
			return false;
		if (keyCode == KeyCodes::S && !shift)
		{
			SaveAsset();
			return true;
		}
		if (keyCode == KeyCodes::R && !shift)
		{
			ReloadAsset();
			return true;
		}
		// 文本框持有焦点时 Ctrl+Z 属于文本自身的撤销，不能把整张图回退。
		if (Wui::WuiTextFocus::Get().Active())
			return false;
		if (keyCode == KeyCodes::Z)
		{
			if (shift)
				Redo();
			else
				Undo();
			return true;
		}
		if (keyCode == KeyCodes::Y && !shift)
		{
			Redo();
			return true;
		}
		return false;
	}
	void AnimGraphPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& inRect, PanelHost& host)
	{
		// 独立窗口(FloatWindowHost)传入的是物理像素客户区,而 WUI 布局单位是
		// "物理像素 / UiScale"。不换算的话,150% 缩放下三栏合计超出窗口:
		// 右栏(Live Preview)被挤出可视区、混合空间画布右段被裁掉。
		Wui::WuiRect rect = inRect;
		if (ctx.WindowKey().rfind("float:", 0) == 0)
		{
			const float scale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
			rect = { inRect.X / scale, inRect.Y / scale, inRect.W / scale, inRect.H / scale };
		}
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg);

		const float dt = ctx.Input().FPS > 1.0f ? 1.0f / ctx.Input().FPS : 1.0f / 60.0f;
		if (m_StatusAge > 0.0f)
		{
			m_StatusAge = std::max(0.0f, m_StatusAge - dt);
			if (m_StatusAge == 0.0f)
				m_Status.clear();
		}
		if (!m_DeleteArmState.empty())
		{
			m_DeleteArmAge = std::max(0.0f, m_DeleteArmAge - dt);
			if (m_DeleteArmAge == 0.0f)
				m_DeleteArmState.clear();
		}

		// 文本字段"点别处即提交":必须发生在工具栏处理本帧点击之前,
		// 否则"改了名字/条件 → 立刻点 Save"会保存旧值。
		if (ctx.Input().MouseClicked[0])
		{
			if (!Wui::HitTest(m_NameFieldRect, ctx.Input().MousePos))
			{
				if (m_NameFieldWasFocused)
				{
					m_NameFieldWasFocused = false;
					RenameSelectedState(m_NameBuffer);
				}
			}
			if (!Wui::HitTest(m_ConditionFieldRect, ctx.Input().MousePos))
				CommitPendingCondition();
		}
		ConsumeOpenRequest();
		RefreshLiveBinding(host);
		UpdateSimulation(dt);
		PushParameters(host);

		const float outer = kGap;
		const Wui::WuiRect toolbar { rect.X + outer, rect.Y + outer, rect.W - outer * 2.0f, kToolbarH };
		RenderToolbar(ctx, toolbar, host, theme);

		float bodyTop = toolbar.Y + toolbar.H + kGap * 0.5f;
		if (m_OpenRowVisible)
		{
			const Wui::WuiRect openRow { toolbar.X, bodyTop, toolbar.W, kRowH + kGap };
			RenderOpenRow(ctx, openRow, host, theme);
			bodyTop += openRow.H + kGap * 0.5f;
		}

		const float bodyBottom = rect.Y + rect.H - outer - kStatusH - kGap * 0.5f;
		const float bodyH = std::max(0.0f, bodyBottom - bodyTop);
		const float bodyW = rect.W - outer * 2.0f;

		m_Narrow = bodyW < 900.0f;
		const float leftW = m_Narrow ? 190.0f : 232.0f;
		const float rightW = m_Narrow ? 0.0f : 300.0f;

		const Wui::WuiRect leftArea { rect.X + outer, bodyTop, leftW, bodyH };
		const float centerX = leftArea.X + leftArea.W + kGap;
		const float centerW = std::max(120.0f, bodyW - leftW - rightW - (m_Narrow ? kGap : kGap * 2.0f));
		const Wui::WuiRect centerArea { centerX, bodyTop, centerW, bodyH };

		RenderLeftColumn(ctx, leftArea, host, theme);

		if (m_Narrow)
		{
			// 右栏折叠为底部条(派工 §3.4):中栏在上、实时区在下,互不重叠。
			const float liveH = std::min(200.0f, std::max(140.0f, bodyH * 0.34f));
			const Wui::WuiRect centerTop { centerArea.X, centerArea.Y, centerArea.W, std::max(0.0f, bodyH - liveH - kGap) };
			RenderCenterColumn(ctx, centerTop, host, theme);
			const Wui::WuiRect liveArea { centerArea.X, centerTop.Y + centerTop.H + kGap, centerArea.W, liveH };
			RenderLivePanel(ctx, liveArea, host, theme);
		}
		else
		{
			RenderCenterColumn(ctx, centerArea, host, theme);
			const Wui::WuiRect liveArea { centerArea.X + centerArea.W + kGap, bodyTop, rightW, bodyH };
			RenderLivePanel(ctx, liveArea, host, theme);
		}

		// 滑杆可能在本帧刚被拖动:再推一次,让运行时的参数覆盖从"用户看到的那一帧"就生效。
		PushParameters(host);

		const Wui::WuiRect statusBar { rect.X + outer, rect.Y + rect.H - outer - kStatusH, bodyW, kStatusH };
		RenderStatusBar(ctx, statusBar, theme);

		// 新建状态下拉菜单:最后画(覆盖列表),并在关闭时同步 popup 生命周期。
		const Wui::WuiId menuId = Wui::HashId("animgraph.newstate.menu");
		if (m_NewStateMenuOpen)
		{
			const Wui::WuiRect panel { m_NewStateMenuPos.x, m_NewStateMenuPos.y, 176.0f, kRowH * 3.0f + kGap };
			Wui::DrawPanelSurface(ctx, panel, theme);
			bool picked = false;
			const std::string labels[3] = { AGTr("kind.single", "Single Clip"), AGTr("kind.blend1d", "Blend 1D"),
				AGTr("kind.blend2d", "Blend 2D") };
			for (int i = 0; i < 3; ++i)
			{
				const Wui::WuiRect row { panel.X + 2.0f, panel.Y + 2.0f + kRowH * static_cast<float>(i), panel.W - 4.0f, kRowH };
				if (Wui::MenuItem(ctx, Wui::HashId(("animgraph.newstate.item." + std::to_string(i)).c_str()),
					row, labels[i], true, theme))
				{
					AddStateOfKind(static_cast<AnimStateKind>(i));
					picked = true;
				}
			}
			if (picked || ctx.ClosePopupsOnOutsideClick({ menuId }, panel))
			{
				ctx.ClosePopup(menuId);
				m_NewStateMenuOpen = false;
			}
		}

		// 一次按住拖动 = 一次撤销记录：松开鼠标后结束连续编辑事务。
		if (!ctx.Input().MouseDown[0])
			m_EditTransactionKey.clear();
	}

	// ---------------------------------------------------------------------------
	// 工具栏 / 路径行
	// ---------------------------------------------------------------------------

	void AnimGraphPanel::RenderToolbar(Wui::WuiContext& ctx, const Wui::WuiRect& bar, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		Wui::BarSurface(ctx, bar, theme.PanelHeader, theme.Border);

		struct ToolButton
		{
			Wui::WuiId Id;
			std::string Label;
			float Width;
			bool Enabled;
			std::string Tooltip;
		};
		// 窄面板:按钮文字与宽度一起收短,保证工具栏在任何宽度下都不越出面板
		// (极端窄时路径标签让位,只留按钮 + tooltip)。
		const bool compactBar = bar.W < 760.0f;
		const ToolButton buttons[] = {
			{ Wui::HashId("animgraph.toolbar.open"), AGTr("toolbar.open", "Open"), compactBar ? 52.0f : 58.0f, true,
				AGTr("toolbar.open.tooltip", "Open a .wanimgraph by logical path (content-root relative, or engine/...).") },
			{ Wui::HashId("animgraph.toolbar.locate"), compactBar ? AGTr("toolbar.locate", "Find") : AGTr("toolbar.locate", "Locate"),
				compactBar ? 48.0f : 62.0f, !m_LoadedPath.empty(),
				AGTr("toolbar.locate.tooltip", "Reveal the asset file in the OS file explorer.") },
			{ Wui::HashId("animgraph.toolbar.save"), AGTr("toolbar.save", "Save"), compactBar ? 50.0f : 54.0f, true,
				AGTr("toolbar.save.tooltip", "Save the graph to its .wanimgraph file (Ctrl+S while the panel is focused).") },
			{ Wui::HashId("animgraph.toolbar.reload"), AGTr("toolbar.reload", "Reload"), compactBar ? 56.0f : 66.0f,
				!m_LoadedPath.empty(), AGTr("toolbar.reload.tooltip", "Discard unsaved edits and load the file again (Ctrl+R while the panel is focused).") },
			{ Wui::HashId("animgraph.toolbar.undo"),
				compactBar ? AGTr("toolbar.undo.compact", "Undo") : AGTr("toolbar.undo", "Undo"),
				compactBar ? 48.0f : 58.0f, !m_UndoStack.empty(),
				m_UndoStack.empty() ? AGTr("toolbar.undo.disabled", "Nothing to undo.")
					: AGTr("toolbar.undo.tooltip", "Undo the last animation-graph edit (Ctrl+Z while the panel is focused).") },
			{ Wui::HashId("animgraph.toolbar.redo"),
				compactBar ? AGTr("toolbar.redo.compact", "Redo") : AGTr("toolbar.redo", "Redo"),
				compactBar ? 48.0f : 58.0f, !m_RedoStack.empty(),
				m_RedoStack.empty() ? AGTr("toolbar.redo.disabled", "Nothing to redo.")
					: AGTr("toolbar.redo.tooltip", "Redo the last undone edit (Ctrl+Y or Ctrl+Shift+Z while the panel is focused).") },
			{ Wui::HashId("animgraph.toolbar.apply"),
				compactBar ? AGTr("toolbar.apply.compact", "Apply") : AGTr("toolbar.apply", "Apply to Selected Entity"), compactBar ? 56.0f : 176.0f, true,
				AGTr("toolbar.apply.tooltip", "Bind this graph to the selected entity's AnimGraphComponent and reload the runtime.") },
		};

		float totalW = 0.0f;
		for (const ToolButton& button : buttons)
			totalW += button.Width;
		totalW += static_cast<float>(std::size(buttons) - 1) * theme.PadSmall;

		float buttonX = bar.X + bar.W - theme.Pad - totalW;
		const float buttonY = bar.Y + (bar.H - kRowH) * 0.5f;

		const std::string pathText = m_LoadedPath.empty() ? AGTr("toolbar.untitled", "Untitled animation graph") : m_LoadedPath;
		const float pathW = buttonX - (bar.X + theme.Pad) - theme.Pad;
		if (pathW > 64.0f)
		{
			const std::string clipped = Wui::EllipsizeMiddleToWidth(ctx, pathText, pathW - 18.0f,
				theme.FontSizeBody);
			Wui::Label(ctx, { bar.X + theme.Pad, bar.Y + (bar.H - theme.FontSizeBody) * 0.5f - 1.0f },
				clipped, m_LoadedPath.empty() ? theme.TextMuted : theme.Text, theme.FontSizeBody);
			Wui::Tooltip(ctx, { bar.X + theme.Pad, bar.Y, pathW, bar.H }, pathText);
			if (m_Dirty)
			{
				const float dirtyX = bar.X + theme.Pad
					+ ctx.MeasureTextWidth(clipped, theme.FontSizeBody) + 4.0f;
				Wui::Label(ctx, { dirtyX, bar.Y + (bar.H - theme.FontSizeTitle) * 0.5f - 1.0f },
					"*", theme.Warning, theme.FontSizeTitle);
			}
		}

		for (const ToolButton& button : buttons)
		{
			const Wui::WuiRect r { buttonX, buttonY, button.Width, kRowH };
			const bool pressed = Wui::ActionButton(ctx, button.Id, r, button.Label, theme,
				button.Enabled, button.Tooltip, false);
			if (pressed)
			{
				if (button.Id == Wui::HashId("animgraph.toolbar.open"))
				{
					m_OpenRowVisible = !m_OpenRowVisible;
					if (m_OpenRowVisible)
					{
						m_OpenPathBuffer = m_LoadedPath;
						ctx.SetFocus(Wui::HashId("animgraph.open.path"));
					}
				}
				else if (button.Id == Wui::HashId("animgraph.toolbar.locate"))
				{
					if (!Wui::LocateAssetWithHook(m_LoadedPath))
						SetStatus(AGFmt("status.reveal_failed", "Could not reveal {path} in the file explorer",
				{ { "path", m_LoadedPath } }), true);
				}
				else if (button.Id == Wui::HashId("animgraph.toolbar.save"))
				{
					SaveAsset();
				}
				else if (button.Id == Wui::HashId("animgraph.toolbar.reload"))
				{
					ReloadAsset();
				}
				else if (button.Id == Wui::HashId("animgraph.toolbar.undo"))
				{
					Undo();
				}
				else if (button.Id == Wui::HashId("animgraph.toolbar.redo"))
				{
					Redo();
				}
				else if (button.Id == Wui::HashId("animgraph.toolbar.apply"))
				{
					ApplyToSelected(host);
				}
			}
			buttonX += button.Width + theme.PadSmall;
		}

		// 3 秒淡出的状态条:工具栏底边一条细线,成功绿 / 失败红。
		if (m_StatusAge > 0.0f)
		{
			const float alpha = std::min(1.0f, m_StatusAge / 1.5f);
			const Wui::WuiColor color = m_StatusError ? theme.Danger : theme.Success;
			Wui::PanelBackground(ctx, { bar.X, bar.Y + bar.H - 2.0f, bar.W, 2.0f },
				WithAlpha(color, alpha), 0.0f);
		}
	}

	void AnimGraphPanel::RenderOpenRow(Wui::WuiContext& ctx, const Wui::WuiRect& row, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		(void)host;
		Wui::PanelBackground(ctx, row, theme.ContentBg, theme.Radius);

		const float openW = 64.0f;
		const float cancelW = 64.0f;
		const float fieldW = std::max(60.0f, row.W - openW - cancelW - theme.Pad * 3.0f);
		const Wui::WuiRect field { row.X + theme.Pad, row.Y + 2.0f, fieldW, kRowH };

		Wui::TextFieldA11y a11y;
		a11y.Label = AGTr("open.path", "Animation graph path");
		a11y.Placeholder = "animation/locomotion.wanimgraph";
		const bool submitted = Wui::TextField(ctx, Wui::HashId("animgraph.open.path"), field,
			m_OpenPathBuffer, theme, nullptr, &a11y);

		const Wui::WuiRect openButton { field.X + field.W + theme.PadSmall, field.Y, openW, kRowH };
		const Wui::WuiRect cancelButton { openButton.X + openW + theme.PadSmall, field.Y, cancelW, kRowH };

		const bool openPressed = Wui::ActionButton(ctx, Wui::HashId("animgraph.open.confirm"), openButton,
			AGTr("open.confirm", "Open"), theme, true,
			AGTr("open.confirm.tooltip", "Load the graph at this logical path (content root or engine/...)."), false);
		const bool cancelPressed = Wui::ActionButton(ctx, Wui::HashId("animgraph.open.cancel"), cancelButton,
			AGTr("open.cancel", "Cancel"), theme, true,
			AGTr("open.cancel.tooltip", "Close the path entry without loading anything."), false);

		if (submitted || openPressed)
		{
			if (!m_OpenPathBuffer.empty())
				LoadAsset(m_OpenPathBuffer);
			m_OpenRowVisible = false;
			ctx.SetFocus(0);
		}
		else if (cancelPressed)
		{
			m_OpenRowVisible = false;
			ctx.SetFocus(0);
		}
	}

	// ---------------------------------------------------------------------------
	// 左栏:状态 / 层
	// ---------------------------------------------------------------------------

	void AnimGraphPanel::RenderLeftColumn(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		(void)host;
		Wui::PanelBackground(ctx, area, theme.CardBg, theme.Radius);

		// ---- States ----
		Wui::Label(ctx, { area.X + theme.Pad, area.Y + 5.0f }, AGTr("states.header", "STATES"), theme.TextMuted,
			theme.FontSizeCaption);
		const Wui::WuiRect addState { area.X + area.W - kRowH - theme.PadSmall, area.Y + 1.0f, kRowH, kRowH };
		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.states.add"), addState, "+", theme, true,
			AGTr("states.add.tooltip", "New state: Single Clip, Blend 1D or Blend 2D."), false))
		{
			m_NewStateMenuOpen = !m_NewStateMenuOpen;
			m_NewStateMenuPos = { addState.X - 136.0f, addState.Y + addState.H };
			if (m_NewStateMenuOpen)
				ctx.OpenPopup(Wui::HashId("animgraph.newstate.menu"));
			else
				ctx.ClosePopup(Wui::HashId("animgraph.newstate.menu"));
		}

		// 垂直预算:列表顶 → 底部(留出 Delete Layer 按钮)之间,按 55/45 分给 States / Layers。
		// 每一步的矩形都从游标推出来,保证任何面板高度下两块列表与按钮都不重叠。
		const float deleteH = kRowH + theme.PadSmall;
		const float layersHeaderH = kRowH + kGap;
		const float footerH = m_SelectedLayer.empty() ? 0.0f : (kRowH + theme.PadSmall);
		const float listTop = area.Y + kRowH + 1.0f;
		const float listBottom = area.Y + area.H - footerH - theme.PadSmall;
		const float listBudget = std::max(140.0f, listBottom - listTop - deleteH - layersHeaderH);
		const float statesH = std::max(72.0f, listBudget * 0.55f);

		const Wui::WuiRect statesList { area.X + theme.PadSmall, listTop,
			area.W - theme.PadSmall * 2.0f, statesH };

		std::vector<std::string> stateNames;
		std::vector<Wui::ListViewItem> stateItems;
		stateNames.reserve(m_Graph.GetStates().size());
		stateItems.reserve(m_Graph.GetStates().size());
		for (const auto& entry : m_Graph.GetStates())
		{
			stateNames.push_back(entry.first);
			Wui::ListViewItem item;
			item.Id = Wui::HashId(("animgraph.state.row." + entry.first).c_str());
			item.Label = entry.first;
			item.SubLabel = KindBadge(entry.second.Kind);
			item.Selected = entry.first == m_SelectedState;
			stateItems.push_back(std::move(item));
		}

		if (stateItems.empty())
		{
			Wui::EmptyState(ctx, statesList, "", AGTr("states.empty.title", "No states"),
				AGTr("states.empty.body", "Create Clip / Blend 1D / Blend 2D states with +."),
				"", 0, theme);
		}
		else
		{
			const Wui::ListViewResult result = Wui::ListView(ctx, statesList, stateItems, kRowH, m_StateScroll,
				theme, Wui::HashId("animgraph.states.list"));
			if (result.Clicked >= 0 && static_cast<size_t>(result.Clicked) < stateNames.size())
			{
				const std::string& picked = stateNames[static_cast<size_t>(result.Clicked)];
				if (picked != m_SelectedState)
				{
					m_SelectedState = picked;
					m_SelectedSample = -1;
					m_SelectedTransition = -1;
					m_TrianglesDirty = true;
					m_CenterScroll = 0.0f;
					m_NameBufferKey.clear();
					m_ConditionKey.clear();
				}
			}
		}

		float y = statesList.Y + statesList.H + theme.PadSmall;
		const Wui::WuiRect deleteState { area.X + theme.Pad, y, area.W - theme.Pad * 2.0f, kRowH };
		const bool armed = !m_SelectedState.empty() && m_DeleteArmState == m_SelectedState;
		const std::string deleteLabel = armed
			? AGTr("states.delete.confirm", "Confirm Delete")
			: (m_SelectedState.empty() ? AGTr("states.delete.none", "Delete State")
				: AGFmt("states.delete.named", "Delete '{name}'", { { "name", m_SelectedState } }));
		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.states.delete"), deleteState, deleteLabel, theme,
			!m_SelectedState.empty(),
			armed ? AGTr("states.delete.tooltip.armed", "Click again to delete this state. Transitions pointing at it are removed.")
				: AGTr("states.delete.tooltip", "Delete the selected state (two clicks: the first one only arms the action)."),
			true))
		{
			if (armed)
			{
				DeleteSelectedState();
			}
			else
			{
				m_DeleteArmState = m_SelectedState;
				m_DeleteArmAge = 3.0f;
			}
		}

		y += kRowH + kGap;
		Wui::Label(ctx, { area.X + theme.Pad, y + 5.0f }, AGTr("layers.header", "LAYERS"), theme.TextMuted, theme.FontSizeCaption);
		const Wui::WuiRect addLayer { area.X + area.W - kRowH - theme.PadSmall, y + 1.0f, kRowH, kRowH };
		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.layers.add"), addLayer, "+", theme, true,
			AGTr("layers.add.tooltip", "Add an additive upper-body layer (clip + weight parameter + bone mask)."), false))
		{
			PushUndo();
			AnimLayer layer;
			layer.Name = UniqueLayerName(m_Graph, "UpperBody");
			layer.ClipName = "Aim";
			layer.WeightParameter = "AimWeight";
			layer.BoneMask.push_back(BoneMaskEntry { "Spine", 0.25f });
			layer.BoneMask.push_back(BoneMaskEntry { "Chest", 1.0f });
			layer.BoneMask.push_back(BoneMaskEntry { "Neck", 1.0f });
			layer.BoneMask.push_back(BoneMaskEntry { "Head", 1.0f });
			std::vector<AnimLayer> layers = m_Graph.GetLayers();
			layers.push_back(layer);
			m_Graph.SetLayers(std::move(layers));
			m_SelectedLayer = layer.Name;
			MarkDirty();
		}

		Wui::Label(ctx, { area.X + theme.Pad, y + kRowH + 2.0f },
			Wui::EllipsizeMiddleToWidth(ctx, AGTr("layers.hint", "Select a layer, then edit clip / weight / mask in the center column."),
				area.W - theme.Pad * 2.0f, theme.FontSizeCaption),
			theme.TextDisabled, theme.FontSizeCaption);
		const float layersTop = y + kRowH + 16.0f;
		const Wui::WuiRect layersList { area.X + theme.PadSmall, layersTop,
			area.W - theme.PadSmall * 2.0f, std::max(40.0f, listBottom - layersTop) };
		std::vector<std::string> layerNames;
		std::vector<Wui::ListViewItem> layerItems;
		for (const AnimLayer& layer : m_Graph.GetLayers())
		{
			layerNames.push_back(layer.Name);
			Wui::ListViewItem item;
			item.Id = Wui::HashId(("animgraph.layer.row." + layer.Name).c_str());
			item.Label = layer.Name;
			const std::string clipLabel = layer.ClipName.empty() ? AGTr("layers.no_clip", "(no clip)") : layer.ClipName;
			const std::string weightLabel = layer.WeightParameter.empty()
				? AGTr("layers.no_weight", "(no weight parameter)") : layer.WeightParameter;
			item.SubLabel = clipLabel + "  ·  " + weightLabel;
			item.Selected = layer.Name == m_SelectedLayer;
			layerItems.push_back(std::move(item));
		}
		if (layerItems.empty())
		{
			Wui::EmptyState(ctx, layersList, "", AGTr("layers.empty.title", "No layers"),
				AGTr("layers.empty.body", "Add a layer to blend an upper-body clip over the base pose."), "", 0, theme);
		}
		else
		{
			const Wui::ListViewResult result = Wui::ListView(ctx, layersList, layerItems, kRowH, m_LayerScroll,
				theme, Wui::HashId("animgraph.layers.list"));
			if (result.Clicked >= 0 && static_cast<size_t>(result.Clicked) < layerNames.size())
			{
				m_SelectedLayer = layerNames[static_cast<size_t>(result.Clicked)];
				m_CenterScroll = 0.0f;
				m_LayerCardScroll = 0.0f;
				m_CenterTab = 1;   // 点层 = 直接进入层编辑页
				m_LayerMaskScroll = 0.0f;
				m_LayerMaskTextKey.clear();
				m_NewParameterRowVisible = false;
				m_NewParameterBuffer.clear();
			}
		}

		if (!m_SelectedLayer.empty())
		{
			const Wui::WuiRect deleteLayer { area.X + theme.Pad, area.Y + area.H - kRowH - theme.PadSmall,
				area.W - theme.Pad * 2.0f, kRowH };
			if (Wui::ActionButton(ctx, Wui::HashId("animgraph.layers.delete"), deleteLayer,
				AGTr("layers.delete", "Delete Layer"), theme, true,
				AGTr("layers.delete.tooltip", "Remove the selected layer."), true))
			{
				PushUndo();
				std::vector<AnimLayer> layers = m_Graph.GetLayers();
				layers.erase(std::remove_if(layers.begin(), layers.end(),
					[this](const AnimLayer& layer) { return layer.Name == m_SelectedLayer; }), layers.end());
				m_Graph.SetLayers(std::move(layers));
				m_SelectedLayer.clear();
				MarkDirty();
			}
		}
	}

	void AnimGraphPanel::RenderLayersCard(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		RenderLayerCard(ctx, area, host, theme);
	}

	// ---------------------------------------------------------------------------
	// 中栏:状态属性 / 混合空间 / 样本 / 过渡 / 层属性
	// ---------------------------------------------------------------------------

	void AnimGraphPanel::RenderCenterColumn(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		const AnimState* state = SelectedState();
		const size_t transitionCount = state ? state->Transitions.size() : 0u;
		const size_t sampleCount = state
			? (state->Kind == AnimStateKind::BlendSpace1D ? state->Blend1D.GetSampleCount()
				: (state->Kind == AnimStateKind::BlendSpace2D ? state->Blend2D.GetSampleCount() : 0u))
			: 0u;

		// 卡片高度与绘制时的行距使用同一组公式(含行间 4px),否则最后一行会被按钮压住。
		const float samplesCardH = kRowH + 1.0f + kSampleRowH * static_cast<float>(sampleCount) + kRowH + 2.0f;
		const float transitionsCardH = kRowH + 4.0f
			+ (kTransitionRowH + theme.PadSmall) * static_cast<float>(transitionCount) + kRowH + theme.PadSmall;

		// 顶部页签「状态 / 层」：窄窗口下中栏可视高度有限，层编辑（尤其是遮罩骨骼列表）
		// 放在长滚动区底部会让用户找不到（2026-10-23 用户反馈「遮罩如何编辑呢」）。
		const Wui::WuiRect tabBar { area.X + theme.PadSmall, area.Y,
			area.W - theme.PadSmall * 2.0f, kRowH };
		const float tabW = (tabBar.W - theme.PadSmall * 2.0f) / 3.0f;
		const Wui::WuiRect stateTab { tabBar.X, tabBar.Y, tabW, kRowH };
		const Wui::WuiRect layerTab { stateTab.X + tabW + theme.PadSmall, tabBar.Y, tabW, kRowH };
		const Wui::WuiRect poseTab { layerTab.X + tabW + theme.PadSmall, tabBar.Y,
			tabBar.W - tabW * 2.0f - theme.PadSmall * 2.0f, kRowH };
		const std::string stateTabLabel = (m_CenterTab == 0 ? "● " : "")
			+ AGTr("center.tab.state", "状态");
		const std::string layerTabLabel = (m_CenterTab == 1 ? "● " : "")
			+ AGTr("center.tab.layer", "层");
		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.center.tab.state"), stateTab, stateTabLabel,
			theme, true, AGTr("center.tab.state.tooltip", "State properties / blend space / transitions / pose preview."), false))
			m_CenterTab = 0;
		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.center.tab.layer"), layerTab, layerTabLabel,
			theme, true, AGTr("center.tab.layer.tooltip", "Additive layers: clip / weight parameter / bone mask."), false))
			m_CenterTab = 1;
		const std::string poseTabLabel = (m_CenterTab == 2 ? "● " : "")
			+ AGTr("center.tab.pose", "姿态");
		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.center.tab.pose"), poseTab, poseTabLabel,
			theme, true, AGTr("center.tab.pose.tooltip", "Pose preview: the current graph pose (enable 模拟 to play it)."), false))
			m_CenterTab = 2;

		const Wui::WuiRect contentArea { area.X, tabBar.Y + kRowH + kGap * 0.5f, area.W,
			std::max(40.0f, area.H - kRowH - kGap * 0.5f) };

		if (m_CenterTab == 2)
		{
			// 姿态页：整块给预览，窄窗口也不用滚动。
			const Wui::WuiRect poseCard { contentArea.X + theme.PadSmall, contentArea.Y + kGap,
				contentArea.W - theme.PadSmall * 2.0f, std::max(120.0f, contentArea.H - kGap * 2.0f) };
			RenderPosePreview(ctx, poseCard, theme);
			return;
		}

		if (m_CenterTab == 1)
		{
			const float layerH = std::max(kLayerCardH, contentArea.H - kGap * 2.0f);
			const float contentH = layerH + kGap * 2.0f;
			if (!Wui::BeginScrollArea(ctx, contentArea, contentH, m_LayerCardScroll, theme, 0))
				return;
			const Wui::WuiRect layerCard { contentArea.X + theme.PadSmall, contentArea.Y + kGap - m_LayerCardScroll,
				contentArea.W - theme.PadSmall * 2.0f, layerH };
			if (m_SelectedLayer.empty())
			{
				Wui::EmptyState(ctx, layerCard, "", AGTr("center.layer.empty.title", "No layer selected"),
					AGTr("center.layer.empty.body", "Pick a layer in the left column, or add one with +."),
					"", 0, theme);
			}
			else
			{
				RenderLayerCard(ctx, layerCard, host, theme);
			}
			Wui::EndScrollArea(ctx);
			return;
		}

		float contentH = kGap + kStateGraphH + kGap;
		contentH += kStateCardH + kGap;
		if (state && state->Kind != AnimStateKind::SingleClip)
		{
			contentH += kBlendCardH + kGap;
			contentH += samplesCardH + kGap;
		}
		contentH += transitionsCardH + kGap;
		contentH += kGap;

		if (!Wui::BeginScrollArea(ctx, contentArea, contentH, m_CenterScroll, theme, 0))
			return;

		float y = contentArea.Y + kGap - m_CenterScroll;
		const Wui::WuiRect stateGraph { area.X + theme.PadSmall, y, area.W - theme.PadSmall * 2.0f, kStateGraphH };
		RenderStateGraph(ctx, stateGraph, host, theme);
		y += kStateGraphH + kGap;

		const Wui::WuiRect stateCard { area.X + theme.PadSmall, y, area.W - theme.PadSmall * 2.0f, kStateCardH };
		RenderStateCard(ctx, stateCard, host, theme);
		y += kStateCardH + kGap;

		// 每张卡片都可能改 AnimGraph(ReplaceState 会重建状态),因此每次重新取指针,
		// 不复用上一张卡片拿到的 const AnimState*(改名/改类型后它就是悬空指针)。
		state = SelectedState();
		if (state && state->Kind != AnimStateKind::SingleClip)
		{
			const Wui::WuiRect blendCard { area.X + theme.PadSmall, y, area.W - theme.PadSmall * 2.0f, kBlendCardH };
			RenderBlendSpaceCard(ctx, blendCard, host, theme);
			y += kBlendCardH + kGap;

			state = SelectedState();
			const size_t liveSamples = state ? static_cast<size_t>(state->Kind == AnimStateKind::BlendSpace1D
				? state->Blend1D.GetSampleCount()
				: (state->Kind == AnimStateKind::BlendSpace2D ? state->Blend2D.GetSampleCount() : 0u)) : 0u;
			const float samplesH = kRowH + 1.0f + kSampleRowH * static_cast<float>(liveSamples) + kRowH + 2.0f;
			const Wui::WuiRect samplesCard { area.X + theme.PadSmall, y, area.W - theme.PadSmall * 2.0f, samplesH };
			RenderSamplesCard(ctx, samplesCard, host, theme);
			y += samplesH + kGap;
		}

		state = SelectedState();
		const size_t liveTransitions = state ? state->Transitions.size() : 0u;
		const float transitionsH = kRowH + theme.PadSmall + (kTransitionRowH + theme.PadSmall) * static_cast<float>(liveTransitions)
			+ kRowH + theme.PadSmall;
		const Wui::WuiRect transitionsCard { area.X + theme.PadSmall, y, area.W - theme.PadSmall * 2.0f, transitionsH };
		RenderTransitionsCard(ctx, transitionsCard, host, theme);
		y += transitionsH + kGap;

		Wui::EndScrollArea(ctx);
	}

	void AnimGraphPanel::RenameSelectedState(const std::string& newName)
	{
		const AnimState* state = SelectedState();
		if (state == nullptr || newName.empty() || newName == state->Name)
			return;
		AnimState updated = *state;
		updated.Name = newName;
		if (ReplaceState(state->Name, updated))
			m_NameBufferKey.clear();
	}

	void AnimGraphPanel::CommitPendingCondition()
	{
		if (m_ConditionFocusKey.empty())
			return;
		const size_t separator = m_ConditionFocusKey.find('#');
		const AnimState* live = separator == std::string::npos
			? nullptr : m_Graph.FindState(m_ConditionFocusKey.substr(0, separator));
		if (live == nullptr || separator == std::string::npos)
		{
			m_ConditionFocusKey.clear();
			return;
		}
		const size_t index = static_cast<size_t>(std::atoi(m_ConditionFocusKey.c_str() + separator + 1));
		AnimState updated = *live;
		if (index < updated.Transitions.size()
			&& updated.Transitions[index].ConditionExpression != m_ConditionBuffer)
		{
			updated.Transitions[index].ConditionExpression = m_ConditionBuffer;
			ReplaceState(updated.Name, updated);
		}
		m_ConditionFocusKey.clear();
	}
	void AnimGraphPanel::RenderStateCard(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		(void)host;
		Wui::PanelBackground(ctx, area, theme.CardBg, theme.Radius);
		Wui::SectionHeader(ctx, { area.X + theme.Pad, area.Y + 2.0f, area.W - theme.Pad * 2.0f, kRowH },
			AGTr("state.title", "State"), theme.Text, theme, theme.FontSizeTitle);

		// 注意:本函数里任何一次提交都会让 AnimGraph 重建状态对象(ReplaceState),
		// 因此每读一个字段前都重新取 SelectedState(),不跨提交持有指针。
		const AnimState* state = SelectedState();
		if (state == nullptr)
		{
			Wui::EmptyState(ctx, area, "", AGTr("state.empty.title", "No state selected"),
				AGTr("state.empty.body", "Pick a state on the left, or create one with the + button."), "", 0, theme);
			return;
		}
		const std::string stateName = state->Name;

		const float labelW = 76.0f;
		float y = area.Y + kRowH + 6.0f;
		const float controlX = area.X + theme.Pad + labelW;
		const float controlW = std::max(40.0f, area.W - theme.Pad * 2.0f - labelW);

		// Name
		Wui::Label(ctx, { area.X + theme.Pad, y + 4.0f }, AGTr("state.name", "Name"), theme.TextMuted, theme.FontSizeSmall);
		if (m_NameBufferKey != stateName)
		{
			m_NameBufferKey = stateName;
			m_NameBuffer = stateName;
		}
		m_NameFieldRect = { controlX, y, controlW, kRowH };
		Wui::TextFieldA11y nameA11y;
		nameA11y.Label = AGTr("state.name.a11y", "State name");
		nameA11y.Placeholder = "Idle";
		const Wui::WuiId nameId = Wui::HashId("animgraph.state.name");
		const bool wasFocused = m_NameFieldWasFocused;
		const bool submitted = Wui::TextField(ctx, nameId, { controlX, y, controlW, kRowH }, m_NameBuffer,
			theme, nullptr, &nameA11y);
		Wui::Tooltip(ctx, { controlX, y, controlW, kRowH },
			AGTr("state.name.tooltip", "State name used by transitions and by the runtime readback. Enter or click away to commit."));
		const bool focused = ctx.Focus() == nameId;
		if (submitted)
		{
			RenameSelectedState(m_NameBuffer);
			ctx.SetFocus(0);
		}
		else if (wasFocused && !focused)
		{
			RenameSelectedState(m_NameBuffer);
		}
		m_NameFieldWasFocused = focused;
		y += kRowH + 4.0f;

		// Kind
		state = SelectedState();
		if (state == nullptr)
			return;
		Wui::Label(ctx, { area.X + theme.Pad, y + 4.0f }, AGTr("state.kind", "Kind"), theme.TextMuted, theme.FontSizeSmall);
		std::vector<std::string> kinds { AGTr("kind.single", "Single Clip"), AGTr("kind.blend1d", "Blend 1D"),
			AGTr("kind.blend2d", "Blend 2D") };
		int kindIndex = static_cast<int>(state->Kind);
		if (Wui::Combo(ctx, Wui::HashId("animgraph.state.kind"), { controlX, y, controlW, kRowH }, "",
			kinds, kindIndex, theme))
		{
			AnimState updated = *state;
			updated.Kind = static_cast<AnimStateKind>(kindIndex);
			if (updated.Kind == AnimStateKind::BlendSpace1D && updated.Blend1D.GetSampleCount() == 0)
			{
				updated.Blend1D.AddSample(0.0f, updated.ClipName.empty() ? "Idle" : updated.ClipName);
				updated.Blend1D.AddSample(1.6f, "Walk");
				updated.Blend1D.AddSample(4.5f, "Run");
			}
			if (updated.Kind == AnimStateKind::BlendSpace2D && updated.Blend2D.GetSampleCount() == 0)
			{
				updated.Blend2D.AddSample({ 0.0f, 0.0f }, updated.ClipName.empty() ? "Idle" : updated.ClipName);
				updated.Blend2D.AddSample({ 1.6f, 0.0f }, "Walk");
				updated.Blend2D.AddSample({ 4.5f, 0.0f }, "Run");
				updated.Blend2D.AddSample({ 1.6f, 90.0f }, "StrafeRight");
				updated.Blend2D.AddSample({ 1.6f, -90.0f }, "StrafeLeft");
			}
			if (ReplaceState(stateName, updated))
			{
				m_SelectedSample = -1;
				m_TrianglesDirty = true;
			}
		}
		Wui::Tooltip(ctx, { controlX, y, controlW, kRowH },
			AGTr("state.kind.tooltip", "Single Clip plays one clip; Blend 1D/2D blend samples by Speed / Direction."));
		y += kRowH + 4.0f;

		// Clip
		state = SelectedState();
		if (state == nullptr)
			return;
		Wui::Label(ctx, { area.X + theme.Pad, y + 4.0f }, AGTr("state.clip", "Clip"), theme.TextMuted, theme.FontSizeSmall);
		if (state->Kind == AnimStateKind::SingleClip)
		{
			if (m_ModelValid && !m_Model.Animations.empty())
			{
				const std::vector<std::string> clips = ClipOptionsForCombo(state->ClipName);
				int clipIndex = 0;
				const auto found = std::find(clips.begin(), clips.end(), state->ClipName);
				if (found != clips.end())
					clipIndex = static_cast<int>(std::distance(clips.begin(), found));
				if (Wui::Combo(ctx, Wui::HashId("animgraph.state.clip"), { controlX, y, controlW, kRowH }, "",
					clips, clipIndex, theme,
					state->ClipName.empty() ? AGTr("state.pick_clip", "(pick a clip)") : state->ClipName))
				{
					if (clipIndex >= 0 && static_cast<size_t>(clipIndex) < clips.size())
					{
						AnimState updated = *state;
						updated.ClipName = clips[static_cast<size_t>(clipIndex)];
						ReplaceState(stateName, updated);
					}
				}
				Wui::Tooltip(ctx, { controlX, y, controlW, kRowH },
					AGFmt("state.clip_tooltip", "Clip from the selected entity's model ({count} clips available).",
					{ { "count", std::to_string(m_Model.Animations.size()) } }));
			}
			else
			{
				const std::string hint = m_ModelKey.empty()
					? AGTr("state.no_model", "No model bound: select a character with a SkinnedMeshRenderer.")
					: (m_ModelError.empty()
						? AGFmt("state.no_clips", "No animation clips in {path}", { { "path", m_ModelKey } })
						: AGTr("state.model_load_failed", "Model failed to load: ") + m_ModelError);
				Wui::Label(ctx, { controlX, y + 4.0f },
					Wui::EllipsizeMiddleToWidth(ctx, hint, controlW, theme.FontSizeSmall), theme.TextMuted,
					theme.FontSizeSmall);
				Wui::Tooltip(ctx, { controlX, y, controlW, kRowH }, hint);
			}
		}
		else
		{
			Wui::Label(ctx, { controlX, y + 4.0f },
				AGFmt("state.samples_hint", "{count} samples - edit the clip column below",
					{ { "count", std::to_string(state->Kind == AnimStateKind::BlendSpace1D
						? state->Blend1D.GetSampleCount() : state->Blend2D.GetSampleCount()) } }),
				theme.TextMuted, theme.FontSizeSmall);
		}
		y += kRowH + 4.0f;

		// Loop + Speed(快照:Checkbox 提交后指针会失效)
		state = SelectedState();
		if (state == nullptr)
			return;
		bool loop = state->Loop;
		const float speedValue = state->Speed;
		if (Wui::Checkbox(ctx, Wui::HashId("animgraph.state.loop"), { area.X + theme.Pad, y, 92.0f, kRowH },
			AGTr("state.loop", "Loop"), loop, theme))
		{
			AnimState updated = *state;
			updated.Loop = loop;
			ReplaceState(stateName, updated);
		}
		Wui::Tooltip(ctx, { area.X + theme.Pad, y, 92.0f, kRowH },
			AGTr("state.loop.tooltip", "Loop the clip at its end. Non-looping clips clamp to the last frame."));

		const Wui::WuiRect speedRect { area.X + theme.Pad + 108.0f, y, 96.0f, kRowH };
		float speed = speedValue;
		const Wui::WuiId speedId = Wui::HashId("animgraph.state.speed");
		if (Wui::DragFloat(ctx, speedId, speedRect, speed, 0.01f, 0.0f, 10.0f, theme)
			&& std::fabs(speed - speedValue) > 0.00001f)
		{
			const bool continuous = ctx.Input().MouseDown[0]
				&& (ctx.IsHovered(speedRect) || ctx.Focus() == speedId);
			BeginContinuousEdit("state.speed", continuous);
			state = SelectedState();
			if (state != nullptr)
			{
				AnimState updated = *state;
				updated.Speed = speed;
				ReplaceState(stateName, updated, false);
			}
		}
		Wui::Tooltip(ctx, speedRect, AGTr("state.speed.tooltip", "Clip playback speed multiplier (x), 0..10, default 1.0."));

		const Wui::WuiRect speedLabel { speedRect.X + speedRect.W + theme.PadSmall, y + 4.0f, 120.0f, kRowH };
		Wui::Label(ctx, { speedLabel.X, speedLabel.Y }, AGTr("state.speed_label", "Speed (x)"), theme.TextMuted, theme.FontSizeSmall);

		const std::string modelHint = m_ModelKey.empty() ? m_Graph.GetModelHint() : m_ModelKey;
		if (!modelHint.empty())
		{
			const std::string line = AGTr("state.model_line", "Model: ") + modelHint;
			Wui::Label(ctx, { area.X + theme.Pad, area.Y + area.H - 18.0f },
				Wui::EllipsizeMiddleToWidth(ctx, line, area.W - theme.Pad * 2.0f, theme.FontSizeCaption),
				theme.TextDisabled, theme.FontSizeCaption);
			Wui::Tooltip(ctx, { area.X + theme.Pad, area.Y + area.H - 18.0f, area.W - theme.Pad * 2.0f, 16.0f },
				AGTr("state.model_tooltip", "Model used to populate clip names. Change it on the entity's SkinnedMeshRendererComponent."));
		}
	}
	void AnimGraphPanel::RenderTransitionsCard(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		(void)host;
		const AnimState* selected = SelectedState();
		if (selected == nullptr)
			return;
		Wui::PanelBackground(ctx, area, theme.CardBg, theme.Radius);
		Wui::SectionHeader(ctx, { area.X + theme.Pad, area.Y + 2.0f, area.W - theme.Pad * 2.0f, kRowH },
			AGFmt("transitions.title", "Transitions from {name}", { { "name", selected->Name } }), theme.Text, theme, theme.FontSizeTitle);

		// ReplaceState 会重建状态对象:本函数只按值持有快照,任何写入前都重新取一份最新状态。
		const std::string stateName = selected->Name;
		const std::vector<AnimTransition> transitions = selected->Transitions;
		auto copyState = [this, &stateName]() -> AnimState
		{
			const AnimState* live = m_Graph.FindState(stateName);
			return live ? *live : AnimState {};
		};

		float y = area.Y + kRowH + 4.0f;
		std::vector<std::string> targets;
		for (const auto& entry : m_Graph.GetStates())
			if (entry.first != stateName)
				targets.push_back(entry.first);
		if (targets.empty())
			targets.push_back(AGTr("transitions.no_other", "(no other state)"));

		for (size_t i = 0; i < transitions.size(); ++i)
		{
			const AnimTransition transition = transitions[i];
			const Wui::WuiRect row { area.X + theme.Pad, y, area.W - theme.Pad * 2.0f, kTransitionRowH };

			const float deleteW = kRowH;
			const float durationW = 96.0f;
			const float targetW = std::max(60.0f, row.W - deleteW - durationW - theme.Pad * 2.0f);
			const Wui::WuiRect targetRect { row.X, row.Y, targetW, kRowH };
			const Wui::WuiRect durationRect { targetRect.X + targetW + theme.PadSmall, row.Y, durationW, kRowH };
			const Wui::WuiRect deleteRect { durationRect.X + durationW + theme.PadSmall, row.Y, deleteW, kRowH };

			int targetIndex = 0;
			const auto found = std::find(targets.begin(), targets.end(), transition.TargetState);
			if (found != targets.end())
				targetIndex = static_cast<int>(std::distance(targets.begin(), found));
			if (Wui::Combo(ctx, Wui::HashId(("animgraph.transition.target." + std::to_string(i)).c_str()),
				targetRect, "", targets, targetIndex, theme,
				transition.TargetState.empty() ? AGTr("transitions.target", "Target") : transition.TargetState))
			{
				AnimState updated = copyState();
				if (i < updated.Transitions.size())
					updated.Transitions[i].TargetState = targets[static_cast<size_t>(targetIndex)];
				ReplaceState(stateName, updated);
			}
			Wui::Tooltip(ctx, targetRect, AGTr("transitions.target.tooltip", "Target state reached when the condition expression is true."));

			float duration = transition.Duration;
			const Wui::WuiId durationId = Wui::HashId(("animgraph.transition.duration." + std::to_string(i)).c_str());
			if (Wui::DragFloat(ctx, durationId, durationRect, duration, 0.005f, 0.0f, 2.0f, theme)
				&& std::fabs(duration - transition.Duration) > 0.00001f)
			{
				const bool continuous = ctx.Input().MouseDown[0]
					&& (ctx.IsHovered(durationRect) || ctx.Focus() == durationId);
				BeginContinuousEdit(stateName + ".transition.duration." + std::to_string(i), continuous);
				AnimState updated = copyState();
				if (i < updated.Transitions.size())
					updated.Transitions[i].Duration = duration;
				ReplaceState(stateName, updated, false);
			}
			Wui::Tooltip(ctx, durationRect, AGTr("transitions.duration.tooltip", "Cross-fade duration in seconds (0..2, default 0.20)."));

			if (Wui::ActionButton(ctx,
				Wui::HashId(("animgraph.transition.delete." + std::to_string(i)).c_str()), deleteRect, "-", theme,
				true, AGTr("transitions.delete.tooltip", "Remove this transition."), true))
			{
				AnimState updated = copyState();
				if (i < updated.Transitions.size())
					updated.Transitions.erase(updated.Transitions.begin() + static_cast<std::ptrdiff_t>(i));
				ReplaceState(stateName, updated);
				break;
			}

			const std::string key = stateName + "#" + std::to_string(i);
			if (m_ConditionKey != key)
			{
				m_ConditionKey = key;
				m_ConditionBuffer = transition.ConditionExpression;
			}
			const Wui::WuiRect conditionRect { row.X, row.Y + kRowH + 2.0f, row.W, kRowH };
			m_ConditionFieldRect = conditionRect;
			Wui::TextFieldA11y conditionA11y;
			conditionA11y.Label = AGTr("transitions.condition.a11y", "Transition condition");
			conditionA11y.Placeholder = "Speed > 0.10 && IsFalling < 0.50";
			const Wui::WuiId conditionId =
				Wui::HashId(("animgraph.transition.condition." + std::to_string(i)).c_str());
			const bool wasFocused = m_ConditionFocusKey == key;
			const bool submitted = Wui::TextField(ctx, conditionId, conditionRect, m_ConditionBuffer,
				theme, nullptr, &conditionA11y);
			const bool focused = ctx.Focus() == conditionId;
			auto commitCondition = [this, &copyState, &stateName, i]()
			{
				AnimState updated = copyState();
				if (i < updated.Transitions.size()
					&& updated.Transitions[i].ConditionExpression != m_ConditionBuffer)
					updated.Transitions[i].ConditionExpression = m_ConditionBuffer;
				else
					return;
				ReplaceState(stateName, updated);
			};
			if (submitted)
			{
				commitCondition();
				ctx.SetFocus(0);
				m_ConditionFocusKey.clear();
			}
			else if (wasFocused && !focused)
			{
				commitCondition();
				m_ConditionFocusKey.clear();
			}
			else if (focused)
			{
				m_ConditionFocusKey = key;
			}
			Wui::Tooltip(ctx, conditionRect,
				AGTr("transitions.condition.tooltip",
					"Condition expression: Param op number, combined with && / || (&& binds tighter). "
					"Unknown parameters read as 0. Empty = always true."));

			y += kTransitionRowH + theme.PadSmall;
		}

		const Wui::WuiRect addRect { area.X + theme.Pad, area.Y + area.H - kRowH - theme.PadSmall,
			area.W - theme.Pad * 2.0f, kRowH };
		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.transition.add"), addRect, AGTr("transitions.add", "+ Add Transition"),
			theme, !targets.empty() && m_Graph.GetStates().size() > 1,
			AGTr("transitions.add.tooltip", "Add a transition from this state to another state."), false))
		{
			AnimState updated = copyState();
			AnimTransition transition;
			transition.TargetState = targets.front();
			transition.Duration = 0.20f;
			updated.Transitions.push_back(transition);
			ReplaceState(stateName, updated);
		}
	}
	void AnimGraphPanel::RenderLayerCard(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		(void)host;
		Wui::PanelBackground(ctx, area, theme.CardBg, theme.Radius);
		Wui::SectionHeader(ctx, { area.X + theme.Pad, area.Y + 2.0f, area.W - theme.Pad * 2.0f, kRowH },
			AGTr("layer.title", "Layer"), theme.Text, theme, theme.FontSizeTitle);
		const Wui::WuiRect helpRect { area.X + area.W - theme.Pad - 18.0f, area.Y + 5.0f, 16.0f, 16.0f };
		Wui::PanelBackground(ctx, helpRect, theme.ButtonBg, 8.0f);
		Wui::Label(ctx, { helpRect.X + 5.0f, helpRect.Y + 1.0f }, "?", theme.TextMuted, theme.FontSizeCaption);
		Wui::Tooltip(ctx, helpRect,
			AGTr("layer.help.tooltip", "Layers are animations stacked on top of the base state machine; commonly used for upper-body aiming or reloads."));

		auto findLayer = [this]() -> const AnimLayer*
		{
			for (const AnimLayer& candidate : m_Graph.GetLayers())
				if (candidate.Name == m_SelectedLayer)
					return &candidate;
			return nullptr;
		};
		const AnimLayer* layer = findLayer();
		if (layer == nullptr)
			return;

		const float labelW = 76.0f;
		const float controlX = area.X + theme.Pad + labelW;
		const float controlW = std::max(40.0f, area.W - theme.Pad * 2.0f - labelW);
		float y = area.Y + kRowH + 6.0f;

		// ---- Clip：名字 + 模型片段元数据 + 缺失提示 ----
		Wui::Label(ctx, { area.X + theme.Pad, y + 4.0f }, AGTr("layer.clip", "Clip"), theme.TextMuted, theme.FontSizeSmall);
		{
			const std::vector<std::string> clips = ClipOptionsForCombo(layer->ClipName);
			std::vector<std::string> clipLabels;
			clipLabels.reserve(clips.size());
			for (const std::string& name : clips)
			{
				const ModelClipInfo info = FindModelClip(m_Model, name);
				if (info.Clip != nullptr)
				{
					clipLabels.push_back(name + "    ·  " + AGFmt("layer.clip.meta", "模型片段 · {frames} 帧 / {seconds}s",
						{ { "frames", std::to_string(info.Frames) }, { "seconds", FormatFloat(info.Duration, 2) } }));
				}
				else
				{
					clipLabels.push_back(name);
				}
			}
			int clipIndex = 0;
			const auto found = std::find(clips.begin(), clips.end(), layer->ClipName);
			if (found != clips.end())
				clipIndex = static_cast<int>(std::distance(clips.begin(), found));
			const bool clipMissing = m_ModelValid && !layer->ClipName.empty()
				&& FindModelClip(m_Model, layer->ClipName).Clip == nullptr;
			const float warningW = clipMissing ? 150.0f : 0.0f;
			const float comboW = std::max(90.0f, controlW - warningW - (clipMissing ? theme.PadSmall : 0.0f));
			if (Wui::Combo(ctx, Wui::HashId("animgraph.layer.clip"), { controlX, y, comboW, kRowH }, "",
				clipLabels, clipIndex, theme, layer->ClipName))
			{
				PushUndo();
				std::vector<AnimLayer> updated = m_Graph.GetLayers();
				for (AnimLayer& candidate : updated)
					if (candidate.Name == m_SelectedLayer)
						candidate.ClipName = clips[static_cast<size_t>(clipIndex)];
				m_Graph.SetLayers(std::move(updated));
				MarkDirty();
			}
			Wui::Tooltip(ctx, { controlX, y, comboW, kRowH }, AGTr("layer.clip.tooltip", "Clip sampled by this layer."));
			if (clipMissing)
			{
				const Wui::WuiRect warning { controlX + comboW + theme.PadSmall, y, warningW, kRowH };
				Wui::Label(ctx, { warning.X, warning.Y + 4.0f },
					Wui::EllipsizeMiddleToWidth(ctx, AGTr("layer.clip.missing", "（模型中没有此片段）"), warning.W,
						theme.FontSizeCaption), theme.Danger, theme.FontSizeCaption);
				Wui::Tooltip(ctx, warning, AGTr("layer.clip.missing.tooltip",
					"该片段在当前模型中不存在，运行时会静默不播放。"));
			}
		}
		y += kRowH + 2.0f;

		layer = findLayer();
		if (layer == nullptr)
			return;
		if (!layer->ClipName.empty())
		{
			const ModelClipInfo selectedClip = FindModelClip(m_Model, layer->ClipName);
			const std::string metaLine = selectedClip.Clip != nullptr
				? AGFmt("layer.clip.selected_meta", "模型片段 · {frames} 帧 / {seconds}s",
					{ { "frames", std::to_string(selectedClip.Frames) }, { "seconds", FormatFloat(selectedClip.Duration, 2) } })
				: AGTr("layer.clip.no_model", "选择带模型的角色后显示片段帧数与时长。");
			Wui::Label(ctx, { controlX, y }, Wui::EllipsizeMiddleToWidth(ctx, metaLine, controlW, theme.FontSizeCaption),
				theme.TextMuted, theme.FontSizeCaption);
			Wui::Tooltip(ctx, { controlX, y, controlW, 14.0f }, metaLine);
		}
		y += 16.0f;

		const std::string aimExplain = AGTr("layer.aim_explain",
			"Aim = 模型里的一条动画片段（瞄准姿态），层把它叠加到基础状态上。");
		Wui::Label(ctx, { controlX, y }, Wui::EllipsizeMiddleToWidth(ctx, aimExplain, controlW, theme.FontSizeCaption),
			theme.TextMuted, theme.FontSizeCaption);
		Wui::Tooltip(ctx, { controlX, y, controlW, 14.0f }, aimExplain);
		y += 20.0f;

		// ---- Weight：参数名（float），不是数值本身 ----
		layer = findLayer();
		if (layer == nullptr)
			return;
		Wui::Label(ctx, { area.X + theme.Pad, y + 4.0f }, AGTr("layer.weight", "Weight"), theme.TextMuted, theme.FontSizeSmall);
		{
			const auto& defaults = m_Graph.GetParameterDefaults();
			std::vector<std::string> parameterNames;
			parameterNames.reserve(defaults.size() + 1);
			for (const auto& entry : defaults)
				parameterNames.push_back(entry.first);
			const bool weightDefined = defaults.find(layer->WeightParameter) != defaults.end();
			int weightIndex = 0;
			const auto found = std::find(parameterNames.begin(), parameterNames.end(), layer->WeightParameter);
			if (found != parameterNames.end())
				weightIndex = static_cast<int>(std::distance(parameterNames.begin(), found));
			else if (!layer->WeightParameter.empty())
			{
				parameterNames.insert(parameterNames.begin(), layer->WeightParameter);
				weightIndex = 0;
			}
			const bool hasParameters = !defaults.empty() || !layer->WeightParameter.empty();
			if (parameterNames.empty())
				parameterNames.push_back(AGTr("layer.weight.no_parameters", "(no parameters)"));

			const float badgeW = 42.0f;
			const bool compactWeightRow = controlW < 280.0f;
			const float createW = compactWeightRow ? 28.0f : 96.0f;
			const float weightComboW = std::max(60.0f, controlW - badgeW - createW - theme.PadSmall * 2.0f);
			if (Wui::Combo(ctx, Wui::HashId("animgraph.layer.weight.parameter"),
				{ controlX, y, weightComboW, kRowH }, "", parameterNames, weightIndex, theme,
				layer->WeightParameter.empty() ? AGTr("layer.weight.pick", "(pick a parameter)") : layer->WeightParameter))
			{
				if (hasParameters && static_cast<size_t>(weightIndex) < parameterNames.size())
				{
					PushUndo();
					std::vector<AnimLayer> updated = m_Graph.GetLayers();
					for (AnimLayer& candidate : updated)
						if (candidate.Name == m_SelectedLayer)
							candidate.WeightParameter = parameterNames[static_cast<size_t>(weightIndex)];
					m_Graph.SetLayers(std::move(updated));
					MarkDirty();
				}
			}
			Wui::Tooltip(ctx, { controlX, y, weightComboW, kRowH },
				AGTr("layer.weight.tooltip", "权重 = 驱动本层强度的 float 参数名（0=关闭，1=全叠加），不是数值本身。"));

			const Wui::WuiRect badge { controlX + weightComboW + theme.PadSmall, y + 2.0f, badgeW, kRowH - 4.0f };
			Wui::Badge(ctx, badge, "float", WithAlpha(theme.Accent, 0.22f), theme.Accent, theme, theme.FontSizeCaption);
			const Wui::WuiRect create { badge.X + badge.W + theme.PadSmall, y, createW, kRowH };
			if (Wui::ActionButton(ctx, Wui::HashId("animgraph.layer.weight.create"), create,
				compactWeightRow ? std::string("+") : AGTr("layer.weight.create", "＋ 新建参数"), theme, true,
				AGTr("layer.weight.create.tooltip", "创建一个默认值为 0 的 float 参数，并把它设为当前层的权重参数。"), false))
			{
				m_NewParameterRowVisible = !m_NewParameterRowVisible;
				m_NewParameterBuffer.clear();
				ctx.SetFocus(m_NewParameterRowVisible
					? Wui::HashId("animgraph.layer.weight.new_name") : 0);
			}
			y += kRowH + 2.0f;

			if (!weightDefined && !layer->WeightParameter.empty())
			{
				const std::string warning = AGTr("layer.weight.missing", "（未定义参数，运行时按 0 处理）");
				Wui::Label(ctx, { controlX, y }, warning, theme.Warning, theme.FontSizeCaption);
				Wui::Tooltip(ctx, { controlX, y, controlW, 14.0f }, AGTr("layer.weight.missing.tooltip",
					"当前权重名不在图的参数表中；运行时会按 0 处理，这一层不会产生叠加效果。"));
				y += 16.0f;
			}

			if (m_NewParameterRowVisible)
			{
				const float cancelW = 54.0f;
				const float confirmW = 58.0f;
				const float fieldW = std::max(70.0f, controlW - cancelW - confirmW - theme.PadSmall * 2.0f);
				const Wui::WuiRect field { controlX, y, fieldW, kRowH };
				Wui::TextFieldA11y a11y;
				a11y.Label = AGTr("layer.weight.new_name", "New float parameter name");
				a11y.Placeholder = AGTr("layer.weight.new_placeholder", "Parameter name, e.g. AimWeight");
				const Wui::WuiId fieldId = Wui::HashId("animgraph.layer.weight.new_name");
				const bool submitted = Wui::TextField(ctx, fieldId, field, m_NewParameterBuffer, theme, nullptr, &a11y);
				const Wui::WuiRect confirm { field.X + field.W + theme.PadSmall, y, confirmW, kRowH };
				const Wui::WuiRect cancel { confirm.X + confirm.W + theme.PadSmall, y, cancelW, kRowH };
				const bool createPressed = Wui::ActionButton(ctx, Wui::HashId("animgraph.layer.weight.new_confirm"),
					confirm, AGTr("layer.weight.new_confirm", "创建"), theme, true,
					AGTr("layer.weight.new_confirm.tooltip", "创建这个 float 参数（默认值 0）。"), false);
				const bool cancelPressed = Wui::ActionButton(ctx, Wui::HashId("animgraph.layer.weight.new_cancel"),
					cancel, AGTr("layer.weight.new_cancel", "取消"), theme, true,
					AGTr("layer.weight.new_cancel.tooltip", "关闭新建参数输入行。"), true);
				if (submitted || createPressed)
				{
					const size_t first = m_NewParameterBuffer.find_first_not_of(" \t");
					const size_t last = m_NewParameterBuffer.find_last_not_of(" \t");
					const std::string name = first == std::string::npos ? std::string()
						: m_NewParameterBuffer.substr(first, last - first + 1);
					if (name.empty())
					{
						SetStatus(AGTr("layer.weight.new_empty", "参数名不能为空"), true);
					}
					else if (m_Graph.GetParameterDefaults().find(name) != m_Graph.GetParameterDefaults().end())
					{
						SetStatus(AGFmt("layer.weight.new_exists", "参数“{name}”已存在", { { "name", name } }), true);
					}
					else
					{
						PushUndo();
						m_Graph.SetParameterDefault(name, 0.0f);
						std::vector<AnimLayer> updated = m_Graph.GetLayers();
						for (AnimLayer& candidate : updated)
							if (candidate.Name == m_SelectedLayer)
								candidate.WeightParameter = name;
						m_Graph.SetLayers(std::move(updated));
						MarkDirty();
						SetStatus(AGFmt("layer.weight.new_created", "已创建 float 参数“{name}”（默认值 0）",
							{ { "name", name } }), false);
						m_NewParameterRowVisible = false;
						m_NewParameterBuffer.clear();
						ctx.SetFocus(0);
					}
				}
				else if (cancelPressed)
				{
					m_NewParameterRowVisible = false;
					m_NewParameterBuffer.clear();
					ctx.SetFocus(0);
				}
				y += kRowH + 4.0f;
			}
		}

		// ---- Bone mask：骨骼行 + 预设 + 添加 + 高级文本 ----
		layer = findLayer();
		if (layer == nullptr)
			return;
		Wui::Label(ctx, { area.X + theme.Pad, y + 4.0f }, AGTr("layer.mask", "Mask"), theme.TextMuted, theme.FontSizeSmall);
		y += kRowH;

		std::vector<std::string> boneNames;
		std::vector<std::string> modelBones;
		if (m_ModelValid)
		{
			// 只列骨骼节点：挂 mesh 的叶子节点（MannequinMesh 之类）不是骨骼，
			// 出现在遮罩列表里只会让用户困惑。
			for (const Asset::WModelNode& node : m_Model.Nodes)
			{
				if (node.Name.empty() || node.MeshIndex >= 0)
					continue;
				if (std::find(modelBones.begin(), modelBones.end(), node.Name) == modelBones.end())
					modelBones.push_back(node.Name);
			}
			boneNames = modelBones;
		}
		for (const BoneMaskEntry& bone : layer->BoneMask)
			if (std::find(boneNames.begin(), boneNames.end(), bone.BoneName) == boneNames.end())
				boneNames.push_back(bone.BoneName);

		auto setMask = [this](std::vector<BoneMaskEntry> mask)
		{
			PushUndo();
			std::vector<AnimLayer> updated = m_Graph.GetLayers();
			for (AnimLayer& candidate : updated)
				if (candidate.Name == m_SelectedLayer)
					candidate.BoneMask = std::move(mask);
			m_Graph.SetLayers(std::move(updated));
			MarkDirty();
			m_LayerMaskTextKey.clear();
		};

		const float presetGap = theme.PadSmall;
		const float presetW = std::max(50.0f, (controlW - presetGap * 2.0f) / 3.0f);
		const Wui::WuiRect upperButton { controlX, y, presetW, kRowH };
		const Wui::WuiRect fullButton { upperButton.X + upperButton.W + presetGap, y, presetW, kRowH };
		const Wui::WuiRect clearButton { fullButton.X + fullButton.W + presetGap, y,
			std::max(50.0f, controlX + controlW - (fullButton.X + fullButton.W + presetGap)), kRowH };
		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.layer.mask.upper"), upperButton,
			AGTr("layer.mask.upper", "上半身"), theme, !modelBones.empty(),
			AGTr("layer.mask.upper.tooltip", "勾选 Spine / Chest / Neck / Head / Shoulder / UpperArm / LowerArm / Hand 等上半身骨骼。"), false))
		{
			static const char* kUpperTokens[] = { "Spine", "Chest", "Neck", "Head", "Shoulder",
				"UpperArm", "LowerArm", "Hand" };
			std::vector<BoneMaskEntry> mask;
			for (const std::string& bone : boneNames)
			{
				for (const char* token : kUpperTokens)
				{
					if (ContainsTokenNoCase(bone, token))
					{
						mask.push_back(BoneMaskEntry { bone, 1.0f });
						break;
					}
				}
			}
			setMask(std::move(mask));
		}
		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.layer.mask.full"), fullButton,
			AGTr("layer.mask.full", "全身"), theme, !modelBones.empty(),
			AGTr("layer.mask.full.tooltip", "把模型里的全部具名骨骼加入遮罩（权重 1）。"), false))
		{
			std::vector<BoneMaskEntry> mask;
			for (const std::string& bone : modelBones)
				mask.push_back(BoneMaskEntry { bone, 1.0f });
			setMask(std::move(mask));
		}
		if (Wui::ActionButton(ctx, Wui::HashId("animgraph.layer.mask.clear"), clearButton,
			AGTr("layer.mask.clear", "清空"), theme, true,
			AGTr("layer.mask.clear.tooltip", "清空遮罩；所有骨骼都不再参与这层叠加。"), true))
		{
			setMask({});
		}
		y += kRowH + 2.0f;

		layer = findLayer();
		if (layer == nullptr)
			return;
		{
			std::vector<std::string> addOptions { AGTr("layer.mask.add", "＋ 添加骨骼") };
			std::vector<std::string> unlisted;
			for (const std::string& bone : boneNames)
			{
				const bool present = std::any_of(layer->BoneMask.begin(), layer->BoneMask.end(),
					[&bone](const BoneMaskEntry& entry) { return entry.BoneName == bone; });
				if (!present)
				{
					unlisted.push_back(bone);
					addOptions.push_back(bone);
				}
			}
			int addIndex = 0;
			if (Wui::Combo(ctx, Wui::HashId("animgraph.layer.mask.add"), { controlX, y, controlW, kRowH }, "",
				addOptions, addIndex, theme, AGTr("layer.mask.add", "＋ 添加骨骼")))
			{
				if (addIndex > 0 && static_cast<size_t>(addIndex - 1) < unlisted.size())
				{
					std::vector<BoneMaskEntry> mask = layer->BoneMask;
					mask.push_back(BoneMaskEntry { unlisted[static_cast<size_t>(addIndex - 1)], 1.0f });
					setMask(std::move(mask));
				}
			}
			Wui::Tooltip(ctx, { controlX, y, controlW, kRowH },
				AGTr("layer.mask.add.tooltip", "把尚未列出的骨骼加入遮罩（默认权重 1）。"));
		}
		y += kRowH + 2.0f;

		const Wui::WuiRect maskList { controlX, y, controlW, 180.0f };
		Wui::PanelBackground(ctx, maskList, theme.ContentBg, theme.Radius);
		if (boneNames.size() == 0)
		{
			Wui::EmptyState(ctx, maskList, "", AGTr("layer.mask.empty.title", "没有可用骨骼"),
				AGTr("layer.mask.empty.body", "选择带模型的角色，或使用高级文本手动输入骨骼名。"), "", 0, theme);
		}
		else
		{
			const float maskContentH = std::max(maskList.H, static_cast<float>(boneNames.size()) * 28.0f);
			if (Wui::BeginScrollArea(ctx, maskList, maskContentH, m_LayerMaskScroll, theme,
				Wui::HashId("animgraph.layer.mask.list")))
			{
				float rowY = maskList.Y + 2.0f - m_LayerMaskScroll;
				for (const std::string& bone : boneNames)
				{
					const Wui::WuiRect row { maskList.X + 2.0f, rowY, maskList.W - 10.0f, 24.0f };
					const AnimLayer* liveLayer = findLayer();
					if (liveLayer == nullptr)
						break;
					const auto found = std::find_if(liveLayer->BoneMask.begin(), liveLayer->BoneMask.end(),
						[&bone](const BoneMaskEntry& entry) { return entry.BoneName == bone; });
					bool participating = found != liveLayer->BoneMask.end();
					const bool inModel = std::find(modelBones.begin(), modelBones.end(), bone) != modelBones.end();
					const std::string label = inModel ? bone : (bone + "  " + AGTr("layer.mask.missing_bone", "（模型中没有此骨骼）"));
					const float dragW = 88.0f;
					const Wui::WuiRect checkRect { row.X + 2.0f, row.Y, std::max(80.0f, row.W - dragW - 6.0f), row.H };
					if (Wui::Checkbox(ctx, Wui::HashId(("animgraph.layer.mask.check." + bone).c_str()),
						checkRect, label, participating, theme))
					{
						std::vector<AnimLayer> updated = m_Graph.GetLayers();
						for (AnimLayer& candidate : updated)
						{
							if (candidate.Name != m_SelectedLayer)
								continue;
							candidate.BoneMask.erase(std::remove_if(candidate.BoneMask.begin(), candidate.BoneMask.end(),
								[&bone](const BoneMaskEntry& entry) { return entry.BoneName == bone; }), candidate.BoneMask.end());
							if (participating)
								candidate.BoneMask.push_back(BoneMaskEntry { bone, 1.0f });
						}
						PushUndo();
						m_Graph.SetLayers(std::move(updated));
						MarkDirty();
						m_LayerMaskTextKey.clear();
					}
					Wui::Tooltip(ctx, checkRect, AGTr("layer.mask.check.tooltip",
						"勾选表示这根骨骼参与本层叠加；取消勾选即从遮罩移除。"));

					const Wui::WuiRect weightRect { row.X + row.W - dragW, row.Y + 2.0f, dragW - 2.0f, 20.0f };
					if (participating)
					{
						float weight = found != liveLayer->BoneMask.end() ? found->Weight : 1.0f;
						const float previous = weight;
						const Wui::WuiId weightId = Wui::HashId(("animgraph.layer.mask.weight." + bone).c_str());
						if (Wui::DragFloat(ctx, weightId, weightRect, weight, 0.01f, 0.0f, 1.0f, theme)
							&& std::fabs(weight - previous) > 0.0001f)
						{
							weight = std::clamp(std::round(weight / 0.05f) * 0.05f, 0.0f, 1.0f);
							const bool continuous = ctx.Input().MouseDown[0]
								&& (ctx.IsHovered(weightRect) || ctx.Focus() == weightId);
							BeginContinuousEdit("layer.mask." + bone, continuous);
							std::vector<AnimLayer> updated = m_Graph.GetLayers();
							for (AnimLayer& candidate : updated)
								if (candidate.Name == m_SelectedLayer)
									for (BoneMaskEntry& entry : candidate.BoneMask)
										if (entry.BoneName == bone)
											entry.Weight = weight;
							m_Graph.SetLayers(std::move(updated));
							MarkDirty();
							m_LayerMaskTextKey.clear();
						}
						Wui::Tooltip(ctx, weightRect, AGTr("layer.mask.weight.tooltip",
							"本骨在本层的权重（0..1，步进 0.05）。0 = 不参与，1 = 完全叠加。"));
					}
					else
					{
						Wui::PanelBackground(ctx, weightRect, theme.TrackBg, 3.0f);
						Wui::Label(ctx, { weightRect.X + 34.0f, weightRect.Y + 2.0f }, "—", theme.TextDisabled, theme.FontSizeCaption);
						Wui::Tooltip(ctx, weightRect, AGTr("layer.mask.weight.disabled.tooltip",
							"先勾选这根骨骼，才能编辑它的遮罩权重。"));
					}
					rowY += 28.0f;
				}
				Wui::EndScrollArea(ctx);
			}
		}
		y += maskList.H + theme.PadSmall;

		layer = findLayer();
		if (layer == nullptr)
			return;
		const Wui::WuiRect advancedHeader { area.X + theme.Pad, y, area.W - theme.Pad * 2.0f, kRowH };
		Wui::CollapsibleHeader(ctx, Wui::HashId("animgraph.layer.mask.advanced"), advancedHeader,
			AGTr("layer.mask.advanced", "高级：文本"), m_LayerMaskAdvancedOpen, theme, std::string(), std::string(),
			AGTr("layer.mask.advanced.tooltip", "以 'Bone 0.0..1.0' 逗号分隔的文本编辑遮罩，与上方骨骼行双向同步。"),
			theme.FontSizeSmall);
		y += kRowH + 2.0f;
		if (m_LayerMaskAdvancedOpen)
		{
			const std::string serialized = SerializeBoneMask(layer->BoneMask);
			const std::string key = m_SelectedLayer + "|" + serialized;
			if (m_LayerMaskTextKey != key)
			{
				m_LayerMaskTextKey = key;
				m_LayerMaskTextBuffer = serialized;
			}
			Wui::TextFieldA11y maskA11y;
			maskA11y.Label = AGTr("layer.mask.a11y", "Bone mask");
			maskA11y.Placeholder = AGTr("layer.mask.advanced.placeholder", "Spine 0.25, Chest 1.0, Head 1.0");
			const bool submitted = Wui::TextField(ctx, Wui::HashId("animgraph.layer.mask.text"),
				{ controlX, y, controlW, kRowH }, m_LayerMaskTextBuffer, theme, nullptr, &maskA11y);
			if (submitted)
			{
				std::vector<BoneMaskEntry> mask = ParseBoneMaskText(m_LayerMaskTextBuffer);
				setMask(std::move(mask));
				m_LayerMaskTextKey.clear();
			}
			Wui::Tooltip(ctx, { controlX, y, controlW, kRowH },
				AGTr("layer.mask.tooltip", "Bone weights as 'Bone 0.0..1.0' separated by commas. Bones not listed get weight 0."));
			y += kRowH + 4.0f;
		}

		const std::string maskExplain = AGTr("layer.explain.mask",
			"Mask: only listed bones blend; unlisted bones get weight 0.");
		Wui::Label(ctx, { controlX, y }, Wui::EllipsizeMiddleToWidth(ctx, maskExplain, controlW, theme.FontSizeCaption),
			theme.TextMuted, theme.FontSizeCaption);
		Wui::Tooltip(ctx, { controlX, y, controlW, 14.0f }, maskExplain);
	}
	// 右栏:Live Preview
	// ---------------------------------------------------------------------------

	void AnimGraphPanel::RenderLivePanel(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		Wui::PanelBackground(ctx, area, theme.CardBg, theme.Radius);

		const AnimGraphComponent* component = nullptr;
		if (m_BoundEntity != entt::null && m_BoundScene != nullptr)
		{
			const auto& registry = static_cast<const Scene&>(*m_BoundScene).GetRegistry();
			component = registry.try_get<AnimGraphComponent>(m_BoundEntity);
		}

		Wui::SectionHeader(ctx, { area.X + theme.Pad, area.Y + 2.0f, area.W - theme.Pad * 2.0f, kRowH },
			AGTr("live.title", "Live Preview"), theme.Text, theme, theme.FontSizeTitle);
		const Wui::WuiRect helpRect { area.X + area.W - theme.Pad - 18.0f, area.Y + 5.0f, 16.0f, 16.0f };
		Wui::PanelBackground(ctx, helpRect, theme.ButtonBg, 8.0f);
		Wui::Label(ctx, { helpRect.X + 5.0f, helpRect.Y + 1.0f }, "?", theme.TextMuted, theme.FontSizeCaption);
		Wui::Tooltip(ctx, helpRect,
			AGTr("live.help.tooltip", "State = the AnimGraph state currently playing.\nIdle (no transition) = no CrossFade is active.\nState time = seconds played in the current state.\nRuntime = Speed / Direction currently fed to the graph."));

		const Wui::WuiRect simulateRect { helpRect.X - 86.0f, area.Y + 2.0f, 78.0f, kRowH };
		if (Wui::Checkbox(ctx, Wui::HashId("animgraph.live.simulate"), simulateRect,
			AGTr("live.simulate", "模拟"), m_Simulate, theme))
		{
			if (m_Simulate)
			{
				ResetSimulation();
				EnsureSimulationGraph();
			}
		}
		Wui::Tooltip(ctx, simulateRect, AGTr("live.simulate.tooltip",
			"开启后用内部动画图副本自动循环待机、行走、奔跑、侧移、后退与跳跃/下落；绑定实体时仍同步推送参数。"));

		const float innerX = area.X + theme.Pad;
		const float innerW = area.W - theme.Pad * 2.0f;
		float y = area.Y + kRowH + 6.0f;

		// ---- 绑定实体 ----
		if (m_BoundEntity == entt::null)
		{
			Wui::Label(ctx, { innerX, y }, AGTr("live.no_entity", "No entity selected"), theme.TextMuted,
				theme.FontSizeSmall);
			Wui::Tooltip(ctx, { innerX, y, innerW, 18.0f },
				AGTr("live.bind.tooltip", "Binding follows the editor selection; it never overwrites the asset."));
			y += 18.0f;
			Wui::Label(ctx, { innerX, y }, AGTr("live.select_hint", "Select a character in the scene to drive these parameters live."),
				theme.TextMuted, theme.FontSizeCaption);
			Wui::Tooltip(ctx, { innerX, y, innerW, 16.0f },
				AGTr("live.select_hint", "Select a character in the scene to drive these parameters live."));
			y += 18.0f;
		}
		else
		{
			const std::string bound = m_BoundName + (component ? "" : AGTr("live.no_component", "  (no AnimGraphComponent)"));
			Wui::Label(ctx, { innerX, y }, Wui::EllipsizeMiddleToWidth(ctx, bound, innerW, theme.FontSizeSmall),
				component ? theme.Text : theme.Warning, theme.FontSizeSmall);
			Wui::Tooltip(ctx, { innerX, y, innerW, 18.0f },
				component
					? AGTr("live.params.tooltip", "Parameters below call AnimGraphSystem::SetParameter every frame for this entity.")
					: AGTr("live.apply_first", "Apply this graph first: the entity has no AnimGraphComponent yet."));
			y += 20.0f;
		}

		// ---- 运行时读数(先取一次,布局与绘制共用) ----
		std::string stateLine = m_Simulate
			? AGTr("live.sim.state", "State: simulated preview")
			: (m_BoundEntity == entt::null
				? AGTr("live.state.no_entity", "State: not selected")
				: AGTr("live.state.none", "State: -"));
		std::string progressLabel = AGTr("live.idle", "Idle (no transition)");
		std::string runtimeError;
		std::string runtimeParams;
		float progress = 0.0f;
		float stateTime = 0.0f;
		if (m_Simulate && m_SimGraphReady)
		{
			const std::string current = m_SimGraph.GetCurrentStateName();
			stateLine = AGTr("live.state.prefix", "State: ")
				+ (current.empty() ? AGTr("live.state.none_name", "(none)") : current);
			if (m_SimGraph.IsInTransition())
			{
				const std::string target = m_SimGraph.GetTransitionTargetState();
				if (!target.empty())
				{
					stateLine += "  ->  " + target;
					progress = std::clamp(m_SimGraph.GetTransitionProgress(), 0.0f, 1.0f);
					progressLabel = AGFmt("live.crossfade", "Cross-fade {percent}%",
						{ { "percent", FormatFloat(progress * 100.0f, 0) } });
				}
			}
			stateTime = m_SimGraph.GetStateTime();
			runtimeParams = AGFmt("live.runtime", "Runtime: Speed {speed} m/s   Direction {direction} deg",
				{ { "speed", FormatFloat(m_ParamSpeed, 2) }, { "direction", FormatFloat(m_ParamDirection, 0) } });
		}
		else
		{
#if WE_HAS_ANIMGRAPH_SYSTEM
			if (m_BoundEntity != entt::null)
			{
				AnimGraphRuntimeInfo info;
				if (AnimGraphSystem::GetRuntimeInfo(m_BoundEntity, info))
				{
					stateLine = AGTr("live.state.prefix", "State: ") + (info.CurrentState.empty() ? AGTr("live.state.none_name", "(none)") : info.CurrentState);
					if (info.InTransition && !info.TargetState.empty())
					{
						stateLine += "  ->  " + info.TargetState;
						progress = std::clamp(info.TransitionProgress, 0.0f, 1.0f);
						progressLabel = AGFmt("live.crossfade", "Cross-fade {percent}%",
							{ { "percent", FormatFloat(progress * 100.0f, 0) } });
					}
					stateTime = info.StateTime;
					runtimeError = info.Error;
					runtimeParams = AGFmt("live.runtime", "Runtime: Speed {speed} m/s   Direction {direction} deg",
						{ { "speed", FormatFloat(info.Speed, 2) }, { "direction", FormatFloat(info.Direction, 0) } });
				}
				else
				{
					stateLine = AGTr("live.state.not_updated", "State: runtime not updated yet");
				}
			}
#else
			stateLine = AGTr("live.state.unavailable", "State: animation runtime unavailable");
			runtimeError = AGTr("live.error.no_system", "AnimGraphSystem is not part of this build yet.");
#endif
		}

		auto drawReadout = [&](const Wui::WuiRect& readout, bool compact)
		{
			Wui::PanelBackground(ctx, readout, theme.ContentBg, theme.Radius);
			float ry = readout.Y + 4.0f;
			Wui::Label(ctx, { readout.X + theme.PadSmall, ry },
				Wui::EllipsizeMiddleToWidth(ctx, stateLine, readout.W - theme.PadSmall * 2.0f, theme.FontSizeSmall),
				theme.Text, theme.FontSizeSmall);
			ry += 16.0f;

			const Wui::WuiRect track { readout.X + theme.PadSmall, ry + 2.0f,
				readout.W - theme.PadSmall * 2.0f, 6.0f };
			Wui::PanelBackground(ctx, track, theme.TrackBg, 3.0f);
			if (progress > 0.0f)
				Wui::PanelBackground(ctx, { track.X, track.Y, track.W * progress, track.H }, theme.Accent, 3.0f);
			ry += 10.0f;

			if (compact)
			{
				Wui::Label(ctx, { readout.X + theme.PadSmall, ry },
					progressLabel + "   " + FormatFloat(stateTime, 2) + " s", theme.TextMuted,
					theme.FontSizeCaption);
			}
			else
			{
				Wui::Label(ctx, { readout.X + theme.PadSmall, ry + 2.0f }, progressLabel, theme.TextMuted,
					theme.FontSizeCaption);
				ry += 16.0f;
				Wui::Label(ctx, { readout.X + theme.PadSmall, ry }, AGFmt("live.state_time", "State time {time} s",
					{ { "time", FormatFloat(stateTime, 2) } }),
					theme.TextMuted, theme.FontSizeCaption);
				if (!runtimeParams.empty())
				{
					ry += 14.0f;
					Wui::Label(ctx, { readout.X + theme.PadSmall, ry }, runtimeParams, theme.TextDisabled,
						theme.FontSizeCaption);
				}
			}
			Wui::Tooltip(ctx, readout,
				AGTr("live.readout.tooltip", "State: current AnimGraph state; an arrow shows the CrossFade target.\nIdle (no transition): no CrossFade is active.\nState time: seconds played in the current state.\nRuntime: Speed / Direction values fed to the graph."));
		};

		struct ParamRow
		{
			std::string Label;
			float* Value;
			float Min;
			float Max;
			const char* Unit;
			std::string Tooltip;
		};
		ParamRow params[] = {
			{ AGTr("params.speed", "Speed"), &m_ParamSpeed, 0.0f, 10.0f, " m/s",
				AGTr("params.speed.tooltip", "Horizontal speed in m/s. Drives the 1D/2D blend spaces.") },
			{ AGTr("params.direction", "Direction"), &m_ParamDirection, -180.0f, 180.0f, " deg",
				AGTr("params.direction.tooltip", "Facing relative to movement in degrees (-180..180).") },
			{ AGTr("params.is_falling", "IsFalling"), &m_ParamFalling, 0.0f, 1.0f, "",
				AGTr("params.is_falling.tooltip", "0 = grounded, 1 = airborne. Drives the Fall transition.") },
			{ AGTr("params.aim_weight", "AimWeight"), &m_ParamAimWeight, 0.0f, 1.0f, "",
				AGTr("params.aim_weight.tooltip", "Upper-body aim layer weight (0..1).") },
		};
		auto drawParam = [&](const ParamRow& param, const Wui::WuiRect& row, float labelW)
		{
			Wui::Label(ctx, { row.X, row.Y + 4.0f }, param.Label, theme.TextMuted, theme.FontSizeCaption);
			const Wui::WuiRect slider { row.X + labelW, row.Y, std::max(40.0f, row.W - labelW), kRowH };
			Wui::WuiNumberStyle style;
			style.Unit = param.Unit;
			style.Decimals = 2;
			style.ValueWidth = 58.0f;
			Wui::DragBarFloat(ctx, Wui::HashId((std::string("animgraph.param.") + param.Label).c_str()),
				slider, *param.Value, param.Min, param.Max, theme, style);
			Wui::Tooltip(ctx, slider, param.Tooltip);
		};

		auto drawToggles = [&](float tx, float ty, float totalW, bool horizontal)
		{
			if (component == nullptr || m_BoundScene == nullptr)
				return;
			Entity boundEntity(m_BoundScene, m_BoundEntity);
			AnimGraphComponent* writable = static_cast<AnimGraphComponent*>(
				boundEntity.GetComponent(entt::type_id<AnimGraphComponent>().hash()));
			if (writable == nullptr)
				return;
			bool rootMotion = component->EnableRootMotion != 0;
			bool footIk = component->EnableFootIk != 0;
			bool upperBody = component->EnableUpperBodyLayer != 0;
			const std::string labels[3] = { AGTr("toggle.root_motion", "Root Motion"), AGTr("toggle.foot_ik", "Foot IK"),
				horizontal ? AGTr("toggle.upper_body", "Upper Body") : AGTr("toggle.upper_body_layer", "Upper Body Layer") };
			bool* values[3] = { &rootMotion, &footIk, &upperBody };
			const float itemW = horizontal ? (totalW - theme.PadSmall * 2.0f) / 3.0f : totalW;
			for (int i = 0; i < 3; ++i)
			{
				const Wui::WuiRect r { tx + (horizontal ? (itemW + theme.PadSmall) * static_cast<float>(i) : 0.0f),
					ty + (horizontal ? 0.0f : (kRowH + 2.0f) * static_cast<float>(i)), itemW, kRowH };
				if (Wui::Checkbox(ctx, Wui::HashId((std::string("animgraph.toggle.") + labels[i]).c_str()),
					r, labels[i], *values[i], theme))
					host.MarkDocumentDirty();
				Wui::Tooltip(ctx, r, i == 0
					? AGTr("toggle.root_motion.tooltip", "Write root motion into the entity transform (AnimGraphComponent.EnableRootMotion).")
					: (i == 1 ? AGTr("toggle.foot_ik.tooltip", "Align both feet to the terrain surface (AnimGraphComponent.EnableFootIk).")
						: AGTr("toggle.upper_body.tooltip", "Enable additive upper-body layers (AnimGraphComponent.EnableUpperBodyLayer).")));
			}
			writable->EnableRootMotion = rootMotion ? 1 : 0;
			writable->EnableFootIk = footIk ? 1 : 0;
			writable->EnableUpperBodyLayer = upperBody ? 1 : 0;
		};

		// 错误信息先算出来:窄条布局要按"剩余高度"决定画到哪一行,错误本身必须占位,
		// 这样任何高度下都不会出现文字压住控件。
		std::string errorText = runtimeError;
		if (component != nullptr && !m_LoadedPath.empty() && component->AnimGraphAsset.HasPath())
		{
			const std::string boundPath = StringPool::Get().PathOf(component->AnimGraphAsset.Path);
			if (boundPath != m_LoadedPath)
				errorText += (errorText.empty() ? "" : "\n")
					+ AGFmt("live.bound_differs", "Bound asset differs: {path} (press Apply)",
						{ { "path", boundPath } });
		}
		std::vector<std::string> errorLines;
		if (!errorText.empty())
			errorLines = Wui::WrapTextLines(errorText, theme.FontSizeCaption, innerW, Wui::WuiFontFamily::Ui);
		const size_t drawnErrorLines = std::min<size_t>(errorLines.size(), 3);
		const float errorReserve = drawnErrorLines == 0
			? 0.0f
			: static_cast<float>(drawnErrorLines) * (theme.FontSizeCaption + 1.0f) + 2.0f;
		const float bottomLimit = area.Y + area.H - errorReserve;

		const bool compact = area.H < 380.0f;
		if (compact)
		{
			// 窄窗口底部条:2×2 滑杆 → 读数(有空间时)→ 单行开关;每一项都先检查
			// bottomLimit,放不下就跳过,绝不让控件越出卡片。
			const float colW = (innerW - theme.PadSmall) * 0.5f;
			for (int i = 0; i < 4; ++i)
			{
				const Wui::WuiRect cell { innerX + (i % 2) * (colW + theme.PadSmall),
					y + (i / 2) * (kRowH + 2.0f), colW, kRowH };
				if (cell.Y + cell.H > bottomLimit)
					break;
				drawParam(params[i], cell, 62.0f);
			}
			y += (kRowH + 2.0f) * 2.0f + 2.0f;

			if (y + 50.0f <= bottomLimit)
			{
				const Wui::WuiRect readout { innerX, y, innerW, 50.0f };
				drawReadout(readout, true);
				y += readout.H + 4.0f;
			}
			else if (y + 16.0f <= bottomLimit)
			{
				const Wui::WuiRect compactState { innerX, y, innerW, 16.0f };
				Wui::Label(ctx, { innerX, y },
					Wui::EllipsizeMiddleToWidth(ctx, stateLine + "   " + FormatFloat(stateTime, 2) + " s",
						innerW, theme.FontSizeCaption),
					theme.TextMuted, theme.FontSizeCaption);
				Wui::Tooltip(ctx, compactState,
					AGTr("live.readout.tooltip", "State: current AnimGraph state; an arrow shows the CrossFade target.\nIdle (no transition): no CrossFade is active.\nState time: seconds played in the current state.\nRuntime: Speed / Direction values fed to the graph."));
				y += 16.0f;
			}

			if (y + kRowH <= bottomLimit)
			{
				drawToggles(innerX, y, innerW, true);
				y += kRowH;
			}
		}
		else
		{
			for (const ParamRow& param : params)
			{
				const Wui::WuiRect row { innerX, y, innerW, kRowH };
				drawParam(param, row, 72.0f);
				y += kRowH + 4.0f;
			}
			y += 2.0f;

			const Wui::WuiRect readout { innerX, y, innerW, 90.0f };
			drawReadout(readout, false);
			y += readout.H + kGap;

			drawToggles(innerX, y, innerW, false);
			y += (kRowH + 2.0f) * 3.0f;
		}

		// 错误画在预留区里(红色多行,最多 3 行)。
		if (drawnErrorLines > 0)
		{
			float lineY = bottomLimit + 2.0f;
			for (size_t i = 0; i < drawnErrorLines; ++i)
			{
				Wui::Label(ctx, { innerX, lineY }, errorLines[i], theme.Danger, theme.FontSizeCaption);
				lineY += theme.FontSizeCaption + 1.0f;
			}
		}
	}
	void AnimGraphPanel::RenderStatusBar(Wui::WuiContext& ctx, const Wui::WuiRect& bar, const Wui::WuiTheme& theme)
	{
		Wui::BarSurface(ctx, bar, theme.PanelHeader, theme.Border);

		const AnimState* state = SelectedState();
		size_t samples = 0;
		if (state)
			samples = state->Kind == AnimStateKind::BlendSpace1D ? state->Blend1D.GetSampleCount()
				: (state->Kind == AnimStateKind::BlendSpace2D ? state->Blend2D.GetSampleCount() : 0u);
		const size_t triangles = state && state->Kind == AnimStateKind::BlendSpace2D ? m_Triangles.size() : 0u;

		const std::string left = AGFmt("statusbar.summary",
			"Samples {samples}   Triangles {triangles}   Parameters {params}   Layers {layers}",
			{ { "samples", std::to_string(samples) }, { "triangles", std::to_string(triangles) },
			  { "params", std::to_string(m_Graph.GetParameterDefaults().size()) },
			  { "layers", std::to_string(m_Graph.GetLayers().size()) } });
		Wui::Label(ctx, { bar.X + theme.Pad, bar.Y + 4.0f }, left, theme.TextMuted, theme.FontSizeCaption);

		if (!m_Status.empty())
		{
			const Wui::WuiColor color = m_StatusError ? theme.Danger : theme.Success;
			const float statusW = ctx.MeasureTextWidth(m_Status, theme.FontSizeCaption);
			const float statusX = bar.X + bar.W * 0.5f - statusW * 0.5f;
			Wui::Label(ctx, { std::max(bar.X + 320.0f, statusX), bar.Y + 4.0f },
				Wui::EllipsizeMiddleToWidth(ctx, m_Status, bar.W * 0.4f, theme.FontSizeCaption), color,
				theme.FontSizeCaption);
		}

		if (!m_LoadedPath.empty())
		{
			const float pathW = bar.W * 0.28f;
			const std::string path = Wui::EllipsizeMiddleToWidth(ctx, m_LoadedPath, pathW, theme.FontSizeCaption);
			const float x = bar.X + bar.W - theme.Pad - ctx.MeasureTextWidth(path, theme.FontSizeCaption);
			Wui::Label(ctx, { x, bar.Y + 4.0f }, path, theme.TextDisabled, theme.FontSizeCaption);
			Wui::Tooltip(ctx, { x, bar.Y, pathW, bar.H }, m_LoadedPath);
		}
	}
}




