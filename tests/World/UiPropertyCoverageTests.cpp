// World.UiPropertyCoverage — GameUI(M30)控件属性覆盖度门禁 + 本地化查询(headless)。
//
// 契约:
//   ① 每个内置 `.wui` 类型的**绘制函数真正读取的属性名**都必须在该类型的组件登记项里有声明
//      (登记表是设计器属性面板的唯一事实源;漏声明 = 面板看不到、也编不了)。期望表由
//      Engine/src/World/UI/UiPainter.cpp 的 Token*/ScaledProp 调用实测得到,硬编码在这里当
//      契约 —— 绘制加了属性,必须同步改登记表与这张表两处。
//   ② 声明属性的 Kind 与文本编码一致:Color 的默认值能被 ParseComponentColor 解析、
//      Size2 能被 ParseComponentSize 解析、Bool/数值的默认文本可解析。
//   ③ 按状态分槽的属性(名字形如 bg./border./text.)必须标 StateScoped=true;
//      StateScoped=true 的属性名必须带 '.'(契约:形如 bg.hover)。
//   ④ List 与 Grid 共用组件 id `listview`(由 tests/World/UiScrollTests.cpp 的 ComponentId
//      断言钉住),网格专属属性(cellColor/cellH/cellW/columns/gap)一并声明在同一张表里。
//   ⑤ 本地化查询:HasLocalizationKey 在英文族(内联默认)与 zh-CN 目录里都返回 true、
//      对未登记的键返回 false;LocalizationLanguages 排序稳定、去重并排除非语言目录。
//
// 说明:本文件不构建、不渲染;只读组件登记表与本地化状态。

#include "World/UI/UiNodeRegistry.h"
#include "World/WUI/WuiComponentRegistry.h"
#include "World/WUI/WuiLocalization.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using World::UI::UiNodeRegistry;
	using World::Wui::ParseComponentColor;
	using World::Wui::ParseComponentSize;
	using World::Wui::WuiColor;
	using World::Wui::WuiComponentDesc;
	using World::Wui::WuiComponentProperty;
	using World::Wui::WuiComponentRegistry;

	void Check(bool condition, const char* expression, int line, const std::string& detail = std::string())
	{
		if (condition)
			return;
		std::string message = std::string("line ") + std::to_string(line) + ": " + expression;
		if (!detail.empty())
			message += " — " + detail;
		throw std::runtime_error(message);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)
