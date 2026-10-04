#include "PropertiesPanel_Internal.h"

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


float PropertiesPanel::DrawCameraInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect, std::vector<std::string>* changedFields){
		const Wui::WuiTheme& theme = m_Host.Theme();
		// CameraComponent 的 schema 字段:Primary / FixedAspectRatio / Camera(Object Of SceneCamera)。
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
		auto* camera = static_cast<SceneCamera*>(cameraInstance);

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
		SceneCamera::ProjectionType projection = camera->GetProjectionType();
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
				projection = camera->GetProjectionType();
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
				CameraProjectionLabel(projection == SceneCamera::ProjectionType::Perspective ? "Perspective" : "Orthographic"),
				theme);
		}
		y += kRowHeight;

		// 按当前投影类型只显示对应参数(迁移前语义),全部经 SceneCamera setter 提交。
		if (projection == SceneCamera::ProjectionType::Perspective)
		{
			float fov = camera->GetPerspectiveFOV();
			const Wui::WuiRect fovRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool fovChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.FOV"),
				fovRow, Wui::TrLabel("panel.properties.camera.fov", "FOV"), fov, 1.0f, -1.0f, theme, reachable(fovRow));
			y += kRowHeight;
			float nearClip = camera->GetPerspectiveNearClip();
			const Wui::WuiRect nearRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool nearChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.NearClip"),
				nearRow, Wui::TrLabel("panel.properties.camera.near_clip", "NearClip"), nearClip, 1.0f, -1.0f, theme,
				reachable(nearRow));
			y += kRowHeight;
			float farClip = camera->GetPerspectiveFarClip();
			const Wui::WuiRect farRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool farChanged = DrawFloatRow(ctx, PropPath(cameraType, "Perspective.FarClip"),
				farRow, Wui::TrLabel("panel.properties.camera.far_clip", "FarClip"), farClip, 1.0f, -1.0f, theme,
				reachable(farRow));
			y += kRowHeight;
			if (fovChanged)
			{
				camera->SetPerspectiveFOV(fov);
				changed = true;
				markField(cameraType + ".Perspective.FOV");
			}
			if (nearChanged)
			{
				camera->SetPerspectiveNearClip(nearClip);
				changed = true;
				markField(cameraType + ".Perspective.NearClip");
			}
			if (farChanged)
			{
				camera->SetPerspectiveFarClip(farClip);
				changed = true;
				markField(cameraType + ".Perspective.FarClip");
			}
		}
		else
		{
			float zoom = camera->GetOrthographicZoom();
			const Wui::WuiRect zoomRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool zoomChanged = DrawFloatRow(ctx, PropPath(cameraType, "Orthographic.Zoom"),
				zoomRow, Wui::TrLabel("panel.properties.camera.zoom", "Zoom"), zoom, 1.0f, -1.0f, theme, reachable(zoomRow));
			y += kRowHeight;
			float nearClip = camera->GetOrthographicNearClip();
			const Wui::WuiRect nearRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool nearChanged = DrawFloatRow(ctx, PropPath(cameraType, "Orthographic.NearClip"),
				nearRow, Wui::TrLabel("panel.properties.camera.near_clip", "NearClip"), nearClip, 1.0f, -1.0f, theme,
				reachable(nearRow));
			y += kRowHeight;
			float farClip = camera->GetOrthographicFarClip();
			const Wui::WuiRect farRow { rect.X, rect.Y + y, rect.W, kRowHeight };
			const bool farChanged = DrawFloatRow(ctx, PropPath(cameraType, "Orthographic.FarClip"),
				farRow, Wui::TrLabel("panel.properties.camera.far_clip", "FarClip"), farClip, 1.0f, -1.0f, theme,
				reachable(farRow));
			y += kRowHeight;
			if (zoomChanged)
			{
				camera->SetOrthographicZoom(zoom);
				changed = true;
				markField(cameraType + ".Orthographic.Zoom");
			}
			if (nearChanged)
			{
				camera->SetOrthographicNearClip(nearClip);
				changed = true;
				markField(cameraType + ".Orthographic.NearClip");
			}
			if (farChanged)
			{
				camera->SetOrthographicFarClip(farClip);
				changed = true;
				markField(cameraType + ".Orthographic.FarClip");
			}
		}

		if (changed)
			m_Host.MarkDocumentDirty();
		return y + 2.0f;
	}

}
