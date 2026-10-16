#include "wldpch.h"
#include "World/Script/Bindings/BindStreaming.h"

#include "World/Level/Streaming/SpatialGridManager.h"
#include "World/Gameplay/Framework/GlobalStateStore.h"
#include "World/Script/Bindings/BindServices.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Vm/ScriptBindingContext.h"
#include "World/Script/Vm/ScriptRef.h"
#include "World/Script/Vm/ScriptValue.h"

#include <stdexcept>
#include <string>

namespace World
{
	namespace
	{
		static Gameplay::GlobalStateStore s_GlobalStore;
		static Level::SpatialGridManager s_GridManager;

		void NormalizeArgs(const ScriptValue*& args, std::size_t& count)
		{
			if (count > 0 && args[0].IsTable())
			{
				++args;
				--count;
			}
		}

		double RequireNum(const std::string& table, const ScriptServiceMethod& method,
		                  const ScriptValue* args, std::size_t count, std::size_t index)
		{
			if (index >= count || args[index].IsNil())
				throw std::logic_error(table + "." + method.Name + ": missing number argument " + std::to_string(index + 1));
			double num = 0.0;
			if (!args[index].AsNumber(&num))
				throw std::logic_error(table + "." + method.Name + ": argument " + std::to_string(index + 1) + " must be number");
			return num;
		}

		std::string RequireStr(const std::string& table, const ScriptServiceMethod& method,
		                       const ScriptValue* args, std::size_t count, std::size_t index)
		{
			if (index >= count || args[index].IsNil())
				throw std::logic_error(table + "." + method.Name + ": missing string argument " + std::to_string(index + 1));
			std::string s;
			if (!args[index].AsString(&s))
				throw std::logic_error(table + "." + method.Name + ": argument " + std::to_string(index + 1) + " must be string");
			return s;
		}

		// Streaming Methods
		ScriptValue StreamingGetChunkCoordImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const auto& method = StreamingServiceBindings(nullptr)[0].Methods[0];
			float x = static_cast<float>(RequireNum("Streaming", method, args, count, 0));
			float y = static_cast<float>(RequireNum("Streaming", method, args, count, 1));
			float z = static_cast<float>(RequireNum("Streaming", method, args, count, 2));
			float chunkSize = (count >= 4 && args[3].IsNumber()) ? static_cast<float>(RequireNum("Streaming", method, args, count, 3)) : 64.0f;

			auto coord = Level::ChunkCoord3D::WorldToChunk(glm::vec3(x, y, z), chunkSize);

			ScriptTableRef res = ScriptEngine::GetState().CreateTable();
			res.SetField("x", ScriptValue::Number(coord.x));
			res.SetField("y", ScriptValue::Number(coord.y));
			res.SetField("z", ScriptValue::Number(coord.z));
			return res.ToValue();
		}

		ScriptValue StreamingGetActiveChunkCountImpl(const ScriptValue* args, std::size_t count)
		{
			(void)args; (void)count;
			return ScriptValue::Number(static_cast<double>(s_GridManager.ActiveChunkCount()));
		}

		// GlobalState Methods
		ScriptValue GlobalStateSetImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const auto& method = StreamingServiceBindings(nullptr)[1].Methods[0];
			std::string key = RequireStr("GlobalState", method, args, count, 0);
			if (count < 2 || args[1].IsNil())
				throw std::logic_error("GlobalState.Set: value argument cannot be nil");

