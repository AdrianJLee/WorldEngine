#include "wldpch.h"
#include "PropertiesPanel.h"

#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiWidgets.h"

namespace World
{
	namespace
	{
		// 只读展示用:把 schema 值渲染成一行文本(交互路径的控件不参与)。
		std::string FormatReadOnlyValue(const Schema::FieldSchema& field, const Schema::Value& value)
		{
			char buffer[160] = {};
			switch (field.K)
			{
				case Schema::Kind::Bool: return std::get<bool>(value) ? "true" : "false";
				case Schema::Kind::Int8: return std::to_string(std::get<int8_t>(value));
				case Schema::Kind::Int16: return std::to_string(std::get<int16_t>(value));
				case Schema::Kind::Int32: return std::to_string(std::get<int32_t>(value));
				case Schema::Kind::Int64: return std::to_string(std::get<int64_t>(value));
				case Schema::Kind::UInt8: return std::to_string(std::get<uint8_t>(value));
				case Schema::Kind::UInt16: return std::to_string(std::get<uint16_t>(value));
				case Schema::Kind::UInt32: return std::to_string(std::get<uint32_t>(value));
				case Schema::Kind::UInt64: return std::to_string(std::get<uint64_t>(value));
				case Schema::Kind::Float: std::snprintf(buffer, sizeof(buffer), "%.3f", std::get<float>(value)); return buffer;
				case Schema::Kind::Double: std::snprintf(buffer, sizeof(buffer), "%.3f", std::get<double>(value)); return buffer;
				case Schema::Kind::Vec2: { const glm::vec2 v = std::get<glm::vec2>(value); std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f)", v.x, v.y); return buffer; }
				case Schema::Kind::Vec3: { const glm::vec3 v = std::get<glm::vec3>(value); std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f, %.2f)", v.x, v.y, v.z); return buffer; }
				case Schema::Kind::Vec4: { const glm::vec4 v = std::get<glm::vec4>(value); std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f, %.2f, %.2f)", v.x, v.y, v.z, v.w); return buffer; }
				case Schema::Kind::String: return std::get<std::string>(value);
				case Schema::Kind::Enum: return std::to_string(std::get<int32_t>(value));
				default: return "(...)";   // 资产/对象/矩阵等:只读态给出占位,避免误导
			}
		}

		std::string ScriptStateName(ScriptInstanceState state)
		{
			switch (state)
			{
				case ScriptInstanceState::Pending: return "Pending";
				case ScriptInstanceState::Creating: return "Creating";
				case ScriptInstanceState::Running: return "Running";
				case ScriptInstanceState::Destroying: return "Destroying";
				case ScriptInstanceState::Stopped: return "Stopped";
				case ScriptInstanceState::Faulted: return "Faulted";
				default: return "?";
			}
		}

		// 字符串字段的编辑期缓冲区:Enter/失焦提交,Escape 丢弃。
		struct SchemaTextState
		{
			std::string Buffer;
			bool Editing = false;
		};
	}

	void PropertiesPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		// Play/Simulate = 只读查看:字段只显示不写回,并给出提示(用户确认的语义)。
		m_ReadOnly = host.IsReadOnlyMode();
		Entity entity = host.GetSelectedEntity();
		if (!entity.IsValid() || entity.GetScene() != host.GetActiveScene().get())
		{
			Label(ctx, { rect.X + 8, rect.Y + 8 }, "No entity selected", theme.TextMuted, 14.0f);
			return;
		}
		Scene* scene = entity.GetScene();
		Schema::SchemaRegistry& schemas = scene->GetContext().Schemas();
		if (m_ReadOnly)
			Label(ctx, { rect.X + 8, rect.Y + 8 }, "Play/Simulate 运行中:只读查看(暂停或退出后可编辑)",
				theme.TextMuted, 13.0f);

		const Wui::WuiRect addButton { rect.X + 8, rect.Y + (m_ReadOnly ? 30.0f : 8.0f), 140, 24 };
		if (!m_ReadOnly && Button(ctx, Wui::HashId("prop.add"), addButton, "Add Component", theme))
			ctx.OpenPopup(Wui::HashId("prop.add.popup"));

