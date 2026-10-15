#pragma once

#include "World/WUI/WuiJson.h"
#include <string>
#include <vector>

namespace World::Mcp
{
	struct McpRequest
	{
		std::string jsonrpc = "2.0";
		std::string id;
		std::string method;
		Wui::JsonValue params;
	};

	struct McpResponse
	{
		std::string jsonrpc = "2.0";
		std::string id;
		Wui::JsonValue result;
		bool isError = false;
		int errorCode = 0;
		std::string errorMessage;

		std::string ToJson() const
		{
			using namespace World::Wui;
			JsonValue root;
			root.type = JsonValue::Type::Object;
			root.Object.push_back({ "jsonrpc", JsonValue::MakeString(jsonrpc) });
			root.Object.push_back({ "id", JsonValue::MakeString(id) });

			if (isError)
			{
				JsonValue errObj;
				errObj.type = JsonValue::Type::Object;
				errObj.Object.push_back({ "code", JsonValue::MakeNumber(static_cast<double>(errorCode)) });
				errObj.Object.push_back({ "message", JsonValue::MakeString(errorMessage) });
				root.Object.push_back({ "error", std::move(errObj) });
			}
			else
			{
				root.Object.push_back({ "result", result });
			}

			return root.Dump();
		}
	};
}
