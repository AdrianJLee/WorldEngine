#include "World/Audio/AudioSystem.h"
#include "World/Renderer/Tilemap.h"
#include "World/Scene/Components.h"
#include "World/Core/StringPool.h"
#include "PropertiesPanel_Internal.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace World
{

using namespace PropertiesPanelDetail;


	// ---- 组件检视器 ----

float PropertiesPanel::DrawComponentInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect){
		const Wui::WuiTheme& theme = m_Host.Theme();
		void* instance = entity.GetComponent(schema.Storage->ComponentId);
		if (!instance)
			return 0;
		const Wui::WuiId base = Wui::HashId(schema.DisplayName.c_str());
		// P4-U13b:本分区内被编辑的字段名收在这里,分区画完统一登记成实例覆盖。
		// 归属判定(这个实体属于哪条实例记录)放在 RegisterPrefabOverrides —— 普通实体编辑不产生记录。
		std::vector<std::string> changedFields;
		float height = 0.0f;

		// PURE-ECS:合成 arena 每组件重新开始(`Used` 原本没人清零;接上原生容器字段后,
		// 第 257 个节点起的容器行会静默退化成只读摘要 —— 见 ResetScriptRowArenas)。
		ResetScriptRowArenas();
		// 原生容器元素的 `+`/子行需要 schema(命名 struct 元素按它建子行)。与脚本路径的
		// `m_ScriptInspectingEntity → scene.Schemas()` 同一来源。
		{
			Scene* rowScene = entity.IsValid() ? entity.GetScene() : nullptr;
			m_ContainerElementSchemas = rowScene ? &rowScene->GetContext().Schemas() : nullptr;
		}

		// 自定义检查器:与迁移前一致的三行 Location/Rotation(度)/Scale。
		if (schema.Id.Name == "World::TransformComponent")
			height = DrawTransformInspector(ctx, rect, *static_cast<TransformComponent*>(instance), schema,
				visibleRect, &changedFields);
		// 自定义检查器:Primary / Fixed Aspect Ratio + 投影类型下拉 + 对应参数组。
		else if (schema.Id.Name == "World::CameraComponent")
			height = DrawCameraInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::AudioSourceComponent")
			height = DrawAudioSourceInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::SpriteComponent")
			height = DrawSpriteInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::CapsuleCollider2DComponent")
			height = DrawCapsuleCollider2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::PolygonCollider2DComponent")
			height = DrawPolygonCollider2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::ChainCollider2DComponent")
			height = DrawChainCollider2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::PlatformEffector2DComponent")
			height = DrawPlatformEffector2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::AreaEffector2DComponent")
			height = DrawAreaEffector2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::BuoyancyEffector2DComponent")
			height = DrawBuoyancyEffector2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::SurfaceEffector2DComponent")
			height = DrawSurfaceEffector2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::DistanceJoint2DComponent")
			height = DrawDistanceJoint2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::RevoluteJoint2DComponent")
			height = DrawRevoluteJoint2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::PrismaticJoint2DComponent")
			height = DrawPrismaticJoint2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::WheelJoint2DComponent")
			height = DrawWheelJoint2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::BoxCollider2DComponent")
			height = DrawBoxCollider2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::CircleCollider2DComponent")
			height = DrawCircleCollider2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::SpriteAnimatorComponent")
			height = DrawSpriteAnimatorInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::TilemapComponent")
			height = DrawTilemapInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::PixelPerfectCameraComponent")
			height = DrawPixelPerfectCameraInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::ParallaxLayerComponent")
			height = DrawParallaxLayerInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::PointLight2DComponent")
			height = DrawPointLight2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::GlobalLight2DComponent")
			height = DrawGlobalLight2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		else if (schema.Id.Name == "World::ShadowCaster2DComponent")
			height = DrawShadowCaster2DInspector(ctx, rect, instance, schema, visibleRect, &changedFields);
		// 自定义检查器:关卡数据层归属(勾选替代裸掩码数字);Play/Simulate 只读时退回普通行。
		else if (schema.Id.Name == "World::LevelEntityComponent" && !m_ReadOnly)
			height = DrawLevelEntityInspector(ctx, rect, instance, schema, visibleRect, &changedFields);


		else
			height = DrawSchemaFields(ctx, base, rect, instance, schema.DisplayName, schema, visibleRect,
				&changedFields);

		RegisterPrefabOverrides(entity, changedFields);
		return height;
	}


	// ---- 2026-09-26 脚本组件重写:统一脚本检视器(两个脚本组件共用)----
	//
	// 布局:脚本引用行 → 状态行(State + LastError,Luau 再加 ReloadDiagnostic)→ 属性表
	//      (每个 `PropertyNode` 一行)→ 动作行(Luau 的 Reload,id 保持 `lua.reload`)。
	//
	// 三条硬口径(方案 v2 §3 + 派工单):
	//   ① **编辑态不实例化脚本**:属性直接读写组件里的 `Properties`;不建 VM、不跑 OnCreate、
	//      不构造脚本实例(实例只由 Scene 在 Play/Simulate 按 schema 工厂创建);
	//   ② **Play/Simulate 只读**:控件走既有只读行 / 禁用按钮契约 + 一行只读说明;
	//      C++ 组件在运行实例在场时按**实例**读真实值(只读展示),Luau 用组件里保存的值;
	//   ③ **属性行复用既有类型化行渲染**:每条属性造一条临时 `FieldSchema`(Get/Set 直连属性值),
	//      逐个调用 `DrawSchemaFields` —— 控件 / 只读行 / 无障碍节点 / 悬停说明 / 预制体覆盖登记
	//      都不另写一套。
	//
	// 无障碍 id 契约:`properties.<组件名>.<属性名>`(脚本可直接算);状态与诊断行见各段注释。
float PropertiesPanel::DrawScriptComponentInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity, void* instance, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect, std::vector<std::string>* changedFields){
		(void)ctx;
		(void)rect;
		(void)entity;
		(void)instance;
		(void)schema;
		(void)visibleRect;
		(void)changedFields;
		return 0.0f;
	}


float PropertiesPanel::DrawTransformInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, TransformComponent& transform, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect, std::vector<std::string>* changedFields){
		const Wui::WuiTheme& theme = m_Host.Theme();
		const std::string& typeName = schema.DisplayName;
		const bool writable = !m_ReadOnly;
		// 自定义检查器也走同一份字段标签(schema 字段名 → 显示文案),id 仍是 PropPath。
		const Wui::LocalizedLabel locationLabel = SchemaFieldLabel(schema, "Location");
		const Wui::LocalizedLabel rotationLabel = SchemaFieldLabel(schema, "Rotation");
		const Wui::LocalizedLabel scaleLabel = SchemaFieldLabel(schema, "Scale");

		if (!writable)
		{
			// Play/Simulate:只读展示(与通用只读字段同一契约:properties.<...> 文本节点)。
			DrawReadOnlyRow(ctx, PropPath(typeName, "Location"), ComponentRect(rect, 0, 20),
				locationLabel, FormatFloatText(transform.Location.x, 2) + ", "
					+ FormatFloatText(transform.Location.y, 2) + ", " + FormatFloatText(transform.Location.z, 2), theme);
			const glm::vec3 degrees = glm::degrees(transform.GetEulerAngles());
			DrawReadOnlyRow(ctx, PropPath(typeName, "Rotation"), ComponentRect(rect, 1, 20),
				rotationLabel, FormatFloatText(degrees.x, 2) + ", " + FormatFloatText(degrees.y, 2) + ", "
					+ FormatFloatText(degrees.z, 2), theme);
			DrawReadOnlyRow(ctx, PropPath(typeName, "Scale"), ComponentRect(rect, 2, 20),
				scaleLabel, FormatFloatText(transform.Scale.x, 2) + ", " + FormatFloatText(transform.Scale.y, 2) + ", "
					+ FormatFloatText(transform.Scale.z, 2), theme);
			return 66.0f;
		}

		bool changed = false;
		// Rotation 面板按度数显示;写回统一走 SetTransform(同步四元数与状态)。
		glm::vec3 rotationDegrees = glm::degrees(transform.GetEulerAngles());
		// 逐行取"这一行是否被编辑":覆盖登记要精确到 Location/Rotation/Scale,
		// 不能只记"Transform 动过"(否则覆盖计数与实际改动对不上)。
		// 行高由库件的横排/竖排决定(窄控件列竖排 = 3 倍行高),下一行用上一行的返回值累加;
		// 行内无障碍节点由库件登记(前面板自算的 reachable 不再被向量行使用)。
		bool locationChanged = false;
		const float locationHeight = DrawVec3Row(ctx, PropPath(typeName, "Location"), rect.X, rect.Y, rect.W,
			locationLabel, transform.Location, theme, locationChanged);
		bool rotationChanged = false;
		const float rotationHeight = DrawVec3Row(ctx, PropPath(typeName, "Rotation"), rect.X,
			rect.Y + locationHeight, rect.W, rotationLabel, rotationDegrees, theme, rotationChanged);
		bool scaleChanged = false;
		const float scaleHeight = DrawVec3Row(ctx, PropPath(typeName, "Scale"), rect.X,
			rect.Y + locationHeight + rotationHeight, rect.W, scaleLabel, transform.Scale, theme, scaleChanged);
		changed |= locationChanged || rotationChanged || scaleChanged;
		if (changed)
		{
			transform.SetTransform(transform.Location, glm::radians(rotationDegrees), transform.Scale);
			m_Host.MarkDocumentDirty();
			if (changedFields)
			{
				if (locationChanged) changedFields->push_back(typeName + ".Location");
				if (rotationChanged) changedFields->push_back(typeName + ".Rotation");
				if (scaleChanged) changedFields->push_back(typeName + ".Scale");
			}
		}
		return locationHeight + rotationHeight + scaleHeight;
	}