		const Wui::WuiId addPopup = Wui::HashId("prop.add.popup");
		if (ctx.IsPopupOpen(addPopup))
		{
			ctx.PushOverlay();
			std::vector<const Schema::TypeSchema*> candidates;
			for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
				if (schema && schema->Storage && !entity.HasComponent(schema->Storage->ComponentId))
					candidates.push_back(schema);
			const Wui::WuiRect panel { addButton.X, addButton.Y + addButton.H, 220, candidates.size() * 22.0f + 8 };
			DrawPanelSurface(ctx, panel, theme);
			for (size_t i = 0; i < candidates.size(); ++i)
			{
				const Wui::WuiRect item { panel.X + 4, panel.Y + 4 + i * 22, panel.W - 8, 22 };
				if (MenuItem(ctx, Wui::HashId(("prop.add." + candidates[i]->DisplayName).c_str()), item, candidates[i]->DisplayName, true, theme))
				{
					const uint32_t componentId = candidates[i]->Storage->ComponentId;
					const entt::entity handle = entity;
					if (scene->DeferStructuralChange([handle, componentId](Scene& s)
					{
						Entity target(&s, handle);
						if (target.IsValid() && target.CanAddComponent(componentId))
							target.AddComponent(componentId);
					}))
						host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
			}
			ctx.ClosePopupsOnOutsideClick({ addPopup }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(addPopup);
			ctx.PopOverlay();
		}

		// 组件分区进入保留模式布局树;字段内容复用已测的 schema 绘制逻辑。
		std::vector<const Schema::TypeSchema*> componentSchemas;
		for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
		{
			if (!schema || !schema->Storage || !entity.HasComponent(schema->Storage->ComponentId))
				continue;
			componentSchemas.push_back(schema);
		}

		std::vector<std::string> schemaNames;
		for (const Schema::TypeSchema* schema : componentSchemas)
			schemaNames.push_back(schema->DisplayName);
		if (!m_Root)
		{
			m_Root = std::make_shared<Wui::WuiBox>();
			m_Root->Direction = Wui::WuiDirection::Column;
			m_Root->Gap = 2;
		}
		if (schemaNames != m_LastSchemaNames)
		{
			m_LastSchemaNames = std::move(schemaNames);
			m_Sections.clear();
			m_Root = std::make_shared<Wui::WuiBox>();
			m_Root->Direction = Wui::WuiDirection::Column;
			m_Root->Gap = 2;
			for (const Schema::TypeSchema* schema : componentSchemas)
			{
				auto section = std::make_shared<Wui::WuiSection>();
				section->Title = schema->DisplayName;
				m_Root->Add(section);
				m_Sections.push_back({ schema->DisplayName, section });
			}
		}

		for (size_t i = 0; i < m_Sections.size(); ++i)
		{
			const Schema::TypeSchema* schema = componentSchemas[i];
			bool& open = ctx.Persist<bool>(Wui::HashId(("prop.open." + schema->DisplayName).c_str()), false);
			m_Sections[i].Section->Open = open;
			m_Sections[i].Section->DrawContent = [this, entity, schema, &m_HeightCache = m_Sections[i].Section](Wui::WuiContext& drawCtx, const Wui::WuiRect& inner)
			{
				const float height = DrawComponentInspector(drawCtx, { inner.X, inner.Y, inner.W, 0 }, entity, *schema);
				m_HeightCache->ContentHeight = height + 4.0f;
			};
		}

		Wui::LayoutWidgetTree(m_Root, { rect.X + 6, rect.Y + 40, rect.W - 12, rect.H - 40 });
		Wui::WuiPaintContext paint(ctx);
		m_Root->Paint(paint);
		for (size_t i = 0; i < m_Sections.size(); ++i)
		{
			const Schema::TypeSchema* schema = componentSchemas[i];
			bool& open = ctx.Persist<bool>(Wui::HashId(("prop.open." + schema->DisplayName).c_str()), false);
			open = m_Sections[i].Section->Open;
		}
	}

	float PropertiesPanel::DrawSchemaFields(Wui::WuiContext& ctx, Wui::WuiId base, const Wui::WuiRect& rect,
		void* instance, const std::string& typeName, const Schema::TypeSchema& schema)
	{
		const Wui::WuiTheme& theme = m_Host.Theme();
		float y = 0;
		bool changed = false;
		const float labelWidth = std::min(140.0f, rect.W * 0.45f);
		for (const Schema::FieldSchema& field : schema.Fields)
		{
			if (field.Meta.Transient)
				continue;
			const Wui::WuiId fid = Wui::HashId(("f." + typeName + "." + field.Name).c_str()) ^ base;
			const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, 22 };
			const Wui::WuiRect ctrl { row.X + labelWidth, row.Y + 1, row.W - labelWidth - 4, 20 };
			const std::string label = field.Meta.DisplayName.empty() ? field.Name : field.Meta.DisplayName;

			if (field.K == Schema::Kind::Object)
			{
				const Schema::TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
				void* nestedInstance = field.GetPtr ? field.GetPtr(instance) : nullptr;
				// UUID 等身份标识只读展示,不提供编辑控件。
				if (nested && nestedInstance && (nested->Id.Name == "World::UUID" || nested->DisplayName == "UUID"))
				{
					std::string display = "(invalid)";
					if (!nested->Fields.empty() && nested->Fields[0].Get)
					{
						const Schema::Value inner = nested->Fields[0].Get(nestedInstance);
						if (std::holds_alternative<uint64_t>(inner))
						{
							char buffer[32];
							std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(std::get<uint64_t>(inner)));
							display = buffer;
						}
					}
					Label(ctx, { row.X + 4, row.Y + 3 }, label, theme.TextMuted, 13.0f);
					Label(ctx, { ctrl.X, row.Y + 3 }, display, theme.Text, 13.0f);
					y += 20;
					continue;
				}
				bool& open = ctx.Persist<bool>(fid, false);
				if (ctx.IsClicked(row))
					open = !open;
				Label(ctx, { row.X + 4, row.Y + 3 }, (open ? "- " : "+ ") + label, theme.Text, 13.0f);
				y += 20;
				if (open && nested && nestedInstance)
					y += DrawSchemaFields(ctx, fid ^ 0x9e3779b9u, { row.X + 10, row.Y + 20, row.W - 10, 0 }, nestedInstance, nested->DisplayName, *nested);
				continue;
			}

			if (m_ReadOnly || field.Meta.ReadOnly || !field.Get || !field.Set)
			{
				// 只读也要显示"值":否则 Play/Simulate 下属性面板只剩字段名,看起来像"什么都不显示"。
				std::string text = label;
				if (field.Get)
					text += ": " + FormatReadOnlyValue(field, field.Get(instance));
				Label(ctx, { row.X + 4, row.Y + 3 }, text, theme.TextMuted, 13.0f);
				y += 20;
				continue;
			}

			Label(ctx, { row.X + 4, row.Y + 3 }, label, theme.TextMuted, 13.0f);
			Schema::Value value = field.Get(instance);
			bool fieldChanged = false;
			switch (field.K)
			{
				case Schema::Kind::Bool:
				{
					bool b = std::get<bool>(value);
					const bool before = b;
					Checkbox(ctx, fid, ctrl, "", b, theme);
					fieldChanged = b != before;
					if (fieldChanged) value = b;
					break;
				}
				case Schema::Kind::Int8:
				case Schema::Kind::Int16:
				case Schema::Kind::Int32:
				case Schema::Kind::Int64:
				{
					int64_t raw = field.K == Schema::Kind::Int8 ? std::get<int8_t>(value)
						: field.K == Schema::Kind::Int16 ? std::get<int16_t>(value)
						: field.K == Schema::Kind::Int32 ? std::get<int32_t>(value) : std::get<int64_t>(value);
					const int64_t before = raw;
					const int64_t lo = field.Meta.Min.has_value() ? static_cast<int64_t>(*field.Meta.Min) : INT64_MIN;
					const int64_t hi = field.Meta.Max.has_value() ? static_cast<int64_t>(*field.Meta.Max) : INT64_MAX;
					DragInt(ctx, fid, ctrl, raw, lo, hi, theme);
					fieldChanged = raw != before;
					if (fieldChanged)
					{
						if (field.K == Schema::Kind::Int8) value = static_cast<int8_t>(raw);
						else if (field.K == Schema::Kind::Int16) value = static_cast<int16_t>(raw);
						else if (field.K == Schema::Kind::Int32) value = static_cast<int32_t>(raw);
						else value = raw;
					}
					break;
				}
				case Schema::Kind::UInt8:
				case Schema::Kind::UInt16:
				case Schema::Kind::UInt32:
				case Schema::Kind::UInt64:
				{
					uint64_t raw = field.K == Schema::Kind::UInt8 ? std::get<uint8_t>(value)
						: field.K == Schema::Kind::UInt16 ? std::get<uint16_t>(value)
						: field.K == Schema::Kind::UInt32 ? std::get<uint32_t>(value) : std::get<uint64_t>(value);
					int64_t signedRaw = static_cast<int64_t>(raw);
					const int64_t before = signedRaw;
					const int64_t lo = field.Meta.Min.has_value() ? static_cast<int64_t>(*field.Meta.Min) : 0;
					const int64_t hi = field.Meta.Max.has_value() ? static_cast<int64_t>(*field.Meta.Max) : INT64_MAX;
					DragInt(ctx, fid, ctrl, signedRaw, lo, hi, theme);
					fieldChanged = signedRaw != before;
					if (fieldChanged)
					{
						if (field.K == Schema::Kind::UInt8) value = static_cast<uint8_t>(signedRaw);
						else if (field.K == Schema::Kind::UInt16) value = static_cast<uint16_t>(signedRaw);
						else if (field.K == Schema::Kind::UInt32) value = static_cast<uint32_t>(signedRaw);
						else value = static_cast<uint64_t>(signedRaw);
					}
					break;
				}
				case Schema::Kind::Float:
				case Schema::Kind::Double:
				{
					float f = field.K == Schema::Kind::Float ? std::get<float>(value) : static_cast<float>(std::get<double>(value));
					const float before = f;
					const float lo = field.Meta.Min.has_value() ? *field.Meta.Min : 1.0f;   // 1,-1 哨兵 = 无范围
					const float hi = field.Meta.Max.has_value() ? *field.Meta.Max : -1.0f;
					DragFloat(ctx, fid, ctrl, f, 0.01f, lo, hi, theme);
					fieldChanged = f != before;
					if (fieldChanged) value = field.K == Schema::Kind::Float ? Schema::Value(f) : Schema::Value(static_cast<double>(f));
					break;
				}
				case Schema::Kind::Vec2:
				case Schema::Kind::Vec3:
				case Schema::Kind::Vec4:
				{
					const int components = field.K == Schema::Kind::Vec2 ? 2 : (field.K == Schema::Kind::Vec3 ? 3 : 4);
					const float slot = ctrl.W / components;
					for (int c = 0; c < components; ++c)
					{
						float f = field.K == Schema::Kind::Vec2 ? std::get<glm::vec2>(value)[c]
							: field.K == Schema::Kind::Vec3 ? std::get<glm::vec3>(value)[c] : std::get<glm::vec4>(value)[c];
						const float before = f;
						DragFloat(ctx, fid ^ static_cast<Wui::WuiId>(c + 1), { ctrl.X + slot * c, ctrl.Y, slot - 2, ctrl.H }, f, 0.01f, 1.0f, -1.0f, theme);
						if (f != before)
						{
							fieldChanged = true;
							if (field.K == Schema::Kind::Vec2) std::get<glm::vec2>(value)[c] = f;
							else if (field.K == Schema::Kind::Vec3) std::get<glm::vec3>(value)[c] = f;
							else std::get<glm::vec4>(value)[c] = f;
						}
					}
					break;
				}
				case Schema::Kind::String:
				case Schema::Kind::Asset:
				{
					// 与 TextField 的 WuiEditState 共用 fid 会导致类型混淆,
					// 编辑缓冲必须使用独立 id。
					auto& state = ctx.Persist<SchemaTextState>(Wui::HashId("schema.text.state") ^ fid, {});
					const std::string current = std::get<std::string>(value);
					if (!state.Editing)
						state.Buffer = current;
					bool cancelled = false;
					if (TextField(ctx, fid, ctrl, state.Buffer, theme, &cancelled))
					{
						// Enter 提交
						if (state.Editing && state.Buffer != current)
						{
							value = state.Buffer;
							fieldChanged = true;
						}
						state.Editing = false;
					}
					else if (state.Editing)
					{
						if (cancelled)
						{
							// Escape 丢弃
							state.Editing = false;
						}
						else if (ctx.Focus() != fid)
						{
							// 失焦提交
							if (state.Buffer != current)
							{
								value = state.Buffer;
								fieldChanged = true;
							}
							state.Editing = false;
						}
					}
					// 本次点击进入编辑
					if (!state.Editing && ctx.Focus() == fid)
						state.Editing = true;
					break;
				}
				case Schema::Kind::Enum:
				{
					const Schema::EnumSchema* es = field.GetEnum ? field.GetEnum() : nullptr;
					if (es)
					{
						std::vector<std::string> names;
						int selected = 0;
						const int64_t raw = es->IsSigned ? std::get<int64_t>(value) : static_cast<int64_t>(std::get<uint64_t>(value));
						for (size_t i = 0; i < es->Values.size(); ++i)
						{
							names.push_back(es->Values[i].first);
							if (es->Values[i].second == raw)
								selected = static_cast<int>(i);
						}
						const int before = selected;
						Combo(ctx, fid, ctrl, "", names, selected, theme);
						fieldChanged = selected != before;
						if (fieldChanged)
							value = es->IsSigned ? Schema::Value(es->Values[selected].second) : Schema::Value(static_cast<uint64_t>(es->Values[selected].second));
					}
					break;
				}
				default:
					Label(ctx, { ctrl.X, ctrl.Y + 3 }, "(unsupported)", theme.TextMuted, 12.0f);
					break;
			}
			if (fieldChanged)
			{
				field.Set(instance, value);
				changed = true;
			}
			y += 22;
		}