#define CHECK_MSG(expression, detail) Check(static_cast<bool>(expression), #expression, __LINE__, (detail))

	using Kind = WuiComponentProperty::Kind;

	// 绘制函数真正读取的属性 —— 契约表(见文件头 ①)。
	struct ExpectedProp
	{
		const char* Name;
		Kind Type;
	};

	struct ExpectedType
	{
		const char* WuiType;
		const char* ComponentId;
		std::vector<ExpectedProp> Props;
	};

	std::vector<ExpectedType> ExpectedTable()
	{
		return {
			{ "Panel", "box",
				{ { "bg", Kind::Color }, { "border", Kind::Color }, { "radius", Kind::Float },
					{ "title", Kind::Text } } },
			{ "Label", "label",
				{ { "label", Kind::Text }, { "text", Kind::Text }, { "fontSize", Kind::Float },
					{ "color", Kind::Color }, { "bold", Kind::Bool } } },
			{ "Button", "button",
				{ { "bg", Kind::Color }, { "bg.default", Kind::Color }, { "border", Kind::Color },
					{ "border.default", Kind::Color }, { "text", Kind::Color }, { "text.default", Kind::Color },
					{ "label", Kind::Text }, { "fontSize", Kind::Float }, { "bold", Kind::Bool },
					{ "padding", Kind::Float }, { "radius", Kind::Float }, { "disabled", Kind::Bool } } },
			{ "Image", "image",
				{ { "label", Kind::Text }, { "radius", Kind::Float }, { "textureId", Kind::Int },
					{ "tint", Kind::Color } } },
			{ "ProgressBar", "progress",
				{ { "disabled", Kind::Bool }, { "fillColor", Kind::Color }, { "label", Kind::Text },
					{ "radius", Kind::Float }, { "trackColor", Kind::Color }, { "value", Kind::Float } } },
			{ "Toggle", "toggle",
				{ { "boxSize", Kind::Float }, { "color", Kind::Color }, { "disabled", Kind::Bool },
					{ "fontSize", Kind::Float }, { "label", Kind::Text }, { "value", Kind::Bool } } },
			{ "Slider", "slider.float",
				{ { "disabled", Kind::Bool }, { "fillColor", Kind::Color }, { "fontSize", Kind::Float },
					{ "knobColor", Kind::Color }, { "knobSize", Kind::Float }, { "label", Kind::Text },
					{ "max", Kind::Float }, { "min", Kind::Float }, { "radius", Kind::Float },
					{ "trackColor", Kind::Color }, { "trackHeight", Kind::Float }, { "value", Kind::Float } } },
			{ "Checkbox", "checkbox",
				{ { "boxSize", Kind::Float }, { "checked", Kind::Bool }, { "color", Kind::Color },
					{ "disabled", Kind::Bool }, { "fontSize", Kind::Float }, { "label", Kind::Text },
					{ "value", Kind::Bool } } },
			{ "TextField", "textfield",
				{ { "bg", Kind::Color }, { "border", Kind::Color }, { "color", Kind::Color },
					{ "disabled", Kind::Bool }, { "fontSize", Kind::Float }, { "label", Kind::Text },
					{ "padding", Kind::Float }, { "placeholder", Kind::Text }, { "radius", Kind::Float },
					{ "value", Kind::Text } } },
			// List / Grid 共用 id `listview`(见文件头 ④):网格专属属性也在这张表里。
			{ "List", "listview",
				{ { "bg", Kind::Color }, { "disabled", Kind::Bool }, { "fontSize", Kind::Float },
					{ "label", Kind::Text }, { "padding", Kind::Float }, { "radius", Kind::Float },
					{ "rowColor", Kind::Color }, { "rowColorAlt", Kind::Color }, { "rowCount", Kind::Int },
					{ "rowHeight", Kind::Float }, { "rowPrefix", Kind::Text } } },
			{ "Grid", "listview",
				{ { "bg", Kind::Color }, { "cellColor", Kind::Color }, { "cellH", Kind::Float },
					{ "cellW", Kind::Float }, { "columns", Kind::Int }, { "disabled", Kind::Bool },
					{ "fontSize", Kind::Float }, { "gap", Kind::Float }, { "label", Kind::Text },
					{ "radius", Kind::Float }, { "rowCount", Kind::Int } } },
		};
	}

	const WuiComponentProperty* FindDeclared(const WuiComponentDesc& desc, const char* name)
	{
		for (const WuiComponentProperty& property : desc.Properties)
			if (property.Name == name)
				return &property;
		return nullptr;
	}

	bool IsBoolLiteral(const std::string& text)
	{
		return text == "0" || text == "1" || text == "true" || text == "false"
			|| text == "True" || text == "False" || text == "yes" || text == "no"
			|| text == "on" || text == "off";
	}

	bool IsNumberLiteral(const std::string& text)
	{
		if (text.empty())
			return false;
		char* end = nullptr;
		(void)std::strtod(text.c_str(), &end);
		if (end == text.c_str())
			return false;
		while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end)) != 0)
			++end;
		return *end == '\0';
	}

	// ① + ④:绘制读的属性必须在登记表里;Kind 也必须对得上。
	void TestPainterPropertiesAreDeclared()
	{
		for (const ExpectedType& expected : ExpectedTable())
		{
			const World::UI::UiNodeTypeDesc* type = UiNodeRegistry::Find(expected.WuiType);
			CHECK_MSG(type != nullptr, std::string("missing .wui type '") + expected.WuiType + "'");
			if (type == nullptr)
				continue;
			CHECK_MSG(type->ComponentId == expected.ComponentId,
				std::string(expected.WuiType) + " maps to '" + type->ComponentId
					+ "' but the contract says '" + expected.ComponentId + "'");
			const WuiComponentDesc* component = WuiComponentRegistry::Find(expected.ComponentId);
			CHECK_MSG(component != nullptr,
				std::string("component '") + expected.ComponentId + "' is not registered");
			if (component == nullptr)
				continue;
			for (const ExpectedProp& prop : expected.Props)
			{
				const WuiComponentProperty* declared = FindDeclared(*component, prop.Name);
				CHECK_MSG(declared != nullptr,
					std::string(expected.WuiType) + " paints property '" + prop.Name
						+ "' but component '" + expected.ComponentId + "' does not declare it");
				if (declared == nullptr)
					continue;
				CHECK_MSG(declared->Type == prop.Type,
					std::string(expected.ComponentId) + "." + prop.Name + " has the wrong Kind");
			}
		}
	}

	// ②:声明属性的默认文本编码必须与 Kind 一致。
	void TestDeclaredKindsMatchEncoding()
	{
		for (const WuiComponentDesc& desc : WuiComponentRegistry::All())
		{
			for (const WuiComponentProperty& property : desc.Properties)
			{
				const std::string label = desc.Id + "." + property.Name + " ('" + property.DefaultText + "')";
				if (property.Type == Kind::Color)
				{
					WuiColor parsed {};
					CHECK_MSG(ParseComponentColor(property.DefaultText, parsed),
						"Color default is not #RRGGBB[AA]: " + label);
				}
				else if (property.Type == Kind::Size2)
				{
					float width = 0.0f;
					float height = 0.0f;
					CHECK_MSG(ParseComponentSize(property.DefaultText, width, height),
						"Size2 default is not WxH: " + label);
				}
				else if (property.Type == Kind::Bool && !property.DefaultText.empty())
				{
					CHECK_MSG(IsBoolLiteral(property.DefaultText),
						"Bool default is not 0/1/true/false: " + label);
				}
				else if ((property.Type == Kind::Int || property.Type == Kind::Float)
					&& !property.DefaultText.empty())
				{
					CHECK_MSG(IsNumberLiteral(property.DefaultText),
						"numeric default is not a number: " + label);
				}
			}
		}
	}

	// ③:StateScoped 契约。
	void TestStateScopedContract()
	{
		const auto isStateChannel = [](const std::string& name)
		{
			return name.rfind("bg.", 0) == 0 || name.rfind("border.", 0) == 0 || name.rfind("text.", 0) == 0;
		};
		for (const WuiComponentDesc& desc : WuiComponentRegistry::All())
		{
			for (const WuiComponentProperty& property : desc.Properties)
			{
				if (isStateChannel(property.Name))
					CHECK_MSG(property.StateScoped,
						desc.Id + "." + property.Name
							+ " uses a channel.state name but is not marked StateScoped");
				if (property.StateScoped)
					CHECK_MSG(property.Name.find('.') != std::string::npos,
						desc.Id + "." + property.Name
							+ " is StateScoped but its name has no state suffix");
			}
		}
	}

	void WriteTextFile(const std::filesystem::path& path, const std::string& text)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		CHECK(static_cast<bool>(stream));
		stream << text;
	}

	// ⑤:本地化查询 API。
	void TestLocalizationQueries()
	{
		const std::filesystem::path root =
			std::filesystem::temp_directory_path() / "wld-ui-property-coverage";
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
		WriteTextFile(root / "layer" / "zh-CN" / "ui.json",
			"{ \"ui.panel.title\": \"panel-title\", \"ui.generic\": \"generic\" }");

		World::Wui::ClearLocalizationLayers();
		World::Wui::RegisterLocalizationLayer("probe", root / "layer", 0);

		// zh-CN:目录里有的键 = 有译文;乱写的键 = 没有(与 Tr 的 fallback 口径一致)。
		World::Wui::SetLanguage("zh-CN");
		World::Wui::ReloadLocalization();
		CHECK(World::Wui::Tr("ui.panel.title", "fallback") == "panel-title");
		CHECK(World::Wui::HasLocalizationKey("ui.panel.title"));
		CHECK(World::Wui::HasLocalizationKey("ui.generic"));
		CHECK(!World::Wui::HasLocalizationKey("ui.no.such.key"));
		CHECK(World::Wui::Tr("ui.no.such.key", "fallback") == "fallback");

		// 英文族 = 源码内联默认:该键的当前语言文本就是内联英文源文,恒有值。
		World::Wui::SetLanguage("en");
		World::Wui::ReloadLocalization();
		// M33:英文族现在也读目录 ⇒ 探针里没有 en 目录时**诚实报 false**
		// (旧契约"英文族恒 true"会让 `.wui` 的 `@key` 在 en 下静默显示裸键名)。
		CHECK(!World::Wui::HasLocalizationKey("ui.panel.title"));
		CHECK(!World::Wui::HasLocalizationKey("ui.no.such.key"));
		CHECK(World::Wui::Tr("ui.no.such.key", "inline") == "inline");
		// 补一个 en 目录:同一批键在 en 下必须被找到(这就是修好的那条路径)。
		WriteTextFile(root / "layer" / "en" / "ui.json", "{ \"ui.panel.title\": \"Panel\" }");
		World::Wui::ReloadLocalization();
		CHECK(World::Wui::HasLocalizationKey("ui.panel.title"));
		CHECK(World::Wui::Tr("ui.panel.title", "fallback") == "Panel");
		CHECK(!World::Wui::HasLocalizationKey("ui.generic"));   // en 目录里没有它就仍然是缺键

		// 语言码:各层目录下的语言子目录(排序稳定、去重;排除 glossary 这类非语言目录)。
		WriteTextFile(root / "layer" / "zh" / "ui.json", "{ \"ui.generic\": \"generic\" }");
		WriteTextFile(root / "layer" / "glossary" / "terms.json", "{ \"ui.generic\": \"x\" }");
		World::Wui::ReloadLocalization();
		const std::vector<std::string> languages = World::Wui::LocalizationLanguages();
		// 上面为了验"英文族也读目录"补了 en 目录 ⇒ 语言清单是三者(排序稳定:en < zh < zh-CN)。
		CHECK(languages == std::vector<std::string>({ "en", "zh", "zh-CN" }));

		World::Wui::ClearLocalizationLayers();
		World::Wui::SetLanguage("en");
		std::filesystem::remove_all(root, ec);
	}
}

int main()
{
	try
	{
		TestPainterPropertiesAreDeclared();
		TestDeclaredKindsMatchEncoding();
		TestStateScopedContract();
		TestLocalizationQueries();
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "World.UiPropertyCoverage FAILED: %s\n", e.what());
		return 1;
	}
	std::printf("World.UiPropertyCoverage OK\n");
	return 0;
}