// 关卡数据层归属:勾选式(掩码数字 → 层的名字)。层的定义来自**关卡 manifest**(host 提供),
// 所以没打开关卡时本函数不可用(调用方已按 !m_ReadOnly 分派,这里再兜一层 host 判定)。
float PropertiesPanel::DrawLevelEntityInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect, std::vector<std::string>* changedFields){
		const Wui::WuiTheme& theme = m_Host.Theme();

		std::vector<Level::DataLayerDefinition> layers;
		if (!m_Host.GetEditableDataLayers(&layers) || layers.empty())
			return 0.0f;   // 没打开关卡 ⇒ 0 行,由调用方保证这里不会被走到(退化为普通行)

		auto* component = static_cast<LevelEntityComponent*>(instance);
		if (!component)
			return 0.0f;

		const Schema::FieldSchema* maskField = nullptr;
		for (const Schema::FieldSchema& field : schema.Fields)
			if (field.Name == "DataLayerMask")
				maskField = &field;
		if (!maskField)
			return 0.0f;

		const std::string& typeName = schema.DisplayName;
		const auto reachable = [&visibleRect](const Wui::WuiRect& control)
		{
			return control.X + control.W * 0.5f >= visibleRect.X
				&& control.X + control.W * 0.5f <= visibleRect.X + visibleRect.W
				&& control.Y + control.H * 0.5f >= visibleRect.Y
				&& control.Y + control.H * 0.5f <= visibleRect.Y + visibleRect.H;
		};
		const auto markField = [changedFields, &typeName]()
		{
			if (changedFields)
				changedFields->push_back(typeName + ".DataLayerMask");
		};

		float y = 0.0f;
		uint32_t mask = component->DataLayerMask;
		const uint32_t original = mask;

		// 标题行:说明这一区是"关卡数据层归属",末尾给出原始掩码(排查时与 manifest 对得上)。
		const auto hex32 = [](uint32_t value)
		{
			static const char* digits = "0123456789ABCDEF";
			std::string text;
			for (int shift = 28; shift >= 0; shift -= 4)
				text.push_back(digits[(value >> shift) & 0xF]);
			return text;
		};
		Wui::Label(ctx, { rect.X + 4.0f, rect.Y + y + 3.0f },
			Wui::Tr("panel.properties.level_layers", "Level data layers"),
			theme.TextMuted, 13.0f);
		Wui::Label(ctx, { rect.X + rect.W - 74.0f, rect.Y + y + 3.0f }, "0x" + hex32(mask),
			theme.TextMuted, 12.0f);
		y += kRowHeight;

		// 一层一个勾选框;勾选 = 该位。(0 = 常驻可见,由最后那一行表达。)
		for (const Level::DataLayerDefinition& layer : layers)
		{
			const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, kRowHeight };
			if (reachable(row))
			{
				bool on = (mask & layer.LayerMask) != 0;
				const std::string label = layer.Name.empty()
					? ("Layer" + std::to_string(layer.LayerMask)) : layer.Name;
				const std::string term = "mask " + std::to_string(layer.LayerMask);
				const Wui::WuiRect box { row.X + 8.0f, row.Y + 2.0f, row.W - 16.0f, row.H - 4.0f };
				if (Wui::Checkbox(ctx,
					Wui::HashId(("panel.properties.level_layers." + std::to_string(layer.LayerMask)).c_str()),
					box, label, term, on, theme))
				{
					mask = on ? (mask | layer.LayerMask) : (mask & ~layer.LayerMask);
				}
			}
			y += kRowHeight;
		}

		// "常驻可见"(掩码 0):不属于任何层的实体在**所有**层组合下都可见。
		{
			const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, kRowHeight };
			if (reachable(row))
			{
				bool always = mask == 0;
				const Wui::WuiRect box { row.X + 8.0f, row.Y + 2.0f, row.W - 16.0f, row.H - 4.0f };
				if (Wui::Checkbox(ctx, Wui::HashId("panel.properties.level_layers.always"),
					box, Wui::Tr("panel.properties.level_always", "Always visible (no layer)"),
					always, theme))
				{
					mask = always ? 0u : 1u;   // 取消"常驻" ⇒ 回到常驻层(第一个层位)
				}
			}
			y += kRowHeight;
		}

		if (mask != original)
		{
			component->DataLayerMask = mask;
			markField();
		}
		return y;
	}


