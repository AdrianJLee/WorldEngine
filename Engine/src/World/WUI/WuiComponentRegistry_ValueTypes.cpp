#include "WuiComponentRegistry_Internal.h"

namespace World::Wui
{

using namespace WuiComponentRegistryDetail;

namespace WuiComponentRegistryDetail
{

		// ---- WUI-P1.5 加值:分组 / 单位 / 类型化默认 / Color / Size2 / 交互契约 ----

Property Grouped(Property property, WuiComponentPropertyGroup group, const char* doc){
			property.Group = group;
			property.Doc = doc;
			return property;
		}


Property UnitOf(Property property, const char* unit){
			property.Unit = unit;
			return property;
		}


Property NumberDefault(Property property, float value){
			property.DefaultNumber = value;
			return property;
		}


		// 颜色属性:DefaultText 与 DefaultColor 都写(旧文本行编辑器只认前者,新面板用后者)。
Property PropColor(const char* name, const char* defaultText, bool stateScoped, const char* doc){
			Property property;
			property.Name = name;
			property.Type = Property::Kind::Color;
			property.DefaultText = defaultText;
			WuiColor parsed {};
			if (ParseComponentColor(defaultText, parsed))
				property.DefaultColor = parsed;
			property.StateScoped = stateScoped;
			property.Group = WuiComponentPropertyGroup::Style;
			property.Doc = doc;
			return property;
		}


		// 尺寸属性(宽×高,设计单位)。
Property PropSize2(const char* name, float width, float height, const char* doc){
			Property property;
			property.Name = name;
			property.Type = Property::Kind::Size2;
			property.DefaultSize = { width, height };
			property.DefaultText = FormatComponentSize(width, height);
			property.Group = WuiComponentPropertyGroup::Layout;
			property.Doc = doc;
			return property;
		}


Interaction Item(WuiInteractionKind kind, const char* targetId, WuiInteractionExpect expect, const char* steps, const char* note){
			Interaction out;
			out.Kind = kind;
			out.TargetId = targetId;
			out.Expect = expect;
			out.Steps = steps;
			out.Note = note;
			return out;
		}


const char* StateLabel(const std::string& id){
			static const std::pair<const char*, const char*> kLabels[] = {
				{ "default", "Default" },
				{ "hover", "Hover" },
				{ "pressed", "Pressed" },
				{ "focus", "Focus" },
				{ "disabled", "Disabled" },
				{ "error", "Error" },
				{ "open", "Open" },
				{ "on", "On" },
				{ "off", "Off" },
				{ "checked", "Checked" },
				{ "unchecked", "Unchecked" },
				{ "mixed", "Mixed" },
				{ "selected", "Selected" },
				{ "scrolled", "Scrolled" },
				{ "empty", "Empty" },
				{ "modified", "Modified" },
				{ "long-text", "Long text" },
				{ "vertical", "Vertical" },
			};
			for (const std::pair<const char*, const char*>& entry : kLabels)
				if (id == entry.first)
					return entry.second;
			return nullptr;
		}


std::vector<State> StateList(std::initializer_list<const char*> ids){
			std::vector<State> states;
			states.reserve(ids.size());
			for (const char* id : ids)
			{
				State state;
				state.Id = id;
				const char* label = StateLabel(state.Id);
				state.Label = label != nullptr ? label : state.Id;
				states.push_back(std::move(state));
			}
			return states;
		}


std::vector<std::string> A11yIds(std::initializer_list<const char*> ids){
			return std::vector<std::string>(ids.begin(), ids.end());
		}


		// 外壳锚点 id(HashId("showcase." + 登记 id));控件自身登记同 id 节点时以控件为准。
std::vector<std::string> ShellIds(const char* componentId){
			std::vector<std::string> ids;
			ids.push_back(std::string("showcase.") + componentId);
			return ids;
		}


		// typeName 见文件头约定 5):面板侧控件入口名(保留模式类名,或立即模式控件入口名)。
		// interactions(P1.5)= 交互契约;不传 = 空表(静态件/尚未声明的件),既有登记条目一个都不用改。
WuiComponentDesc Desc(const char* id, const char* typeName, const char* displayName, const char* category, WuiComponentStatus status, const char* sourceFile, const char* a11yNotes, const char* sizeNotes, std::vector<std::string> extraA11yIds, std::vector<State> states, std::vector<Property> properties, void (*showcase)(const WuiComponentDraw&), std::vector<Interaction> interactions ){
			WuiComponentDesc desc;
			desc.Id = id;
			desc.TypeName = typeName;
			desc.DisplayName = displayName;
			desc.Category = category;
			desc.Status = status;
			desc.SourceFile = sourceFile;
			desc.A11yNotes = a11yNotes;
			desc.SizeNotes = sizeNotes;
			desc.ExtraA11yIds = std::move(extraA11yIds);
			desc.States = std::move(states);
			desc.Properties = std::move(properties);
			desc.Interactions = std::move(interactions);
			desc.Showcase = showcase;
			return desc;
		}

}
}
