#include "World/UI/UiHost.h"
#include "wldpch.h"

#include "World/Renderer/Texture/TextureLibrary.h"
#include "World/Script/Bindings/BindUI.h"
#include "World/Core/Application.h"

#include "World/Renderer/Renderer.h"
#include "World/Renderer/Texture/TextureLibrary.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Bindings/BindServices.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Vm/ScriptRef.h"
#include "World/Script/Vm/ScriptValue.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/WuiWidgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

		void NormalizeArgs(const ScriptValue*& args, std::size_t& count)
		{
			if (count > 0 && args[0].IsTable())
			{
				++args;
				--count;
			}
		}

		bool TryParseHexColor(const std::string& text, Wui::WuiColor& out)
		{
			std::string hex = text;
			if (!hex.empty() && hex.front() == '#') hex.erase(0, 1);
			if (hex.size() == 6 || hex.size() == 8)
			{
				unsigned int r = 0, g = 0, b = 0, a = 255;
				if (std::sscanf(hex.substr(0, 2).c_str(), "%x", &r) == 1 &&
				    std::sscanf(hex.substr(2, 2).c_str(), "%x", &g) == 1 &&
				    std::sscanf(hex.substr(4, 2).c_str(), "%x", &b) == 1)
				{
					if (hex.size() == 8) std::sscanf(hex.substr(6, 2).c_str(), "%x", &a);
					out = Wui::WuiColor {
						static_cast<float>(r) / 255.0f,
						static_cast<float>(g) / 255.0f,
						static_cast<float>(b) / 255.0f,
						static_cast<float>(a) / 255.0f
					};
					return true;
				}
			}
			return false;
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
			// 设备重建(WuiTextureRegistry::Clear → Generation 变化)后旧句柄已失效:
			// 重新向 TextureLibrary 取(它自己也按设备失效重建),再登记一次。
			if (entry.Generation != registry.Generation())
				entry.Id = 0;
			if (entry.Id == 0)
			{
				// 唯一的 GPU 纹理驻留:与材质贴图/2D 精灵共用同一份,不在这里再建一份。
				const Rhi::Handle<Rhi::Texture> texture =
					TextureLibrary::Get().Get(path, /*srgb*/ true);
				if (!texture)
					throw std::logic_error("ui.image could not create a texture for '" + path + "'");
				entry.Id = registry.Register(texture);
				entry.Generation = registry.Generation();
			}
			return entry.Id;
		}

		// ---- 每个方法的实现 ----

		ScriptValue UiPanelImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = ScriptUiBindings(nullptr)[0].Methods[0];
			CheckArgumentCount("ui", method, count, 5);
			const std::string title = RequireString("ui", method, args, count, 4);
			const Wui::WuiRect rect = RequireRect("ui", method, args, count, 0);
			Wui::Panel(CurrentUiContext(), rect, title, Wui::WuiDefaultTheme());
			return ScriptValue::Nil();
		}

		ScriptValue UiTextImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = ScriptUiBindings(nullptr)[0].Methods[1];
			CheckArgumentCount("ui", method, count, 3);
			const glm::vec2 position {
				static_cast<float>(RequireNumber("ui", method, args, count, 0)),
				static_cast<float>(RequireNumber("ui", method, args, count, 1)),
			};
			const std::string text = RequireString("ui", method, args, count, 2);
			const float fontSize = static_cast<float>(OptionalNumber("ui", method, args, count, 3, 15.0));
			Wui::WuiColor textColor = Wui::WuiDefaultTheme().Text;
			if (count >= 5 && args[4].IsString())
			{
				std::string colorHex;
				args[4].AsString(&colorHex);
				TryParseHexColor(colorHex, textColor);
			}
			Wui::Label(CurrentUiContext(), position, text, textColor, fontSize);
			return ScriptValue::Nil();
		}

		ScriptValue UiButtonImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			if (count < 5)
				throw std::logic_error("ui.button expects at least (label, x, y, w, h) or (id, x, y, w, h, label)");

			std::string id;
			Wui::WuiRect rect{};
			std::string label;
			const ScriptValue* optionsTable = nullptr;

			if (args[0].IsString() && args[1].IsNumber() && args[2].IsNumber() && args[3].IsNumber() && args[4].IsNumber())
			{
				// 现代便捷用法: ui.button(label, x, y, w, h, [options])
				args[0].AsString(&label);
				double x = 0, y = 0, w = 100, h = 30;
				args[1].AsNumber(&x);
				args[2].AsNumber(&y);
				args[3].AsNumber(&w);
				args[4].AsNumber(&h);
				rect = { static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h) };
				id = label + "@" + std::to_string(static_cast<int>(rect.X)) + "," + std::to_string(static_cast<int>(rect.Y));
				if (count >= 6 && args[5].IsTable())
				{
					optionsTable = &args[5];
				}
			}
			else
			{
				// 兼容传统方法签名: ui.button(id, x, y, w, h, label, [options])
				args[0].AsString(&id);
				double x = 0, y = 0, w = 100, h = 30;
				if (count >= 2 && args[1].IsNumber()) args[1].AsNumber(&x);
				if (count >= 3 && args[2].IsNumber()) args[2].AsNumber(&y);
				if (count >= 4 && args[3].IsNumber()) args[3].AsNumber(&w);
				if (count >= 5 && args[4].IsNumber()) args[4].AsNumber(&h);
				rect = { static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h) };
				if (count >= 6 && args[5].IsString()) args[5].AsString(&label);
				if (count >= 7 && args[6].IsTable()) optionsTable = &args[6];
			}

			auto& ctx = CurrentUiContext();
			const Wui::WuiId widgetId = HashUiId(id);
			const bool hovered = ctx.IsHovered(rect);
			const bool clicked = hovered && ctx.Input().MouseClicked[0];
			const bool pressed = hovered && ctx.Input().MouseDown[0];

			ScriptTableRef optTable;
			if (optionsTable && optionsTable->AsTable(&optTable) && optTable.IsValid())
			{
				Wui::WuiColor bgCol{ 0.18f, 0.22f, 0.28f, 0.95f };
				Wui::WuiColor hoverCol{ 0.25f, 0.35f, 0.50f, 1.0f };
				Wui::WuiColor pressCol{ 0.12f, 0.18f, 0.25f, 1.0f };
				Wui::WuiColor textCol{ 1.0f, 1.0f, 1.0f, 1.0f };
				Wui::WuiColor borderCol{ 0.35f, 0.45f, 0.60f, 0.8f };
				float radius = 4.0f;
				float borderThick = 1.0f;
				float fontSize = 13.0f;

				auto readStr = [&](const char* f1, const char* f2 = nullptr, const char* f3 = nullptr) -> std::string {
					ScriptValue v = optTable.GetField(f1);
					std::string res;
					if (v.AsString(&res)) return res;
					if (f2) { v = optTable.GetField(f2); if (v.AsString(&res)) return res; }
					if (f3) { v = optTable.GetField(f3); if (v.AsString(&res)) return res; }
					return "";
				};
				auto readNum = [&](const char* f1, const char* f2 = nullptr) -> double {
					ScriptValue v = optTable.GetField(f1);
					double res = 0;
					if (v.AsNumber(&res)) return res;
					if (f2) { v = optTable.GetField(f2); if (v.AsNumber(&res)) return res; }
					return -1e9;
				};

				std::string bgStr = readStr("bgColor", "bg_color", "color");
				if (!bgStr.empty()) TryParseHexColor(bgStr, bgCol);

				std::string hoverStr = readStr("hoverColor", "hover_color");
				if (!hoverStr.empty()) TryParseHexColor(hoverStr, hoverCol);
				else hoverCol = { std::min(1.0f, bgCol.R * 1.25f), std::min(1.0f, bgCol.G * 1.25f), std::min(1.0f, bgCol.B * 1.25f), 1.0f };

				std::string pressStr = readStr("pressColor", "press_color");
				if (!pressStr.empty()) TryParseHexColor(pressStr, pressCol);
				else pressCol = { bgCol.R * 0.8f, bgCol.G * 0.8f, bgCol.B * 0.8f, 1.0f };

				std::string textStr = readStr("textColor", "text_color");
				if (!textStr.empty()) TryParseHexColor(textStr, textCol);

				std::string borderStr = readStr("borderColor", "border_color");
				if (!borderStr.empty()) TryParseHexColor(borderStr, borderCol);

				double r = readNum("borderRadius", "radius");
				if (r > -1e8) radius = static_cast<float>(r);
				double bw = readNum("borderWidth", "border_width");
				if (bw > -1e8) borderThick = static_cast<float>(bw);
				double fs = readNum("fontSize", "font_size");
				if (fs > -1e8) fontSize = static_cast<float>(fs);

				const Wui::WuiColor currentBg = pressed ? pressCol : (hovered ? hoverCol : bgCol);
				auto& cmds = ctx.Commands();
				cmds.push_back({ Wui::WuiDrawKind::Rect, rect, currentBg, radius });
				if (borderThick > 0.0f)
				{
					cmds.push_back({ Wui::WuiDrawKind::RectOutline, rect, borderCol, radius, borderThick });
				}
				const float estimatedTextWidth = static_cast<float>(label.length()) * (fontSize * 0.55f);
				const float textX = std::max(rect.X + 4.0f, rect.X + (rect.W - estimatedTextWidth) * 0.5f);
				const float textY = rect.Y + (rect.H - fontSize) * 0.5f;
				cmds.push_back({ Wui::WuiDrawKind::Text, { textX, textY, 0, 0 }, textCol, 0, 1.0f, label, fontSize, true });
				return ScriptValue::Boolean(clicked);
			}

			const bool res = Wui::Button(ctx, widgetId, rect, label, Wui::WuiDefaultTheme());
			return ScriptValue::Boolean(res);
		}

		ScriptValue UiCheckboxImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
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
			NormalizeArgs(args, count);
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
			NormalizeArgs(args, count);
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
			NormalizeArgs(args, count);
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
			NormalizeArgs(args, count);
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

		ScriptValue UiRectImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = ScriptUiBindings(nullptr)[0].Methods[14];
			CheckArgumentCount("ui", method, count, 5);
			const Wui::WuiRect rect = RequireRect("ui", method, args, count, 0);
			const std::string colorHex = RequireString("ui", method, args, count, 4);
			Wui::WuiColor fillColor { 1.0f, 1.0f, 1.0f, 1.0f };
			TryParseHexColor(colorHex, fillColor);
			const float radius = static_cast<float>(OptionalNumber("ui", method, args, count, 5, 0.0));
			CurrentUiContext().Commands().push_back({ Wui::WuiDrawKind::Rect, rect, fillColor, radius });
			if (count >= 7 && args[6].IsString())
			{
				std::string borderHex;
				args[6].AsString(&borderHex);
				Wui::WuiColor borderColor { 0.0f, 0.0f, 0.0f, 1.0f };
				if (TryParseHexColor(borderHex, borderColor))
				{
					const float borderThick = static_cast<float>(OptionalNumber("ui", method, args, count, 7, 1.0));
					CurrentUiContext().Commands().push_back({ Wui::WuiDrawKind::RectOutline, rect, borderColor, radius, borderThick });
				}
			}
			return ScriptValue::Nil();
		}

		ScriptValue UiLineImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = ScriptUiBindings(nullptr)[0].Methods[15];
			CheckArgumentCount("ui", method, count, 5);
			const glm::vec2 from {
				static_cast<float>(RequireNumber("ui", method, args, count, 0)),
				static_cast<float>(RequireNumber("ui", method, args, count, 1)),
			};
			const glm::vec2 to {
				static_cast<float>(RequireNumber("ui", method, args, count, 2)),
				static_cast<float>(RequireNumber("ui", method, args, count, 3)),
			};
			const std::string colorHex = RequireString("ui", method, args, count, 4);
			Wui::WuiColor color { 1.0f, 1.0f, 1.0f, 1.0f };
			TryParseHexColor(colorHex, color);
			const float thickness = static_cast<float>(OptionalNumber("ui", method, args, count, 5, 1.0));
			const glm::vec2 delta = to - from;
			const float len = glm::length(delta);
			if (len > 0.0001f)
			{
				const glm::vec2 normal { -delta.y / len * thickness * 0.5f, delta.x / len * thickness * 0.5f };
				Wui::WuiDrawCommand command;
				command.Kind = Wui::WuiDrawKind::Quad;
				command.Color = color;
				command.Vertices = { from + normal, to + normal, to - normal, from - normal };
				CurrentUiContext().Commands().push_back(std::move(command));
			}
			return ScriptValue::Nil();
		}

		ScriptValue UiProgressBarImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const ScriptServiceMethod& method = ScriptUiBindings(nullptr)[0].Methods[16];
			CheckArgumentCount("ui", method, count, 6);
			const Wui::WuiRect rect = RequireRect("ui", method, args, count, 0);
			const double progressVal = RequireNumber("ui", method, args, count, 4);
			const float progress = static_cast<float>(std::clamp(progressVal, 0.0, 1.0));
			const std::string fillHex = RequireString("ui", method, args, count, 5);
			Wui::WuiColor fillColor { 0.3f, 0.75f, 0.5f, 1.0f };
			TryParseHexColor(fillHex, fillColor);
			Wui::WuiColor trackColor { 0.12f, 0.14f, 0.18f, 0.9f };
			if (count >= 7 && args[6].IsString())
			{
				std::string trackHex;
				args[6].AsString(&trackHex);
				TryParseHexColor(trackHex, trackColor);
			}
			const float radius = static_cast<float>(OptionalNumber("ui", method, args, count, 7, 3.0));
			auto& cmds = CurrentUiContext().Commands();
			cmds.push_back({ Wui::WuiDrawKind::Rect, rect, trackColor, radius });
			if (progress > 0.001f)
			{
				const Wui::WuiRect fillRect { rect.X, rect.Y, rect.W * progress, rect.H };
				cmds.push_back({ Wui::WuiDrawKind::Rect, fillRect, fillColor, radius });
			}
			return ScriptValue::Nil();
		}

		ScriptValue UiSetImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			if (count < 3 || !args[0].IsString() || !args[1].IsString() || !args[2].IsString())
				return ScriptValue::Boolean(false);
			std::string nodeId, prop, val;
			args[0].AsString(&nodeId);
			args[1].AsString(&prop);
			args[2].AsString(&val);
			if (UiHost* host = UiHost::GetActive())
			{
				host->SetProperty(nodeId, prop, val);
				return ScriptValue::Boolean(true);
			}
			return ScriptValue::Boolean(false);
		}

		ScriptValue UiAnchorImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			std::string anchor = "center";
			if (count >= 1 && args[0].IsString()) args[0].AsString(&anchor);
			for (char& c : anchor) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

			float w = 0.0f;
			float h = 0.0f;
			if (count >= 2 && args[1].IsNumber()) { double v; args[1].AsNumber(&v); w = static_cast<float>(v); }
			if (count >= 3 && args[2].IsNumber()) { double v; args[2].AsNumber(&v); h = static_cast<float>(v); }

			float vw = 1280.0f;
			float vh = 720.0f;
			if (Application::HasInstance())
			{
				vw = static_cast<float>(Application::Get().GetWindow().GetWidth());
				vh = static_cast<float>(Application::Get().GetWindow().GetHeight());
			}
			float scale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
			vw /= scale;
			vh /= scale;
			float margin = 16.0f;

			float x = 0.0f;
			float y = 0.0f;
			if (anchor == "top_left") { x = margin; y = margin; }
			else if (anchor == "top_center") { x = (vw - w) * 0.5f; y = margin; }
			else if (anchor == "top_right") { x = vw - w - margin; y = margin; }
			else if (anchor == "center_left") { x = margin; y = (vh - h) * 0.5f; }
			else if (anchor == "center") { x = (vw - w) * 0.5f; y = (vh - h) * 0.5f; }
			else if (anchor == "center_right") { x = vw - w - margin; y = (vh - h) * 0.5f; }
			else if (anchor == "bottom_left") { x = margin; y = vh - h - margin; }
			else if (anchor == "bottom_center") { x = (vw - w) * 0.5f; y = vh - h - margin; }
			else if (anchor == "bottom_right") { x = vw - w - margin; y = vh - h - margin; }
			else { x = (vw - w) * 0.5f; y = (vh - h) * 0.5f; }

			ScriptTableRef res = ScriptEngine::GetState().CreateTable();
			res.SetField("x", ScriptValue::Number(static_cast<double>(x)));
			res.SetField("y", ScriptValue::Number(static_cast<double>(y)));
			res.SetField("w", ScriptValue::Number(static_cast<double>(w)));
			res.SetField("h", ScriptValue::Number(static_cast<double>(h)));
			return res.ToValue();
		}

		ScriptValue UiBadgeImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			if (count < 3 || !args[0].IsString()) return ScriptValue::Nil();
			std::string text;
			args[0].AsString(&text);
			double x = 0.0, y = 0.0;
			args[1].AsNumber(&x);
			args[2].AsNumber(&y);

			Wui::WuiColor bgCol{ 0.15f, 0.20f, 0.28f, 0.85f };
			Wui::WuiColor textCol{ 0.95f, 0.95f, 0.95f, 1.0f };
			float fontSize = 12.0f;
			float padX = 8.0f;
			float padY = 4.0f;
			float radius = 999.0f;

			ScriptTableRef badgeTable;
			if (count >= 4 && args[3].IsTable() && args[3].AsTable(&badgeTable) && badgeTable.IsValid())
			{
				ScriptValue bgVal = badgeTable.GetField("bgColor");
				std::string s;
				if (bgVal.AsString(&s)) TryParseHexColor(s, bgCol);
				else {
					bgVal = badgeTable.GetField("color");
					if (bgVal.AsString(&s)) TryParseHexColor(s, bgCol);
				}
				ScriptValue textVal = badgeTable.GetField("textColor");
				if (textVal.AsString(&s)) TryParseHexColor(s, textCol);
				double n = 0;
				ScriptValue fsVal = badgeTable.GetField("fontSize");
				if (fsVal.AsNumber(&n)) fontSize = static_cast<float>(n);
				ScriptValue radVal = badgeTable.GetField("radius");
				if (radVal.AsNumber(&n)) radius = static_cast<float>(n);
			}

			float textW = static_cast<float>(text.length()) * (fontSize * 0.55f);
			float totalW = textW + padX * 2.0f;
			float totalH = fontSize + padY * 2.0f;
			Wui::WuiRect rect{ static_cast<float>(x), static_cast<float>(y), totalW, totalH };

			auto& cmds = CurrentUiContext().Commands();
			cmds.push_back({ Wui::WuiDrawKind::Rect, rect, bgCol, radius });
			cmds.push_back({ Wui::WuiDrawKind::Text, { rect.X + padX, rect.Y + padY, 0, 0 }, textCol, 0, 1.0f, text, fontSize, true });

			ScriptTableRef res = ScriptEngine::GetState().CreateTable();
			res.SetField("w", ScriptValue::Number(static_cast<double>(totalW)));
			res.SetField("h", ScriptValue::Number(static_cast<double>(totalH)));
			return res.ToValue();
		}

		ScriptValue UiGetImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			if (count < 2 || !args[0].IsString() || !args[1].IsString())
				return ScriptValue::Nil();
			std::string nodeId, prop;
			args[0].AsString(&nodeId);
			args[1].AsString(&prop);
			if (UiHost* host = UiHost::GetActive())
			{
				const std::string val = host->GetProperty(nodeId, prop);
				if (!val.empty())
					return ScriptValue::String(val);
			}
			return ScriptValue::Nil();
		}
		ScriptValue UiAxisSplitImpl(const ScriptValue* args, std::size_t count, bool rows)
		{
			NormalizeArgs(args, count);
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
			NormalizeArgs(args, count);
			return UiAxisSplitImpl(args, count, true);
		}

		ScriptValue UiColumnsImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			return UiAxisSplitImpl(args, count, false);
		}

		// M55:`ui.onDraw(fn)` —— 在**系统脚本加载期**注册一个 UI 绘制回调。
		//
		// 为什么是"注册回调"而不是让脚本自己每帧跑:
		//   * 系统脚本只在加载时执行一次(它注册的是 systems),UI 阶段晚于 Update;
		//   * 每帧由宿主 `ScriptEngine::DrawScriptUi` 在 `ScriptUiScope(脚本逻辑路径)` 里调用回调
		//     ⇒ `ui.*` 只在 UI 阶段可用(否则绑定层抛"can only be called during the UI phase"),
		//     控件 id 自动带脚本前缀,两份脚本用同一个 "go" 也不撞;
		//   * 热重载 = 整份重跑 ⇒ 旧回调先被清掉(否则每改一次脚本界面就叠一层)。
		
		ScriptValue UiOpenImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			if (count < 1 || !args[0].IsString())
				return ScriptValue::Boolean(false);
			std::string path;
			args[0].AsString(&path);
			if (UiHost* host = UiHost::GetActive())
				return ScriptValue::Boolean(host->OpenDocument(path));
			return ScriptValue::Boolean(false);
		}

		ScriptValue UiCloseImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			(void)args; (void)count;
			if (UiHost* host = UiHost::GetActive())
			{
				host->CloseDocument();
				return ScriptValue::Boolean(true);
			}
			return ScriptValue::Boolean(false);
		}

		ScriptValue UiIsOpenImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			(void)args; (void)count;
			if (UiHost* host = UiHost::GetActive())
				return ScriptValue::Boolean(host->Enabled());
			return ScriptValue::Boolean(false);
		}

		ScriptValue UiOnDrawImpl(const ScriptValue* args, std::size_t count)
		{
			const ScriptValue* callbackArg = nullptr;
			if (count == 1 && args[0].IsFunction())
			{
				callbackArg = &args[0];
			}
			else if (count >= 2 && args[0].IsTable() && args[1].IsFunction())
			{
				// 容错: 兼容 ui:onDraw(fn) 冒号语法(args[0]=ui table, args[1]=callback function)
				callbackArg = &args[1];
			}
			else
			{
				throw std::logic_error("ui.onDraw expects a single function argument");
			}
			std::string error;
			ScriptEngine::RegisterScriptUiDraw(*callbackArg, &error);
			if (!error.empty())
				throw std::logic_error(error);
			return ScriptValue();
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
			{ "color", "string", ScriptServiceArgType::String, false, "Text color hex (e.g. '#FFCC00' or '#FFCC00FF'); defaults to theme text color." },
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
		static const ScriptServiceParam onDrawParams[] = {
			{ "draw", "function", ScriptServiceArgType::Table, true,
				"UI draw callback; it runs every frame during the UI phase, after script systems have updated." },
		};
				static const ScriptServiceParam openParams[] = {
			{ "path", "string", ScriptServiceArgType::String, true, "Relative or absolute path to the .wui document." },
		};
		static const ScriptServiceParam splitParams[] = {
			{ "x", "number", ScriptServiceArgType::Number, true, "Left edge in UI pixels." },
			{ "y", "number", ScriptServiceArgType::Number, true, "Top edge in UI pixels." },
			{ "w", "number", ScriptServiceArgType::Number, true, "Width in UI pixels." },
			{ "h", "number", ScriptServiceArgType::Number, true, "Height in UI pixels." },
			{ "count", "integer", ScriptServiceArgType::Number, true, "Number of cells; zero yields an empty table." },
			{ "gap", "number", ScriptServiceArgType::Number, false, "Gap between cells in pixels; defaults to 0." },
		};
		static const ScriptServiceParam rectParams[] = {
			{ "x", "number", ScriptServiceArgType::Number, true, "Left edge in UI pixels." },
			{ "y", "number", ScriptServiceArgType::Number, true, "Top edge in UI pixels." },
			{ "w", "number", ScriptServiceArgType::Number, true, "Width in UI pixels." },
			{ "h", "number", ScriptServiceArgType::Number, true, "Height in UI pixels." },
			{ "color", "string", ScriptServiceArgType::String, true, "Fill color in hex format (#RRGGBB or #RRGGBBAA)." },
			{ "radius", "number", ScriptServiceArgType::Number, false, "Corner radius in pixels; defaults to 0." },
			{ "borderColor", "string", ScriptServiceArgType::String, false, "Optional border outline color in hex format." },
			{ "borderThickness", "number", ScriptServiceArgType::Number, false, "Border outline thickness; defaults to 1." },
		};
		static const ScriptServiceParam lineParams[] = {
			{ "x1", "number", ScriptServiceArgType::Number, true, "Start X in UI pixels." },
			{ "y1", "number", ScriptServiceArgType::Number, true, "Start Y in UI pixels." },
			{ "x2", "number", ScriptServiceArgType::Number, true, "End X in UI pixels." },
			{ "y2", "number", ScriptServiceArgType::Number, true, "End Y in UI pixels." },
			{ "color", "string", ScriptServiceArgType::String, true, "Line color in hex format (#RRGGBB or #RRGGBBAA)." },
			{ "thickness", "number", ScriptServiceArgType::Number, false, "Line thickness; defaults to 1." },
		};
		static const ScriptServiceParam progressParams[] = {
			{ "x", "number", ScriptServiceArgType::Number, true, "Left edge in UI pixels." },
			{ "y", "number", ScriptServiceArgType::Number, true, "Top edge in UI pixels." },
			{ "w", "number", ScriptServiceArgType::Number, true, "Width in UI pixels." },
			{ "h", "number", ScriptServiceArgType::Number, true, "Height in UI pixels." },
			{ "progress", "number", ScriptServiceArgType::Number, true, "Progress fraction [0.0, 1.0]." },
			{ "fillColor", "string", ScriptServiceArgType::String, true, "Progress fill color in hex format." },
			{ "trackColor", "string", ScriptServiceArgType::String, false, "Track background color in hex format; defaults to dark." },
			{ "radius", "number", ScriptServiceArgType::Number, false, "Corner radius in pixels; defaults to 3." },
		};
		static const ScriptServiceParam setPropParams[] = {
			{ "nodeId", "string", ScriptServiceArgType::String, true, "Node ID in active .wui document." },
			{ "prop", "string", ScriptServiceArgType::String, true, "Property name (text, value, color, bg, border, radius)." },
			{ "value", "string", ScriptServiceArgType::String, true, "New string value." },
		};
		static const ScriptServiceParam getPropParams[] = {
			{ "nodeId", "string", ScriptServiceArgType::String, true, "Node ID in active .wui document." },
			{ "prop", "string", ScriptServiceArgType::String, true, "Property name to query." },
		};
		static const ScriptServiceParam anchorParams[] = {
			{ "anchor", "string", ScriptServiceArgType::String, false, "Screen anchor name ('top_center', 'center', 'bottom_right', etc.)." },
			{ "w", "number", ScriptServiceArgType::Number, false, "Element width in UI pixels." },
			{ "h", "number", ScriptServiceArgType::Number, false, "Element height in UI pixels." },
		};
		static const ScriptServiceParam badgeParams[] = {
			{ "text", "string", ScriptServiceArgType::String, true, "Badge label text." },
			{ "x", "number", ScriptServiceArgType::Number, true, "X position in UI pixels." },
			{ "y", "number", ScriptServiceArgType::Number, true, "Y position in UI pixels." },
			{ "options", "table", ScriptServiceArgType::Table, false, "Styling options {bgColor, textColor, fontSize, radius}." },
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
			}, textParams, 5, 5, "",
				"Draw a single line of text with optional font size and hex color." },
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
			// M55:唯一的"注册型"方法 —— 它是 UI 阶段唯一的入口(其余方法都只能在回调里调用)。
			{ "onDraw", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiOnDrawImpl(args, count);
			}, onDrawParams, 1, 1, "",
				"Register a function that draws this script's UI every frame. Both ui.onDraw(draw) and ui:onDraw(draw) are supported." },
			{ "open", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiOpenImpl(args, count);
			}, openParams, 1, 1, "boolean",
				"Actively open and display a declarative .wui document." },
			{ "close", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiCloseImpl(args, count);
			}, nullptr, 0, 0, "boolean",
				"Close the currently active .wui document." },
			{ "isOpen", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiIsOpenImpl(args, count);
			}, nullptr, 0, 0, "boolean",
				"Check if a .wui document is currently open." },
			{ "rect", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiRectImpl(args, count);
			}, rectParams, 8, 8, "",
				"Draw a filled rounded rectangle with optional border outline." },
			{ "line", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiLineImpl(args, count);
			}, lineParams, 6, 6, "",
				"Draw an antialiased line between two UI points." },
			{ "progressBar", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiProgressBarImpl(args, count);
			}, progressParams, 8, 8, "",
				"Draw a smooth health/mana/progress bar." },
			{ "set", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiSetImpl(args, count);
			}, setPropParams, 3, 3, "boolean",
				"Dynamically override a property on an active .wui document node." },
			{ "get", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiGetImpl(args, count);
			}, getPropParams, 2, 2, "string|nil",
				"Get the effective property value of an active .wui document node." },
			{ "anchor", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiAnchorImpl(args, count);
			}, anchorParams, 3, 3, "table",
				"Calculate auto-anchored screen coordinates for a HUD element (e.g. 'top_center', 'bottom_right')." },
			{ "badge", [](const ScriptValue* args, std::size_t count) -> ScriptValue
			{
				return UiBadgeImpl(args, count);
			}, badgeParams, 4, 4, "table",
				"Draw a styled pill tag / badge for scores, streaks and combos; returns {w, h}." },
		};
		static const ScriptServiceBinding uiTables[] = {
			{ "ui", "Immediate-mode script UI table and declarative .wui document control.",
				uiMethods, sizeof(uiMethods) / sizeof(uiMethods[0]) },
		};
		if (count)
			*count = sizeof(uiTables) / sizeof(uiTables[0]);
		return uiTables;
	}
}