float PropertiesPanel::DrawCameraInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect, std::vector<std::string>* changedFields){
		const Wui::WuiTheme& theme = m_Host.Theme();
		// CameraComponent 的 schema 字段:Primary / FixedAspectRatio / Camera(Object Of CameraSettings)。
		const Schema::FieldSchema* primaryField = nullptr;
		const Schema::FieldSchema* fixedField = nullptr;
		const Schema::FieldSchema* cameraField = nullptr;
		for (const Schema::FieldSchema& field : schema.Fields)
		{
			if (field.Name == "Primary") primaryField = &field;
			else if (field.Name == "FixedAspectRatio") fixedField = &field;
			else if (field.K == Schema::Kind::Object) cameraField = &field;
		}
		void* cameraInstance = cameraField && cameraField->GetPtr ? cameraField->GetPtr(instance) : nullptr;
		const Schema::TypeSchema* cameraSchema = cameraField && cameraField->GetNested ? cameraField->GetNested() : nullptr;
		if (!primaryField || !fixedField || !cameraInstance || !cameraSchema)
			return 0;
		auto* camera = static_cast<CameraSettings*>(cameraInstance);

		const std::string& typeName = schema.DisplayName;
		const std::string& cameraType = cameraSchema->DisplayName;
		const bool writable = !m_ReadOnly;
		const auto reachable = [&visibleRect](const Wui::WuiRect& control)
		{
			return control.X + control.W * 0.5f >= visibleRect.X
				&& control.X + control.W * 0.5f <= visibleRect.X + visibleRect.W
				&& control.Y + control.H * 0.5f >= visibleRect.Y
				&& control.Y + control.H * 0.5f <= visibleRect.Y + visibleRect.H;
		};
		float y = 0.0f;
		bool changed = false;
		// 覆盖字段名与属性行 id 同一口径(PropPath 去掉 "properties." 前缀)。
		const auto markField = [changedFields](const std::string& field)
		{
			if (changedFields)
				changedFields->push_back(field);
		};

		const Wui::WuiRect primaryRow { rect.X, rect.Y + y, rect.W, kRowHeight };
		const Wui::LocalizedLabel primaryLabel = SchemaFieldLabel(*primaryField);
		if (writable)
		{
			bool primary = std::get<bool>(primaryField->Get(instance));
			const bool before = primary;
			// 复选框列固定在 +140(标签列宽 140):术语对照预算 136,不压到控件上。
			Wui::LabelWithTerm(ctx, { primaryRow.X + 4, primaryRow.Y + 3 }, primaryLabel.Text, primaryLabel.Term,
				theme.TextMuted, 13.0f, theme, 136.0f);
			Wui::Checkbox(ctx, Wui::HashId(PropPath(typeName, "Primary").c_str()),
				{ primaryRow.X + 140.0f, primaryRow.Y + 1, 20, 20 }, "", primary, theme);
			const Wui::WuiRect primaryBox { primaryRow.X + 140.0f, primaryRow.Y + 1, 20, 20 };
			RegisterNode(Wui::HashId(PropPath(typeName, "Primary").c_str()), "checkbox",
				primaryBox, TermText(primaryLabel), primary ? "true" : "false", reachable(primaryBox));
			if (primary != before)
			{
				primaryField->Set(instance, Schema::Value(primary));
				changed = true;
				markField(typeName + ".Primary");
			}
		}
		else
		{
			DrawReadOnlyRow(ctx, PropPath(typeName, "Primary"), primaryRow, primaryLabel,
				std::get<bool>(primaryField->Get(instance)) ? "true" : "false", theme);
		}
		y += kRowHeight;

		const Wui::WuiRect fixedRow { rect.X, rect.Y + y, rect.W, kRowHeight };
		const Wui::LocalizedLabel fixedLabel = SchemaFieldLabel(*fixedField);
		if (writable)
		{
			bool fixed = std::get<bool>(fixedField->Get(instance));
			const bool before = fixed;
			Wui::LabelWithTerm(ctx, { fixedRow.X + 4, fixedRow.Y + 3 }, fixedLabel.Text, fixedLabel.Term,
				theme.TextMuted, 13.0f, theme, 136.0f);
			Wui::Checkbox(ctx, Wui::HashId(PropPath(typeName, "FixedAspectRatio").c_str()),
				{ fixedRow.X + 140.0f, fixedRow.Y + 1, 20, 20 }, "", fixed, theme);
			const Wui::WuiRect fixedBox { fixedRow.X + 140.0f, fixedRow.Y + 1, 20, 20 };
			RegisterNode(Wui::HashId(PropPath(typeName, "FixedAspectRatio").c_str()), "checkbox",
				fixedBox, TermText(fixedLabel), fixed ? "true" : "false", reachable(fixedBox));
			if (fixed != before)
			{
				fixedField->Set(instance, Schema::Value(fixed));
				changed = true;
				markField(typeName + ".FixedAspectRatio");
			}
		}
		else
		{
			DrawReadOnlyRow(ctx, PropPath(typeName, "FixedAspectRatio"), fixedRow, fixedLabel,
				std::get<bool>(fixedField->Get(instance)) ? "true" : "false", theme);
		}
		y += kRowHeight;

		const Schema::FieldSchema* projectionField = nullptr;
		for (const Schema::FieldSchema& field : cameraSchema->Fields)
			if (field.K == Schema::Kind::Enum)
			{
				projectionField = &field;
				break;
			}
		const std::string projectionId = PropPath(cameraType, "ProjectionType");
		const Wui::WuiRect projectionRow { rect.X, rect.Y + y, rect.W, kRowHeight };
		const Wui::LocalizedLabel projectionLabel = Wui::TrLabel("panel.properties.camera.projection", "Projection");
		CameraSettings::ProjectionType projection = camera->m_ProjectionType;
		if (projectionField && projectionField->GetEnum && projectionField->Set && writable)
		{
			const Schema::EnumSchema* enumSchema = projectionField->GetEnum();
			const int64_t raw = enumSchema ? EnumRawOf(*enumSchema, projectionField->Get(cameraInstance)) : 0;
			std::vector<std::string> names;
			int selected = 0;
			if (enumSchema)
			{
				for (size_t i = 0; i < enumSchema->Values.size(); ++i)
				{
					// 选项文案可本地化;写回仍按下标取 enumSchema->Values[i].second(raw 枚举值)。
					names.push_back(CameraProjectionLabel(enumSchema->Values[i].first));
					if (enumSchema->Values[i].second == raw)
						selected = static_cast<int>(i);
				}
			}
			Wui::LabelWithTerm(ctx, { projectionRow.X + 4, projectionRow.Y + 3 }, projectionLabel.Text,
				projectionLabel.Term, theme.TextMuted, 13.0f, theme, 136.0f);
			const Wui::WuiRect comboRect { projectionRow.X + 140.0f, projectionRow.Y + 1, projectionRow.W - 144.0f, 20 };
			if (Wui::Combo(ctx, Wui::HashId(projectionId.c_str()), comboRect, "", names, selected, theme))
			{
				projectionField->Set(cameraInstance, Schema::Value(enumSchema->Values[selected].second));
				projection = camera->m_ProjectionType;
				changed = true;
				markField(cameraType + ".ProjectionType");
			}
			RegisterNode(Wui::HashId(projectionId.c_str()), "combo", comboRect, TermText(projectionLabel),
				(selected >= 0 && selected < static_cast<int>(names.size())) ? names[selected] : std::string(),
				reachable(comboRect));
		}
		else
		{
			DrawReadOnlyRow(ctx, projectionId, projectionRow, projectionLabel,
				CameraProjectionLabel(projection == CameraSettings::ProjectionType::Perspective ? "Perspective" : "Orthographic"),
				theme);
		}
		y += kRowHeight;

		// 按当前投影类型只显示对应参数(迁移前语义),直接写 CameraSettings 字段提交;
		// 投影矩阵由 CameraSystem 按参数指纹自动重算,写入者不需要补算。
		if (projection == CameraSettings::ProjectionType::Perspective)
		{
			float fov = camera->m_PerspectiveFOV;
			const Wui::WuiRect fovRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool fovChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.FOV"),
				fovRow, Wui::TrLabel("panel.properties.camera.fov", "FOV"), fov, 1.0f, -1.0f, theme, reachable(fovRow));
			y += kRowHeight;
			float nearClip = camera->m_PerspectiveNearClip;
			const Wui::WuiRect nearRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool nearChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.NearClip"),
				nearRow, Wui::TrLabel("panel.properties.camera.near_clip", "NearClip"), nearClip, 1.0f, -1.0f, theme,
				reachable(nearRow));
			y += kRowHeight;
			float farClip = camera->m_PerspectiveFarClip;
			const Wui::WuiRect farRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool farChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.FarClip"),
				farRow, Wui::TrLabel("panel.properties.camera.far_clip", "FarClip"), farClip, 1.0f, -1.0f, theme,
				reachable(farRow));
			y += kRowHeight;
			if (fovChanged)
			{
				camera->m_PerspectiveFOV = fov;
				changed = true;
				markField(cameraType + ".Perspective.FOV");
			}
			if (nearChanged)
			{
				camera->m_PerspectiveNearClip = nearClip;
				changed = true;
				markField(cameraType + ".Perspective.NearClip");
			}
			if (farChanged)
			{
				camera->m_PerspectiveFarClip = farClip;
				changed = true;
				markField(cameraType + ".Perspective.FarClip");
			}
		}
		else
		{
			float zoom = camera->m_OrthographicZoom;
			const Wui::WuiRect zoomRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool zoomChanged = DrawFloatRow(ctx, PropPath(cameraType, "Orthographic.Zoom"),
				zoomRow, Wui::TrLabel("panel.properties.camera.zoom", "Zoom"), zoom, 1.0f, -1.0f, theme, reachable(zoomRow));
			y += kRowHeight;
			float nearClip = camera->m_OrthographicNearClip;
			const Wui::WuiRect nearRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool nearChanged = DrawFloatRow(ctx, PropPath(cameraType, "Orthographic.NearClip"),
				nearRow, Wui::TrLabel("panel.properties.camera.near_clip", "NearClip"), nearClip, 1.0f, -1.0f, theme,
				reachable(nearRow));
			y += kRowHeight;
			float farClip = camera->m_OrthographicFarClip;
			const Wui::WuiRect farRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool farChanged = DrawFloatRow(ctx, PropPath(cameraType, "Orthographic.FarClip"),
				farRow, Wui::TrLabel("panel.properties.camera.far_clip", "FarClip"), farClip, 1.0f, -1.0f, theme,
				reachable(farRow));
			y += kRowHeight;
			if (zoomChanged)
			{
				camera->m_OrthographicZoom = zoom;
				changed = true;
				markField(cameraType + ".Orthographic.Zoom");
			}
			if (nearChanged)
			{
				camera->m_OrthographicNearClip = nearClip;
				changed = true;
				markField(cameraType + ".Orthographic.NearClip");
			}
			if (farChanged)
			{
				camera->m_OrthographicFarClip = farClip;
				changed = true;
				markField(cameraType + ".Orthographic.FarClip");
			}
		}

		if (changed)
			m_Host.MarkDocumentDirty();
		return y + 2.0f;
	}


float PropertiesPanel::DrawAudioSourceInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* comp = static_cast<World::AudioSourceComponent*>(instance);
	std::string soundPath;
	if (comp && comp->Sound.HasPath())
	{
		soundPath = StringPool::Get().PathOf(comp->Sound.Path);
	}

	bool hasSound = !soundPath.empty();
	std::string soundEventId = soundPath;

	if (hasSound)
	{
		auto& audio = Audio::AudioSystem::Get();
		if (!audio.FindSoundEvent(soundEventId))
		{
			std::filesystem::path p(soundPath);
			std::string stem = p.stem().string();
			if (audio.FindSoundEvent(stem))
			{
				soundEventId = stem;
			}
			else
			{
				std::ifstream in(p);
				if (!in.is_open() && !p.is_absolute())
				{
					std::filesystem::path candidate = std::filesystem::current_path() / "projects" / "default" / p;
					if (std::filesystem::exists(candidate)) in.open(candidate);
				}
				if (in.is_open())
				{
					std::stringstream ss;
					ss << in.rdbuf();
					Audio::SoundEvent ev;
					if (ev.DeserializeYaml(ss.str()))
					{
						audio.RegisterSoundEvent(ev);
						soundEventId = ev.GetId();
					}
				}
			}
		}
	}

	std::string soundDisplay = hasSound ? soundEventId : "(未指定声音)";
	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "绑定声音: " + soundDisplay, hasSound ? theme.Accent : theme.TextMuted, 11.5f);
	y += 22.0f;

	const Wui::WuiRect btnPlay { rect.X + 12.0f, y, 80.0f, 24.0f };
	const Wui::WuiRect btnStop { rect.X + 98.0f, y, 80.0f, 24.0f };

	std::string playTooltip = hasSound ? ("试听声音: " + soundEventId) : "未指定声音，请先绑定声音资产！";

	Wui::Tooltip(ctx, btnPlay, playTooltip);
	if (Wui::Button(ctx, Wui::HashId("prop.audiosrc.play"), btnPlay, "▶ 试听", theme))
	{
		if (hasSound)
		{
			auto& audio = Audio::AudioSystem::Get();
			if (comp->Spatial3D)
			{
				audio.PostEvent(soundEventId, glm::vec3(0.0f), comp->Volume, comp->Pitch);
			}
			else
			{
				audio.PostEvent2D(soundEventId, comp->Volume, comp->Pitch);
			}
		}
	}

	Wui::Tooltip(ctx, btnStop, "停止试听声音");
	if (Wui::Button(ctx, Wui::HashId("prop.audiosrc.stop"), btnStop, "■ 停止", theme))
	{
		Audio::AudioSystem::Get().StopAll(0.05f);
	}

	float noticeY = y + 28.0f;
	if (hasSound)
	{
		std::string statusText = "● 状态: 已就绪" + std::string(comp->Spatial3D ? " [3D 空间声源]" : " [2D 立体声]");
		Wui::Label(ctx, { rect.X + 14.0f, noticeY }, statusText, { 0.35f, 0.85f, 0.45f, 1.0f }, 10.5f);
	}

	return (noticeY + 20.0f) - rect.Y;
}