		if (changed)
		{
			if (typeName == "TransformComponent")
				static_cast<TransformComponent*>(instance)->RecalculateTransform();
			else if (typeName == "SceneCamera")
				static_cast<SceneCamera*>(instance)->ApplyEdit();
			m_Host.MarkDocumentDirty();
		}
		return y;
	}

	float PropertiesPanel::DrawComponentInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity, const Schema::TypeSchema& schema)
	{
		const Wui::WuiTheme& theme = m_Host.Theme();
		void* instance = entity.GetComponent(schema.Storage->ComponentId);
		if (!instance)
			return 0;
		const Wui::WuiId base = Wui::HashId(schema.DisplayName.c_str());

		if (schema.Id.Name == "World::NativeScriptComponent")
		{
			auto* script = static_cast<NativeScriptComponent*>(instance);
			Scene* scene = entity.GetScene();
			Schema::SchemaRegistry& schemas = scene->GetContext().Schemas();
			std::vector<std::string> names;
			std::vector<const Schema::TypeSchema*> scripts = schemas.List(Schema::TypeCategory::Script);
			int selected = -1;
			for (size_t i = 0; i < scripts.size(); ++i)
			{
				names.push_back(scripts[i]->DisplayName);
				if (scripts[i]->DisplayName == script->ScriptName)
					selected = static_cast<int>(i);
			}
			if (Combo(ctx, base ^ 1u, { rect.X, rect.Y, rect.W, 22 }, "Script", names, selected, theme) && selected >= 0)
			{
				if (scripts[selected]->Script)
					scripts[selected]->Script->Bind(static_cast<void*>(script));
				script->ScriptName = names[selected];
				script->ResetEditorFieldState();
				m_Host.MarkDocumentDirty();
			}
			float y = 26;
			Label(ctx, { rect.X, rect.Y + y }, "state: " + ScriptStateName(script->State), theme.TextMuted, 13.0f);
			y += 18;
			if (!script->LastError.empty())
			{
				Label(ctx, { rect.X, rect.Y + y }, script->LastError, { 1, 0.4f, 0.4f, 1 }, 12.0f);
				y += 18;
			}
			const Schema::TypeSchema* scriptSchema = schemas.Find(script->ScriptName);
			if (scriptSchema)
			{
				bool owned = false;
				ScriptableEntity* preview = script->GetOrCreateEditorInstance(!scene->IsActive(), owned);
				if (preview)
				{
					y += DrawSchemaFields(ctx, base ^ 2u, { rect.X, rect.Y + y, rect.W, 0 }, preview, scriptSchema->DisplayName, *scriptSchema);
					for (const Schema::FieldSchema& field : scriptSchema->Fields)
						if (field.Get)
							script->FieldValues[field.Name] = field.Get(preview);
					script->ReleaseEditorInstance(preview);
				}
			}
			return y;
		}

		if (schema.Id.Name == "World::LuaScriptComponent")
		{
			auto* script = static_cast<LuaScriptComponent*>(instance);
			const std::string before = script->ScriptFilePath;
			TextField(ctx, base ^ 1u, { rect.X, rect.Y, rect.W, 22 }, script->ScriptFilePath, theme);
			if (script->ScriptFilePath != before)
				m_Host.MarkDocumentDirty();
			Label(ctx, { rect.X, rect.Y + 26 }, "state: " + ScriptStateName(script->State), theme.TextMuted, 13.0f);
			return 46;
		}

		return DrawSchemaFields(ctx, base, rect, instance, schema.DisplayName, schema);
	}
}
