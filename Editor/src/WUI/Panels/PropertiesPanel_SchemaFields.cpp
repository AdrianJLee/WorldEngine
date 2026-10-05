#include "PropertiesPanel_Internal.h"

namespace World
{

using namespace PropertiesPanelDetail;


	// ---- Schema 字段与容器绘制 ----

float PropertiesPanel::DrawSchemaFields(Wui::WuiContext& ctx, Wui::WuiId base, const Wui::WuiRect& rect, void* instance, const std::string& typeName, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect, std::vector<std::string>* changedFields, bool scriptPropertyRow, ScriptCollectionRows* collectionRows, int depth){
		const Wui::WuiTheme& theme = m_Host.Theme();
		float y = 0;
		bool changed = false;
		// ---- PURE-ECS:本帧从 arena 水合出的原生容器节点 ----
		// BuildScriptTableSchema 通过 `arena.Owners[]` 直接引用它们(绘制期间指针必须稳定)。折回 Value / 写回在**容器自己那一层**
		// (元素增删改都发生在那里,只有那一层知道 `changed`)。
		std::deque<PropertyNode> plainNodes;
		// VEC-H2:标签列宽来自库件(与 PropertyRow/PropertyGroupHeader 共用同一条口径),
		// 同一面板传同一值 ⇒ 标签列竖向对齐;面板不再自己算 0.45 倍。
		const float labelWidth = Wui::PropertyRowLabelWidth(rect);
		// VEC-H4:层级 = 标签文字缩进(每层 12px,4px 栅格)。**值列不跟着挪** —— 面板把同一个
		// labelWidth 传进每一行,缩进只作用在标签文字上,所以任意深度的字段列仍然竖向对齐。
		constexpr float kIndentPerLevel = 12.0f;
		const float labelIndent = static_cast<float>(depth) * kIndentPerLevel;
		// 稳定无障碍 id 契约:properties.<TypeDisplayName>.<字段名>(脚本用同样字符串算 HashId)。
		const auto propId = [](const std::string& type, const std::string& field)
		{ return "properties." + type + "." + field; };
		// 只登记"中心点落在面板可视区内"的控件:滚出去的控件保留节点但 visible=false,
		// 与控件的真实可点性一致(滚回来即可被 ui.invoke 命中)。
		const auto reachable = [&visibleRect](const Wui::WuiRect& control)
		{
			return control.X + control.W * 0.5f >= visibleRect.X
				&& control.X + control.W * 0.5f <= visibleRect.X + visibleRect.W
				&& control.Y + control.H * 0.5f >= visibleRect.Y
				&& control.Y + control.H * 0.5f <= visibleRect.Y + visibleRect.H;
		};
		// VEC-C2:数组/映射元素行的行尾 `-`(删除)。**只登记"这一行要删"**,真正的 erase /
		// 重排在本调用画完所有元素行之后统一做 —— 合成 schema 的元素访问器按编译期下标实例化,
		// 循环中途 erase 会让后面的行读到错元素(同帧一帧错位)。
		// 只读态不画增删控件(Play 里这些行本来就不出现;这条兜住 C++ 实例只读展示的路径)。
		const bool collectionWritable = collectionRows != nullptr && collectionRows->Writable
			&& collectionRows->Container != nullptr;
		std::string pendingEraseName;
		// VEC-H2:行尾 `-` 走库件 `Wui::CollectionActionButton`(方按钮 + 悬停/焦点/禁用态),
		// 落点 = 行矩形右侧的行外动作槽(容器已按 `CollectionActionColumnWidth()` 收窄子行)。
		const auto drawCollectionRemove = [&](const std::string& rowName, const Wui::WuiRect& row)
		{
			if (!collectionWritable)
				return;
			const Wui::WuiRect button { row.X + row.W + 2.0f, row.Y + (row.H - kRowHeight) * 0.5f, kRowHeight,
				kRowHeight };
			if (Wui::CollectionActionButton(ctx,
				Wui::HashId((collectionRows->IdText + ".remove." + rowName).c_str()), button, "-",
				Wui::Tr("panel.properties.collection_remove.tooltip",
					"Remove this element from the collection (the scene stores the list)"), true, theme))
				pendingEraseName = rowName;
		};
		// SCRIPT-V2:脚本属性行的说明 = 脚本自己的注释(`PropertyNode::Doc`,由调用方带进
		// `FieldSchema.Meta.Doc`)—— **不**回落 schema 的 `schema.field.<Name>.doc`;没写说明就回落
		// 类型文案(VEC-C2 / v4 §1),不假装有文档。普通 schema 字段行为不变(仍走 schema 本地化表)。
		const auto fieldDocFor = [&](const Schema::FieldSchema& candidate)
		{
			if (!scriptPropertyRow)
				return FieldDocLabel(schema, candidate);
			if (!candidate.Meta.Doc.empty())
				return candidate.Meta.Doc;
			// VEC-C2(方案 v4 §1):没有注解说明 → 回落成**类型文案**,不再给英文兜底文案。
			// 推导字段(`level` 这类)与数组/映射子行同样走这一条。
			return ScriptFieldTypeText(candidate);
		};
		// VEC-H6:当前行的声明节点(名字路径 = m_ScriptRowPath + 行名)。声明读不出来 = nullptr ——
		// 复位可见性退化成"只看值/Default",不猜形状。
		const auto scriptRowDeclaration = [this](const std::string& rowName)
			-> const ComponentPropertyModel::Declaration*
		{
			if (!m_ScriptDeclarationsValid)
				return nullptr;
			std::vector<std::string> path = m_ScriptRowPath;
			path.push_back(rowName);
			return ResolveDeclarationPath(m_ScriptDeclarations, path);
		};
		for (const Schema::FieldSchema& field : schema.Fields)
		{
			if (field.Meta.Transient)
				continue;
			const Wui::WuiId fid = Wui::HashId(("f." + typeName + "." + field.Name).c_str()) ^ base;
			const std::string rowIdText = propId(typeName, field.Name);
			const Wui::WuiId rowNodeId = Wui::HashId(rowIdText.c_str());
			// VEC-C2 / VEC-F2:**单项** `↺` 复位(叶子 / 数组元素 / 映射值行)。`modified` 语义 =
			// "编辑态可复位":Play/只读态用同一 rect 画禁用占位(库件两态共用同一几何,行布局零位移)。
			// 点中后只把**这一行**清成"未设" —— 显示由 `PropertyNodeDisplayValue` 回落脚本默认值,
			// 存档按 D1 判定"未设不写";不再触发整表重同步(那会把同一集合的增删按声明重建 =
			// 用户反馈的"点一个元素把整个集合都复原了")。
			const Wui::WuiId resetId = Wui::HashId((rowIdText + ".reset").c_str());
			const bool resetEnabled = scriptPropertyRow && !m_ReadOnly && !field.Meta.ReadOnly;
			// PURE-ECS:原生容器行的 `↺` 回到**声明种子**(不再是"脚本默认值"),文案跟着分开,
			// 避免对着纯 ECS 组件说"脚本"。
			const std::string resetLabel = m_PlainContainerRows
				? Wui::Tr("panel.properties.plain_row_reset", "Reset this item to its declared default")
				: Wui::Tr("panel.properties.component_reset", "Reset this item to the script default");
			const std::string resetDoc = resetEnabled
				? Wui::Tr("panel.properties.component_reset.tooltip", ScriptItemResetDoc())
				: Wui::Tr("panel.properties.component_readonly_notice",
					"Play/Simulate: script properties are read-only (pause or stop to edit)");
			// VEC-H6:`↺` 只在"当前值/形状 != 脚本声明默认"时出现(用户口径:一致时不画,不是禁用态)。
			// 合成路径(编辑态 + Luau 只读)拿得到 PropertyNode;Play 里的 C++ 实例走真实结构体指针,
			// 没有 Value/Default 可比 —— 保持既有"画禁用占位 + 理由"的口径(判据不可用时不去猜)。
			const size_t fieldIndex = static_cast<size_t>(&field - schema.Fields.data());
			const PropertyNode* scriptRow = (scriptPropertyRow && m_ScriptInspectingScriptRows)
				? ScriptRowModel(schema, instance, fieldIndex, m_ScriptInspectingScriptRows) : nullptr;
			const bool resetModified = scriptRow
				? ScriptRowModified(*scriptRow, scriptRowDeclaration(field.Name)) : true;
			const auto applyItemReset = [&]()
			{
				if (!field.Set)
					return;
				// VEC-F2:单项复位 = 该行回到"未设",显示回落**脚本当前声明**里同名位置的默认值。
				// Luau 的数组在面板里 `+`/`-` 后会重排下标,行的旧 Default 会留在改名后的行上 ——
				// 所以先按声明刷新这一行的 Default;声明拿不到(C++ / 无 VM)才退回"只清 Value"。
				const bool declared = scriptPropertyRow && ApplyScriptRowDeclaredReset(field.Name);
				if (!declared)
					field.Set(instance, Schema::Value {});
				changed = true;
				if (changedFields)
					changedFields->push_back(typeName + "." + field.Name);
			};
			// 显示文案:普通字段 = Meta.DisplayName 优先,空则人类可读化 C++ 字段名后查目录;
			// **脚本属性行 = 脚本里的原始字段名,一律不过本地化表** —— 脚本字段不是 schema 字段,
			// 同名查 `schema.field.*` 会串台(用户实测:脚本字段 `Speed` 显示成别的组件的「速度」)。
			// 行 id(fid / propId)与持久化仍用 field.Name,不受影响。
			const Wui::LocalizedLabel label = scriptPropertyRow
				? Wui::LocalizedLabel { field.Name, std::string() }
				: SchemaFieldLabel(field);
			// 无障碍节点 label 按约定写成 "中文 (English)";显示值/句子本身不加英文。
			const std::string labelText = TermText(label);

			if (field.K == Schema::Kind::Object)
			{
				// PURE-ECS:原生容器行会把本行(含子树)临时切进脚本行渲染路径。RAII 在本次循环
				// 迭代结束(含 `continue`)时还原,所以不需要在每个出口写还原代码。
				struct PlainContainerScopeGuard
				{
					bool& Plain;
					bool& ScriptRow;
					bool PlainSaved;
					bool ScriptRowSaved;
					PlainContainerScopeGuard(bool& plain, bool& scriptRow)
						: Plain(plain), ScriptRow(scriptRow), PlainSaved(plain), ScriptRowSaved(scriptRow) {}
					~PlainContainerScopeGuard()
					{
						Plain = PlainSaved;
						ScriptRow = ScriptRowSaved;
					}
				} plainScopeGuard(m_PlainContainerRows, scriptPropertyRow);
				const std::string& idText = rowIdText;
				const Schema::TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
				void* nestedInstance = field.GetPtr ? field.GetPtr(instance) : nullptr;
				// ---- PURE-ECS:原生组件的容器字段(Array/Map)----
				// 生成的容器访问器把字段做成 `K = Object` + `Collection != None`,而
				// `GetPtr`/`GetNested` **都是空** ⇒ 天然落进下面那条"只读摘要"分支,容器字段
				// 在纯 ECS 组件里完全没有编辑入口。这里先把实例值水合成行模型,后面按
				// `scriptPropertyRow = true` 复用脚本行那一整套渲染(元素行 / `+` / `-` / 键输入)。
				//
				// 行 id 必须与字段自己的 `rowIdText` 一致,否则元素行的 `+`/`-` 动作与元素控件
				// 会落到 `properties.<类型>.<字段>.<子字段>` 之外的前缀上(见 BuildScriptTableSchema
				// 用 DisplayName 当递归 id 前缀)。容器节点的行名就是字段名(管道两侧同名约定)。
				const bool plainContainerField = !scriptPropertyRow && m_ContainerElementSchemas != nullptr && !nested
					&& field.Collection != Schema::CollectionKind::None && field.K == Schema::Kind::Object;
				if (plainContainerField)
				{
					plainNodes.push_back(HydratePlainContainer(field, field.Get(instance), m_ContainerElementSchemas));
					PropertyNode& containerNode = plainNodes.back();
					containerNode.Name = field.Name;
					nestedInstance = &containerNode;
					// 合成器只对外给「字段」入口(MakeScriptTableField),容器需要的嵌套节点从它的
					// GetNested() 取(指针落在 arena 里,绘制期间稳定)。arena 满 = 退化成只读摘要行。
					const Schema::FieldSchema containerField =
						MakeScriptTableField(containerNode, rowIdText, m_ContainerElementSchemas, &field);
					nested = containerField.GetNested ? containerField.GetNested() : nullptr;
					// 这一行(及其子树)切到**脚本行渲染路径**:分组头 + 元素行 + 行外 `+`/`-`
					// 就是容器需要的全部交互,原生路径只是喂给它不同的数据源。
					// `m_ScriptInspectingScriptRows` 保持 false ⇒ 复位/声明默认那几条消费脚本模型的
					// 分支不会打开;`m_PlainContainerRows` 让子层知道该按 schema 补元素子行、并且
					// 折回 Value 时不做"场景是否记录过"的判定。
					scriptPropertyRow = true;
					m_PlainContainerRows = true;
				}
				// VEC-B3:脚本属性里的裸 table / 面板侧降级的结构化表 = 只读摘要行。
				// 这一支只对脚本属性行开放(`scriptPropertyRow`),普通 schema 的 Object 字段
				// 行为不变;摘要行不可展开、不可编辑、不进存档(值根本没有合成到这里)。
				if (scriptPropertyRow && (!nested || !nestedInstance))
				{
					const std::string summary = field.Meta.DisplayName.empty()
						? std::string("table") : field.Meta.DisplayName;
					const std::string summaryDoc = fieldDocFor(field);
					// 只读摘要行:行结构走属性行库件(内联值 + 悬停说明 + 行尾动作列),文本形态与旧口径一致。
					const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, kRowHeight };
					Wui::PropertyRowDesc desc;
					desc.Label = label.Text;
					desc.Term = label.Term;
					desc.Tooltip = summaryDoc;
					desc.A11yKind = "text";
					desc.A11yLabel = labelText;
					desc.A11yValue = summary;
					desc.A11yEnabled = false;
					desc.InlineValue = true;
					desc.InlineValueText = summary;
					desc.LabelWidth = labelWidth;
					desc.LabelIndent = labelIndent;
					Wui::PropertyRow(ctx, rowNodeId, row, desc, theme);
					drawCollectionRemove(field.Name, row);
					y += kRowHeight;
					continue;
				}
				// UUID 等身份标识只读展示,不提供编辑控件。
				if (nested && nestedInstance && (nested->Id.Name == "World::UUID" || nested->DisplayName == "UUID"))
				{
					std::string display = Wui::Tr("panel.properties.invalid_value", "(invalid)");
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
					const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, kRowHeight };
					Wui::PropertyRowDesc desc;
					desc.Label = label.Text;
					desc.Term = label.Term;
					desc.A11yKind = "text";
					desc.A11yLabel = labelText;
					desc.A11yValue = display;
					desc.A11yEnabled = false;
					desc.InlineValue = true;
					desc.InlineValueText = display;
					desc.LabelWidth = labelWidth;
					desc.LabelIndent = labelIndent;
					Wui::PropertyRow(ctx, rowNodeId, row, desc, theme);
					drawCollectionRemove(field.Name, row);
					y += kRowHeight;
					continue;
				}
				// VEC-H2:折叠分组头走库件 `Wui::PropertyGroupHeader`(展开标记 + 标签 + 悬停说明 +
				// 行尾集合复位;点复位不折叠)。语义与 id 契约与旧实现逐条一致。
				const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, kRowHeight };
				bool& open = ctx.Persist<bool>(fid, false);
				// VEC-F2:集合头 `↺`(数组 / 映射 / 结构化表的折叠头行)= 复原**整个集合** ——
				// 与单项 `↺` 同一图标,但文案/说明说清"整集合 + 丢弃所有增删改";VEC-H6 起
				// **单击即复原**(二次确认已删除),且只在集合偏离默认时出现(见 head.ResetModified)。
				// 只读/Play 画禁用占位并给只读理由(与叶子行同一套 disabled hint 口径)。
				const bool headResetDrawn = scriptPropertyRow && nested != nullptr && nestedInstance != nullptr;
				// `nestedInstance` 只有在合成属性表路径上才是 `PropertyNode*`(Play 里 C++ 实例走真实
				// 结构体指针)→ 形态/复位只对合成路径成立;Play 那一路只画禁用占位。
				const bool headScriptRow = headResetDrawn && m_ScriptInspectingScriptRows;
				// PURE-ECS:原生容器行同样走合成节点(值就是组件字段),但它没有"脚本声明的默认形状"
				// 可复原 ⇒ 只借形态/条数显示,复位整条不出现(点它没有可落地的语义)。
				const bool headPlainRow = headResetDrawn && m_PlainContainerRows;
				const bool headResettable = headScriptRow && !m_ReadOnly && !field.Meta.ReadOnly;
				// 集合头 `↺` 的 id 契约:`properties.<组件>.<属性>.reset`(与单项同一字符串,
				// 差别只在落点:集合头落在容器行,单项落在元素/键值行)。
				PropertyCollection headKind = PropertyCollection::Struct;
				if (headScriptRow || headPlainRow)
					headKind = static_cast<const PropertyNode*>(nestedInstance)->Collection;
				const std::string headLabel = ScriptCollectionHeadResetLabel(headKind);
				const std::string headDoc = headResettable
					? std::string(ScriptCollectionHeadResetDoc())
					: Wui::Tr("panel.properties.component_readonly_notice",
						"Play/Simulate: script properties are read-only (pause or stop to edit)");
				Wui::PropertyGroupHeaderDesc head;
				head.Label = label.Text;
				head.Term = label.Term;
				head.Tooltip = fieldDocFor(field);
				head.LabelWidth = labelWidth;
				head.LabelIndent = labelIndent;
				// VEC-H4:集合头右侧常驻"当前条数"(数组/映射/结构化表都算),折叠时也知道里面有几项。
				if ((headScriptRow || headPlainRow) && !field.Meta.ReadOnly)
				{
					const size_t childCount = static_cast<const PropertyNode*>(nestedInstance)->Children.size();
					head.Trailing = Wui::Tr("panel.properties.collection_count", "{n} item(s)");
					const std::string marker = "{n}";
					const size_t at = head.Trailing.find(marker);
					if (at != std::string::npos)
						head.Trailing.replace(at, marker.size(), std::to_string(childCount));
					else
						head.Trailing = std::to_string(childCount);
				}
				head.A11yLabel = labelText;
				head.Open = open;
				head.Enabled = reachable(row);
				head.ShowReset = headResetDrawn && !headPlainRow;
				head.ResetEnabled = headResettable;
				// VEC-H6:集合头 `↺` 只在"有任一元素/键值/形状偏离默认"时出现。
				head.ResetModified = headPlainRow ? false : resetModified;
				head.ResetId = resetId;
				head.ResetLabel = headLabel;
				head.ResetTooltip = headDoc;
				const Wui::PropertyGroupHeaderResult header =
					Wui::PropertyGroupHeader(ctx, rowNodeId, row, head, theme);
				if (header.Toggled)
					open = !open;
				if (header.ResetClicked && headResettable && !headPlainRow)
				{
					// VEC-H6:记下请求,等本组件画完再落地(同一帧后面的子行仍按旧 schema 画)。
					m_PendingCollectionReset = static_cast<PropertyNode*>(nestedInstance);
					m_PendingCollectionResetLuau = m_ScriptInspectingLuau;
					m_PendingCollectionResetPath = m_ScriptRowPath;
					m_PendingCollectionResetPath.push_back(field.Name);
				}
				drawCollectionRemove(field.Name, row);
				y += kRowHeight;
				if (open && nested && nestedInstance)
				{
					// VEC-C2:元素自身是数组/映射(嵌套集合,如 `{{number}}`)时,它的子行也要能增删;
					// 普通结构化表的子行不是集合行 —— 集合形态只从合成 arena 查(`None` = 不传上下文)。
					ScriptCollectionRows nestedRows;
					ScriptCollectionRows* nestedRowsPtr = nullptr;
					if (scriptPropertyRow)
					{
						// PURE-ECS:元素自身是容器(命名 struct 里的嵌套 Array/Map)分两种来源 ——
						// ① 节点在合成 arena 里(脚本行 / 原生容器的子节点),形态由 `Collections[]` 给出;
						// ② 原生容器路径下,元素自己的 `PropertyNode` 直接带 `Collection`(水合时从声明抄的),
						//    `Collections[]` 对它也命中,所以上一条已经覆盖 —— 这里只在 arena 未命中时兜底。
						PropertyCollection nestedCollection = ScriptTableCollectionOf(nested);
						// 兜底只对**合成节点**做(arena 里有它的 owner)。原生指针(Play 里的真实
						// 结构体)不在 arena 里 → 这里绝不强转成 PropertyNode*,与 ScriptRowModel 同一条防线。
						if (nestedCollection == PropertyCollection::None)
							if (const PropertyNode* owner = ScriptTableNodeOwner(nested))
								nestedCollection = owner->Collection;
						if (nestedCollection == PropertyCollection::Array
							|| nestedCollection == PropertyCollection::Map)
						{
							nestedRows.Container = static_cast<PropertyNode*>(nestedInstance);
							nestedRows.Kind = nestedCollection;
							nestedRows.IdText = idText;
							nestedRows.Writable = !m_ReadOnly;
							nestedRows.PlainRows = m_PlainContainerRows;
							nestedRows.ElementSchemas = m_ContainerElementSchemas;
							// 回写目标:本字段在本实例上的 setter(容器折回 Value 后写进结构体)。
							nestedRows.WriteInstance = instance;
							nestedRows.WriteField = &field;
							nestedRowsPtr = &nestedRows;
						}
					}
					const float actionReserve = nestedRowsPtr ? kCollectionActionWidth : 0.0f;
					if (m_ScriptInspectingScriptRows)
						m_ScriptRowPath.push_back(field.Name);   // 子行路径 = 容器路径 + 本行名
					y += DrawSchemaFields(ctx, fid ^ 0x9e3779b9u,
						{ row.X, row.Y + kRowHeight, std::max(40.0f, row.W - actionReserve), 0 },
						nestedInstance, nested->DisplayName, *nested, visibleRect, changedFields,
						scriptPropertyRow, nestedRowsPtr, depth + 1);
					if (m_ScriptInspectingScriptRows)
						m_ScriptRowPath.pop_back();
				}
				continue;
			}

			if (m_ReadOnly || field.Meta.ReadOnly || !field.Get || !field.Set)
			{
				const std::string docText = fieldDocFor(field);
				// 只读也要显示"值":否则 Play/Simulate 下属性面板只剩字段名,看起来像"什么都不显示"。
				const std::string display = field.Get ? FormatReadOnlyValue(field, field.Get(instance)) : std::string();
				// VEC-H2:只读行也走属性行库件(内联「标签: 值」+ 不可交互文本节点 + 禁用复位占位)。
				const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, kRowHeight };
				Wui::PropertyRowDesc desc;
				desc.Label = label.Text;
				desc.Term = label.Term;
				desc.Tooltip = docText;
				desc.A11yKind = "text";
				desc.A11yLabel = labelText;
				desc.A11yValue = display;
				desc.A11yEnabled = false;
				desc.InlineValue = true;
				desc.InlineValueText = display;
				desc.LabelWidth = labelWidth;
				desc.LabelIndent = labelIndent;
				// VEC-H4:禁用/只读行的 tooltip 必须给出**理由**(Blender HIG 的 disabled hint):
				// 先写用途,再写为什么现在是灰的。理由与面板顶部那行只读说明同源。
				if (m_ReadOnly || field.Meta.ReadOnly)
				{
					const std::string reason = m_ReadOnly
						? Wui::Tr("panel.properties.readonly_notice",
							"Play/Simulate running: read-only (pause or exit to edit)")
						: (scriptPropertyRow && ComponentPropertyModel::IsSummaryKind(field.K)
							// CPPT-3:IVec*/UVec*/Quat/Mat* —— 面板没有行控件;值不进属性表/不进存档,
							// 这里给"为什么只读"的可读理由(禁用必须能解释原因)。
							? Wui::Tr("panel.properties.component_summary_readonly",
								"This field type has no editor control yet — shown as a read-only "
								"summary and not saved here")
							: Wui::Tr("panel.properties.field_core_readonly",
								"Core field: read-only by design, it cannot be edited here"));
					desc.Tooltip = docText.empty() ? reason : (docText + "\n" + reason);
				}
				// 只读态:复位按钮可见但禁用(disabled hint = 面板顶部那行"Play/Simulate 只读"说明)。
				desc.ShowReset = scriptPropertyRow;
				desc.ResetEnabled = false;
				desc.ResetModified = resetModified;
				desc.ResetId = resetId;
				desc.ResetLabel = resetLabel;
				desc.ResetTooltip = resetDoc;
				Wui::PropertyRow(ctx, rowNodeId, row, desc, theme);
				y += kRowHeight;
				continue;
			}

			// 交互字段:标签登记为静态节点(不可点),控件本体按真实 kind 登记
			// (脚本用 properties.<Type>.<Field> 直接 ui.invoke)。
			const std::string& idText = rowIdText;
			// P4-U9:字段说明(如果有)—— 悬停提示 + 无障碍节点 Tooltip。
			const std::string fieldDoc = fieldDocFor(field);
			// VEC-H2:行结构(标签列 + 值列 + 悬停说明 + 行尾复位)走库件;面板只把控件画进 FieldRect。
			// 向量行(非颜色)是多行控件:先"只算不画"拿值列宽 → 决定排布与行高,再画行。
			const bool vectorRow = (field.K == Schema::Kind::Vec2 || field.K == Schema::Kind::Vec3
				|| field.K == Schema::Kind::Vec4) && !(field.Meta.Color && field.K != Schema::Kind::Vec2);
			const int vectorComponents = field.K == Schema::Kind::Vec2 ? 2
				: (field.K == Schema::Kind::Vec3 ? 3 : 4);
			const Wui::PropertyRowLayout measured = Wui::MeasurePropertyRow(
				{ rect.X, rect.Y + y, rect.W, kRowHeight }, labelWidth, scriptPropertyRow);
			float rowHeight = kRowHeight;
			if (vectorRow)
				rowHeight = VecFieldHeight(VecFieldLayout(measured.Field.W), vectorComponents);
			const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, rowHeight };
			// VEC-H4:行值先取(纯读),用来判定"值不可用"(脚本行 + 存的值类型与声明不符)。
			Schema::Value value = field.Get(instance);
			const bool valueUnavailable = scriptPropertyRow && field.K != Schema::Kind::Object
				&& std::holds_alternative<std::monostate>(value);
			// 不可用 = 值列给 "—"(多值/不可用的统一表达,反模式 4:不显示伪零);原因进 tooltip;
			// 不画字段控件、不写盘。修复路径:改脚本声明或场景里的那一条值。
			const std::string unavailableDoc = Wui::Tr("panel.properties.value_unavailable.tooltip",
				"The stored value's type does not match the script declaration, so it is not editable here");
			Wui::WuiRect ctrl;
			if (collectionWritable)
			{
				// VEC-H2:数组元素 / 映射键值行走库件 `Wui::CollectionRow` —— 行内 `↺`(单项复位)
				// 与行外 `-`(删除这条元素)都是库件的动作按钮,面板只消费事件(删除延迟到收口统一做)。
				Wui::CollectionRowDesc element;
				element.Label = label.Text;
				element.Term = label.Term;
				element.Tooltip = fieldDoc;
				element.A11yKind = "label";
				element.A11yLabel = labelText;
				element.A11yEnabled = false;
				element.LabelWidth = labelWidth;
				// 元素行比容器头再深一层(H4 §规则 2:12px/层,只挪标签,值列仍对齐)。
				element.LabelIndent = labelIndent + kIndentPerLevel;
				if (valueUnavailable)
				{
					element.FieldPlaceholder = Wui::Tr("panel.properties.value_mixed", "\u2014");
					element.Tooltip = unavailableDoc;
				}
				element.FieldHeight = vectorRow ? rowHeight : 0.0f;
				element.ActionsOutside = true;
				element.ShowReset = scriptPropertyRow;
				element.ResetEnabled = resetEnabled;
				element.ResetModified = resetModified;
				element.ResetId = resetId;
				element.ResetLabel = resetLabel;
				element.ResetTooltip = resetDoc;
				element.ShowRemove = true;
				element.RemoveId = Wui::HashId((collectionRows->IdText + ".remove." + field.Name).c_str());
				element.RemoveTooltip = Wui::Tr("panel.properties.collection_remove.tooltip",
					"Remove this element from the collection (the scene stores the list)");
				const Wui::CollectionRowResult elementRow =
					Wui::CollectionRow(ctx, rowNodeId, row, element, theme);
				if (elementRow.ResetClicked)
					applyItemReset();
				if (elementRow.RemoveClicked)
					pendingEraseName = field.Name;
				ctrl = elementRow.FieldRect;
			}
			else
			{
				Wui::PropertyRowDesc rowDesc;
				rowDesc.Label = label.Text;
				rowDesc.Term = label.Term;
				rowDesc.Tooltip = fieldDoc;
				rowDesc.A11yKind = "label";
				rowDesc.A11yLabel = labelText;
				rowDesc.A11yEnabled = false;      // 行标签不可交互;控件本体登记自己的节点
				rowDesc.LabelWidth = labelWidth;
				rowDesc.LabelIndent = labelIndent;
				if (valueUnavailable)
				{
					rowDesc.FieldPlaceholder = Wui::Tr("panel.properties.value_mixed", "\u2014");
					rowDesc.Tooltip = unavailableDoc;
					rowDesc.A11yValue = "unavailable";
				}
				rowDesc.FieldHeight = vectorRow ? rowHeight : 0.0f;
				rowDesc.ShowReset = scriptPropertyRow;
				rowDesc.ResetEnabled = resetEnabled;
				rowDesc.ResetModified = resetModified;
				rowDesc.ResetId = resetId;
				rowDesc.ResetLabel = resetLabel;
				rowDesc.ResetTooltip = resetDoc;
				const Wui::PropertyRowResult rowResult = Wui::PropertyRow(ctx, rowNodeId, row, rowDesc, theme);
				if (rowResult.ResetClicked)
					applyItemReset();
				ctrl = rowResult.FieldRect;
			}
			bool fieldChanged = false;
			// 本行推进量:普通行 = 库件行高(24);向量行按库件排布给足高度(见 VecFieldHeight)。
			float rowAdvance = rowHeight;
			if (valueUnavailable)
			{
				// 值不可用:值列已经画了 "—"(上面的行控件),这里不画字段控件、不写盘,只推进布局。
				y += rowAdvance;
				continue;
			}
			switch (field.K)
			{
				case Schema::Kind::Bool:
				{
					bool b = std::get<bool>(value);
					const bool before = b;
					Checkbox(ctx, fid, ctrl, "", b, theme);
					fieldChanged = b != before;
					if (fieldChanged) value = b;
					RegisterNode(Wui::HashId(idText.c_str()), "checkbox", ctrl, labelText, b ? "true" : "false",
						reachable(ctrl), fieldDoc);
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
					// CPPT-3-FIX1:声明 Unit 的行把行后缀接进库件数值样式(`WuiNumberStyle.Unit`)。
					const bool unitRow = !field.Meta.Unit.empty();
					DrawScriptIntControl(ctx, fid, ctrl, raw, lo, hi, field.Meta, theme);
					fieldChanged = raw != before;
					if (fieldChanged)
					{
						if (field.K == Schema::Kind::Int8) value = static_cast<int8_t>(raw);
						else if (field.K == Schema::Kind::Int16) value = static_cast<int16_t>(raw);
						else if (field.K == Schema::Kind::Int32) value = static_cast<int32_t>(raw);
						else value = raw;
					}
					RegisterNode(Wui::HashId(idText.c_str()), unitRow ? "number-field" : "drag-int", ctrl, labelText,
						unitRow ? (std::to_string(raw) + " " + field.Meta.Unit) : std::to_string(raw),
						reachable(ctrl), fieldDoc);
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
					// CPPT-3-FIX1:与有符号分支同一口径(Unit → NumberFieldInt + 单位后缀)。
					const bool unitRow = !field.Meta.Unit.empty();
					DrawScriptIntControl(ctx, fid, ctrl, signedRaw, lo, hi, field.Meta, theme);
					fieldChanged = signedRaw != before;
					if (fieldChanged)
					{
						if (field.K == Schema::Kind::UInt8) value = static_cast<uint8_t>(signedRaw);
						else if (field.K == Schema::Kind::UInt16) value = static_cast<uint16_t>(signedRaw);
						else if (field.K == Schema::Kind::UInt32) value = static_cast<uint32_t>(signedRaw);
						else value = static_cast<uint64_t>(signedRaw);
					}
					RegisterNode(Wui::HashId(idText.c_str()), unitRow ? "number-field" : "drag-int", ctrl, labelText,
						unitRow ? (std::to_string(signedRaw) + " " + field.Meta.Unit) : std::to_string(signedRaw),
						reachable(ctrl), fieldDoc);
					break;
				}
				case Schema::Kind::Float:
				case Schema::Kind::Double:
				{
					float f = field.K == Schema::Kind::Float ? std::get<float>(value) : static_cast<float>(std::get<double>(value));
					const float before = f;
					const float lo = field.Meta.Min.has_value() ? *field.Meta.Min : 1.0f;   // 1,-1 哨兵 = 无范围
					const float hi = field.Meta.Max.has_value() ? *field.Meta.Max : -1.0f;
					// CPPT-3-FIX1:声明 Unit 的浮点行把行后缀接进既有 WUI 数值样式(`WuiNumberStyle.Unit`;
					// DragBarFloat = 值区固定宽度 + 单位右对齐,与材质预览的角度行同一件)。
					// 只在**声明了有效范围**时改走 DragBarFloat:它的条体拖动按值域映射(无范围时内部用
					// ±1e30 哨兵,拖一次就冲出量程)—— 无范围的带单位行保持 DragFloat,拖拽/方向键步长 = Step。
					const std::string& unit = field.Meta.Unit;
					const int decimals = StepDecimals(field.Meta.Step.has_value(),
						field.Meta.Step.has_value() ? *field.Meta.Step : 0.0f);
					const float speed = (field.Meta.Step.has_value() && *field.Meta.Step > 0.0f)
						? *field.Meta.Step : 0.01f;
					if (!unit.empty() && lo < hi)
					{
						Wui::WuiNumberStyle style;
						style.Unit = unit.c_str();
						style.Decimals = decimals;
						style.ValueWidth = 68.0f;   // "-1000.0 hp" 这类值 + 单位也能完整显示
						Wui::DragBarFloat(ctx, fid, ctrl, f, lo, hi, theme, style);
					}
					else
					{
						DragFloat(ctx, fid, ctrl, f, speed, lo, hi, theme);
					}
					fieldChanged = f != before;
					if (fieldChanged) value = field.K == Schema::Kind::Float ? Schema::Value(f) : Schema::Value(static_cast<double>(f));
					if (!unit.empty() && lo < hi)
						RegisterNode(Wui::HashId(idText.c_str()), "slider", ctrl, labelText,
							FormatFloatText(f, decimals) + " " + unit, reachable(ctrl), fieldDoc);
					else
						RegisterNode(Wui::HashId(idText.c_str()), "drag-float", ctrl, labelText,
							FormatFloatText(f), reachable(ctrl), fieldDoc);
					break;
				}
				case Schema::Kind::Vec2:
				case Schema::Kind::Vec3:
				case Schema::Kind::Vec4:
				{
					// P4-U9:Color() 标记的向量 = 颜色 → 取色器(色块 + hex + R/G/B/A 滑杆 + 预设),
					// 不再让用户对着四个数字框猜颜色。
					if (field.Meta.Color && field.K != Schema::Kind::Vec2)
					{
						const glm::vec4 before = field.K == Schema::Kind::Vec3
							? glm::vec4 { std::get<glm::vec3>(value), 1.0f }
							: std::get<glm::vec4>(value);
						glm::vec4 edited = before;
						Wui::ColorField(ctx, fid, ctrl, edited, theme);
						if (edited != before)
						{
							fieldChanged = true;
							value = field.K == Schema::Kind::Vec3
								? Schema::Value(glm::vec3 { edited.x, edited.y, edited.z })
								: Schema::Value(edited);
						}
						RegisterNode(Wui::HashId(idText.c_str()), "color", ctrl, labelText,
							FormatColorHexText(edited), reachable(ctrl), fieldDoc);
						break;
					}
					const int components = field.K == Schema::Kind::Vec2 ? 2 : (field.K == Schema::Kind::Vec3 ? 3 : 4);
					// VEC-A4:向量行 = 库件 `Vec2Field`/`Vec3Field`/`Vec4Field`(与材质编辑器的
					// 参数行同一个控件:轴标签 + 数值区 + 拖动/键入/↑↓),不再逐分量手拼 DragFloat。
					// 行 id 仍是 `properties.<组件>.<字段>`;分量节点由库件登记为 `...axis.0/1/2/3`
					// (不再手写 `.x/.y/.z/.w` 节点)。
					const int layout = VecFieldLayout(ctrl.W);
					const float fieldHeight = VecFieldHeight(layout, components);
					const Wui::WuiRect fieldRect { ctrl.X, ctrl.Y, ctrl.W, fieldHeight };
					const Wui::WuiId rowId = Wui::HashId(idText.c_str());
					if (components == 2)
					{
						glm::vec2 vector = std::get<glm::vec2>(value);
						if (Wui::Vec2Field(ctx, rowId, fieldRect, vector, 0.01f, 1.0f, -1.0f, theme, layout))
						{
							value = vector;
							fieldChanged = true;
						}
					}
					else if (components == 3)
					{
						glm::vec3 vector = std::get<glm::vec3>(value);
						if (Wui::Vec3Field(ctx, rowId, fieldRect, vector, 0.01f, 1.0f, -1.0f, theme, layout))
						{
							value = vector;
							fieldChanged = true;
						}
					}
					else
					{
						glm::vec4 vector = std::get<glm::vec4>(value);
						if (Wui::Vec4Field(ctx, rowId, fieldRect, vector, 0.01f, 1.0f, -1.0f, theme, layout))
						{
							value = vector;
							fieldChanged = true;
						}
					}
					// 行变高(Vec4 横排 2×2 = 两行)后悬停说明仍覆盖整行:外层 tooltip 登记在
					// 行首 22px 的行矩形上,这里对控件矩形再挂一次(库件自身不带 tooltip)。
					if (!fieldDoc.empty())
						Wui::Tooltip(ctx, fieldRect, fieldDoc);
					rowAdvance = fieldHeight + 2.0f;
					break;
				}
				case Schema::Kind::String:
				case Schema::Kind::Name:   // 名字 = 自由文本行(与 String 同形)
				case Schema::Kind::Text:   // 有界文本也是自由文本行(截断由 TextOps 警告)
				case Schema::Kind::Asset:
				{
					const std::string current = std::get<std::string>(value);
					// P4-U9:资产路径(Asset("Material") / Of("Texture2D"))→ 可搜索资产下拉,
					// 明确给"(无)"选项 —— 手打路径既容易写错也发现不了拼写问题。
					const std::string assetType = !field.Meta.AssetType.empty() ? field.Meta.AssetType
						: (field.K == Schema::Kind::Asset && field.AssetTypeName ? std::string(field.AssetTypeName)
							: std::string());
					if (!assetType.empty())
					{
						const std::vector<std::string>& paths = Editor::AssetCatalog::PathsForName(assetType);
						std::vector<std::string> options;
						options.reserve(paths.size() + 2);
						options.push_back(Wui::Tr("panel.properties.asset_none", "(none)"));
						options.insert(options.end(), paths.begin(), paths.end());
						// 当前值不在扫描结果里(文件名写错/资产还没建):照样显示出来,不假装它是"(无)"。
						int selected = 0;
						const auto found = std::find(paths.begin(), paths.end(), current);
						if (found != paths.end())
							selected = static_cast<int>(found - paths.begin()) + 1;
						else if (!current.empty())
						{
							options.push_back(current);
							selected = static_cast<int>(options.size()) - 1;
						}
						// 注意类型:beforePick 必须是 int —— 写成 bool 时 selected(1) != true 恒为 false,
						// "选到第 1 个资产"就永远不会写回(实测踩过)。
						const int beforePick = selected;
						if (Wui::SearchableCombo(ctx, fid, ctrl, "", options, selected, theme)
							&& selected != beforePick)
						{
							value = selected <= 0 ? std::string() : options[static_cast<size_t>(selected)];
							fieldChanged = true;
						}
						RegisterNode(Wui::HashId(idText.c_str()), "searchable-combo", ctrl, labelText,
							current, reachable(ctrl), fieldDoc);
						break;
					}
					// P4-U9:固定集合的字符串(Choices("cube","sphere"...))→ 下拉,别无谓地手打。
					if (!field.Meta.Choices.empty())
					{
						std::vector<std::string> options = field.Meta.Choices;
						int selected = 0;
						const auto found = std::find(options.begin(), options.end(), current);
						if (found != options.end())
							selected = static_cast<int>(found - options.begin());
						const int beforePick = selected;
						if (Wui::Combo(ctx, fid, ctrl, "", options, selected, theme) && selected != beforePick)
						{
							value = options[static_cast<size_t>(selected)];
							fieldChanged = true;
						}
						RegisterNode(Wui::HashId(idText.c_str()), "combo", ctrl, labelText, current,
							reachable(ctrl), fieldDoc);
						break;
					}
					// 与 TextField 的 WuiEditState 共用 fid 会导致类型混淆,
					// 编辑缓冲必须使用独立 id。
					auto& state = ctx.Persist<SchemaTextState>(Wui::HashId("schema.text.state") ^ fid, {});
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
					RegisterNode(Wui::HashId(idText.c_str()), "text-field", ctrl, labelText, current,
						reachable(ctrl), fieldDoc);
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
						RegisterNode(Wui::HashId(idText.c_str()), "combo", ctrl, labelText,
							(selected >= 0 && selected < static_cast<int>(names.size())) ? names[selected] : std::string(),
							reachable(ctrl), fieldDoc);
					}
					break;
				}
				default:
					Label(ctx, { ctrl.X, ctrl.Y + 3 },
						Wui::Tr("panel.properties.unsupported", "(unsupported)"), theme.TextMuted, 12.0f);
					RegisterNode(Wui::HashId(idText.c_str()), "text", row, labelText,
						Wui::Tr("panel.properties.unsupported", "(unsupported)"), false);
					break;
			}
			if (fieldChanged)
			{
				field.Set(instance, value);
				changed = true;
				// P4-U13b:编辑实例字段 → 由"改动发生处"登记覆盖(不靠全量 diff 反推)。
				// 具体落账在 DrawComponentInspector(那里才知道编辑的是哪个实体)。
				if (changedFields)
					changedFields->push_back(typeName + "." + field.Name);
			}
			// 数组/映射的**叶子元素行**行尾 `-`(Object 元素行在各自分支里画)。
			// 叶子行的 `↺` 复位已随行结构(PropertyRow)画过,这里只剩 `-` 与推进。
			// 集合元素行走 CollectionRow 时,`-` 已由库件画过(不要重复登记/重复绘制)。
			if (!collectionWritable)
				drawCollectionRemove(field.Name, row);
			y += rowAdvance;
		}

		// ---- VEC-C2:数组/映射容器的收口(删除 / 追加 / 映射键名输入)----
		//
		// 放在所有元素行画完之后:合成 schema 的元素访问器按编译期下标实例化,循环中途 erase 会让
		// 后面的行读到错元素。删除按**行名**(数组 = 下标字符串,映射 = 键)定位,数组删完重排 1..n。
		if (collectionWritable)
		{
			PropertyNode& container = *collectionRows->Container;
			// CPPT-6-ED-COLLECTIONS:空容器的元素模板要吃 schema(命名 struct 的子字段从哪来)。
			// 注册表从当前正在画的脚本组件所属场景取;**只有 C++ 脚本属性行走它**
			// (Luau 的元素形状来自注解声明,传 nullptr = 保持既有的空容器回落)。
			const Schema::SchemaRegistry* collectionElementSchemas = nullptr;
			if (m_ScriptInspectingScriptRows && !m_ScriptInspectingLuau)
			{
				Entity rowEntity = m_ScriptInspectingEntity;
				Scene* rowScene = rowEntity.IsValid() ? rowEntity.GetScene() : nullptr;
				if (rowScene)
					collectionElementSchemas = &rowScene->GetContext().Schemas();
			}
			const Wui::WuiId addingId = Wui::HashId(("script.collection.adding." + collectionRows->IdText).c_str());
			bool& adding = ctx.Persist<bool>(addingId, false);
			const Wui::WuiId keyFieldId = Wui::HashId((collectionRows->IdText + ".add.key").c_str());
			SchemaTextState& keyState = ctx.Persist<SchemaTextState>(
				Wui::HashId(("script.collection.add.state." + collectionRows->IdText).c_str()), {});
			const auto markContainerChanged = [&]()
			{
				changed = true;
				if (changedFields)
					changedFields->push_back(typeName);
			};
			// VEC-H4:空集合 = 一行次要说明(§规则 21 的空态口径);`+` 按钮仍在下一行,点它出现第一项。
			if (container.Children.empty())
			{
				const Wui::WuiRect emptyRow { rect.X, rect.Y + y, rect.W, kRowHeight };
				Wui::PropertyRowDesc emptyDesc;
				emptyDesc.A11yKind = "text";
				emptyDesc.A11yLabel = collectionRows->IdText;
				emptyDesc.A11yValue = "0";
				emptyDesc.A11yEnabled = false;
				emptyDesc.Enabled = false;
				emptyDesc.LabelWidth = labelWidth;
				emptyDesc.LabelIndent = labelIndent + kIndentPerLevel;
				emptyDesc.InlineValue = true;
				emptyDesc.InlineValueText = Wui::Tr("panel.properties.collection_empty", "No items yet");
				Wui::PropertyRow(ctx, Wui::HashId((collectionRows->IdText + ".empty").c_str()), emptyRow,
					emptyDesc, theme);
				y += kRowHeight;
			}
			if (collectionRows->Kind == PropertyCollection::Map && adding)
			{
				// 映射 `+`:先给一个**键名文本输入**,回车建行(空键 / 重名忽略;Esc 取消)。
				const Wui::WuiRect keyRect { rect.X, rect.Y + y + (kRowHeight - 20.0f) * 0.5f,
					std::max(60.0f, rect.W - 4.0f), 20.0f };
				bool cancelled = false;
				const bool committed = Wui::TextField(ctx, keyFieldId, keyRect, keyState.Buffer, theme, &cancelled);
				const std::string keyText = keyState.Buffer;
				RegisterNode(keyFieldId, "text-field", keyRect, "key",
					keyText.empty() ? "new key" : keyText, true,
					Wui::Tr("panel.properties.collection_add_key.tooltip",
						"Type a new key name, then press Enter to add the row (empty or duplicate keys are ignored)"));
				if (committed)
				{
					if (!keyText.empty() && !CollectionKeyTaken(container, keyText))
					{
						PropertyNode child = MakeCollectionElement(container, collectionElementSchemas,
						collectionRows->PlainRows);
						child.Name = keyText;
						container.Children.push_back(std::move(child));
						markContainerChanged();
						WLD_CORE_INFO("[script-ui] collection add: {0}.{1}[{2}]", typeName, container.Name, keyText);
					}
					keyState.Buffer.clear();
					adding = false;
				}
				else if (cancelled || (keyText.empty() && ctx.Focus() != keyFieldId))
				{
					// Esc / 点空且没输入内容:收起输入行,不建行。
					keyState.Buffer.clear();
					adding = false;
				}
				y += kRowHeight;
			}
			else
			{
				// 追加行:`+` 与元素行的 `-` 同一条行外动作槽(CollectionActionColumnWidth)。
				const Wui::WuiRect addButton { rect.X + rect.W + 2.0f,
					rect.Y + y, kRowHeight, kRowHeight };
				const std::string addDoc = collectionRows->Kind == PropertyCollection::Map
					? Wui::Tr("panel.properties.collection_add_map.tooltip",
						"Add a key/value row (the key name is typed next)")
					: Wui::Tr("panel.properties.collection_append.tooltip", "Append one element to the list");
				if (Wui::CollectionActionButton(ctx, Wui::HashId((collectionRows->IdText + ".add").c_str()),
					addButton, "+", addDoc, true, theme, false))
				{
					if (collectionRows->Kind == PropertyCollection::Array)
					{
						PropertyNode child = MakeCollectionElement(container, collectionElementSchemas,
						collectionRows->PlainRows);
						child.Name = std::to_string(container.Children.size() + 1);
						container.Children.push_back(std::move(child));
						markContainerChanged();
						WLD_CORE_INFO("[script-ui] collection add: {0}.{1} -> {2} element(s)", typeName,
							container.Name, container.Children.size());
					}
					else
					{
						adding = true;
						keyState.Buffer.clear();
						ctx.SetFocus(keyFieldId);
					}
				}
				y += kRowHeight;
			}
			if (!pendingEraseName.empty())
			{
				const auto found = std::find_if(container.Children.begin(), container.Children.end(),
					[&pendingEraseName](const PropertyNode& child) { return child.Name == pendingEraseName; });
				if (found != container.Children.end())
				{
					WLD_CORE_INFO("[script-ui] collection remove: {0}.{1}[{2}]", typeName, container.Name,
						pendingEraseName);
					container.Children.erase(found);
					if (collectionRows->Kind == PropertyCollection::Array)
						RenumberArrayChildren(container);
					markContainerChanged();
				}
			}
		}

		// ---- PURE-ECS:原生组件的容器字段 → 折回 `Schema::Value` 写回结构体 ----
		// 落点在**容器自己这一层**:元素/键的增删改都发生在这里,只有这里知道 `changed`。
		// (顶层那次调用拿不到这个信号 —— 第一版把折回放在顶层,实测症状是"面板里加了元素、
		// 存盘却丢掉"。)`FoldContainerRows` 折出的形状与生成访问器
		// (`UnpackSequence`/`UnpackMap`)逐条对齐,包括命名 struct 元素的"字段名 → Value";
		// 无条件折(不做"场景是否记录过"的判定):原生路径的值/形状就是实例本身。
		if (changed && collectionRows && collectionRows->PlainRows
			&& collectionRows->WriteField && collectionRows->WriteField->Set && collectionRows->WriteInstance)
		{
			Schema::Value folded;
			if (ComponentPropertyModel::FoldContainerRows(*collectionRows->Container, &folded))
				collectionRows->WriteField->Set(collectionRows->WriteInstance, folded);
		}

		if (changed)
			m_Host.MarkDocumentDirty();
		return y;
	}

}