float PropertiesPanel::DrawSpriteInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* sprite = static_cast<World::SpriteComponent*>(instance);
	if (!sprite) return height;

	// Pivot Presets
	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "锚点快捷预设 (Pivot Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 90.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnCenter { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnFoot   { rect.X + 108.0f, y, btnW, btnH };
	const Wui::WuiRect btnTopLeft{ rect.X + 204.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnCenter, "设为几何中心 (0.5, 0.5)");
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.pivot_center"), btnCenter, "中心 (0.5, 0.5)", theme))
	{
		sprite->Pivot = { 0.5f, 0.5f };
		if (changedFields) changedFields->push_back("Pivot");
	}

	Wui::Tooltip(ctx, btnFoot, "设为脚底中心 (0.5, 0.0) —— 适用于角色与地面站立");
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.pivot_foot"), btnFoot, "脚底 (0.5, 0.0)", theme))
	{
		sprite->Pivot = { 0.5f, 0.0f };
		if (changedFields) changedFields->push_back("Pivot");
	}

	Wui::Tooltip(ctx, btnTopLeft, "设为左上角 (0.0, 1.0) —— 适用于 UI 或网格瓦片");
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.pivot_topleft"), btnTopLeft, "左上 (0.0, 1.0)", theme))
	{
		sprite->Pivot = { 0.0f, 1.0f };
		if (changedFields) changedFields->push_back("Pivot");
	}
	y += 28.0f;

	// Quick Flip Toggles
	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "图形翻转 (Quick Flip):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const Wui::WuiRect btnFlipX { rect.X + 12.0f, y, 110.0f, btnH };
	const Wui::WuiRect btnFlipY { rect.X + 128.0f, y, 110.0f, btnH };

	std::string flipXText = sprite->FlipX ? "✔ 水平翻转 (X)" : "水平翻转 (X)";
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.flip_x"), btnFlipX, flipXText, theme))
	{
		sprite->FlipX = !sprite->FlipX;
		if (changedFields) changedFields->push_back("FlipX");
	}

	std::string flipYText = sprite->FlipY ? "✔ 垂直翻转 (Y)" : "垂直翻转 (Y)";
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.flip_y"), btnFlipY, flipYText, theme))
	{
		sprite->FlipY = !sprite->FlipY;
		if (changedFields) changedFields->push_back("FlipY");
	}
	y += 28.0f;

	// Quick Sorting Layer Presets
	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "排序层预设 (Sorting Layer):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const Wui::WuiRect btnLyrBg  { rect.X + 12.0f, y, 70.0f, btnH };
	const Wui::WuiRect btnLyrDef { rect.X + 88.0f, y, 70.0f, btnH };
	const Wui::WuiRect btnLyrEnt { rect.X + 164.0f, y, 70.0f, btnH };
	const Wui::WuiRect btnLyrFg  { rect.X + 240.0f, y, 70.0f, btnH };

	if (Wui::Button(ctx, Wui::HashId("prop.sprite.lyr_bg"), btnLyrBg, "背景 (-10)", theme))
	{
		sprite->SortingLayer = -10;
		if (changedFields) changedFields->push_back("SortingLayer");
	}
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.lyr_def"), btnLyrDef, "默认 (0)", theme))
	{
		sprite->SortingLayer = 0;
		if (changedFields) changedFields->push_back("SortingLayer");
	}
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.lyr_ent"), btnLyrEnt, "实体 (5)", theme))
	{
		sprite->SortingLayer = 5;
		if (changedFields) changedFields->push_back("SortingLayer");
	}
	if (Wui::Button(ctx, Wui::HashId("prop.sprite.lyr_fg"), btnLyrFg, "前景 (20)", theme))
	{
		sprite->SortingLayer = 20;
		if (changedFields) changedFields->push_back("SortingLayer");
	}
	y += 28.0f;

	// Normal Map Selector (2D Lighting Relief)
	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "法线贴图 (Normal Map):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const std::vector<std::string>& texPaths = Editor::AssetCatalog::PathsForName("Texture2D");
	std::vector<std::string> normalMapOptions;
	normalMapOptions.reserve(texPaths.size() + 2);
	normalMapOptions.push_back("(None / 无法线贴图)");
	for (const auto& p : texPaths)
		normalMapOptions.push_back(p);

	const std::string currentNormalMap = sprite->NormalMap.IsValid()
		? World::StringPool::Get().NameOf(sprite->NormalMap)
		: std::string();
	int selectedNormalIdx = 0;
	if (!currentNormalMap.empty())
	{
		const auto found = std::find(normalMapOptions.begin() + 1, normalMapOptions.end(), currentNormalMap);
		if (found != normalMapOptions.end())
		{
			selectedNormalIdx = static_cast<int>(found - normalMapOptions.begin());
		}
		else
		{
			normalMapOptions.push_back(currentNormalMap);
			selectedNormalIdx = static_cast<int>(normalMapOptions.size()) - 1;
		}
	}

	const Wui::WuiRect normalComboRect { rect.X + 12.0f, y, std::max(140.0f, rect.W - 24.0f), 22.0f };
	Wui::Tooltip(ctx, normalComboRect, "选择用于 2D 动态光照凹凸受光的法线贴图资产 (prop.sprite.normal_map)");
	const int beforeNormalIdx = selectedNormalIdx;
	if (Wui::SearchableCombo(ctx, Wui::HashId("prop.sprite.normal_map"), normalComboRect, "",
		normalMapOptions, selectedNormalIdx, theme) && selectedNormalIdx != beforeNormalIdx)
	{
		if (selectedNormalIdx <= 0)
		{
			sprite->NormalMap = World::NameId{};
		}
		else
		{
			const std::string& selectedPath = normalMapOptions[static_cast<size_t>(selectedNormalIdx)];
			sprite->NormalMap = World::StringPool::Get().InternName(selectedPath);
		}
		m_Host.MarkDocumentDirty();
		if (changedFields) changedFields->push_back("NormalMap");
	}
	y += 28.0f;

	return (y + 10.0f) - rect.Y;
}

float PropertiesPanel::DrawCapsuleCollider2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* cap = static_cast<World::CapsuleCollider2DComponent*>(instance);
	if (!cap) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "胶囊体方向 (Direction):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 100.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnVert { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnHoriz{ rect.X + 118.0f, y, btnW, btnH };

	std::string vertText = cap->Direction == 0 ? "✔ 垂直 (Vertical)" : "垂直 (Vertical)";
	if (Wui::Button(ctx, Wui::HashId("prop.cap2d.vert"), btnVert, vertText, theme))
	{
		cap->Direction = 0;
		if (changedFields) changedFields->push_back("Direction");
	}

	std::string horizText = cap->Direction == 1 ? "✔ 水平 (Horizontal)" : "水平 (Horizontal)";
	if (Wui::Button(ctx, Wui::HashId("prop.cap2d.horiz"), btnHoriz, horizText, theme))
	{
		cap->Direction = 1;
		if (changedFields) changedFields->push_back("Direction");
	}
	y += 28.0f;

	const bool editingCollider = m_Host.IsEditingCollider();
	const Wui::WuiRect btnEditCol { rect.X + 12.0f, y, std::min(220.0f, rect.W - 24.0f), 24.0f };
	std::string editColText = editingCollider ? "✓ 退出碰撞体编辑 (Done)" : "✎ 视口编辑碰撞体 (Edit Collider)";
	Wui::Tooltip(ctx, btnEditCol, "在场景视口中直接拖动控制手柄交互式编辑碰撞体尺寸与偏移 (prop.cap2d.edit)");
	if (Wui::Button(ctx, Wui::HashId("prop.cap2d.edit"), btnEditCol, editColText, theme))
	{
		m_Host.SetEditingCollider(!editingCollider);
	}
	y += 28.0f;

	return (y + 10.0f) - rect.Y;
}

