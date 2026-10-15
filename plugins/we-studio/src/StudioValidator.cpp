#include "StudioValidator.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"

namespace World::Studio
{
	using namespace World::Wui;

	std::string StudioValidationResult::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;
		root.Object.push_back({ "valid", JsonValue::MakeBool(valid) });

		JsonValue eArr; eArr.type = JsonValue::Type::Array;
		for (const auto& e : errors) eArr.Array.push_back(JsonValue::MakeString(e));
		root.Object.push_back({ "errors", std::move(eArr) });

		JsonValue wArr; wArr.type = JsonValue::Type::Array;
		for (const auto& w : warnings) wArr.Array.push_back(JsonValue::MakeString(w));
		root.Object.push_back({ "warnings", std::move(wArr) });

		return root.Dump();
	}

	StudioValidationResult StudioValidator::Validate(const StudioPolicy& policy)
	{
		StudioValidationResult res;
		const auto& roles = policy.GetRoles();

		for (const auto& kv : roles)
		{
			const auto& r = kv.second;

			// 1. 循环评审/自审自过检测
			for (const auto& revTarget : r.reviews)
			{
				if (revTarget == r.id)
				{
					res.errors.push_back("Circular review error: Role '" + r.id + "' cannot review itself.");
					res.valid = false;
				}
			}

			// 2. 写入范围越界检测
			for (const auto& w : r.write)
			{
				if (w.rfind("..", 0) == 0 || w.find("/../") != std::string::npos)
				{
					res.errors.push_back("Illegal write path: Role '" + r.id + "' write scope '" + w + "' escapes project root.");
					res.valid = false;
				}
				if (w.rfind("Engine/", 0) == 0 || w.rfind("Editor/", 0) == 0 || w.rfind("Runtime/", 0) == 0)
				{
					res.errors.push_back("Security violation: Role '" + r.id + "' attempts to write into protected engine tree: " + w);
					res.valid = false;
				}
			}
		}

		res.valid = res.errors.empty();
		return res;
	}

	void StudioValidator::RegisterCommands(World::Ai::CommandRegistry& reg)
	{
		using namespace World::Ai;
		CommandDescriptor desc;
		desc.name = "studio.validate";
		desc.description = "Validate studio preset, role definitions, circular reviews, and write scopes.";
		desc.effect = CommandEffect::Read;
		desc.scope = CommandScope::Both;
		desc.owner = "plugin:we-studio";
		desc.paramsSchema = "{\"type\":\"object\"}";
		desc.resultSchema = "{\"type\":\"object\",\"properties\":{\"valid\":{\"type\":\"boolean\"},\"errors\":{\"type\":\"array\"}}}";
		desc.handler = [](const std::map<std::string, std::string>& /*args*/, std::string& result, std::string& /*error*/)
		{
			StudioPolicy policy;
			result = Validate(policy).ToJson();
			return true;
		};
		reg.RegisterCommand(std::move(desc));
	}
}
