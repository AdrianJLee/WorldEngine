#include "wldpch.h"
#include "World/Script/LuauSyntax.h"

#include <Luau/Compiler.h>

#include <cctype>

namespace World
{
	bool CheckLuauSyntax(std::string_view source, const char* chunkName, LuauSyntaxError* error)
	{
		std::string bytecode;
		try
		{
			bytecode = Luau::compile(std::string(source), {});
		}
		catch (const std::exception& exception)
		{
			if (error)
			{
				error->Line = 0;
				error->Message = exception.what();
			}
			return false;
		}
		if (bytecode.empty())
		{
			if (error)
			{
				error->Line = 0;
				error->Message = "compiler returned no bytecode";
			}
			return false;
		}
		// version 0 = 错误装载体:第 1 字节起是 ":<line>: <message>"(与 ScriptArtifact::Pack 同一口径)。
		if (static_cast<unsigned char>(bytecode[0]) != 0)
			return true;
		std::string text = bytecode.substr(1);
		int line = 0;
		std::size_t i = 0;
		if (i < text.size() && text[i] == ':')
		{
			++i;
			int value = 0;
			bool digits = false;
			while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
			{
				value = value * 10 + (text[i] - '0');
				++i;
				digits = true;
			}
			if (digits && i < text.size() && text[i] == ':')
			{
				line = value;
				++i;
				if (i < text.size() && text[i] == ' ')
					++i;
			}
			else
				i = 0;
		}
		if (error)
		{
			error->Line = line;
			error->Message = text.substr(i);
			if (error->Message.empty())
				error->Message = text;
		}
		(void)chunkName;
		return false;
	}
}