float PropertiesPanel::DrawBoxCollider2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	const bool editingCollider = m_Host.IsEditingCollider();
	const Wui::WuiRect btnEditCol { rect.X + 12.0f, y, std::min(220.0f, rect.W - 24.0f), 24.0f };
	std::string editColText = editingCollider ? "✓ 退出碰撞体编辑 (Done)" : "✎ 视口编辑碰撞体 (Edit Collider)";
	Wui::Tooltip(ctx, btnEditCol, "在场景视口中直接拖动控制手柄交互式编辑碰撞体尺寸与偏移 (prop.box2d.edit)");
	if (Wui::Button(ctx, Wui::HashId("prop.box2d.edit"), btnEditCol, editColText, theme))
	{
		m_Host.SetEditingCollider(!editingCollider);
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawCircleCollider2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	const bool editingCollider = m_Host.IsEditingCollider();
	const Wui::WuiRect btnEditCol { rect.X + 12.0f, y, std::min(220.0f, rect.W - 24.0f), 24.0f };
	std::string editColText = editingCollider ? "✓ 退出碰撞体编辑 (Done)" : "✎ 视口编辑碰撞体 (Edit Collider)";
	Wui::Tooltip(ctx, btnEditCol, "在场景视口中直接拖动控制手柄交互式编辑碰撞体尺寸与偏移 (prop.circle2d.edit)");
	if (Wui::Button(ctx, Wui::HashId("prop.circle2d.edit"), btnEditCol, editColText, theme))
	{
		m_Host.SetEditingCollider(!editingCollider);
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawSpriteAnimatorInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* anim = static_cast<World::SpriteAnimatorComponent*>(instance);
	if (!anim) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "动画播控 (Playback Controls):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 80.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnPlay { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnReset{ rect.X + 98.0f, y, btnW, btnH };

	std::string playText = anim->Playing ? "❚❚ 暂停" : "▶ 播放";
	if (Wui::Button(ctx, Wui::HashId("prop.anim.play"), btnPlay, playText, theme))
	{
		anim->Playing = !anim->Playing;
		if (anim->Playing) anim->Finished = false;
		if (changedFields) changedFields->push_back("Playing");
	}

	if (Wui::Button(ctx, Wui::HashId("prop.anim.reset"), btnReset, "■ 重置", theme))
	{
		anim->CurrentTime = 0.0f;
		anim->CurrentFrame = 0;
		anim->Finished = false;
		if (changedFields) { changedFields->push_back("CurrentTime"); changedFields->push_back("CurrentFrame"); }
	}
	y += 28.0f;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "循环模式 (Loop Mode):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const Wui::WuiRect btnLoop { rect.X + 12.0f, y, 70.0f, btnH };
	const Wui::WuiRect btnOnce { rect.X + 88.0f, y, 70.0f, btnH };
	const Wui::WuiRect btnPing { rect.X + 164.0f, y, 80.0f, btnH };

	std::string loopStr = anim->LoopMode == 0 ? "✔ 循环" : "循环";
	if (Wui::Button(ctx, Wui::HashId("prop.anim.mode_loop"), btnLoop, loopStr, theme))
	{
		anim->LoopMode = 0;
		if (changedFields) changedFields->push_back("LoopMode");
	}

	std::string onceStr = anim->LoopMode == 1 ? "✔ 单次" : "单次";
	if (Wui::Button(ctx, Wui::HashId("prop.anim.mode_once"), btnOnce, onceStr, theme))
	{
		anim->LoopMode = 1;
		if (changedFields) changedFields->push_back("LoopMode");
	}

	std::string pingStr = anim->LoopMode == 2 ? "✔ 往返" : "往返";
	if (Wui::Button(ctx, Wui::HashId("prop.anim.mode_ping"), btnPing, pingStr, theme))
	{
		anim->LoopMode = 2;
		if (changedFields) changedFields->push_back("LoopMode");
	}
	y += 28.0f;

	std::string statusMsg = anim->Playing ? ("● 播放中 | 进度: " + std::to_string(anim->CurrentTime).substr(0, 4) + "s | 帧 " + std::to_string(anim->CurrentFrame))
		: (anim->Finished ? "■ 播放已结束" : "❚❚ 已暂停");
	Wui::Label(ctx, { rect.X + 14.0f, y + 2.0f }, statusMsg, anim->Playing ? theme.Accent : theme.TextMuted, 10.5f);
	y += 20.0f;

	return y - rect.Y;
}

float PropertiesPanel::DrawTilemapInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* tmComp = static_cast<World::TilemapComponent*>(instance);
	if (!tmComp) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "网格拓扑形态 (Grid Topology):", theme.TextMuted, 11.0f);
	y += 20.0f;

	const float topoBtnW = 64.0f;
	const float topoBtnH = 22.0f;
	struct TopoPreset { const char* id; const char* label; uint8_t type; float cw; float ch; };
	const TopoPreset topos[4] = {
		{ "prop.tm.topo_ortho", "正交直角", 0, 1.0f, 1.0f },
		{ "prop.tm.topo_iso",   "等轴2.5D", 1, 1.5f, 0.75f },
		{ "prop.tm.topo_hexp",  "尖顶六边", 2, 1.0f, 1.0f },
		{ "prop.tm.topo_hexf",  "平顶六边", 3, 1.0f, 1.0f }
	};
	for (int i = 0; i < 4; ++i)
	{
		const Wui::WuiRect bRect { rect.X + 12.0f + i * (topoBtnW + 4.0f), y, topoBtnW, topoBtnH };
		auto btnTheme = theme;
		if (tmComp->GridType == topos[i].type)
		{
			btnTheme.ButtonBg = theme.ActiveBg;
			btnTheme.Border = theme.Accent;
		}
		if (Wui::Button(ctx, Wui::HashId(topos[i].id), bRect, topos[i].label, btnTheme))
		{
			tmComp->GridType = topos[i].type;
			tmComp->CellSize = { topos[i].cw, topos[i].ch };
			tmComp->ShowGrid = true;
			if (changedFields)
			{
				changedFields->push_back("GridType");
				changedFields->push_back("CellSize");
				changedFields->push_back("ShowGrid");
			}
		}
	}
	y += 28.0f;

	const std::string tmPath = tmComp->Tilemap.HasPath() ? StringPool::Get().PathOf(tmComp->Tilemap.Path) : "";

	const Wui::WuiRect btnOpen { rect.X + 12.0f, y, std::min(220.0f, rect.W - 24.0f), 24.0f };
	Wui::Tooltip(ctx, btnOpen, tmPath.empty() ? "请先绑定 .wtilemap 瓦片地图资产" : ("在瓦片地图工作台中打开: " + tmPath + " (prop.tilemap.open)"));
	if (Wui::Button(ctx, Wui::HashId("prop.tilemap.open"), btnOpen, "🗺 打开瓦片地图编辑器", theme))
	{
		if (!tmPath.empty())
			m_Host.OpenTilemapEditor(tmPath);
	}
	y += 28.0f;

	if (!tmPath.empty())
	{
		if (auto tm = Tilemap::GetOrLoad(tmPath))
		{
			const size_t mergedCount = tm->ComputeMergedColliderRects().size();
			std::string summary = "网格: " + std::to_string(tm->GetWidth()) + "x" + std::to_string(tm->GetHeight())
				+ " | 已绘瓦片: " + std::to_string(tm->CountPaintedTiles())
				+ " | 合并碰撞盒: " + std::to_string(mergedCount) + " 个";
			Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, summary, theme.Accent, 10.5f);
			y += 20.0f;
		}
	}

	return y - rect.Y;
}




