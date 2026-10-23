#include "WUI/Panels/AnimGraphPanel.h"

#include "World/Command/CommandHistory.h"
#include "World/Core/StringPool.h"
#include "World/Scene/Components.h"
#include "World/Scene/Systems/TransformSystem.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace World
{
	namespace
	{
		constexpr float kIkRowH = 24.0f;
		constexpr float kIkGap = 8.0f;
		constexpr float kIkChainH = 150.0f;

		std::string IKTr(const char* key, const char* fallback)
		{
			return Wui::Tr(std::string("panel.animgraph.") + key, fallback);
		}

		std::string IKFmt(const char* key, const char* fallback,
			std::initializer_list<std::pair<std::string_view, std::string_view>> args = {})
		{
			return Wui::TrFormat(std::string("panel.animgraph.") + key, fallback,
				std::vector<std::pair<std::string_view, std::string_view>>(args));
		}

		std::string EntityName(const entt::registry& registry, entt::entity entity)
		{
			const TagComponent* tag = registry.try_get<TagComponent>(entity);
			return tag == nullptr ? std::string() : StringPool::Get().NameOf(tag->Tag);
		}

		glm::mat4 EntityWorld(const entt::registry& registry, entt::entity entity)
		{
			if (const WorldTransformComponent* world = registry.try_get<WorldTransformComponent>(entity))
				return world->Matrix;
			if (const TransformComponent* transform = registry.try_get<TransformComponent>(entity))
				return TransformSystem::Compose(transform->Location, transform->Rotation, transform->Scale);
			return glm::mat4(1.0f);
		}

		struct BoneChoice
		{
			std::string Name;
			int NodeIndex = -1;
		};

		// 骨骼 = skin.JointNodes 定义的关节；没有 skin 时才退回“非 mesh 节点”启发式。
		std::vector<BoneChoice> CollectBones(const Asset::WModelData& model)
		{
			std::vector<int> indices;
			if (!model.Skins.empty())
			{
				for (const Asset::WModelSkin& skin : model.Skins)
					for (uint32_t node : skin.JointNodes)
						if (node < model.Nodes.size())
							indices.push_back(static_cast<int>(node));
			}
			else
			{
				for (size_t i = 0; i < model.Nodes.size(); ++i)
					if (model.Nodes[i].MeshIndex < 0 && !model.Nodes[i].Name.empty())
						indices.push_back(static_cast<int>(i));
			}
			if (indices.empty())
			{
				for (size_t i = 0; i < model.Nodes.size(); ++i)
					if (!model.Nodes[i].Name.empty())
						indices.push_back(static_cast<int>(i));
			}

			std::sort(indices.begin(), indices.end());
			indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

			std::vector<BoneChoice> bones;
			std::vector<std::string> seen;
			for (int nodeIndex : indices)
			{
				const std::string& name = model.Nodes[static_cast<size_t>(nodeIndex)].Name;
				if (name.empty() || std::find(seen.begin(), seen.end(), name) != seen.end())
					continue;
				seen.push_back(name);
				bones.push_back(BoneChoice { name, nodeIndex });
			}
			return bones;
		}

		int FindBoneIndex(const std::vector<BoneChoice>& bones, const std::string& name)
		{
			for (size_t i = 0; i < bones.size(); ++i)
				if (bones[i].Name == name)
					return static_cast<int>(i);
			return -1;
		}

		int FindFirstBone(const std::vector<BoneChoice>& bones, std::initializer_list<const char*> candidates)
		{
			for (const char* candidate : candidates)
			{
				const int index = FindBoneIndex(bones, candidate);
				if (index >= 0)
					return index;
			}
			return -1;
		}

		glm::mat4 NodeRestMatrix(const Asset::WModelData& model, int nodeIndex)
		{
			if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= model.Nodes.size())
				return glm::mat4(1.0f);

			std::vector<int> chain;
			int cursor = nodeIndex;
			for (int depth = 0; cursor >= 0 && static_cast<size_t>(cursor) < model.Nodes.size() && depth < 128; ++depth)
			{
				chain.push_back(cursor);
				cursor = model.Nodes[static_cast<size_t>(cursor)].Parent;
			}

			glm::mat4 result(1.0f);
			for (auto it = chain.rbegin(); it != chain.rend(); ++it)
			{
				const Asset::WModelNode& node = model.Nodes[static_cast<size_t>(*it)];
				result *= TransformSystem::Compose(node.Translation, node.Rotation, node.Scale);
			}
			return result;
		}

		struct EntityChoice
		{
			entt::entity Handle = entt::null;
			UUID Id { 0 };
			std::string Label;
		};

		std::vector<EntityChoice> CollectEntities(const entt::registry& registry, entt::entity exclude)
		{
			std::vector<EntityChoice> choices;
			for (const entt::entity entity : registry.view<UUIDComponent>())
			{
				if (entity == exclude)
					continue;
				const UUIDComponent* identity = registry.try_get<UUIDComponent>(entity);
				if (identity == nullptr)
					continue;
				const std::string name = EntityName(registry, entity);
				EntityChoice choice;
				choice.Handle = entity;
				choice.Id = identity->ID;
				choice.Label = (name.empty() ? std::string("Entity") : name)
					+ "  #" + std::to_string(static_cast<uint64_t>(identity->ID));
				choices.push_back(std::move(choice));
			}
			std::sort(choices.begin(), choices.end(),
				[](const EntityChoice& a, const EntityChoice& b) { return a.Label < b.Label; });
			return choices;
		}

		bool IsFiniteVec(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}
	}

	void AnimGraphPanel::RenderIkCard(Wui::WuiContext& ctx, const Wui::WuiRect& area, PanelHost& host,
		const Wui::WuiTheme& theme)
	{
		Wui::PanelBackground(ctx, area, theme.CardBg, theme.Radius);
		Wui::SectionHeader(ctx, { area.X + theme.Pad, area.Y + 2.0f, area.W - theme.Pad * 2.0f, kIkRowH },
			IKTr("ik.title", "IK (inverse kinematics)"), theme.Text, theme, theme.FontSizeTitle);

		const Wui::WuiRect helpRect { area.X + area.W - theme.Pad - 18.0f, area.Y + 5.0f, 16.0f, 16.0f };
		Wui::PanelBackground(ctx, helpRect, theme.ButtonBg, 8.0f);
		Wui::Label(ctx, { helpRect.X + 5.0f, helpRect.Y + 1.0f }, "?", theme.TextMuted, theme.FontSizeCaption);
		Wui::Tooltip(ctx, helpRect, IKTr("ik.help.tooltip",
			"IK solves rotations from a target position. Pick any three-bone chain (root -> mid -> end), then move the target entity.\n"
			"Foot IK is only an example driver on top of this generic solver."));

		if (m_BoundEntity == entt::null || m_BoundScene == nullptr)
		{
			Wui::EmptyState(ctx, { area.X + theme.Pad, area.Y + 28.0f, area.W - theme.Pad * 2.0f, area.H - 34.0f },
				"", IKTr("ik.no_character.title", "Select a character"),
				IKTr("ik.no_character.body", "Select an entity with a SkinnedMeshRenderer in the scene, then create IK chains for its bones."),
				"", 0, theme);
			return;
		}
		if (!m_ModelValid)
		{
			Wui::EmptyState(ctx, { area.X + theme.Pad, area.Y + 28.0f, area.W - theme.Pad * 2.0f, area.H - 34.0f },
				"", IKTr("ik.no_model.title", "No skeleton available"),
				IKTr("ik.no_model.body", "The selected entity has no readable skinned model. Add a SkinnedMeshRenderer with a .wmodel asset."),
				"", 0, theme);
			return;
		}

		Scene& scene = *m_BoundScene;
		const bool runtime = scene.IsActive();
		const auto& constRegistry = static_cast<const Scene&>(scene).GetRegistry();
		const std::vector<BoneChoice> bones = CollectBones(m_Model);

		const UUIDComponent* charIdentity = constRegistry.try_get<UUIDComponent>(m_BoundEntity);
		const uint64_t characterUuid = charIdentity ? static_cast<uint64_t>(charIdentity->ID) : 0;
		uint32_t layerMask = 1;
		if (const LevelEntityComponent* level = constRegistry.try_get<LevelEntityComponent>(m_BoundEntity))
			layerMask = level->DataLayerMask;

		std::vector<entt::entity> chainHandles;
		for (const entt::entity entity : constRegistry.view<TwoBoneIkComponent>())
		{
			const TwoBoneIkComponent* chain = constRegistry.try_get<TwoBoneIkComponent>(entity);
			if (chain == nullptr)
				continue;
			const bool owned = chain->OwnerUuid != 0
				? chain->OwnerUuid == characterUuid
				: entity == m_BoundEntity;
			if (owned)
				chainHandles.push_back(entity);
		}
		std::sort(chainHandles.begin(), chainHandles.end(),
			[&constRegistry](entt::entity a, entt::entity b)
			{
				return EntityName(constRegistry, a) < EntityName(constRegistry, b);
			});

		const std::vector<EntityChoice> entities = CollectEntities(constRegistry, m_BoundEntity);
		const int armRoot = FindFirstBone(bones, { "UpperArm.R", "UpperArm_R", "RightArm", "UpperArm.L", "UpperArm_L" });
		const int armMid = FindFirstBone(bones, { "LowerArm.R", "LowerArm_R", "RightForeArm", "LowerArm.L", "LowerArm_L" });
		const int armEnd = FindFirstBone(bones, { "Hand.R", "Hand_R", "RightHand", "Hand.L", "Hand_L" });
		const int legRoot = FindFirstBone(bones, { "Thigh.R", "Thigh_R", "RightUpLeg", "Thigh.L", "Thigh_L" });
		const int legMid = FindFirstBone(bones, { "Shin.R", "Shin_R", "RightLeg", "Shin.L", "Shin_L" });
		const int legEnd = FindFirstBone(bones, { "Foot.R", "Foot_R", "RightFoot", "Foot.L", "Foot_L" });

		const Wui::WuiRect scrollArea { area.X, area.Y + 28.0f, area.W, std::max(40.0f, area.H - 28.0f) };
		const float wizardH = m_IkWizardOpen ? 238.0f : 52.0f;
		const float contentH = 150.0f + static_cast<float>(chainHandles.size()) * kIkChainH + wizardH + 126.0f;
		if (!Wui::BeginScrollArea(ctx, scrollArea, contentH, m_IkScroll, theme,
			Wui::HashId("animgraph.ik.scroll")))
			return;

		const float innerX = area.X + theme.Pad;
		const float innerW = area.W - theme.Pad * 2.0f;
		// 内容必须跟着滚动量上移；只改 m_IkScroll 而不减到 y 上，滚动条会动、
		// 画面却一直停在顶部（实机：向导正文永远进不了视口）。
		float y = scrollArea.Y + 4.0f - m_IkScroll;

		Wui::Label(ctx, { innerX, y }, IKFmt("ik.character", "Character: {name}",
			{ { "name", m_BoundName } }), theme.Text, theme.FontSizeSmall);
		Wui::Tooltip(ctx, { innerX, y, innerW, 16.0f },
			IKTr("ik.character.tooltip", "All IK chains below drive this entity's skeleton."));
		y += 20.0f;

		const char* introKeys[3] = { "ik.intro.chain", "ik.intro.weight", "ik.intro.target" };
		const char* introText[3] = {
			"IK solves a three-bone chain: root -> mid -> end. Any skeleton works (arm, leg, tail, finger).",
			"Weight 0 keeps the animation pose; weight 1 reaches the target. Pole Angle rotates the bend plane.",
			"The target is where the end bone should go. Use a new sphere, an existing entity, or world coordinates." };
		for (int i = 0; i < 3; ++i)
		{
			const std::string line = IKTr(introKeys[i], introText[i]);
			Wui::Label(ctx, { innerX, y }, Wui::EllipsizeMiddleToWidth(ctx, line, innerW, theme.FontSizeCaption),
				theme.TextMuted, theme.FontSizeCaption);
			Wui::Tooltip(ctx, { innerX, y, innerW, 14.0f }, line);
			y += 16.0f;
		}
		y += 4.0f;

		if (runtime)
		{
			const Wui::WuiRect warn { innerX, y, innerW, 20.0f };
			Wui::PanelBackground(ctx, warn, theme.Warning, 3.0f);
			Wui::Label(ctx, { warn.X + 6.0f, warn.Y + 2.0f },
				IKTr("ik.runtime.readonly", "Play mode is read-only: stop playback to create or edit IK."),
				theme.WindowBg, theme.FontSizeCaption);
			y += 24.0f;
		}

		Wui::Label(ctx, { innerX, y }, IKFmt("ik.chains", "IK chains ({count})",
			{ { "count", std::to_string(chainHandles.size()) } }), theme.Text, theme.FontSizeSmall);
		// 打开向导时自动选一组合理的默认骨骼（右臂优先，其次右腿，最后前三个关节）；
		// 顶部按钮与折叠头两条入口都走这里，避免"三个框都显示 Root、创建按钮灰着"。
		auto initWizardBones = [&]()
		{
			if (bones.empty())
				return;
			if (armRoot >= 0 && armMid >= 0 && armEnd >= 0)
			{
				m_IkRootIndex = armRoot;
				m_IkMidIndex = armMid;
				m_IkEndIndex = armEnd;
			}
			else if (legRoot >= 0 && legMid >= 0 && legEnd >= 0)
			{
				m_IkRootIndex = legRoot;
				m_IkMidIndex = legMid;
				m_IkEndIndex = legEnd;
			}
			else
			{
				m_IkRootIndex = 0;
				m_IkMidIndex = std::min(1, static_cast<int>(bones.size()) - 1);
				m_IkEndIndex = std::min(2, static_cast<int>(bones.size()) - 1);
			}
		};
		{
			const float buttonW = 108.0f;
			const Wui::WuiRect newChain { innerX + innerW - buttonW, y - 2.0f, buttonW, kIkRowH };
			if (Wui::ActionButton(ctx, Wui::HashId("animgraph.ik.new_chain"), newChain,
				IKTr("ik.new_chain", "+ New IK chain"), theme, !runtime,
				IKTr("ik.new_chain.tooltip", "Open the wizard: pick three bones and a target to create a chain."), false))
			{
				const bool wasOpen = m_IkWizardOpen;
				m_IkWizardOpen = true;
				if (!wasOpen)
					initWizardBones();
				// 向导在链列表之后：把它滚进视口，省得用户自己找。
				m_IkScroll = std::max(0.0f, 84.0f + static_cast<float>(chainHandles.size()) * kIkChainH - 24.0f);
			}
		}
		y += 20.0f;

		if (chainHandles.empty())
		{
			Wui::Label(ctx, { innerX, y },
				IKTr("ik.chains.empty", "No IK chain yet. Use the wizard below to bind a target to any three bones."),
				theme.TextMuted, theme.FontSizeCaption);
			y += 24.0f;
		}
		for (size_t chainIndex = 0; chainIndex < chainHandles.size(); ++chainIndex)
		{
			const entt::entity handle = chainHandles[chainIndex];
			const TwoBoneIkComponent* readOnly = constRegistry.try_get<TwoBoneIkComponent>(handle);
			if (readOnly == nullptr)
				continue;

			const Wui::WuiRect card { innerX, y, innerW, kIkChainH - kIkGap };
			Wui::PanelBackground(ctx, card, theme.ContentBg, theme.Radius);

			std::string chainName = EntityName(constRegistry, handle);
			if (chainName.empty())
				chainName = IKFmt("ik.chain.unnamed", "IK Chain {index}",
					{ { "index", std::to_string(chainIndex + 1) } });

			const float deleteW = kIkRowH;
			const float enabledW = 88.0f;
			Wui::Label(ctx, { card.X + 8.0f, card.Y + 6.0f },
				Wui::EllipsizeMiddleToWidth(ctx, chainName,
					std::max(60.0f, card.W - deleteW - enabledW - 20.0f), theme.FontSizeSmall),
				theme.Text, theme.FontSizeSmall);

			Entity chainEntity(&scene, handle);
			TwoBoneIkComponent* chain = runtime ? nullptr : &chainEntity.GetComponent<TwoBoneIkComponent>();
			const TwoBoneIkComponent& view = chain ? *chain : *readOnly;

			bool enabled = view.Enabled != 0;
			const Wui::WuiRect enabledRect { card.X + card.W - deleteW - enabledW - 8.0f, card.Y + 2.0f,
				enabledW, kIkRowH };
			if (!runtime && Wui::Checkbox(ctx,
				Wui::HashId(("animgraph.ik.enabled." + std::to_string(chainIndex)).c_str()),
				enabledRect, IKTr("ik.enabled", "Enabled"), enabled, theme))
			{
				chain->Enabled = enabled ? 1 : 0;
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, enabledRect, IKTr("ik.enabled.tooltip", "Temporarily disable this chain without deleting it."));

			const Wui::WuiRect deleteRect { card.X + card.W - deleteW - 4.0f, card.Y + 2.0f, deleteW, kIkRowH };
			if (!runtime && Wui::ActionButton(ctx,
				Wui::HashId(("animgraph.ik.delete." + std::to_string(chainIndex)).c_str()), deleteRect, "-",
				theme, true, IKTr("ik.delete.tooltip", "Delete this IK proxy entity (the target entity is kept)."), true))
			{
				host.GetCommandHistory().PushAndExecute(
					std::make_unique<EntityDestroyCommand>(chainEntity, "Delete IK Chain"));
				host.MarkDocumentDirty();
				m_IkStatus = IKFmt("ik.deleted", "Deleted IK chain '{name}'", { { "name", chainName } });
				m_IkStatusError = false;
				break;
			}

			float rowY = card.Y + kIkRowH + 8.0f;
			const float cellGap = 4.0f;
			const float cellW = (card.W - 16.0f - cellGap * 2.0f) / 3.0f;
			struct BoneField
			{
				const char* LabelKey;
				const char* LabelFallback;
				NameId TwoBoneIkComponent::*Member;
				const char* Id;
				NameId Current;
			};
			// RootBone/MidBone/EndBone 是驻留 NameId（组件必须平凡可拷贝），
			// 展示/选择时经 StringPool 与可读骨骼名互转。
			const std::string rootName = StringPool::Get().NameOf(view.RootBone);
			const std::string midName = StringPool::Get().NameOf(view.MidBone);
			const std::string endName = StringPool::Get().NameOf(view.EndBone);
			const BoneField fields[3] = {
				{ "ik.bone.root", "Root", &TwoBoneIkComponent::RootBone, "root", view.RootBone },
				{ "ik.bone.mid", "Mid", &TwoBoneIkComponent::MidBone, "mid", view.MidBone },
				{ "ik.bone.end", "End", &TwoBoneIkComponent::EndBone, "end", view.EndBone } };
			const std::string fieldNames[3] = { rootName, midName, endName };
			for (int field = 0; field < 3; ++field)
			{
				const Wui::WuiRect rect { card.X + 8.0f + static_cast<float>(field) * (cellW + cellGap),
					rowY, cellW, kIkRowH };
				std::vector<std::string> labels;
				labels.reserve(bones.size());
				for (const BoneChoice& bone : bones)
					labels.push_back(bone.Name);
				if (bones.empty())
				{
					Wui::Label(ctx, { rect.X + 2.0f, rect.Y + 4.0f },
						IKTr(fields[field].LabelKey, fields[field].LabelFallback), theme.TextDisabled, theme.FontSizeCaption);
					continue;
				}
				const std::string& currentName = fieldNames[field];
				int selected = FindBoneIndex(bones, currentName);
				const std::string preview = selected >= 0 ? bones[static_cast<size_t>(selected)].Name
					: (currentName.empty() ? IKTr(fields[field].LabelKey, fields[field].LabelFallback)
						: currentName);
				if (Wui::Combo(ctx,
					Wui::HashId(("animgraph.ik." + std::string(fields[field].Id) + "." + std::to_string(chainIndex)).c_str()),
					rect, "", labels, selected, theme, preview) && !runtime
					&& selected >= 0 && static_cast<size_t>(selected) < bones.size())
				{
					chain->*(fields[field].Member) = StringPool::Get().InternName(bones[static_cast<size_t>(selected)].Name);
					host.MarkDocumentDirty();
				}
				Wui::Tooltip(ctx, rect, IKTr(fields[field].LabelKey,
					field == 0 ? "Root bone of the chain." : (field == 1 ? "Middle bone (elbow/knee)." : "End bone driven to the target.")));
			}

			rowY += kIkRowH + 2.0f;
			const float colW = (card.W - 16.0f - kIkGap) * 0.5f;
			const float fieldLabelW = 44.0f;
			float weight = view.Weight;
			Wui::Label(ctx, { card.X + 8.0f, rowY + 4.0f }, IKTr("ik.weight", "Weight"),
				theme.TextMuted, theme.FontSizeCaption);
			const Wui::WuiRect weightRect { card.X + 8.0f + fieldLabelW, rowY,
				colW - fieldLabelW, kIkRowH };
			if (!runtime && Wui::DragFloat(ctx,
				Wui::HashId(("animgraph.ik.weight." + std::to_string(chainIndex)).c_str()),
				weightRect, weight, 0.01f, 0.0f, 1.0f, theme))
			{
				chain->Weight = std::clamp(weight, 0.0f, 1.0f);
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, weightRect, IKTr("ik.weight.tooltip",
				"Blend weight: 0 = animation only, 1 = fully reach the target."));

			float pole = view.PoleAngleDeg;
			Wui::Label(ctx, { card.X + 8.0f + colW + kIkGap, rowY + 4.0f }, IKTr("ik.pole", "Pole (deg)"),
				theme.TextMuted, theme.FontSizeCaption);
			const Wui::WuiRect poleRect { card.X + 8.0f + colW + kIkGap + fieldLabelW, rowY,
				colW - fieldLabelW, kIkRowH };
			if (!runtime && Wui::DragFloat(ctx,
				Wui::HashId(("animgraph.ik.pole." + std::to_string(chainIndex)).c_str()),
				poleRect, pole, 1.0f, -180.0f, 180.0f, theme))
			{
				chain->PoleAngleDeg = std::clamp(pole, -180.0f, 180.0f);
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, poleRect, IKTr("ik.pole.tooltip",
				"Rotates the bend plane (elbow/knee direction) around the chain axis, in degrees."));

			rowY += kIkRowH + 2.0f;
			std::vector<std::string> targetLabels { IKTr("ik.target.manual", "World position (manual)") };
			int targetIndex = 0;
			for (size_t i = 0; i < entities.size(); ++i)
			{
				targetLabels.push_back(entities[i].Label);
				if (static_cast<uint64_t>(entities[i].Id) == view.TargetUuid)
					targetIndex = static_cast<int>(i) + 1;
			}
			const Wui::WuiRect targetRect { card.X + 8.0f, rowY, card.W - 16.0f, kIkRowH };
			if (Wui::Combo(ctx,
				Wui::HashId(("animgraph.ik.target." + std::to_string(chainIndex)).c_str()),
				targetRect, "", targetLabels, targetIndex, theme,
				targetLabels[static_cast<size_t>(std::max(0, targetIndex))]) && !runtime)
			{
				chain->TargetUuid = targetIndex == 0 ? 0
					: static_cast<uint64_t>(entities[static_cast<size_t>(targetIndex - 1)].Id);
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, targetRect, IKTr("ik.target.tooltip",
				"The end bone is solved toward this target. Pick any entity, or switch to manual world coordinates."));

			rowY += kIkRowH + 2.0f;
			if (view.TargetUuid == 0)
			{
				const float third = (card.W - 16.0f - kIkGap * 2.0f) / 3.0f;
				glm::vec3 manual = view.TargetPosition;
				for (int axis = 0; axis < 3; ++axis)
				{
					float value = manual[axis];
					const Wui::WuiRect rect { card.X + 8.0f + static_cast<float>(axis) * (third + kIkGap),
						rowY, third, kIkRowH };
					if (!runtime && Wui::DragFloat(ctx,
						Wui::HashId(("animgraph.ik.targetpos." + std::to_string(chainIndex) + "." + std::to_string(axis)).c_str()),
						rect, value, 0.01f, -100.0f, 100.0f, theme))
					{
						manual[axis] = value;
						if (IsFiniteVec(manual))
						{
							chain->TargetPosition = manual;
							host.MarkDocumentDirty();
						}
					}
				}
				Wui::Tooltip(ctx, { card.X + 8.0f, rowY, card.W - 16.0f, kIkRowH },
					IKTr("ik.targetpos.tooltip", "World-space target position (meters), used when no target entity is selected."));
			}
			else
			{
				const Wui::WuiRect readout { card.X + 8.0f, rowY, card.W - 16.0f, kIkRowH };
				const uint64_t targetUuid = view.TargetUuid;
				std::string targetName = IKFmt("ik.target.missing", "Target entity missing (#{id})",
					{ { "id", std::to_string(targetUuid) } });
				for (const EntityChoice& choice : entities)
				{
					if (static_cast<uint64_t>(choice.Id) == targetUuid)
					{
						targetName = IKFmt("ik.target.entity", "Target: {name}", { { "name", choice.Label } });
						break;
					}
				}
				Wui::Label(ctx, { readout.X, readout.Y + 4.0f },
					Wui::EllipsizeMiddleToWidth(ctx, targetName, readout.W, theme.FontSizeCaption),
					theme.TextMuted, theme.FontSizeCaption);
				Wui::Tooltip(ctx, readout, IKTr("ik.target.entity.tooltip",
					"Move this entity in the viewport to see the chain follow it."));
			}
			y += kIkChainH;
		}

		y += 4.0f;
		// ---- 新建 IK 链 ----
		const Wui::WuiRect wizardHeader { innerX, y, innerW, kIkRowH };
		const bool wasOpen = m_IkWizardOpen;
		Wui::CollapsibleHeader(ctx, Wui::HashId("animgraph.ik.wizard"), wizardHeader,
			IKTr("ik.wizard.title", "New IK chain"), m_IkWizardOpen, theme, std::string(), std::string(),
			IKTr("ik.wizard.tooltip", "Create a generic two-bone chain. Any three bones work; the target can be a new sphere or an existing entity."),
			theme.FontSizeSmall);
		if (!wasOpen && m_IkWizardOpen && !bones.empty())
		{
			initWizardBones();
		}
		y += kIkRowH + 4.0f;

		if (m_IkWizardOpen)
		{
			const float labelW = 64.0f;
			const float controlX = innerX + labelW;
			const float controlW = innerW - labelW;

			Wui::Label(ctx, { innerX, y + 4.0f }, IKTr("ik.wizard.name", "Name"), theme.TextMuted, theme.FontSizeCaption);
			Wui::TextFieldA11y nameA11y;
			nameA11y.Label = IKTr("ik.wizard.name.a11y", "IK chain name");
			nameA11y.Placeholder = "Right Hand IK";
			if (Wui::TextField(ctx, Wui::HashId("animgraph.ik.wizard.name"),
				{ controlX, y, controlW, kIkRowH }, m_IkChainName, theme, nullptr, &nameA11y))
				ctx.SetFocus(0);
			Wui::Tooltip(ctx, { controlX, y, controlW, kIkRowH },
				IKTr("ik.wizard.name.tooltip", "Entity label for the IK proxy. Keep it descriptive, e.g. 'Right Hand IK'."));
			y += kIkRowH + 2.0f;

			const float presetW = std::max(50.0f, (controlW - kIkGap) * 0.5f);
			if (Wui::ActionButton(ctx, Wui::HashId("animgraph.ik.wizard.preset.arm"),
				{ controlX, y, presetW, kIkRowH }, IKTr("ik.wizard.preset.arm", "Arm preset"), theme,
				armRoot >= 0 && armMid >= 0 && armEnd >= 0,
				IKTr("ik.wizard.preset.arm.tooltip", "Fill Root/Mid/End with UpperArm -> LowerArm -> Hand when those bone names exist."), false)
				&& armRoot >= 0 && armMid >= 0 && armEnd >= 0)
			{
				m_IkRootIndex = armRoot;
				m_IkMidIndex = armMid;
				m_IkEndIndex = armEnd;
			}
			if (Wui::ActionButton(ctx, Wui::HashId("animgraph.ik.wizard.preset.leg"),
				{ controlX + presetW + kIkGap, y, presetW, kIkRowH }, IKTr("ik.wizard.preset.leg", "Leg preset"), theme,
				legRoot >= 0 && legMid >= 0 && legEnd >= 0,
				IKTr("ik.wizard.preset.leg.tooltip", "Fill Root/Mid/End with Thigh -> Shin -> Foot when those bone names exist."), false)
				&& legRoot >= 0 && legMid >= 0 && legEnd >= 0)
			{
				m_IkRootIndex = legRoot;
				m_IkMidIndex = legMid;
				m_IkEndIndex = legEnd;
			}
			y += kIkRowH + 2.0f;

			const float cellW = (controlW - kIkGap * 2.0f) / 3.0f;
			int* indices[3] = { &m_IkRootIndex, &m_IkMidIndex, &m_IkEndIndex };
			const char* ids[3] = { "root", "mid", "end" };
			const char* labels[3] = { "ik.wizard.root", "ik.wizard.mid", "ik.wizard.end" };
			const char* fallbacks[3] = { "Root", "Mid", "End" };
			for (int field = 0; field < 3; ++field)
			{
				std::vector<std::string> names;
				names.reserve(bones.size());
				for (const BoneChoice& bone : bones)
					names.push_back(bone.Name);
				int& index = *indices[field];
				if (bones.empty())
				{
					Wui::Label(ctx, { controlX + static_cast<float>(field) * (cellW + kIkGap), y + 4.0f },
						IKTr(labels[field], fallbacks[field]), theme.TextDisabled, theme.FontSizeCaption);
					continue;
				}
				index = std::clamp(index, 0, static_cast<int>(bones.size()) - 1);
				const Wui::WuiRect rect { controlX + static_cast<float>(field) * (cellW + kIkGap),
					y, cellW, kIkRowH };
				Wui::Combo(ctx, Wui::HashId(("animgraph.ik.wizard.bone." + std::string(ids[field])).c_str()),
					rect, "", names, index, theme, bones[static_cast<size_t>(index)].Name);
				Wui::Tooltip(ctx, rect, IKTr(labels[field], fallbacks[field]));
			}
			y += kIkRowH + 2.0f;

			std::vector<std::string> modes {
				IKTr("ik.wizard.target.create", "New target sphere"),
				IKTr("ik.wizard.target.existing", "Existing entity"),
				IKTr("ik.wizard.target.manual", "World position") };
			Wui::Combo(ctx, Wui::HashId("animgraph.ik.wizard.targetmode"), { controlX, y, controlW, kIkRowH },
				"", modes, m_IkTargetMode, theme, modes[static_cast<size_t>(std::clamp(m_IkTargetMode, 0, 2))]);
			Wui::Tooltip(ctx, { controlX, y, controlW, kIkRowH }, IKTr("ik.wizard.targetmode.tooltip",
				"New target sphere creates a visible entity you can drag in the viewport. Existing entity follows that entity. World position is a fixed point."));
			y += kIkRowH + 2.0f;

			if (m_IkTargetMode == 1)
			{
				std::vector<std::string> labels;
				for (const EntityChoice& choice : entities)
					labels.push_back(choice.Label);
				if (labels.empty())
					labels.push_back(IKTr("ik.wizard.no_entities", "(no other entities)"));
				m_IkTargetEntityIndex = std::clamp(m_IkTargetEntityIndex, 0,
					std::max(0, static_cast<int>(entities.size()) - 1));
				Wui::Combo(ctx, Wui::HashId("animgraph.ik.wizard.targetentity"),
					{ controlX, y, controlW, kIkRowH }, "", labels, m_IkTargetEntityIndex, theme,
					labels[static_cast<size_t>(m_IkTargetEntityIndex)]);
				Wui::Tooltip(ctx, { controlX, y, controlW, kIkRowH },
					IKTr("ik.wizard.targetentity.tooltip", "Any entity with a Transform works as the target."));
				y += kIkRowH + 2.0f;
			}
			else if (m_IkTargetMode == 2)
			{
				const float third = (controlW - kIkGap * 2.0f) / 3.0f;
				for (int axis = 0; axis < 3; ++axis)
				{
					float value = m_IkManualTarget[axis];
					const Wui::WuiRect rect { controlX + static_cast<float>(axis) * (third + kIkGap),
						y, third, kIkRowH };
					if (Wui::DragFloat(ctx,
						Wui::HashId(("animgraph.ik.wizard.manual." + std::to_string(axis)).c_str()),
						rect, value, 0.01f, -100.0f, 100.0f, theme))
						m_IkManualTarget[axis] = value;
				}
				Wui::Tooltip(ctx, { controlX, y, controlW, kIkRowH },
					IKTr("ik.wizard.manual.tooltip", "World-space target position in meters."));
				y += kIkRowH + 2.0f;
			}

			const Wui::WuiRect createRect { controlX, y, controlW, kIkRowH };
			const bool canCreate = !runtime && !bones.empty()
				&& m_IkRootIndex >= 0 && m_IkMidIndex >= 0 && m_IkEndIndex >= 0
				&& m_IkRootIndex != m_IkMidIndex && m_IkMidIndex != m_IkEndIndex && m_IkRootIndex != m_IkEndIndex
				&& (m_IkTargetMode != 1 || !entities.empty());
			if (Wui::ActionButton(ctx, Wui::HashId("animgraph.ik.wizard.create"), createRect,
				IKTr("ik.wizard.create", "Create IK chain"), theme, canCreate,
				canCreate ? IKTr("ik.wizard.create.tooltip", "Create the target (if needed) and the IK proxy entity.")
					: IKTr("ik.wizard.create.disabled", "Pick three different bones and a target first."), false))
			{
				const std::string chainName = m_IkChainName.empty() ? std::string("IK Chain") : m_IkChainName;
				const BoneChoice& rootBone = bones[static_cast<size_t>(m_IkRootIndex)];
				const BoneChoice& midBone = bones[static_cast<size_t>(m_IkMidIndex)];
				const BoneChoice& endBone = bones[static_cast<size_t>(m_IkEndIndex)];

				glm::vec3 initialTarget = m_IkManualTarget;
				if (m_IkTargetMode != 2)
				{
					const glm::mat4 rest = NodeRestMatrix(m_Model, endBone.NodeIndex);
					const glm::mat4 world = EntityWorld(constRegistry, m_BoundEntity);
					initialTarget = glm::vec3(world * glm::vec4(glm::vec3(rest[3]), 1.0f));
				}
				if (m_IkTargetMode == 0)
				{
					Entity targetEntity;
					Entity ikEntity;
					const UUID targetId;
					const UUID ikId;
					const bool queued = scene.DeferStructuralChange([&](Scene& target)
					{
						targetEntity = Entity::CreateEntity(&target, chainName + " Target", targetId);
						targetEntity.AddComponent<TransformComponent>();
						auto& transform = targetEntity.GetComponent<TransformComponent>();
						transform.SetLocation(initialTarget);
						transform.SetScale(glm::vec3(0.12f));
						auto& mesh = targetEntity.AddComponent<MeshRendererComponent>();
						mesh.Primitive = MeshRendererComponent::PrimitiveShape::Sphere;
						mesh.Color = glm::vec4(0.20f, 0.85f, 0.95f, 1.0f);
						targetEntity.AddComponent<LevelEntityComponent>(LevelEntityComponent { layerMask });

						ikEntity = Entity::CreateEntity(&target, chainName, ikId);
						ikEntity.AddComponent<TransformComponent>();
						auto& ik = ikEntity.AddComponent<TwoBoneIkComponent>();
						ik.OwnerUuid = characterUuid;
						ik.TargetUuid = static_cast<uint64_t>(targetId);
						ik.RootBone = StringPool::Get().InternName(rootBone.Name);
						ik.MidBone = StringPool::Get().InternName(midBone.Name);
						ik.EndBone = StringPool::Get().InternName(endBone.Name);
						ik.TargetPosition = initialTarget;
						ik.Weight = 0.85f;
						ik.PoleAngleDeg = 0.0f;
						ik.Enabled = 1;
						ikEntity.AddComponent<LevelEntityComponent>(LevelEntityComponent { layerMask });
					});
					if (queued && targetEntity.IsValid() && ikEntity.IsValid())
					{
						host.PushCommand(std::make_unique<EntityCreateCommand>(targetEntity, "Create IK Target"));
						host.PushCommand(std::make_unique<EntityCreateCommand>(ikEntity, "Create IK Chain"));
						host.MarkDocumentDirty();
						m_IkStatus = IKFmt("ik.wizard.created", "Created IK chain '{name}' with a visible target.",
							{ { "name", chainName } });
						m_IkStatusError = false;
					}
					else
					{
						m_IkStatus = IKTr("ik.wizard.failed", "Could not create the IK entities.");
						m_IkStatusError = true;
					}
				}
				else
				{
					Entity ikEntity;
					const UUID ikId;
					const uint64_t targetUuid = m_IkTargetMode == 1 && !entities.empty()
						? static_cast<uint64_t>(entities[static_cast<size_t>(m_IkTargetEntityIndex)].Id) : 0;
					const bool queued = scene.DeferStructuralChange([&](Scene& target)
					{
						ikEntity = Entity::CreateEntity(&target, chainName, ikId);
						ikEntity.AddComponent<TransformComponent>();
						auto& ik = ikEntity.AddComponent<TwoBoneIkComponent>();
						ik.OwnerUuid = characterUuid;
						ik.TargetUuid = targetUuid;
						ik.RootBone = StringPool::Get().InternName(rootBone.Name);
						ik.MidBone = StringPool::Get().InternName(midBone.Name);
						ik.EndBone = StringPool::Get().InternName(endBone.Name);
						ik.TargetPosition = initialTarget;
						ik.Weight = 0.85f;
						ik.PoleAngleDeg = 0.0f;
						ik.Enabled = 1;
						ikEntity.AddComponent<LevelEntityComponent>(LevelEntityComponent { layerMask });
					});
					if (queued && ikEntity.IsValid())
					{
						host.PushCommand(std::make_unique<EntityCreateCommand>(ikEntity, "Create IK Chain"));
						host.MarkDocumentDirty();
						m_IkStatus = IKFmt("ik.wizard.created", "Created IK chain '{name}'.",
							{ { "name", chainName } });
						m_IkStatusError = false;
					}
					else
					{
						m_IkStatus = IKTr("ik.wizard.failed", "Could not create the IK entities.");
						m_IkStatusError = true;
					}
				}
			}
			Wui::Tooltip(ctx, createRect, IKTr("ik.wizard.create.tooltip",
				"Create the target (if needed) and the IK proxy entity."));
			y += kIkRowH + 4.0f;
		}

		y += 4.0f;

		// ---- 脚步 IK 示例 ----
		Wui::Label(ctx, { innerX, y }, IKTr("ik.foot.title", "Foot IK example (generic solver driver)"),
			theme.Text, theme.FontSizeSmall);
		Wui::Tooltip(ctx, { innerX, y, innerW, 16.0f }, IKTr("ik.foot.title.tooltip",
			"FootIkComponent samples the terrain under each foot and issues two ordinary two-bone IK requests. It contains no solver math."));
		y += 20.0f;

		FootIkComponent* foot = nullptr;
		if (!runtime)
			foot = static_cast<FootIkComponent*>(
				Entity(&scene, m_BoundEntity).GetComponent(entt::type_id<FootIkComponent>().hash()));
		const FootIkComponent* footRead = constRegistry.try_get<FootIkComponent>(m_BoundEntity);
		if (foot == nullptr && footRead == nullptr)
		{
			if (Wui::ActionButton(ctx, Wui::HashId("animgraph.ik.foot.add"),
				{ innerX, y, innerW, kIkRowH }, IKTr("ik.foot.add", "Add Foot IK example"), theme, !runtime,
				IKTr("ik.foot.add.tooltip", "Adds FootIkComponent: terrain-following feet as an example built on the generic IK solver."), false))
			{
				const entt::entity handle = m_BoundEntity;
				if (scene.DeferStructuralChange([handle](Scene& target)
				{
					Entity entity(&target, handle);
					if (entity.IsValid() && !entity.HasComponent<FootIkComponent>())
						entity.AddComponent<FootIkComponent>();
				}))
					host.MarkDocumentDirty();
			}
			y += kIkRowH + 2.0f;
			Wui::Label(ctx, { innerX, y }, IKTr("ik.foot.none",
				"Not added. Foot IK is optional; generic chains above work without it."),
				theme.TextMuted, theme.FontSizeCaption);
			y += 16.0f;
		}
		else
		{
			const FootIkComponent& footView = foot ? *foot : *footRead;
			bool footEnabled = footView.Enabled != 0;
			const float halfW = (innerW - kIkGap) * 0.5f;
			if (!runtime && Wui::Checkbox(ctx, Wui::HashId("animgraph.ik.foot.enabled"),
				{ innerX, y, halfW, kIkRowH }, IKTr("ik.foot.enabled", "Enabled"), footEnabled, theme))
			{
				foot->Enabled = footEnabled ? 1 : 0;
				host.MarkDocumentDirty();
			}
			float footWeight = footView.Weight;
			Wui::Label(ctx, { innerX + halfW + kIkGap, y + 4.0f }, IKTr("ik.weight", "Weight"),
				theme.TextMuted, theme.FontSizeCaption);
			const Wui::WuiRect footWeightRect { innerX + halfW + kIkGap + 44.0f, y,
				halfW - 44.0f, kIkRowH };
			if (!runtime && Wui::DragFloat(ctx, Wui::HashId("animgraph.ik.foot.weight"),
				footWeightRect, footWeight, 0.01f, 0.0f, 1.0f, theme))
			{
				foot->Weight = std::clamp(footWeight, 0.0f, 1.0f);
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, footWeightRect, IKTr("ik.foot.weight.tooltip", "Blend weight of both foot chains."));
			y += kIkRowH + 2.0f;

			const float third = (innerW - kIkGap * 2.0f) / 3.0f;
			float offset = footView.FootOffset;
			Wui::Label(ctx, { innerX, y + 4.0f }, IKTr("ik.foot.offset", "Offset (m)"),
				theme.TextMuted, theme.FontSizeCaption);
			if (!runtime && Wui::DragFloat(ctx, Wui::HashId("animgraph.ik.foot.offset"),
				{ innerX + 60.0f, y, third - 60.0f, kIkRowH }, offset, 0.01f, -0.5f, 0.5f, theme))
			{
				foot->FootOffset = offset;
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, { innerX, y, third, kIkRowH },
				IKTr("ik.foot.offset.tooltip", "Vertical ankle offset above the sampled ground, in meters."));

			float drop = footView.MaxPelvisDrop;
			Wui::Label(ctx, { innerX + third + kIkGap, y + 4.0f }, IKTr("ik.foot.drop", "Pelvis drop (m)"),
				theme.TextMuted, theme.FontSizeCaption);
			if (!runtime && Wui::DragFloat(ctx, Wui::HashId("animgraph.ik.foot.drop"),
				{ innerX + third + kIkGap + 92.0f, y, third - 92.0f, kIkRowH }, drop, 0.01f, 0.0f, 1.0f, theme))
			{
				foot->MaxPelvisDrop = std::clamp(drop, 0.0f, 1.0f);
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, { innerX + third + kIkGap, y, third, kIkRowH },
				IKTr("ik.foot.drop.tooltip", "Maximum pelvis drop while keeping both feet planted, in meters."));

			Wui::Label(ctx, { innerX + third * 2.0f + kIkGap * 2.0f, y + 4.0f },
				IKTr("ik.foot.note", "Terrain-driven"), theme.TextMuted, theme.FontSizeCaption);
			y += kIkRowH + 2.0f;
		}

		if (!m_IkStatus.empty())
		{
			Wui::Label(ctx, { innerX, y },
				Wui::EllipsizeMiddleToWidth(ctx, m_IkStatus, innerW, theme.FontSizeCaption),
				m_IkStatusError ? theme.Danger : theme.TextMuted, theme.FontSizeCaption);
		}

		Wui::EndScrollArea(ctx);
		if (contentH > scrollArea.H + 1.0f)
		{
			const float barW = 6.0f;
			Wui::ScrollBar(ctx, Wui::HashId("animgraph.ik.scrollbar"),
				{ scrollArea.X + scrollArea.W - barW - 2.0f, scrollArea.Y, barW, scrollArea.H },
				contentH, scrollArea.H, m_IkScroll, theme, false);
		}
	}
}
