#include "wldpch.h"
#include "World/Script/Bindings/BindCurve.h"

#include "World/Math/Curve/CubicBezierCurve.h"
#include "World/Math/Curve/CurveLibrary.h"
#include "World/Script/Bindings/BindServices.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Vm/ScriptBindingContext.h"
#include "World/Script/Vm/ScriptRef.h"
#include "World/Script/Vm/ScriptValue.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace World
{
	namespace
	{
		void NormalizeArgs(const ScriptValue*& args, std::size_t& count)
		{
			if (count > 0 && args[0].IsTable())
			{
				++args;
				--count;
			}
		}

		ScriptValue CurveEvaluateImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			if (count < 2)
				throw std::logic_error("Curve.Evaluate: expected (path: string, time: number)");

			std::string path;
			if (!args[0].AsString(&path))
				throw std::logic_error("Curve.Evaluate: argument 1 ('path') must be string");

			double timeVal = 0.0;
			if (!args[1].AsNumber(&timeVal))
				throw std::logic_error("Curve.Evaluate: argument 2 ('time') must be number");

			auto curve = CurveLibrary::Get().Get(path);
			if (!curve)
				curve = CurveLibrary::Get().Load(path);

			if (!curve)
				return ScriptValue::Number(0.0);

			float result = curve->Evaluate(static_cast<float>(timeVal));
			return ScriptValue::Number(static_cast<double>(result));
		}

		ScriptValue CurveEvaluatePresetImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			if (count < 2)
				throw std::logic_error("Curve.EvaluatePreset: expected (presetName: string, time: number)");

			std::string preset;
			if (!args[0].AsString(&preset))
				throw std::logic_error("Curve.EvaluatePreset: argument 1 ('presetName') must be string");

			double timeVal = 0.0;
			if (!args[1].AsNumber(&timeVal))
				throw std::logic_error("Curve.EvaluatePreset: argument 2 ('time') must be number");

			static const CubicBezierCurve s_Linear = CubicBezierCurve::Linear();
			static const CubicBezierCurve s_EaseIn = CubicBezierCurve::EaseIn();
			static const CubicBezierCurve s_EaseOut = CubicBezierCurve::EaseOut();
			static const CubicBezierCurve s_EaseInOut = CubicBezierCurve::EaseInOut();
			static const CubicBezierCurve s_Bounce = CubicBezierCurve::Bounce();

			float t = static_cast<float>(timeVal);
			if (preset == "Linear" || preset == "linear")
				return ScriptValue::Number(static_cast<double>(s_Linear.Evaluate(t)));
			if (preset == "EaseIn" || preset == "easeIn" || preset == "ease_in")
				return ScriptValue::Number(static_cast<double>(s_EaseIn.Evaluate(t)));
			if (preset == "EaseOut" || preset == "easeOut" || preset == "ease_out")
				return ScriptValue::Number(static_cast<double>(s_EaseOut.Evaluate(t)));
			if (preset == "EaseInOut" || preset == "easeInOut" || preset == "ease_in_out")
				return ScriptValue::Number(static_cast<double>(s_EaseInOut.Evaluate(t)));
			if (preset == "Bounce" || preset == "bounce")
				return ScriptValue::Number(static_cast<double>(s_Bounce.Evaluate(t)));

			throw std::logic_error("Curve.EvaluatePreset: unknown preset '" + preset + "'");
		}

		static const ScriptServiceParam evalParams[] = {
			{ "path", "string", ScriptServiceArgType::String, true, "Logical or file path to .wcurve asset." },
			{ "time", "number", ScriptServiceArgType::Number, true, "Evaluation time in seconds." }
		};

		static const ScriptServiceParam evalPresetParams[] = {
			{ "presetName", "string", ScriptServiceArgType::String, true, "Builtin preset ('Linear', 'EaseIn', 'EaseOut', 'EaseInOut', 'Bounce')." },
			{ "time", "number", ScriptServiceArgType::Number, true, "Evaluation time in seconds." }
		};

		static const ScriptServiceMethod curveMethods[] = {
			{ "Evaluate", [](const ScriptValue* args, std::size_t count) { return CurveEvaluateImpl(args, count); }, evalParams, 2, 2, "number", "Evaluate a curve asset at specified time with automatic caching." },
			{ "EvaluatePreset", [](const ScriptValue* args, std::size_t count) { return CurveEvaluatePresetImpl(args, count); }, evalPresetParams, 2, 2, "number", "Fast evaluate a builtin curve preset at specified time." }
		};

		static const ScriptServiceBinding curveBindingsTable[] = {
			{ "Curve", "Industrial curve evaluation and easing preset service.", curveMethods, sizeof(curveMethods)/sizeof(curveMethods[0]) }
		};

		bool RegisterServiceTable(ScriptBindingContext& bindings, const ScriptServiceBinding& service, std::string* error)
		{
			LuauVm* vm = bindings.Vm();
			if (!vm)
			{
				if (error) *error = std::string("cannot bind ") + service.Name + ": Luau VM is not available";
				return false;
			}
			ScriptTableRef table = vm->CreateTable();
			for (std::size_t index = 0; index < service.MethodCount; ++index)
			{
				const ScriptServiceMethod& method = service.Methods[index];
				if (!table.SetField(method.Name, bindings.CreateFunction(method.Name, method.Function)))
				{
					if (error) *error = std::string("failed to bind ") + std::string(service.Name) + "." + std::string(method.Name);
					return false;
				}
			}
			if (!table.SetMetaField("__newindex", bindings.CreateFunction(service.Name,
				[](const ScriptValue*, std::size_t) -> ScriptValue
				{
					throw std::logic_error("service tables are read-only; use the documented methods");
				})))
			{
				if (error) *error = std::string("failed to lock the service table: ") + service.Name;
				return false;
			}
			if (!vm->SetGlobal(service.Name, table.ToValue()))
			{
				if (error) *error = std::string("failed to publish the service table: ") + service.Name;
				return false;
			}
			return true;
		}
	}

	const ScriptServiceBinding* CurveServiceBindings(std::size_t* count)
	{
		if (count) *count = sizeof(curveBindingsTable) / sizeof(curveBindingsTable[0]);
		return curveBindingsTable;
	}

	bool RegisterCurveBindings(ScriptBindingContext& bindings, std::string* error)
	{
		std::size_t count = 0;
		const ScriptServiceBinding* services = CurveServiceBindings(&count);
		for (std::size_t index = 0; index < count; ++index)
		{
			if (!RegisterServiceTable(bindings, services[index], error))
				return false;
		}
		if (error) error->clear();
		return true;
	}

	bool BindCurve(lua_State* L, Scene* scene)
	{
		(void)L;
		if (scene)
			ScriptEngine::SetActiveScene(scene);
		if (ScriptEngine::IsInitialized())
			return RegisterCurveBindings(ScriptEngine::GetBindingContext());
		return true;
	}
}