float PropertiesPanel::DrawPixelPerfectCameraInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* ppc = static_cast<World::PixelPerfectCameraComponent*>(instance);
	if (!ppc) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "常用复古像素分辨率预设:", theme.TextMuted, 11.0f);
	y += 20.0f;

	const float btnW = 64.0f;
	const float btnH = 22.0f;
	struct ResPreset { const char* id; const char* label; int w; int h; };
	const ResPreset presets[4] = {
		{ "prop.ppc.320", "320×180", 320, 180 },
		{ "prop.ppc.640", "640×360", 640, 360 },
		{ "prop.ppc.256", "256×240", 256, 240 },
		{ "prop.ppc.160", "160×144", 160, 144 }
	};
	for (int i = 0; i < 4; ++i)
	{
		const Wui::WuiRect bRect { rect.X + 12.0f + static_cast<float>(i) * (btnW + 4.0f), y, btnW, btnH };
		if (Wui::Button(ctx, Wui::HashId(presets[i].id), bRect, presets[i].label, theme))
		{
			ppc->ReferenceResolution = { presets[i].w, presets[i].h };
			if (changedFields) changedFields->push_back("ReferenceResolution");
		}
	}
	y += 28.0f;

	std::string status = "当前整数倍缩放: " + std::to_string(ppc->CurrentScale) + "x | PPU: "
		+ std::to_string(ppc->PixelsPerUnit) + " px/m | 像素吸附: " + (ppc->SnapToPixelGrid ? "开启" : "关闭");
	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, status, theme.Accent, 10.5f);
	y += 20.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawParallaxLayerInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* plx = static_cast<World::ParallaxLayerComponent*>(instance);
	if (!plx) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "视差景深快速预设 (ScrollFactor):", theme.TextMuted, 11.0f);
	y += 20.0f;

	const float btnW = 68.0f;
	const float btnH = 22.0f;
	struct PlxPreset { const char* id; const char* label; float fx; float fy; };
	const PlxPreset presets[4] = {
		{ "prop.plx.sky", "天空 (0.0)", 0.0f, 0.0f },
		{ "prop.plx.far", "远景 (0.2)", 0.2f, 0.1f },
		{ "prop.plx.mid", "中景 (0.5)", 0.5f, 0.3f },
		{ "prop.plx.fg",  "近景 (1.2)", 1.2f, 1.0f }
	};
	for (int i = 0; i < 4; ++i)
	{
		const Wui::WuiRect bRect { rect.X + 12.0f + static_cast<float>(i) * (btnW + 4.0f), y, btnW, btnH };
		if (Wui::Button(ctx, Wui::HashId(presets[i].id), bRect, presets[i].label, theme))
		{
			plx->ScrollFactor = { presets[i].fx, presets[i].fy };
			if (changedFields) changedFields->push_back("ScrollFactor");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawPolygonCollider2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* poly = static_cast<World::PolygonCollider2DComponent*>(instance);
	if (!poly) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "多边形预设 (Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 60.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnTri   { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnQuad  { rect.X + 76.0f, y, btnW, btnH };
	const Wui::WuiRect btnPenta { rect.X + 140.0f, y, btnW, btnH };
	const Wui::WuiRect btnStar  { rect.X + 204.0f, y, btnW, btnH };

	if (Wui::Button(ctx, Wui::HashId("prop.poly2d.preset_tri"), btnTri, "三角形", theme))
	{
		poly->SetPoints({ { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.0f, 0.5f } });
		if (changedFields) changedFields->push_back("Points");
	}
	if (Wui::Button(ctx, Wui::HashId("prop.poly2d.preset_quad"), btnQuad, "矩形", theme))
	{
		poly->SetPoints({ { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } });
		if (changedFields) changedFields->push_back("Points");
	}
	if (Wui::Button(ctx, Wui::HashId("prop.poly2d.preset_penta"), btnPenta, "五边形", theme))
	{
		std::vector<glm::vec2> pts;
		for (int i = 0; i < 5; ++i)
		{
			float angle = glm::radians(90.0f + i * 72.0f);
			pts.push_back({ 0.5f * std::cos(angle), 0.5f * std::sin(angle) });
		}
		poly->SetPoints(pts);
		if (changedFields) changedFields->push_back("Points");
	}
	if (Wui::Button(ctx, Wui::HashId("prop.poly2d.preset_star"), btnStar, "五角星", theme))
	{
		std::vector<glm::vec2> pts;
		for (int i = 0; i < 10; ++i)
		{
			float angle = glm::radians(90.0f + i * 36.0f);
			float r = (i % 2 == 0) ? 0.6f : 0.25f;
			pts.push_back({ r * std::cos(angle), r * std::sin(angle) });
		}
		poly->SetPoints(pts);
		if (changedFields) changedFields->push_back("Points");
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawChainCollider2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* chain = static_cast<World::ChainCollider2DComponent*>(instance);
	if (!chain) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "地表轮廓预设 (Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 60.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnFlat   { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnSlope  { rect.X + 76.0f, y, btnW, btnH };
	const Wui::WuiRect btnValley { rect.X + 140.0f, y, btnW, btnH };
	const Wui::WuiRect btnWave   { rect.X + 204.0f, y, btnW, btnH };

	if (Wui::Button(ctx, Wui::HashId("prop.chain2d.preset_flat"), btnFlat, "平坦地", theme))
	{
		chain->SetPoints({ { -3.0f, 0.0f }, { -1.0f, 0.0f }, { 1.0f, 0.0f }, { 3.0f, 0.0f } });
		chain->Loop = false;
		if (changedFields) { changedFields->push_back("Points"); changedFields->push_back("Loop"); }
	}
	if (Wui::Button(ctx, Wui::HashId("prop.chain2d.preset_slope"), btnSlope, "斜坡", theme))
	{
		chain->SetPoints({ { -3.0f, -1.0f }, { -1.0f, 0.0f }, { 1.0f, 1.0f }, { 3.0f, 2.0f } });
		chain->Loop = false;
		if (changedFields) { changedFields->push_back("Points"); changedFields->push_back("Loop"); }
	}
	if (Wui::Button(ctx, Wui::HashId("prop.chain2d.preset_valley"), btnValley, "U形谷", theme))
	{
		chain->SetPoints({ { -3.0f, 2.0f }, { -1.5f, 0.0f }, { 1.5f, 0.0f }, { 3.0f, 2.0f } });
		chain->Loop = false;
		if (changedFields) { changedFields->push_back("Points"); changedFields->push_back("Loop"); }
	}
	if (Wui::Button(ctx, Wui::HashId("prop.chain2d.preset_wave"), btnWave, "波浪", theme))
	{
		chain->SetPoints({ { -3.0f, 0.0f }, { -1.5f, 1.0f }, { 0.0f, 0.0f }, { 1.5f, -1.0f }, { 3.0f, 0.0f } });
		chain->Loop = false;
		if (changedFields) { changedFields->push_back("Points"); changedFields->push_back("Loop"); }
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawPlatformEffector2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* effector = static_cast<World::PlatformEffector2DComponent*>(instance);
	if (!effector) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "单向跳板预设 (Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 96.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btn180 { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btn90  { rect.X + 114.0f, y, btnW, btnH };

	if (Wui::Button(ctx, Wui::HashId("prop.plat2d.preset_180"), btn180, "顶面半球 180°", theme))
	{
		effector->SurfaceArc = 180.0f;
		effector->RotationalOffset = 0.0f;
		effector->OneWay = true;
		if (changedFields) { changedFields->push_back("SurfaceArc"); changedFields->push_back("RotationalOffset"); changedFields->push_back("OneWay"); }
	}
	if (Wui::Button(ctx, Wui::HashId("prop.plat2d.preset_90"), btn90, "垂直顶面 90°", theme))
	{
		effector->SurfaceArc = 90.0f;
		effector->RotationalOffset = 0.0f;
		effector->OneWay = true;
		if (changedFields) { changedFields->push_back("SurfaceArc"); changedFields->push_back("RotationalOffset"); changedFields->push_back("OneWay"); }
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawPointLight2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* light = static_cast<World::PointLight2DComponent*>(instance);
	if (!light) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "2D 点光源快捷预设 (Light Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 84.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnTorch   { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnCrystal { rect.X + 102.0f, y, btnW, btnH };
	const Wui::WuiRect btnSpot    { rect.X + 192.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnTorch, "暖橙色全向火把光 (Intensity=1.8, OuterRadius=5.5m, 360°)");
	if (Wui::Button(ctx, Wui::HashId("prop.light2d.preset_torch"), btnTorch, "火把暖光", theme))
	{
		light->Color = { 1.0f, 0.62f, 0.22f };
		light->Intensity = 1.8f;
		light->InnerRadius = 0.4f;
		light->OuterRadius = 5.5f;
		light->InnerAngle = 360.0f;
		light->OuterAngle = 360.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Color");
			changedFields->push_back("Intensity");
			changedFields->push_back("InnerRadius");
			changedFields->push_back("OuterRadius");
			changedFields->push_back("InnerAngle");
			changedFields->push_back("OuterAngle");
		}
	}

	Wui::Tooltip(ctx, btnCrystal, "冰蓝色全向魔法晶石光 (Intensity=1.5, OuterRadius=4.5m, 360°)");
	if (Wui::Button(ctx, Wui::HashId("prop.light2d.preset_crystal"), btnCrystal, "魔法晶石", theme))
	{
		light->Color = { 0.25f, 0.82f, 1.0f };
		light->Intensity = 1.5f;
		light->InnerRadius = 0.3f;
		light->OuterRadius = 4.5f;
		light->InnerAngle = 360.0f;
		light->OuterAngle = 360.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Color");
			changedFields->push_back("Intensity");
			changedFields->push_back("InnerRadius");
			changedFields->push_back("OuterRadius");
			changedFields->push_back("InnerAngle");
			changedFields->push_back("OuterAngle");
		}
	}

	Wui::Tooltip(ctx, btnSpot, "高亮度聚光锥形手电筒 (Intensity=2.0, OuterRadius=8.0m, 32°~65°)");
	if (Wui::Button(ctx, Wui::HashId("prop.light2d.preset_spot"), btnSpot, "锥形手电", theme))
	{
		light->Color = { 0.95f, 0.96f, 0.85f };
		light->Intensity = 2.0f;
		light->InnerRadius = 0.2f;
		light->OuterRadius = 8.0f;
		light->InnerAngle = 32.0f;
		light->OuterAngle = 65.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Color");
			changedFields->push_back("Intensity");
			changedFields->push_back("InnerRadius");
			changedFields->push_back("OuterRadius");
			changedFields->push_back("InnerAngle");
			changedFields->push_back("OuterAngle");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawGlobalLight2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* globalLight = static_cast<World::GlobalLight2DComponent*>(instance);
	if (!globalLight) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "全局氛围光预设 (Atmosphere Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnW = 64.0f;
	const float btnH = 22.0f;
	const Wui::WuiRect btnDay     { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnDusk    { rect.X + 80.0f, y, btnW, btnH };
	const Wui::WuiRect btnDungeon { rect.X + 148.0f, y, btnW, btnH };
	const Wui::WuiRect btnNight   { rect.X + 216.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnDay, "晴朗白昼环境光 (Intensity=1.0, DirectionIntensity=0.15)");
	if (Wui::Button(ctx, Wui::HashId("prop.glight2d.preset_day"), btnDay, "晴朗白昼", theme))
	{
		globalLight->Color = { 1.0f, 1.0f, 1.0f };
		globalLight->Intensity = 1.0f;
		globalLight->DirectionIntensity = 0.15f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Color");
			changedFields->push_back("Intensity");
			changedFields->push_back("DirectionIntensity");
		}
	}

	Wui::Tooltip(ctx, btnDusk, "温暖黄昏斜射夕照 (Intensity=0.8, Direction=(-0.6, -0.8), DirectionIntensity=0.45)");
	if (Wui::Button(ctx, Wui::HashId("prop.glight2d.preset_dusk"), btnDusk, "黄昏夕照", theme))
	{
		globalLight->Color = { 1.0f, 0.72f, 0.48f };
		globalLight->Intensity = 0.8f;
		globalLight->Direction = { -0.6f, -0.8f };
		globalLight->DirectionIntensity = 0.45f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Color");
			changedFields->push_back("Intensity");
			changedFields->push_back("Direction");
			changedFields->push_back("DirectionIntensity");
		}
	}

	Wui::Tooltip(ctx, btnDungeon, "冷暗地牢氛围底光 (Intensity=0.45, DirectionIntensity=0.35)");
	if (Wui::Button(ctx, Wui::HashId("prop.glight2d.preset_dungeon"), btnDungeon, "幽暗地牢", theme))
	{
		globalLight->Color = { 0.22f, 0.25f, 0.35f };
		globalLight->Intensity = 0.45f;
		globalLight->DirectionIntensity = 0.35f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Color");
			changedFields->push_back("Intensity");
			changedFields->push_back("DirectionIntensity");
		}
	}

	Wui::Tooltip(ctx, btnNight, "静谧蓝调月夜环境光 (Intensity=0.55, Direction=(0.3, -0.95), DirectionIntensity=0.4)");
	if (Wui::Button(ctx, Wui::HashId("prop.glight2d.preset_night"), btnNight, "静谧月夜", theme))
	{
		globalLight->Color = { 0.35f, 0.48f, 0.75f };
		globalLight->Intensity = 0.55f;
		globalLight->Direction = { 0.3f, -0.95f };
		globalLight->DirectionIntensity = 0.4f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Color");
			changedFields->push_back("Intensity");
			changedFields->push_back("Direction");
			changedFields->push_back("DirectionIntensity");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawShadowCaster2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* caster = static_cast<World::ShadowCaster2DComponent*>(instance);
	if (!caster) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "遮光轮廓形态切换 (Hull Shape):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float btnH = 22.0f;
	const Wui::WuiRect btnShapeBox  { rect.X + 12.0f, y, 108.0f, btnH };
	const Wui::WuiRect btnShapePoly { rect.X + 126.0f, y, 124.0f, btnH };

	const std::string boxText = (caster->ShapeType == 0) ? "✔ 矩形 Box" : "矩形 Box";
	Wui::Tooltip(ctx, btnShapeBox, "使用 Size 宽高定义的矩形遮光轮廓 (ShapeType=0)");
	if (Wui::Button(ctx, Wui::HashId("prop.shadow2d.shape_box"), btnShapeBox, boxText, theme))
	{
		caster->ShapeType = 0;
		m_Host.MarkDocumentDirty();
		if (changedFields) changedFields->push_back("ShapeType");
	}

	const std::string polyText = (caster->ShapeType == 1) ? "✔ 多边形 Polygon" : "多边形 Polygon";
	Wui::Tooltip(ctx, btnShapePoly, "使用 Points 顶点列表定义的多边形遮光轮廓 (ShapeType=1)");
	if (Wui::Button(ctx, Wui::HashId("prop.shadow2d.shape_poly"), btnShapePoly, polyText, theme))
	{
		caster->ShapeType = 1;
		if (!caster->Points.IsValid())
		{
			caster->SetPoints({ { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } });
			if (changedFields) changedFields->push_back("Points");
		}
		m_Host.MarkDocumentDirty();
		if (changedFields) changedFields->push_back("ShapeType");
	}
	y += 28.0f;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "多边形轮廓预设 (Polygon Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float presetW = 76.0f;
	const Wui::WuiRect btnSquare  { rect.X + 12.0f, y, presetW, btnH };
	const Wui::WuiRect btnOctagon { rect.X + 94.0f, y, presetW, btnH };
	const Wui::WuiRect btnLWall   { rect.X + 176.0f, y, presetW, btnH };

	Wui::Tooltip(ctx, btnSquare, "生成 1×1 标准正方形遮光轮廓并切换为多边形模式");
	if (Wui::Button(ctx, Wui::HashId("prop.shadow2d.preset_square"), btnSquare, "正方形", theme))
	{
		caster->ShapeType = 1;
		caster->SetPoints({ { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } });
		m_Host.MarkDocumentDirty();
		if (changedFields) { changedFields->push_back("ShapeType"); changedFields->push_back("Points"); }
	}

	Wui::Tooltip(ctx, btnOctagon, "生成 8 顶点八角柱遮光轮廓并切换为多边形模式");
	if (Wui::Button(ctx, Wui::HashId("prop.shadow2d.preset_octagon"), btnOctagon, "八角柱", theme))
	{
		std::vector<glm::vec2> pts;
		pts.reserve(8);
		for (int i = 0; i < 8; ++i)
		{
			float angle = glm::radians(22.5f + static_cast<float>(i) * 45.0f);
			pts.push_back({ 0.5f * std::cos(angle), 0.5f * std::sin(angle) });
		}
		caster->ShapeType = 1;
		caster->SetPoints(pts);
		m_Host.MarkDocumentDirty();
		if (changedFields) { changedFields->push_back("ShapeType"); changedFields->push_back("Points"); }
	}

	Wui::Tooltip(ctx, btnLWall, "生成 6 顶点 L 形转角墙体遮光轮廓并切换为多边形模式");
	if (Wui::Button(ctx, Wui::HashId("prop.shadow2d.preset_lwall"), btnLWall, "L形墙体", theme))
	{
		caster->ShapeType = 1;
		caster->SetPoints({
			{ -0.5f, -0.5f },
			{  0.5f, -0.5f },
			{  0.5f, -0.15f },
			{ -0.15f, -0.15f },
			{ -0.15f,  0.5f },
			{ -0.5f,  0.5f }
		});
		m_Host.MarkDocumentDirty();
		if (changedFields) { changedFields->push_back("ShapeType"); changedFields->push_back("Points"); }
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawDistanceJoint2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* joint = static_cast<World::DistanceJoint2DComponent*>(instance);
	if (!joint) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "距离关节预设 (Distance Joint Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float gap = 6.0f;
	const float btnH = 22.0f;
	const float availW = std::max(270.0f, rect.W - 24.0f);
	const float btnW = (availW - gap * 2.0f) / 3.0f;
	const Wui::WuiRect btnRigid  { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnSpring { rect.X + 12.0f + btnW + gap, y, btnW, btnH };
	const Wui::WuiRect btnRope   { rect.X + 12.0f + (btnW + gap) * 2.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnRigid, "刚性硬杆: 刚性连接，无弹性 (EnableSpring=false, Hertz=0.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.dist2d.preset_rigid"), btnRigid, "钢制硬杆 (Rigid)", theme))
	{
		joint->EnableSpring = false;
		joint->Hertz = 0.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableSpring");
			changedFields->push_back("Hertz");
		}
	}

	Wui::Tooltip(ctx, btnSpring, "弹性弹簧: 5Hz 频率弹性阻尼连接 (EnableSpring=true, Hertz=5.0, DampingRatio=0.7)");
	if (Wui::Button(ctx, Wui::HashId("prop.dist2d.preset_spring"), btnSpring, "弹性弹簧 (Spring 5Hz)", theme))
	{
		joint->EnableSpring = true;
		joint->Hertz = 5.0f;
		joint->DampingRatio = 0.7f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableSpring");
			changedFields->push_back("Hertz");
			changedFields->push_back("DampingRatio");
		}
	}

	Wui::Tooltip(ctx, btnRope, "柔性绳索: 允许在 0~3m 范围内活动 (EnableLimit=true, MinDistance=0.0, MaxDistance=3.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.dist2d.preset_rope"), btnRope, "柔性绳索 (Limit)", theme))
	{
		joint->EnableLimit = true;
		joint->MinDistance = 0.0f;
		joint->MaxDistance = 3.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableLimit");
			changedFields->push_back("MinDistance");
			changedFields->push_back("MaxDistance");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawRevoluteJoint2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* joint = static_cast<World::RevoluteJoint2DComponent*>(instance);
	if (!joint) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "销轴铰链预设 (Revolute Joint Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float gap = 6.0f;
	const float btnH = 22.0f;
	const float availW = std::max(270.0f, rect.W - 24.0f);
	const float btnW = (availW - gap * 2.0f) / 3.0f;
	const Wui::WuiRect btnFree   { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnPend   { rect.X + 12.0f + btnW + gap, y, btnW, btnH };
	const Wui::WuiRect btnMotor  { rect.X + 12.0f + (btnW + gap) * 2.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnFree, "自由销轴: 360° 无约束自由旋转 (EnableLimit=false, EnableMotor=false)");
	if (Wui::Button(ctx, Wui::HashId("prop.revolute2d.preset_free"), btnFree, "自由销轴 360°", theme))
	{
		joint->EnableLimit = false;
		joint->EnableMotor = false;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableLimit");
			changedFields->push_back("EnableMotor");
		}
	}

	Wui::Tooltip(ctx, btnPend, "钟摆限位: 限制在 -45°~+45° 摆动 (EnableLimit=true, LowerAngle=-45.0, UpperAngle=45.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.revolute2d.preset_pendulum"), btnPend, "钟摆限位 ±45°", theme))
	{
		joint->EnableLimit = true;
		joint->LowerAngle = -45.0f;
		joint->UpperAngle = 45.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableLimit");
			changedFields->push_back("LowerAngle");
			changedFields->push_back("UpperAngle");
		}
	}

	Wui::Tooltip(ctx, btnMotor, "旋转电机: 180°/s 驱动电机 (EnableMotor=true, MotorSpeed=180.0, MaxMotorTorque=25.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.revolute2d.preset_motor"), btnMotor, "旋转电机 (180°/s)", theme))
	{
		joint->EnableMotor = true;
		joint->MotorSpeed = 180.0f;
		joint->MaxMotorTorque = 25.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableMotor");
			changedFields->push_back("MotorSpeed");
			changedFields->push_back("MaxMotorTorque");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawPrismaticJoint2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* joint = static_cast<World::PrismaticJoint2DComponent*>(instance);
	if (!joint) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "滑轨关节预设 (Prismatic Joint Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float gap = 6.0f;
	const float btnH = 22.0f;
	const float availW = std::max(270.0f, rect.W - 24.0f);
	const float btnW = (availW - gap * 2.0f) / 3.0f;
	const Wui::WuiRect btnRailX  { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnLiftY  { rect.X + 12.0f + btnW + gap, y, btnW, btnH };
	const Wui::WuiRect btnMotor  { rect.X + 12.0f + (btnW + gap) * 2.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnRailX, "水平滑轨: 沿 X 轴滑动 ±2.0m (Axis={1, 0}, LowerTranslation=-2.0, UpperTranslation=2.0, EnableLimit=true)");
	if (Wui::Button(ctx, Wui::HashId("prop.prismatic2d.preset_rail_x"), btnRailX, "水平滑轨 X", theme))
	{
		joint->Axis = { 1.0f, 0.0f };
		joint->LowerTranslation = -2.0f;
		joint->UpperTranslation = 2.0f;
		joint->EnableLimit = true;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Axis");
			changedFields->push_back("LowerTranslation");
			changedFields->push_back("UpperTranslation");
			changedFields->push_back("EnableLimit");
		}
	}

	Wui::Tooltip(ctx, btnLiftY, "垂直升降: 沿 Y 轴升降 -1.5m~3.0m (Axis={0, 1}, LowerTranslation=-1.5, UpperTranslation=3.0, EnableLimit=true)");
	if (Wui::Button(ctx, Wui::HashId("prop.prismatic2d.preset_lift_y"), btnLiftY, "垂直升降 Y", theme))
	{
		joint->Axis = { 0.0f, 1.0f };
		joint->LowerTranslation = -1.5f;
		joint->UpperTranslation = 3.0f;
		joint->EnableLimit = true;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Axis");
			changedFields->push_back("LowerTranslation");
			changedFields->push_back("UpperTranslation");
			changedFields->push_back("EnableLimit");
		}
	}

	Wui::Tooltip(ctx, btnMotor, "往复驱动电机: 2m/s 驱动电机 (EnableMotor=true, MotorSpeed=2.0, MaxMotorForce=100.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.prismatic2d.preset_motor"), btnMotor, "往复驱动电机", theme))
	{
		joint->EnableMotor = true;
		joint->MotorSpeed = 2.0f;
		joint->MaxMotorForce = 100.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableMotor");
			changedFields->push_back("MotorSpeed");
			changedFields->push_back("MaxMotorForce");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawWheelJoint2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* joint = static_cast<World::WheelJoint2DComponent*>(instance);
	if (!joint) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "车轮悬挂关节预设 (Wheel Joint Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float gap = 6.0f;
	const float btnH = 22.0f;
	const float availW = std::max(270.0f, rect.W - 24.0f);
	const float btnW = (availW - gap * 2.0f) / 3.0f;
	const Wui::WuiRect btnOffroad { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnSport   { rect.X + 12.0f + btnW + gap, y, btnW, btnH };
	const Wui::WuiRect btnMotor   { rect.X + 12.0f + (btnW + gap) * 2.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnOffroad, "越野车长行程悬挂 (Hertz=4.0, DampingRatio=0.7, LowerTranslation=-0.3, UpperTranslation=0.3)");
	if (Wui::Button(ctx, Wui::HashId("prop.wheel2d.preset_offroad"), btnOffroad, "越野车悬挂 (4Hz)", theme))
	{
		joint->Hertz = 4.0f;
		joint->DampingRatio = 0.7f;
		joint->LowerTranslation = -0.3f;
		joint->UpperTranslation = 0.3f;
		joint->EnableLimit = true;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Hertz");
			changedFields->push_back("DampingRatio");
			changedFields->push_back("LowerTranslation");
			changedFields->push_back("UpperTranslation");
			changedFields->push_back("EnableLimit");
		}
	}

	Wui::Tooltip(ctx, btnSport, "硬派跑车短行程硬悬挂 (Hertz=8.0, DampingRatio=0.85, LowerTranslation=-0.1, UpperTranslation=0.1)");
	if (Wui::Button(ctx, Wui::HashId("prop.wheel2d.preset_sport"), btnSport, "硬派跑车 (8Hz)", theme))
	{
		joint->Hertz = 8.0f;
		joint->DampingRatio = 0.85f;
		joint->LowerTranslation = -0.1f;
		joint->UpperTranslation = 0.1f;
		joint->EnableLimit = true;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Hertz");
			changedFields->push_back("DampingRatio");
			changedFields->push_back("LowerTranslation");
			changedFields->push_back("UpperTranslation");
			changedFields->push_back("EnableLimit");
		}
	}

	Wui::Tooltip(ctx, btnMotor, "车轮驱动电机: 720°/s 匀速旋转 (EnableMotor=true, MotorSpeed=720.0, MaxMotorTorque=30.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.wheel2d.preset_motor"), btnMotor, "车轮驱动电机", theme))
	{
		joint->EnableMotor = true;
		joint->MotorSpeed = 720.0f;
		joint->MaxMotorTorque = 30.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("EnableMotor");
			changedFields->push_back("MotorSpeed");
			changedFields->push_back("MaxMotorTorque");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}


