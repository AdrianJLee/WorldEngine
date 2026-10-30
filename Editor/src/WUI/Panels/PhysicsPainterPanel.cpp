#include "wldpch.h"
#include "WUI/Panels/PhysicsPainterPanel.h"

#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/Command/CommandHistory.h"

#include <random>
#include <cmath>
#include <algorithm>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace World
{
	PhysicsPainterPanel::PhysicsPainterPanel() = default;
	PhysicsPainterPanel::PhysicsPainterPanel(PanelHost&) {}

	void PhysicsPainterPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		const float pad = 8.0f;
		const float startX = rect.X + pad;
		const float contentW = std::max(100.0f, rect.W - pad * 2.0f);
		float curY = rect.Y + 6.0f;

		// 1. 顶部状态横幅与激活主开关
		const Wui::WuiRect bannerRect { startX, curY, contentW, 36.0f };
		const Wui::WuiColor bannerBg = s_Active
			? Wui::WuiColor{ 0.12f, 0.32f, 0.18f, 0.95f }
			: Wui::WuiColor{ 0.18f, 0.20f, 0.24f, 0.95f };
		Wui::PanelBackground(ctx, bannerRect, bannerBg, 4.0f);
		Wui::HighlightOutline(ctx, bannerRect, s_Active ? theme.Success : theme.Border, 4.0f, 1.0f);

		const std::string statusTitle = s_Active
			? "[已激活 ACTIVE] 点击视口即可在地表/网格散布物体"
			: "[已休眠 INACTIVE] 点击右侧按钮开启视口画笔模式";
		Wui::Label(ctx, { bannerRect.X + 10.0f, bannerRect.Y + 10.0f }, statusTitle,
			s_Active ? theme.Success : theme.TextMuted, 13.0f);

		const float toggleBtnW = 100.0f;
		const Wui::WuiRect toggleBtnRect { bannerRect.X + bannerRect.W - toggleBtnW - 6.0f, bannerRect.Y + 6.0f, toggleBtnW, 24.0f };
		if (Wui::Button(ctx, Wui::HashId("painter.toggle_btn"), toggleBtnRect,
			s_Active ? "关闭画笔" : "开启画笔", theme))
		{
			s_Active = !s_Active;
		}
		Wui::Tooltip(ctx, toggleBtnRect, "开启或关闭视口贴地物理射线散布画笔");
		curY += 44.0f;

		// 2. 笔刷规格调节卡片 (Brush Specs)
		const float cardPad = 8.0f;
		Wui::Label(ctx, { startX, curY }, "笔刷规格 (Brush Specifications)", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		const Wui::WuiRect brushCardRect { startX, curY, contentW, 90.0f };
		Wui::PanelBackground(ctx, brushCardRect, theme.ContentBg, 4.0f);
		Wui::HighlightOutline(ctx, brushCardRect, theme.Border, 4.0f, 1.0f);

		float bY = brushCardRect.Y + cardPad;
		const float labelW = 110.0f;
		const float fieldW = brushCardRect.W - cardPad * 2.0f - labelW - 4.0f;

		// 笔刷半径 (Radius: 0.5m ~ 50.0m)
		Wui::Label(ctx, { brushCardRect.X + cardPad, bY + 3.0f }, "笔刷半径 (Radius):", theme.Text, theme.FontSizeSmall);
		float rad = s_BrushRadius;
		if (Wui::DragFloat(ctx, Wui::HashId("painter.brush_radius"),
			{ brushCardRect.X + cardPad + labelW, bY, fieldW, 22.0f }, rad, 0.2f, 0.5f, 50.0f, theme))
		{
			s_BrushRadius = std::clamp(rad, 0.5f, 50.0f);
		}
		Wui::Tooltip(ctx, { brushCardRect.X + cardPad + labelW, bY, fieldW, 22.0f }, "贴地光圈与散布作用半径 (0.5 ~ 50 米)");
		bY += 26.0f;

		// 单次散布密度 (Density: 1 ~ 30)
		Wui::Label(ctx, { brushCardRect.X + cardPad, bY + 3.0f }, "单次散布密度 (Count):", theme.Text, theme.FontSizeSmall);
		int64_t dens = s_BrushDensity;
		if (Wui::DragInt(ctx, Wui::HashId("painter.brush_density"),
			{ brushCardRect.X + cardPad + labelW, bY, fieldW, 22.0f }, dens, 1, 30, theme))
		{
			s_BrushDensity = static_cast<int>(std::clamp(dens, int64_t(1), int64_t(30)));
		}
		Wui::Tooltip(ctx, { brushCardRect.X + cardPad + labelW, bY, fieldW, 22.0f }, "每次在视口点击刷出的实体数量 (1 ~ 30)");
		bY += 26.0f;

		// 最大坡度限制 (Slope Limit: 0° ~ 60°)
		Wui::Label(ctx, { brushCardRect.X + cardPad, bY + 3.0f }, "最大坡度限制 (Slope):", theme.Text, theme.FontSizeSmall);
		float slope = s_MaxSlope;
		if (Wui::DragFloat(ctx, Wui::HashId("painter.max_slope"),
			{ brushCardRect.X + cardPad + labelW, bY, fieldW, 22.0f }, slope, 0.5f, 0.0f, 60.0f, theme))
		{
			s_MaxSlope = std::clamp(slope, 0.0f, 60.0f);
		}
		Wui::Tooltip(ctx, { brushCardRect.X + cardPad + labelW, bY, fieldW, 22.0f }, "允许物体着陆的最大地表坡度角 (超过则丢弃散布)");
		curY += brushCardRect.H + 12.0f;

		// 3. 姿态随机性与对齐规则 (Orientation & Alignment)
		Wui::Label(ctx, { startX, curY }, "变换随机性 (Transform Variation)", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		const Wui::WuiRect orientCardRect { startX, curY, contentW, 116.0f };
		Wui::PanelBackground(ctx, orientCardRect, theme.ContentBg, 4.0f);
		Wui::HighlightOutline(ctx, orientCardRect, theme.Border, 4.0f, 1.0f);

		float oY = orientCardRect.Y + cardPad;

		// 随机偏航角 Random Yaw
		bool randYaw = s_RandomYaw;
		if (Wui::Checkbox(ctx, Wui::HashId("painter.rand_yaw"),
			{ orientCardRect.X + cardPad, oY, orientCardRect.W - cardPad * 2.0f, 22.0f },
			"随机偏航角 (Random Yaw 0~360°)", randYaw, theme))
		{
			s_RandomYaw = randYaw;
		}
		Wui::Tooltip(ctx, { orientCardRect.X + cardPad, oY, orientCardRect.W - cardPad * 2.0f, 22.0f }, "生成物体时绕自身垂直 Y 轴随机旋转");
		oY += 26.0f;

		// 表面法线对齐 Align to Normal
		bool alignNorm = s_AlignToNormal;
		if (Wui::Checkbox(ctx, Wui::HashId("painter.align_normal"),
			{ orientCardRect.X + cardPad, oY, orientCardRect.W - cardPad * 2.0f, 22.0f },
			"对齐表面法线 (Align to Surface Normal)", alignNorm, theme))
		{
			s_AlignToNormal = alignNorm;
		}
		Wui::Tooltip(ctx, { orientCardRect.X + cardPad, oY, orientCardRect.W - cardPad * 2.0f, 22.0f }, "自动旋转物体令其底面贴合地表或坡度法线");
		oY += 26.0f;

		// 随机缩放区间 Scale Min ~ Max
		Wui::Label(ctx, { orientCardRect.X + cardPad, oY + 3.0f }, "随机缩放区间 (Scale):", theme.Text, theme.FontSizeSmall);
		const float halfFieldW = (fieldW - 6.0f) * 0.5f;
		float scMin = s_ScaleMin;
		float scMax = s_ScaleMax;
		if (Wui::DragFloat(ctx, Wui::HashId("painter.scale_min"),
			{ orientCardRect.X + cardPad + labelW, oY, halfFieldW, 22.0f }, scMin, 0.05f, 0.2f, 5.0f, theme))
		{
			s_ScaleMin = std::clamp(scMin, 0.2f, s_ScaleMax);
		}
		if (Wui::DragFloat(ctx, Wui::HashId("painter.scale_max"),
			{ orientCardRect.X + cardPad + labelW + halfFieldW + 6.0f, oY, halfFieldW, 22.0f }, scMax, 0.05f, s_ScaleMin, 5.0f, theme))
		{
			s_ScaleMax = std::clamp(scMax, s_ScaleMin, 5.0f);
		}
		Wui::Tooltip(ctx, { orientCardRect.X + cardPad + labelW, oY, fieldW, 22.0f }, "物体生成时的随机等比缩放倍率区间");
		curY += orientCardRect.H + 12.0f;

		// 4. 散布模型资产选择 (Target Model)
		Wui::Label(ctx, { startX, curY }, "散布实体原型 (Scatter Preset / Model)", theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		const Wui::WuiRect modelCardRect { startX, curY, contentW, 64.0f };
		Wui::PanelBackground(ctx, modelCardRect, theme.ContentBg, 4.0f);
		Wui::HighlightOutline(ctx, modelCardRect, theme.Border, 4.0f, 1.0f);

		const std::vector<std::string> modelOptions = {
			"Whitebox Rock (白盒碎石 / 倾角凸体)",
			"Whitebox Crate (白盒木箱 / 倒角立方)",
			"Whitebox Pillar (白盒石柱 / 圆柱立石)",
			"Custom Model Asset (.wmodel 自定义资产)"
		};

		int modelIdx = s_PresetModelIdx;
		const Wui::WuiRect comboRect { modelCardRect.X + cardPad, modelCardRect.Y + cardPad, modelCardRect.W - cardPad * 2.0f, 22.0f };
		if (Wui::SearchableCombo(ctx, Wui::HashId("painter.model_preset_combo"), comboRect, "",
			modelOptions, modelIdx, theme, "", false) && modelIdx != s_PresetModelIdx)
		{
			s_PresetModelIdx = modelIdx;
		}
		Wui::Tooltip(ctx, comboRect, "选择要刷出的几何原型或模型资产");

		if (s_PresetModelIdx == 3)
		{
			const Wui::WuiRect pickBtnRect { modelCardRect.X + cardPad, modelCardRect.Y + cardPad + 26.0f, modelCardRect.W - cardPad * 2.0f, 22.0f };
			const std::string btnLabel = s_CustomModelPath.empty() ? "选择模型资产 (.wmodel)..." : s_CustomModelPath;
			if (Wui::Button(ctx, Wui::HashId("painter.pick_custom_model"), pickBtnRect, btnLabel.c_str(), theme))
			{
				host.OpenAssetPicker("选择散布模型资产", AssetCategory::Mesh,
					[](const std::string& path) { s_CustomModelPath = path; });
			}
			Wui::Tooltip(ctx, pickBtnRect, "从工程资产库中选取静态网格模型用于笔刷散布");
		}
		curY += modelCardRect.H + 12.0f;

		// 5. 统计与清除按钮
		char statBuf[128];
		std::snprintf(statBuf, sizeof(statBuf), "本会话已散布: %zu 个物体", s_TotalPaintedCount);
		Wui::Label(ctx, { startX, curY + 2.0f }, statBuf, theme.TextMuted, theme.FontSizeSmall);

		const float clearBtnW = 120.0f;
		const Wui::WuiRect clearBtnRect { startX + contentW - clearBtnW, curY, clearBtnW, 22.0f };
		if (Wui::Button(ctx, Wui::HashId("painter.clear_btn"), clearBtnRect, "清除所有已散布", theme))
		{
			Ref<Scene> sc = host.GetActiveScene();
			if (sc)
			{
				sc->DeferStructuralChange([](Scene& s) {
					std::vector<Entity> toDestroy;
					for (const auto h : s.GetRegistry().view<TagComponent>())
					{
						Entity e(&s, h);
						if (StringPool::Get().NameOf(e.GetComponent<TagComponent>().Tag).find("Painted_") == 0)
							toDestroy.push_back(e);
					}
					for (auto& e : toDestroy)
						s.DestroyEntity(e);
				});
				s_TotalPaintedCount = 0;
				host.Notify("已清除所有散布物体");
				host.MarkDocumentDirty();
			}
		}
		Wui::Tooltip(ctx, clearBtnRect, "一次性清除场景中所有名称以 Painted_ 起始的散布物体");
	}

	void PhysicsPainterPanel::PaintAt(Scene& scene, const glm::vec3& center, const glm::vec3& normal, PanelHost& host)
	{
		if (s_BrushDensity <= 0)
			return;

		const float cosSlope = std::clamp(glm::dot(glm::normalize(normal), glm::vec3(0.0f, 1.0f, 0.0f)), -1.0f, 1.0f);
		const float slopeDeg = std::acos(cosSlope) * 57.2957795f;
		if (slopeDeg > s_MaxSlope)
			return;

		static std::mt19937 s_Rng(1337);
		std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

		scene.DeferStructuralChange([center, normal, &host, dist01](Scene& s) mutable {
			for (int i = 0; i < s_BrushDensity; ++i)
			{
				const float r = s_BrushRadius * std::sqrt(dist01(s_Rng));
				const float theta = dist01(s_Rng) * 6.2831853f;
				const glm::vec3 localOffset(r * std::cos(theta), 0.0f, r * std::sin(theta));
				const glm::vec3 spawnPos = center + localOffset;

				Entity spawned = Entity::CreateEntity(&s, "Painted_" + std::to_string(s_TotalPaintedCount++));
				auto& xf = spawned.AddComponent<TransformComponent>();
				xf.SetLocation(spawnPos);

				glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
				if (s_AlignToNormal)
				{
					const glm::vec3 up(0.0f, 1.0f, 0.0f);
					const glm::vec3 norm = glm::normalize(normal);
					if (glm::length(glm::cross(up, norm)) > 1e-4f)
					{
						rotation = glm::rotation(up, norm);
					}
				}

				if (s_RandomYaw)
				{
					const float yaw = dist01(s_Rng) * 6.2831853f;
					const glm::quat yawRot = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
					rotation = rotation * yawRot;
				}
				xf.SetRotationQuat(rotation);

				const float scaleVal = s_ScaleMin + dist01(s_Rng) * (s_ScaleMax - s_ScaleMin);
				xf.SetScale(glm::vec3(scaleVal));

				switch (s_PresetModelIdx)
				{
				case 0:
					{
						auto& wb = spawned.AddComponent<WhiteboxComponent>();
						wb.Shape = WhiteboxComponent::ShapeType::Wedge;
						wb.Dimensions = glm::vec3(1.2f, 0.8f, 1.4f);
						wb.GenerateCollision = 1;
						auto& col = spawned.AddComponent<BoxCollider3DComponent>();
						col.HalfExtents = wb.Dimensions * 0.5f;
					}
					break;
				case 1:
					{
						auto& wb = spawned.AddComponent<WhiteboxComponent>();
						wb.Shape = WhiteboxComponent::ShapeType::Box;
						wb.Dimensions = glm::vec3(1.0f, 1.0f, 1.0f);
						wb.BevelSize = 0.05f;
						wb.GenerateCollision = 1;
						auto& col = spawned.AddComponent<BoxCollider3DComponent>();
						col.HalfExtents = glm::vec3(0.5f);
					}
					break;
				case 2:
					{
						auto& wb = spawned.AddComponent<WhiteboxComponent>();
						wb.Shape = WhiteboxComponent::ShapeType::Cylinder;
						wb.Dimensions = glm::vec3(0.8f, 2.5f, 0.8f);
						wb.Subdivisions = 12;
						wb.GenerateCollision = 1;
						auto& col = spawned.AddComponent<BoxCollider3DComponent>();
						col.HalfExtents = glm::vec3(0.4f, 1.25f, 0.4f);
					}
					break;
				case 3:
					{
						if (!s_CustomModelPath.empty())
						{
							auto& mr = spawned.AddComponent<MeshRendererComponent>();
							mr.Mesh = AssetRef{ StringPool::Get().InternPath(s_CustomModelPath), AssetId{} };
						}
						else
						{
							auto& wb = spawned.AddComponent<WhiteboxComponent>();
							wb.Shape = WhiteboxComponent::ShapeType::Box;
							wb.Dimensions = glm::vec3(1.0f);
						}
					}
					break;
				}
			}
			host.MarkDocumentDirty();
		});
	}
}
