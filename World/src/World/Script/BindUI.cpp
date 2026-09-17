#include "wldpch.h"
#include "World/Script/BindUI.h"

#include "World/Renderer/Renderer.h"
#include "World/Renderer/Texture.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/BindServices.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/WuiWidgets.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace World
{
	namespace
	{
		// ---- 当前 UI 会话(ScriptEngine::DrawScriptUi 建立) ----

		struct UiScopeFrame
		{
			Wui::WuiContext* Context = nullptr;
			std::string IdPrefix;
		};

		std::vector<UiScopeFrame>& UiScopeStack()
		{
			static std::vector<UiScopeFrame> stack;
			return stack;
		}

		const UiScopeFrame& CurrentUiScope()
		{
			const auto& stack = UiScopeStack();
			if (stack.empty())
				throw std::logic_error("ui.* can only be called during the UI phase (ScriptEngine::DrawScriptUi)");
			return stack.back();
		}

		Wui::WuiContext& CurrentUiContext()
		{
			return *CurrentUiScope().Context;
		}

		Wui::WuiId HashUiId(const std::string& id)
		{
			const std::string key = CurrentUiScope().IdPrefix + "/" + id;
			return Wui::HashId(key.c_str());
		}

		// 滚动偏移不是脚本状态(不进 ctx.Persist,也不随热重载迁移):
		// 按控件 id 存在绑定层,保证列表/网格的滚轮位置跨帧保持。
		std::unordered_map<Wui::WuiId, float>& UiScrollOffsets()
		{
			static std::unordered_map<Wui::WuiId, float> offsets;
			return offsets;
		}

		// ---- 参数读取(不匹配 → 可读 C++ 异常 → Lua error) ----

		std::string MethodSignature(const std::string& table, const ScriptServiceMethod& method)
		{
			std::string signature = table + "." + method.Name + "(";
			for (std::size_t i = 0; i < method.ParamCount; ++i)
			{
				if (i) signature += ", ";
				signature += method.Params[i].Name;
				if (!method.Params[i].Required) signature += "?";
				signature += ": ";
				signature += method.Params[i].LuaType;
			}
			return signature + ")";
		}

		void CheckArgumentCount(const std::string& table, const ScriptServiceMethod& method,
			std::size_t count, std::size_t minimum)
		{
			if (count < minimum || count > method.ExpectedArgs)
				throw std::logic_error(MethodSignature(table, method) + " expects between " +
					std::to_string(minimum) + " and " + std::to_string(method.ExpectedArgs) +
					" argument(s); got " + std::to_string(count));
		}

		bool MatchesType(ScriptServiceArgType accepted, const ScriptValue& value)
		{
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::Boolean)) &&
				value.IsBoolean())
				return true;
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::Number)) &&
				value.IsNumber())
				return true;
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::String)) &&
				value.IsString())
				return true;
			if ((static_cast<uint32_t>(accepted) & static_cast<uint32_t>(ScriptServiceArgType::Table)) &&
				value.IsTable())
				return true;
			return false;
		}

		const ScriptValue& RequireArg(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			if (index >= argCount || args[index].IsNil())
				throw std::logic_error(MethodSignature(table, method) + " is missing required argument " +
					std::to_string(index + 1));
			if (!MatchesType(method.Params[index].Accepted, args[index]))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType +
					", got " + args[index].TypeName());
			return args[index];
		}

		double RequireNumber(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			const ScriptValue& value = RequireArg(table, method, args, argCount, index);
			double number = 0.0;
			if (!value.AsNumber(&number) || !std::isfinite(number))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be a finite number");
			return number;
		}

		double OptionalNumber(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index, double fallback)
		{
			if (index >= argCount || args[index].IsNil())
				return fallback;
			return RequireNumber(table, method, args, argCount, index);
		}

		int64_t RequireInteger(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			const ScriptValue& value = RequireArg(table, method, args, argCount, index);
			double number = 0.0;
			constexpr double kInt64Min = -9223372036854775808.0; // -2^63
			constexpr double kInt64MaxExclusive = 9223372036854775808.0; // 2^63
			if (!value.AsNumber(&number) || !std::isfinite(number) || number != std::floor(number) ||
				number < kInt64Min || number >= kInt64MaxExclusive)
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be an integer");
			return static_cast<int64_t>(number);
		}

		bool RequireBool(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			const ScriptValue& value = RequireArg(table, method, args, argCount, index);
			bool result = false;
			if (!value.AsBool(&result))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType);
			return result;
		}

		std::string RequireString(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			const ScriptValue& value = RequireArg(table, method, args, argCount, index);
			std::string result;
			if (!value.AsString(&result))
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be " + method.Params[index].LuaType);
			return result;
		}

		std::string RequireId(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			const std::string id = RequireString(table, method, args, argCount, index);
			if (id.empty())
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be a non-empty string");
			return id;
		}

		Wui::WuiRect RequireRect(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t first)
		{
			return {
				static_cast<float>(RequireNumber(table, method, args, argCount, first + 0)),
				static_cast<float>(RequireNumber(table, method, args, argCount, first + 1)),
				static_cast<float>(RequireNumber(table, method, args, argCount, first + 2)),
				static_cast<float>(RequireNumber(table, method, args, argCount, first + 3)),
			};
		}

		std::vector<std::string> ReadStringArray(const std::string& table, const ScriptServiceMethod& method,
			const ScriptValue* args, std::size_t argCount, std::size_t index)
		{
			const ScriptValue& value = RequireArg(table, method, args, argCount, index);
			ScriptTableRef items;
			if (!value.AsTable(&items) || !items.IsValid())
				throw std::logic_error(MethodSignature(table, method) + ": argument " + std::to_string(index + 1) +
					" ('" + method.Params[index].Name + "') must be an array of strings");
			const std::vector<ScriptValue> values = items.GetArray();
			std::vector<std::string> result;
			result.reserve(values.size());
			for (std::size_t i = 0; i < values.size(); ++i)
			{
				std::string text;
				if (!values[i].AsString(&text))
					throw std::logic_error(MethodSignature(table, method) + ": " + method.Params[index].Name + "[" +
						std::to_string(i + 1) + "] must be a string, got " + values[i].TypeName());
				result.push_back(std::move(text));
			}
			return result;
		}

		int64_t ClampSelection(int64_t selected, std::size_t itemCount)
		{
			if (itemCount == 0)
				return 0;
			return std::clamp<int64_t>(selected, 1, static_cast<int64_t>(itemCount));
		}

		// ---- 纹理解析(ui.image) ----

		struct UiTextureEntry
		{
			Ref<Texture2D> Source;
			uint64_t Id = 0;
			uint32_t Generation = 0;
		};

		std::unordered_map<std::string, UiTextureEntry>& UiTextureCache()
		{
			static std::unordered_map<std::string, UiTextureEntry> cache;
			return cache;
		}

		uint64_t ResolveUiTexture(const std::string& path)
		{
			if (!Renderer::GetDevice())
				throw std::logic_error("ui.image requires an initialized renderer device; "
					"the UI phase cannot create textures in a headless process");

			Wui::WuiTextureRegistry& registry = Wui::WuiTextureRegistry::Get();
			UiTextureEntry& entry = UiTextureCache()[path];
			// 设备重建(WuiTextureRegistry::Clear → Generation 变化)后旧 GL 纹理已失效,
			// 必须重新创建,而不是把旧 Source 重新登记。
			if (entry.Generation != registry.Generation())
			{
				entry.Source = nullptr;
				entry.Id = 0;
			}
			if (!entry.Source)
				entry.Source = Texture2D::Create(path);
			if (!entry.Source)
				throw std::logic_error("ui.image could not create a texture for '" + path + "'");
			if (entry.Id == 0)
			{
				entry.Id = registry.RegisterTexture2D(entry.Source);
				entry.Generation = registry.Generation();
			}
			return entry.Id;
		}

		// ---- 每个方法的实现 ----

		ScriptValue UiPanelImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = ScriptUiBindings(nullptr)[0].Methods[0];
			CheckArgumentCount("ui", method, count, 5);
			const std::string title = RequireString("ui", method, args, count, 4);
			const Wui::WuiRect rect = RequireRect("ui", method, args, count, 0);
			Wui::Panel(CurrentUiContext(), rect, title, Wui::WuiDefaultTheme());
			return ScriptValue::Nil();
		}

		ScriptValue UiTextImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = ScriptUiBindings(nullptr)[0].Methods[1];
			CheckArgumentCount("ui", method, count, 3);
			const glm::vec2 position {
				static_cast<float>(RequireNumber("ui", method, args, count, 0)),
				static_cast<float>(RequireNumber("ui", method, args, count, 1)),
			};
			const std::string text = RequireString("ui", method, args, count, 2);
			const float fontSize = static_cast<float>(OptionalNumber("ui", method, args, count, 3, 15.0));
			Wui::Label(CurrentUiContext(), position, text, Wui::WuiDefaultTheme().Text, fontSize);
			return ScriptValue::Nil();
		}

		ScriptValue UiButtonImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = ScriptUiBindings(nullptr)[0].Methods[2];
			CheckArgumentCount("ui", method, count, 6);
			const std::string id = RequireId("ui", method, args, count, 0);
			const Wui::WuiRect rect = RequireRect("ui", method, args, count, 1);
			const std::string label = RequireString("ui", method, args, count, 5);
			const bool clicked = Wui::Button(CurrentUiContext(), HashUiId(id), rect, label, Wui::WuiDefaultTheme());
			return ScriptValue::Boolean(clicked);
		}

		ScriptValue UiCheckboxImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = ScriptUiBindings(nullptr)[0].Methods[3];
			CheckArgumentCount("ui", method, count, 7);
			const std::string id = RequireId("ui", method, args, count, 0);
			const Wui::WuiRect rect = RequireRect("ui", method, args, count, 1);
			const std::string label = RequireString("ui", method, args, count, 5);
			bool value = RequireBool("ui", method, args, count, 6);
			Wui::Checkbox(CurrentUiContext(), HashUiId(id), rect, label, value, Wui::WuiDefaultTheme());
			return ScriptValue::Boolean(value);
		}

		ScriptValue UiSliderImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = ScriptUiBindings(nullptr)[0].Methods[4];
			CheckArgumentCount("ui", method, count, 8);
			const std::string id = RequireId("ui", method, args, count, 0);
			const Wui::WuiRect rect = RequireRect("ui", method, args, count, 1);
			float value = static_cast<float>(RequireNumber("ui", method, args, count, 5));
			const float min = static_cast<float>(RequireNumber("ui", method, args, count, 6));
			const float max = static_cast<float>(RequireNumber("ui", method, args, count, 7));
			Wui::SliderFloat(CurrentUiContext(), HashUiId(id), rect, value, min, max, Wui::WuiDefaultTheme());
			return ScriptValue::Number(static_cast<double>(value));
		}

		ScriptValue UiImageImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = ScriptUiBindings(nullptr)[0].Methods[5];
			CheckArgumentCount("ui", method, count, 5);
			const Wui::WuiRect rect = RequireRect("ui", method, args, count, 0);
			const std::string path = RequireString("ui", method, args, count, 4);
			const Wui::WuiRect uv {
				static_cast<float>(OptionalNumber("ui", method, args, count, 5, 0.0)),
				static_cast<float>(OptionalNumber("ui", method, args, count, 6, 0.0)),
				static_cast<float>(OptionalNumber("ui", method, args, count, 7, 1.0)),
				static_cast<float>(OptionalNumber("ui", method, args, count, 8, 1.0)),
			};
			Wui::Image(CurrentUiContext(), rect, ResolveUiTexture(path), uv, Wui::WuiDefaultTheme());
			return ScriptValue::Nil();
		}

		ScriptValue UiListImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = ScriptUiBindings(nullptr)[0].Methods[6];
			CheckArgumentCount("ui", method, count, 8);
			const std::string id = RequireId("ui", method, args, count, 0);
			const Wui::WuiRect rect = RequireRect("ui", method, args, count, 1);
			const std::vector<std::string> labels = ReadStringArray("ui", method, args, count, 5);
			const double rowHeight = RequireNumber("ui", method, args, count, 6);
			if (rowHeight <= 0.0)
				throw std::logic_error(MethodSignature("ui", method) +
					": argument 7 ('rowHeight') must be greater than zero");
			const int64_t selected = RequireInteger("ui", method, args, count, 7);
			if (labels.empty())
				return ScriptValue::Nil();

			const int64_t clamped = ClampSelection(selected, labels.size());
			const Wui::WuiId widgetId = HashUiId(id);
			std::vector<Wui::ListViewItem> items;
			items.reserve(labels.size());
			for (std::size_t i = 0; i < labels.size(); ++i)
			{
				Wui::ListViewItem item;
				item.Id = HashUiId(id + "#" + std::to_string(i + 1));
				item.Label = labels[i];
				item.Selected = static_cast<int64_t>(i + 1) == clamped;
				items.push_back(std::move(item));
			}

			float& scroll = UiScrollOffsets()[widgetId];
			const Wui::ListViewResult result = Wui::ListView(CurrentUiContext(), rect, items,
				static_cast<float>(rowHeight), scroll, Wui::WuiDefaultTheme());
			if (result.Clicked >= 0)
				return ScriptValue::Number(static_cast<double>(result.Clicked + 1));
			// 越界 selected 夹紧成 1..items 并把夹紧结果交给脚本(可直接写回字段)。
			if (clamped != selected)
				return ScriptValue::Number(static_cast<double>(clamped));
			return ScriptValue::Nil();
		}

		ScriptValue UiGridImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptServiceMethod& method = ScriptUiBindings(nullptr)[0].Methods[7];
			CheckArgumentCount("ui", method, count, 9);
			const std::string id = RequireId("ui", method, args, count, 0);
			const Wui::WuiRect rect = RequireRect("ui", method, args, count, 1);
			const std::vector<std::string> labels = ReadStringArray("ui", method, args, count, 5);
			const double cellWidth = RequireNumber("ui", method, args, count, 6);
			const double cellHeight = RequireNumber("ui", method, args, count, 7);
			if (cellWidth <= 0.0 || cellHeight <= 0.0)
				throw std::logic_error(MethodSignature("ui", method) +
					": arguments 7 ('cellW') and 8 ('cellH') must be greater than zero");
			const int64_t selected = RequireInteger("ui", method, args, count, 8);
			if (labels.empty())
				return ScriptValue::Nil();

			const int64_t clamped = ClampSelection(selected, labels.size());
			const Wui::WuiId widgetId = HashUiId(id);
			std::vector<Wui::GridViewItem> items;
			items.reserve(labels.size());
			for (std::size_t i = 0; i < labels.size(); ++i)
			{
				Wui::GridViewItem item;
				item.Id = HashUiId(id + "#" + std::to_string(i + 1));
				item.Label = labels[i];
				item.Selected = static_cast<int64_t>(i + 1) == clamped;
				items.push_back(std::move(item));
			}

			float& scroll = UiScrollOffsets()[widgetId];
			const Wui::GridViewResult result = Wui::GridView(CurrentUiContext(), rect, items,
				static_cast<float>(cellWidth), static_cast<float>(cellHeight), scroll, Wui::WuiDefaultTheme());
			if (result.Clicked >= 0)
				return ScriptValue::Number(static_cast<double>(result.Clicked + 1));
			if (clamped != selected)
				return ScriptValue::Number(static_cast<double>(clamped));
			return ScriptValue::Nil();
		}

		ScriptValue UiAxisSplitImpl(const ScriptValue* args, std::size_t count, bool rows)
		{
			const ScriptServiceMethod& method = ScriptUiBindings(nullptr)[0].Methods[rows ? 8 : 9];
			CheckArgumentCount("ui", method, count, 5);
			const double x = RequireNumber("ui", method, args, count, 0);
			const double y = RequireNumber("ui", method, args, count, 1);
			const double width = RequireNumber("ui", method, args, count, 2);
			const double height = RequireNumber("ui", method, args, count, 3);
			const int64_t cellCount = RequireInteger("ui", method, args, count, 4);
			if (cellCount < 0)
				throw std::logic_error(MethodSignature("ui", method) +
					": argument 5 ('count') must be a non-negative integer");
			const double gap = OptionalNumber("ui", method, args, count, 5, 0.0);

			ScriptTableRef result = ScriptEngine::GetState().CreateTable();
			if (cellCount == 0)
				return result.ToValue();
			const double totalGap = gap * static_cast<double>(cellCount - 1);
			const double cell = std::max(0.0, ((rows ? width : height) - totalGap) / static_cast<double>(cellCount));
			for (int64_t index = 0; index < cellCount; ++index)
			{
				const double offset = (rows ? x : y) + static_cast<double>(index) * (cell + gap);
				ScriptTableRef entry = ScriptEngine::GetState().CreateTable();
				entry.SetField("x", ScriptValue::Number(rows ? offset : x));
				entry.SetField("y", ScriptValue::Number(rows ? y : offset));
				entry.SetField("w", ScriptValue::Number(rows ? cell : width));
				entry.SetField("h", ScriptValue::Number(rows ? height : cell));
				result.SetArrayElement(static_cast<std::size_t>(index + 1), entry.ToValue());
			}
			return result.ToValue();
		}

		ScriptValue UiRowsImpl(const ScriptValue* args, std::size_t count)
		{
			return UiAxisSplitImpl(args, count, true);
		}

		ScriptValue UiColumnsImpl(const ScriptValue* args, std::size_t count)
		{
			return UiAxisSplitImpl(args, count, false);
		}

		// 创建 table,写入全部方法,注册为全局(与 BindServices 的只读表同一形态)。
		bool RegisterUiTable(ScriptBindingContext& bindings, const ScriptServiceBinding& table, std::string* error)
		{
			LuauVm* vm = bindings.Vm();
			if (!vm || !table.Name || !table.Name[0] || !table.Methods || table.MethodCount == 0)
			{
				if (error) *error = "invalid script UI binding descriptor";
				return false;
			}
			if (vm->GetGlobal(table.Name).Type() != ScriptValueType::Nil)
			{
				if (error) *error = std::string("global name is already in use: ") + table.Name;
				return false;
			}
			ScriptTableRef scriptTable = vm->CreateTable();
			if (!scriptTable.IsValid())
			{
				if (error) *error = std::string("failed to create the script UI table: ") + table.Name;
				return false;
			}
			for (std::size_t index = 0; index < table.MethodCount; ++index)
			{
				const ScriptServiceMethod& method = table.Methods[index];
				if (!method.Name || !method.Name[0] || !method.Function)
				{
					if (error) *error = std::string("script UI table '") + table.Name + "' has an invalid method entry";
					return false;
				}
				std::size_t required = 0;
				for (std::size_t param = 0; param < method.ParamCount; ++param)
					if (method.Params[param].Required) ++required;
				if (required > method.ExpectedArgs || method.ExpectedArgs > method.ParamCount)
				{
					if (error) *error = std::string("script UI table '") + table.Name + "." + method.Name +
						"' has an inconsistent parameter descriptor";
					return false;
				}
				if (!scriptTable.SetField(method.Name, bindings.CreateFunction(method.Name, method.Function)))
				{
					if (error) *error = std::string("failed to bind ") + table.Name + "." + method.Name;
					return false;
				}
			}
			if (!scriptTable.SetMetaField("__newindex", bindings.CreateFunction(table.Name,
				[](const ScriptValue*, std::size_t) -> ScriptValue
				{
					throw std::logic_error("the ui table is read-only; use the documented ui functions");
				})))
			{
				if (error) *error = std::string("failed to lock the script UI table: ") + table.Name;
				return false;
			}
			if (!vm->SetGlobal(table.Name, scriptTable.ToValue()))
			{
				if (error) *error = std::string("failed to publish the script UI table: ") + table.Name;
				return false;
			}
			return true;
		}
	}

	bool RegisterUiBindings(ScriptBindingContext& bindings, std::string* error)
	{
		std::size_t count = 0;
		const ScriptServiceBinding* tables = ScriptUiBindings(&count);
		for (std::size_t index = 0; index < count; ++index)
		{
			if (!RegisterUiTable(bindings, tables[index], error))
				return false;
		}
		if (error) error->clear();
		return true;
	}

	void PushScriptUiContext(Wui::WuiContext& context, const char* idPrefix)
	{
		UiScopeStack().push_back({ &context, idPrefix ? std::string(idPrefix) : std::string() });
	}

	void PopScriptUiContext()
	{
		auto& stack = UiScopeStack();
		if (!stack.empty())
			stack.pop_back();
	}

	const ScriptServiceBinding* ScriptUiBindings(std::size_t* count)
	{
		// 描述表必须是 static:运行时注册、存根渲染与方法体的签名诊断共用同一份只读描述。
		static const ScriptServiceParam panelParams[] = {
			{ "x", "number", ScriptServiceArgType::Number, true, "Left edge in UI pixels." },
			{ "y", "number", ScriptServiceArgType::Number, true, "Top edge in UI pixels." },
			{ "w", "number", ScriptServiceArgType::Number, true, "Width in UI pixels." },
			{ "h", "number", ScriptServiceArgType::Number, true, "Height in UI pixels." },
			{ "title", "string", ScriptServiceArgType::String, true, "Panel title." },
		};
		static const ScriptServiceParam textParams[] = {
			{ "x", "number", ScriptServiceArgType::Number, true, "Left edge in UI pixels." },
			{ "y", "number", ScriptServiceArgType::Number, true, "Top edge in UI pixels." },
			{ "text", "string", ScriptServiceArgType::String, true, "Single-line text." },
			{ "fontSize", "number", ScriptServiceArgType::Number, false, "Font size in pixels; defaults to 15." },
		};
		static const ScriptServiceParam buttonParams[] = {
			{ "id", "string", ScriptServiceArgType::String, true, "Non-empty widget id; the script path is prepended before hashing." },
			{ "x", "number", ScriptServiceArgType::Number, true, "Left edge in UI pixels." },
			{ "y", "number", ScriptServiceArgType::Number, true, "Top edge in UI pixels." },
			{ "w", "number", ScriptServiceArgType::Number, true, "Width in UI pixels." },
			{ "h", "number", ScriptServiceArgType::Number, true, "Height in UI pixels." },
			{ "label", "string", ScriptServiceArgType::String, true, "Button label." },
		};
		static const ScriptServiceParam checkboxParams[] = {
			{ "id", "string", ScriptServiceArgType::String, true, "Non-empty widget id; the script path is prepended before hashing." },
			{ "x", "number", ScriptServiceArgType::Number, true, "Left edge in UI pixels." },
			{ "y", "number", ScriptServiceArgType::Number, true, "Top edge in UI pixels." },
			{ "w", "number", ScriptServiceArgType::Number, true, "Width in UI pixels." },
			{ "h", "number", ScriptServiceArgType::Number, true, "Height in UI pixels." },
			{ "label", "string", ScriptServiceArgType::String, true, "Checkbox label." },
			{ "value", "boolean", ScriptServiceArgType::Boolean, true, "Current value supplied by the script." },
		};
		static const ScriptServiceParam sliderParams[] = {
			{ "id", "string", ScriptServiceArgType::String, true, "Non-empty widget id; the script path is prepended before hashing." },
			{ "x", "number", ScriptServiceArgType::Number, true, "Left edge in UI pixels." },
			{ "y", "number", ScriptServiceArgType::Number, true, "Top edge in UI pixels." },
			{ "w", "number", ScriptServiceArgType::Number, true, "Width in UI pixels." },
			{ "h", "number", ScriptServiceArgType::Number, true, "Height in UI pixels." },
			{ "value", "number", ScriptServiceArgType::Number, true, "Current value supplied by the script." },
			{ "min", "number", ScriptServiceArgType::Number, true, "Minimum value." },
			{ "max", "number", ScriptServiceArgType::Number, true, "Maximum value." },
		};
		static const ScriptServiceParam imageParams[] = {
			{ "x", "number", ScriptServiceArgType::Number, true, "Left edge in UI pixels." },
			{ "y", "number", ScriptServiceArgType::Number, true, "Top edge in UI pixels." },
			{ "w", "number", ScriptServiceArgType::Number, true, "Width in UI pixels." },
			{ "h", "number", ScriptServiceArgType::Number, true, "Height in UI pixels." },
			{ "path", "string", ScriptServiceArgType::String, true, "Logical asset path resolved through the VFS." },
			{ "u0", "number", ScriptServiceArgType::Number, false, "Left UV; defaults to 0." },
			{ "v0", "number", ScriptServiceArgType::Number, false, "Top UV; defaults to 0." },
			{ "u1", "number", ScriptServiceArgType::Number, false, "Right UV; defaults to 1." },
			{ "v1", "number", ScriptServiceArgType::Number, false, "Bottom UV; defaults to 1." },
		};
		static const ScriptServiceParam listParams[] = {
			{ "id", "string", ScriptServiceArgType::String, true, "Non-empty widget id; the script path is prepended before hashing." },
			{ "x", "number", ScriptServiceArgType::Number, true, "Left edge in UI pixels." },
			{ "y", "number", ScriptServiceArgType::Number, true, "Top edge in UI pixels." },
			{ "w", "number", ScriptServiceArgType::Number, true, "Width in UI pixels." },
			{ "h", "number", ScriptServiceArgType::Number, true, "Height in UI pixels." },
			{ "items", "string[]", ScriptServiceArgType::Table, true, "Array of row labels." },
			{ "rowHeight", "number", ScriptServiceArgType::Number, true, "Row height in pixels; must be positive." },
			{ "selectedIndex", "integer", ScriptServiceArgType::Number, true, "1-based selected row used for highlighting." },
		};
		static const ScriptServiceParam gridParams[] = {
			{ "id", "string", ScriptServiceArgType::String, true, "Non-empty widget id; the script path is prepended before hashing." },
			{ "x", "number", ScriptServiceArgType::Number, true, "Left edge in UI pixels." },
			{ "y", "number", ScriptServiceArgType::Number, true, "Top edge in UI pixels." },
			{ "w", "number", ScriptServiceArgType::Number, true, "Width in UI pixels." },
			{ "h", "number", ScriptServiceArgType::Number, true, "Height in UI pixels." },
			{ "items", "string[]", ScriptServiceArgType::Table, true, "Array of cell labels." },
			{ "cellW", "number", ScriptServiceArgType::Number, true, "Cell width in pixels; must be positive." },
			{ "cellH", "number", ScriptServiceArgType::Number, true, "Cell height in pixels; must be positive." },
			{ "selectedIndex", "integer", ScriptServiceArgType::Number, true, "1-based selected cell used for highlighting." },
		};
		static const ScriptServiceParam splitParams[] = {
			{ "x", "number", ScriptServiceArgType::Number, true, "Left edge in UI pixels." },
			{ "y", "number", ScriptServiceArgType::Number, true, "Top edge in UI pixels." },
			{ "w", "number", ScriptServiceArgType::Number, true, "Width in UI pixels." },
			{ "h", "number", ScriptServiceArgType::Number, true, "Height in UI pixels." },
			{ "count", "integer", ScriptServiceArgType::Number, true, "Number of cells; zero yields an empty table." },
			{ "gap", "number", ScriptServiceArgType::Number, false, "Gap between cells in pixels; defaults to 0." },
		};
		static const ScriptServiceMethod uiMethods[] = {
			{ "panel", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiPanelImpl(args, count);
			}, panelParams, 5, 5, "",
				"Draw a panel background with a title." },
			{ "text", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiTextImpl(args, count);
			}, textParams, 4, 4, "",
				"Draw a single line of text." },
			{ "button", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiButtonImpl(args, count);
			}, buttonParams, 6, 6, "boolean",
				"Draw a button and return whether it was clicked this frame." },
			{ "checkbox", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiCheckboxImpl(args, count);
			}, checkboxParams, 7, 7, "boolean",
				"Draw a checkbox; the script owns the value and stores the returned state." },
			{ "slider", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiSliderImpl(args, count);
			}, sliderParams, 8, 8, "number",
				"Draw a slider and return the value after this frame's input." },
			{ "image", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiImageImpl(args, count);
			}, imageParams, 9, 9, "",
				"Draw a texture from an asset path; the UV rectangle defaults to the full texture." },
			{ "list", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiListImpl(args, count);
			}, listParams, 8, 8, "integer|nil",
				"Draw a scrollable list; returns the clicked 1-based row, or the clamped selection when selectedIndex is out of range." },
			{ "grid", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiGridImpl(args, count);
			}, gridParams, 9, 9, "integer|nil",
				"Draw a scrollable grid; returns the clicked 1-based cell, or the clamped selection when selectedIndex is out of range." },
			{ "rows", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiRowsImpl(args, count);
			}, splitParams, 6, 6, "table",
				"Split a rectangle into a row of cells; returns {x, y, w, h} tables." },
			{ "columns", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiColumnsImpl(args, count);
			}, splitParams, 6, 6, "table",
				"Split a rectangle into a column of cells; returns {x, y, w, h} tables." },
		};
		static const ScriptServiceBinding uiTables[] = {
			{ "ui", "Immediate-mode script UI table; draw calls are rebuilt every frame and there is no long-lived callback registration.",
				uiMethods, 10 },
		};
		if (count)
			*count = sizeof(uiTables) / sizeof(uiTables[0]);
		return uiTables;
	}
}