float PropertiesPanel::DrawAreaEffector2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* effector = static_cast<World::AreaEffector2DComponent*>(instance);
	if (!effector) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "区域效应器预设 (Area Effector Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float gap = 6.0f;
	const float btnH = 22.0f;
	const float availW = std::max(270.0f, rect.W - 24.0f);
	const float btnW = (availW - gap) / 2.0f;

	const Wui::WuiRect btnUpdraft   { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnCrosswind { rect.X + 12.0f + btnW + gap, y, btnW, btnH };

	Wui::Tooltip(ctx, btnUpdraft, "上升气流: 垂直向上吹拂升力 (ForceDirection={0, 1}, ForceMagnitude=18.0, ForceMode=0 [方向风场])");
	if (Wui::Button(ctx, Wui::HashId("prop.area2d.preset_updraft"), btnUpdraft, "上升气流 (Updraft)", theme))
	{
		effector->ForceDirection = { 0.0f, 1.0f };
		effector->ForceMagnitude = 18.0f;
		effector->ForceMode = 0;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("ForceDirection");
			changedFields->push_back("ForceMagnitude");
			changedFields->push_back("ForceMode");
		}
	}

	Wui::Tooltip(ctx, btnCrosswind, "横向强风: 水平向右吹拂 (ForceDirection={1, 0}, ForceMagnitude=15.0, ForceMode=0 [方向风场])");
	if (Wui::Button(ctx, Wui::HashId("prop.area2d.preset_crosswind"), btnCrosswind, "横向强风 (Crosswind)", theme))
	{
		effector->ForceDirection = { 1.0f, 0.0f };
		effector->ForceMagnitude = 15.0f;
		effector->ForceMode = 0;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("ForceDirection");
			changedFields->push_back("ForceMagnitude");
			changedFields->push_back("ForceMode");
		}
	}
	y += 28.0f;

	const Wui::WuiRect btnAttractor { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnRepulsor  { rect.X + 12.0f + btnW + gap, y, btnW, btnH };

	Wui::Tooltip(ctx, btnAttractor, "引力黑洞: 向区域中心聚集吸引 (ForceMode=1 [点引力], ForceMagnitude=25.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.area2d.preset_attractor"), btnAttractor, "引力黑洞 (Attractor)", theme))
	{
		effector->ForceMode = 1;
		effector->ForceMagnitude = 25.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("ForceMode");
			changedFields->push_back("ForceMagnitude");
		}
	}

	Wui::Tooltip(ctx, btnRepulsor, "冲击排斥: 向外围爆发排斥 (ForceMode=2 [点斥力], ForceMagnitude=30.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.area2d.preset_repulsor"), btnRepulsor, "冲击排斥 (Repulsor)", theme))
	{
		effector->ForceMode = 2;
		effector->ForceMagnitude = 30.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("ForceMode");
			changedFields->push_back("ForceMagnitude");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawBuoyancyEffector2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* effector = static_cast<World::BuoyancyEffector2DComponent*>(instance);
	if (!effector) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "浮力水体预设 (Buoyancy Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float gap = 6.0f;
	const float btnH = 22.0f;
	const float availW = std::max(270.0f, rect.W - 24.0f);
	const float btnW = (availW - gap * 2.0f) / 3.0f;

	const Wui::WuiRect btnWater { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnRiver { rect.X + 12.0f + btnW + gap, y, btnW, btnH };
	const Wui::WuiRect btnMud   { rect.X + 12.0f + (btnW + gap) * 2.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnWater, "静止深水池: 常规清水平静水域 (Density=2.0, LinearDrag=2.5, AngularDrag=1.5, FlowVelocity={0, 0})");
	if (Wui::Button(ctx, Wui::HashId("prop.buoy2d.preset_water"), btnWater, "静止深水池", theme))
	{
		effector->Density = 2.0f;
		effector->LinearDrag = 2.5f;
		effector->AngularDrag = 1.5f;
		effector->FlowVelocity = { 0.0f, 0.0f };
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Density");
			changedFields->push_back("LinearDrag");
			changedFields->push_back("AngularDrag");
			changedFields->push_back("FlowVelocity");
		}
	}

	Wui::Tooltip(ctx, btnRiver, "向右急流河: 沿 X 轴向右流动的水流 (Density=1.8, LinearDrag=2.0, FlowVelocity={4.0, 0})");
	if (Wui::Button(ctx, Wui::HashId("prop.buoy2d.preset_river"), btnRiver, "向右急流河", theme))
	{
		effector->Density = 1.8f;
		effector->LinearDrag = 2.0f;
		effector->FlowVelocity = { 4.0f, 0.0f };
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Density");
			changedFields->push_back("LinearDrag");
			changedFields->push_back("FlowVelocity");
		}
	}

	Wui::Tooltip(ctx, btnMud, "高粘度泥浆: 高密度高粘滞阻尼泥泞环境 (Density=3.0, LinearDrag=6.0, AngularDrag=4.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.buoy2d.preset_mud"), btnMud, "高粘度泥浆", theme))
	{
		effector->Density = 3.0f;
		effector->LinearDrag = 6.0f;
		effector->AngularDrag = 4.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Density");
			changedFields->push_back("LinearDrag");
			changedFields->push_back("AngularDrag");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

float PropertiesPanel::DrawSurfaceEffector2DInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
	const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
	std::vector<std::string>* changedFields)
{
	float height = DrawSchemaFields(ctx, rect.Y, rect, instance, schema.DisplayName, schema, visibleRect, changedFields);
	float y = rect.Y + height + 6.0f;
	const Wui::WuiTheme& theme = m_Host.Theme();

	auto* effector = static_cast<World::SurfaceEffector2DComponent*>(instance);
	if (!effector) return height;

	Wui::Label(ctx, { rect.X + 12.0f, y + 2.0f }, "传送带预设 (Conveyor Presets):", theme.TextMuted, 11.5f);
	y += 20.0f;

	const float gap = 6.0f;
	const float btnH = 22.0f;
	const float availW = std::max(270.0f, rect.W - 24.0f);
	const float btnW = (availW - gap * 2.0f) / 3.0f;

	const Wui::WuiRect btnNormal  { rect.X + 12.0f, y, btnW, btnH };
	const Wui::WuiRect btnFast    { rect.X + 12.0f + btnW + gap, y, btnW, btnH };
	const Wui::WuiRect btnReverse { rect.X + 12.0f + (btnW + gap) * 2.0f, y, btnW, btnH };

	Wui::Tooltip(ctx, btnNormal, "常规传送带: 3.0m/s 标准传送 (Speed=3.0, ForceScale=1.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.surf2d.preset_normal"), btnNormal, "常规传送带 (3m/s)", theme))
	{
		effector->Speed = 3.0f;
		effector->ForceScale = 1.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Speed");
			changedFields->push_back("ForceScale");
		}
	}

	Wui::Tooltip(ctx, btnFast, "高速加速带: 8.0m/s 快速推进强力驱动 (Speed=8.0, ForceScale=2.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.surf2d.preset_fast"), btnFast, "高速加速带 (8m/s)", theme))
	{
		effector->Speed = 8.0f;
		effector->ForceScale = 2.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Speed");
			changedFields->push_back("ForceScale");
		}
	}

	Wui::Tooltip(ctx, btnReverse, "反向传送带: -3.0m/s 逆向传输 (Speed=-3.0, ForceScale=1.0)");
	if (Wui::Button(ctx, Wui::HashId("prop.surf2d.preset_reverse"), btnReverse, "反向传送带 (-3m/s)", theme))
	{
		effector->Speed = -3.0f;
		effector->ForceScale = 1.0f;
		m_Host.MarkDocumentDirty();
		if (changedFields)
		{
			changedFields->push_back("Speed");
			changedFields->push_back("ForceScale");
		}
	}
	y += 28.0f;

	return (y + 6.0f) - rect.Y;
}

}