			const ScriptValue& val = args[1];
			if (val.IsNumber())
			{
				double d = 0.0;
				val.AsNumber(&d);
				s_GlobalStore.SetNumber(key, d);
			}
			else if (val.IsBoolean())
			{
				bool b = false;
				val.AsBool(&b);
				s_GlobalStore.SetBool(key, b);
			}
			else if (val.IsString())
			{
				std::string s;
				val.AsString(&s);
				s_GlobalStore.SetString(key, s);
			}
			return ScriptValue::Nil();
		}

		ScriptValue GlobalStateGetImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const auto& method = StreamingServiceBindings(nullptr)[1].Methods[1];
			std::string key = RequireStr("GlobalState", method, args, count, 0);

			if (!s_GlobalStore.HasKey(key))
			{
				return (count >= 2) ? args[1] : ScriptValue::Nil();
			}

			// Try returning stored value
			std::string s = s_GlobalStore.GetString(key);
			if (!s.empty()) return ScriptValue::String(s);

			double num = s_GlobalStore.GetNumber(key);
			if (num != 0.0) return ScriptValue::Number(num);

			return ScriptValue::Boolean(s_GlobalStore.GetBool(key));
		}

		ScriptValue GlobalStateHasImpl(const ScriptValue* args, std::size_t count)
		{
			NormalizeArgs(args, count);
			const auto& method = StreamingServiceBindings(nullptr)[1].Methods[2];
			std::string key = RequireStr("GlobalState", method, args, count, 0);
			return ScriptValue::Boolean(s_GlobalStore.HasKey(key));
		}

		ScriptValue GlobalStateCommitImpl(const ScriptValue* args, std::size_t count)
		{
			(void)args; (void)count;
			return ScriptValue::Boolean(s_GlobalStore.CommitSnapshot());
		}

		static const ScriptServiceParam getCoordParams[] = {
			{ "x", "number", ScriptServiceArgType::Number, true, "World X coordinate." },
			{ "y", "number", ScriptServiceArgType::Number, true, "World Y coordinate." },
			{ "z", "number", ScriptServiceArgType::Number, true, "World Z coordinate." },
			{ "chunkSize", "number", ScriptServiceArgType::Number, false, "Chunk edge size (default 64)." }
		};

		static const ScriptServiceMethod streamingMethods[] = {
			{ "GetChunkCoord", [](const ScriptValue* args, std::size_t count) { return StreamingGetChunkCoordImpl(args, count); }, getCoordParams, 4, 3, "table", "Calculate 3D chunk coordinates from world coordinates." },
			{ "GetActiveChunkCount", [](const ScriptValue* args, std::size_t count) { return StreamingGetActiveChunkCountImpl(args, count); }, nullptr, 0, 0, "number", "Get total number of active streaming chunks." }
		};

		static const ScriptServiceParam gsKeyValParams[] = {
			{ "key", "string", ScriptServiceArgType::String, true, "Global state key." },
			{ "value", "any", ScriptServiceArgType::None, true, "Value to store (string, number, or boolean)." }
		};
		static const ScriptServiceParam gsKeyOnlyParams[] = {
			{ "key", "string", ScriptServiceArgType::String, true, "Global state key." },
			{ "fallback", "any", ScriptServiceArgType::None, false, "Fallback value if key does not exist." }
		};

		static const ScriptServiceMethod globalStateMethods[] = {
			{ "Set", [](const ScriptValue* args, std::size_t count) { return GlobalStateSetImpl(args, count); }, gsKeyValParams, 2, 2, "", "Store a global persistent key-value entry." },
			{ "Get", [](const ScriptValue* args, std::size_t count) { return GlobalStateGetImpl(args, count); }, gsKeyOnlyParams, 2, 1, "any", "Retrieve a global persistent value." },
			{ "Has", [](const ScriptValue* args, std::size_t count) { return GlobalStateHasImpl(args, count); }, gsKeyOnlyParams, 1, 1, "boolean", "Check if key exists in global store." },
			{ "Commit", [](const ScriptValue* args, std::size_t count) { return GlobalStateCommitImpl(args, count); }, nullptr, 0, 0, "boolean", "Commit WAL transaction journal to disk snapshot." }
		};

		static const ScriptServiceBinding streamingBindingsTable[] = {
			{ "Streaming", "World spatial hash chunk streaming service.", streamingMethods, sizeof(streamingMethods)/sizeof(streamingMethods[0]) },
			{ "GlobalState", "Global game state persistence and transaction journal store.", globalStateMethods, sizeof(globalStateMethods)/sizeof(globalStateMethods[0]) }
		};

		bool RegisterServiceTable(ScriptBindingContext& bindings, const ScriptServiceBinding& service, std::string* error)
		{
			LuauVm* vm = bindings.Vm();
			if (!vm || !service.Name || !service.Name[0] || !service.Methods || service.MethodCount == 0)
			{
				if (error) *error = "invalid service binding descriptor";
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

	const ScriptServiceBinding* StreamingServiceBindings(std::size_t* count)
	{
		if (count) *count = sizeof(streamingBindingsTable) / sizeof(streamingBindingsTable[0]);
		return streamingBindingsTable;
	}

	bool RegisterStreamingBindings(ScriptBindingContext& bindings, std::string* error)
	{
		std::size_t count = 0;
		const ScriptServiceBinding* services = StreamingServiceBindings(&count);
		for (std::size_t index = 0; index < count; ++index)
		{
			if (!RegisterServiceTable(bindings, services[index], error))
				return false;
		}
		if (error) error->clear();
		return true;
	}
}
