// W9-2:LuauHighlighter(逐行 token)+ LuauHighlightCache(按行缓存)headless 回归。
// 覆盖:关键字/已知全局名/数字(0x/0b/指数)/运算符、单双引号字符串与转义、
// 行注释、长括号字符串(含跨行与多等号)、跨行块注释(含多等号)、中文串字节边界、
// 缓存命中/失效与跨行状态传播、10k 行 token 化耗时。

#include "World/Script/Tooling/LuauCompletion.h"
#include "World/Script/Tooling/LuauHighlighter.h"
#include "World/WUI/WuiTextBuffer.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	using World::LuauHighlightCache;
	using World::LuauHighlighter;
	using World::LuauHighlightState;
	using World::LuauEngineTypeSet;
	using World::LuauFileSymbolSet;
	using World::LuauCompletionIndex;
	using World::Wui::WuiCodeToken;
	using World::Wui::WuiCodeTokenKind;
	using World::Wui::WuiTextBuffer;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	WuiCodeTokenKind KindAt(const std::vector<WuiCodeToken>& tokens, std::size_t byte)
	{
		for (const WuiCodeToken& token : tokens)
			if (byte >= token.StartByte && byte < token.EndByte)
				return token.Kind;
		return WuiCodeTokenKind::Default;
	}

	bool HasToken(const std::vector<WuiCodeToken>& tokens, std::size_t start, std::size_t end,
		WuiCodeTokenKind kind)
	{
		for (const WuiCodeToken& token : tokens)
			if (token.StartByte == start && token.EndByte == end && token.Kind == kind)
				return true;
		return false;
	}

	// token 必须按 StartByte 升序、互不重叠、不越界(WuiCodeEditor 的绘制前提)。
	void CheckWellFormed(const std::vector<WuiCodeToken>& tokens, std::size_t lineSize)
	{
		std::size_t previousEnd = 0;
		for (const WuiCodeToken& token : tokens)
		{
			CHECK(token.StartByte >= previousEnd);
			CHECK(token.EndByte <= lineSize);
			CHECK(token.EndByte > token.StartByte);
			previousEnd = token.EndByte;
		}
	}
}

int main()
{
	try
	{
		// ---- 1. 关键字 / 已知全局名 / 数字 / 运算符 ----
		{
			const std::string line = "local ui = events.x + 0x1F * 2.5e3 - 0b1010";
			LuauHighlightState state;
			std::vector<WuiCodeToken> tokens;
			LuauHighlighter::HighlightLine(line, state, tokens);
			CheckWellFormed(tokens, line.size());
			CHECK(state.BlockCommentLevel == -1 && state.LongStringLevel == -1);

			CHECK(KindAt(tokens, 0) == WuiCodeTokenKind::Keyword);   // local
			CHECK(KindAt(tokens, 6) == WuiCodeTokenKind::Global);    // ui
			CHECK(KindAt(tokens, 11) == WuiCodeTokenKind::Global);   // events
			CHECK(KindAt(tokens, 18) == WuiCodeTokenKind::Default);  // x(普通标识符)
			CHECK(KindAt(tokens, line.find("0x1F")) == WuiCodeTokenKind::Number);
			CHECK(KindAt(tokens, line.find("2.5e3")) == WuiCodeTokenKind::Number);
			CHECK(KindAt(tokens, line.find("0b1010")) == WuiCodeTokenKind::Number);
			CHECK(HasToken(tokens, line.find("+"), line.find("+") + 1, WuiCodeTokenKind::Operator));
		}

		// ---- 2. 行注释(到行尾)与字符串(单/双引号 + 转义)----
		{
			const std::string line = "local s = \"a\\\"b\" .. 'c' -- tail";
			LuauHighlightState state;
			std::vector<WuiCodeToken> tokens;
			LuauHighlighter::HighlightLine(line, state, tokens);
			CheckWellFormed(tokens, line.size());
			const std::size_t comment = line.find("--");
			CHECK(HasToken(tokens, line.find('"'), line.find(" .."), WuiCodeTokenKind::String));
			CHECK(KindAt(tokens, line.find('\'')) == WuiCodeTokenKind::String);
			CHECK(HasToken(tokens, comment, line.size(), WuiCodeTokenKind::Comment));
		}

		// ---- 3. 中文串边界:字符串 token 完整覆盖中文,不切碎多字节序列 ----
		{
			const std::string line = u8"local t = \"中文\" .. '好好'";
			LuauHighlightState state;
			std::vector<WuiCodeToken> tokens;
			LuauHighlighter::HighlightLine(line, state, tokens);
			CheckWellFormed(tokens, line.size());
			const std::size_t open = line.find('"');
			CHECK(HasToken(tokens, open, line.find(" .."), WuiCodeTokenKind::String)); // 到闭引号(含)
			CHECK(KindAt(tokens, open + 3) == WuiCodeTokenKind::String); // 中文首字节
			CHECK(KindAt(tokens, line.size() - 4) == WuiCodeTokenKind::String); // 单引号串里的中文
		}

		// ---- 4. 长括号字符串:同行 [[ ]] 与 [==[ ]==];跨行时状态延续 ----
		{
			const std::string line = "local a = [[x]] b = [==[y]==]";
			LuauHighlightState state;
			std::vector<WuiCodeToken> tokens;
			LuauHighlighter::HighlightLine(line, state, tokens);
			CheckWellFormed(tokens, line.size());
			CHECK(state.LongStringLevel == -1);
			CHECK(HasToken(tokens, line.find("[["), line.find("]]") + 2, WuiCodeTokenKind::String));
			CHECK(HasToken(tokens, line.find("[==["), line.find("]==]") + 4, WuiCodeTokenKind::String));
		}
		{
			LuauHighlightState state;
			std::vector<WuiCodeToken> tokens;
			const std::string first = "local s = [[abc";
			LuauHighlighter::HighlightLine(first, state, tokens);
			CHECK(state.LongStringLevel == 0);
			CHECK(HasToken(tokens, first.find("[["), first.size(), WuiCodeTokenKind::String));

			const std::string second = "def]] .. \"z\"";
			LuauHighlighter::HighlightLine(second, state, tokens);
			CheckWellFormed(tokens, second.size());
			CHECK(state.LongStringLevel == -1);
			CHECK(HasToken(tokens, 0, second.find("]]") + 2, WuiCodeTokenKind::String));
			CHECK(KindAt(tokens, second.find("\"z\"")) == WuiCodeTokenKind::String);
		}

		// ---- 5. 跨行块注释(含多等号形式)----
		{
			LuauHighlightState state;
			std::vector<WuiCodeToken> tokens;
			const std::string open = "local x = 1 --[[ note";
			LuauHighlighter::HighlightLine(open, state, tokens);
			CHECK(state.BlockCommentLevel == 0);
			CHECK(HasToken(tokens, open.find("--[["), open.size(), WuiCodeTokenKind::Comment));

			const std::string middle = "still comment -- [[ not a string";
			LuauHighlighter::HighlightLine(middle, state, tokens);
			CHECK(state.BlockCommentLevel == 0);
			CHECK(tokens.size() == 1 && tokens[0].Kind == WuiCodeTokenKind::Comment
				&& tokens[0].StartByte == 0 && tokens[0].EndByte == middle.size());

			const std::string close = "end ]] local y = 2";
			LuauHighlighter::HighlightLine(close, state, tokens);
			CheckWellFormed(tokens, close.size());
			CHECK(state.BlockCommentLevel == -1);
			CHECK(HasToken(tokens, 0, close.find("]]") + 2, WuiCodeTokenKind::Comment));
			CHECK(KindAt(tokens, close.find("local")) == WuiCodeTokenKind::Keyword);
			CHECK(KindAt(tokens, close.find('2')) == WuiCodeTokenKind::Number);
		}
		{
			LuauHighlightState state;
			std::vector<WuiCodeToken> tokens;
			const std::string open = "--[==[ long comment";
			LuauHighlighter::HighlightLine(open, state, tokens);
			CHECK(state.BlockCommentLevel == 2);
			const std::string close = "done ]==] local z = 3";
			LuauHighlighter::HighlightLine(close, state, tokens);
			CHECK(state.BlockCommentLevel == -1);
			CHECK(HasToken(tokens, 0, close.find("]==]") + 4, WuiCodeTokenKind::Comment));
			// 少一个 '=' 的 `]]` 不能关闭 [==[ 块注释。
			const std::string almost = "done ]] still comment";
			LuauHighlightState reopened { 2, -1 };
			LuauHighlighter::HighlightLine(almost, reopened, tokens);
			CHECK(reopened.BlockCommentLevel == 2);
			CHECK(tokens.size() == 1 && tokens[0].Kind == WuiCodeTokenKind::Comment);
		}

		// ---- 6. 缓存:按行复用 + 编辑后的跨行状态传播 ----
		{
			WuiTextBuffer buffer;
			buffer.SetText("local a = 1\n--[[ c\nstill\n]] local b = 2\n");
			LuauHighlightCache cache;
			cache.Update(buffer);
			CHECK(cache.LineCount() == 5);
			CHECK(cache.Tokens(2).size() == 1 && cache.Tokens(2)[0].Kind == WuiCodeTokenKind::Comment);
			CHECK(HasToken(cache.Tokens(3), 0, 2, WuiCodeTokenKind::Comment));
			CHECK(KindAt(cache.Tokens(3), 13) == WuiCodeTokenKind::Number);

			// Find:用 buffer 里的行视图必须命中,陌生串必须落空。
			{
				const std::string& text = buffer.Text();
				const std::pair<std::size_t, std::size_t> range = buffer.LineRange(1);
				const std::string_view view(text.data() + range.first, range.second - range.first);
				const std::vector<WuiCodeToken>* found = cache.Find(view);
				CHECK(found != nullptr);
				CHECK(found->size() == 1 && (*found)[0].Kind == WuiCodeTokenKind::Comment);
				const std::string_view unknown = "not in buffer at all";
				CHECK(cache.Find(unknown) == nullptr);
			}

			// 改动最后一行:块注释之后的 token 重算,前面几行仍复用。
			buffer.SetText("local a = 1\n--[[ c\nstill\n]] local b = 3\n");
			cache.Update(buffer);
			CHECK(KindAt(cache.Tokens(3), 13) == WuiCodeTokenKind::Number);
			CHECK(HasToken(cache.Tokens(3), 0, 2, WuiCodeTokenKind::Comment));
			CHECK(cache.Tokens(2).size() == 1 && cache.Tokens(2)[0].Kind == WuiCodeTokenKind::Comment);

			// 把第 1 行改成"开启块注释":第 2 行起必须变成注释延续(状态传播)。
			buffer.SetText("local a = 1 --[[\nstill\nstill2 ]]\nlocal c = 4\n");
			cache.Update(buffer);
			CHECK(cache.Tokens(1).size() == 1 && cache.Tokens(1)[0].Kind == WuiCodeTokenKind::Comment);
			CHECK(cache.Tokens(2).size() == 1 && cache.Tokens(2)[0].Kind == WuiCodeTokenKind::Comment);
			CHECK(KindAt(cache.Tokens(3), 0) == WuiCodeTokenKind::Keyword);

			cache.Clear();
			CHECK(cache.LineCount() == -1 && cache.Tokens(0).empty());
		}

		// ---- 7. 10k 行 token 化耗时(缓存全量重建)----
		{
			std::string script;
			for (int i = 0; i < 10000; ++i)
				script += "local value_" + std::to_string(i) + " = " + std::to_string(i)
					+ " --[[ note " + std::to_string(i) + " ]]\n";
			WuiTextBuffer buffer;
			buffer.SetText(script);
			LuauHighlightCache cache;
			const auto start = std::chrono::steady_clock::now();
			cache.Update(buffer);
			const auto finish = std::chrono::steady_clock::now();
			const double milliseconds =
				std::chrono::duration<double, std::milli>(finish - start).count();
			std::size_t tokenCount = 0;
			for (int line = 0; line < cache.LineCount(); ++line)
				tokenCount += cache.Tokens(line).size();
			std::printf("World.LuauHighlighter: 10k lines (%zu bytes) tokenized in %.3f ms (%zu tokens)\n",
				buffer.Text().size(), milliseconds, tokenCount);
			CHECK(cache.LineCount() == 10001);
			CHECK(tokenCount >= 10000 * 3);
		}

		// ---- 8. VEC-A7:引擎外部类名色标(可选集合;不传 = 与加这个参数之前逐字节一致)----
		{
			LuauEngineTypeSet engineTypes;
			engineTypes.Set({ "vec2", "vec3", "vec4", "mat3", "mat4", "Entity", "WorldScript" });
			CHECK(!engineTypes.Empty() && engineTypes.Contains("vec3") && !engineTypes.Contains("ui"));

			const std::string code = "local v = vec3.new(1.0, 0.0, 0.0)";
			LuauHighlightState state;
			std::vector<WuiCodeToken> tokens;
			LuauHighlighter::HighlightLine(code, state, tokens, &engineTypes);
			CHECK(KindAt(tokens, code.find("vec3")) == WuiCodeTokenKind::EngineType);
			CHECK(KindAt(tokens, code.find("new")) == WuiCodeTokenKind::Function);

			// 成员名不染:`transform.Location` 的 `Location` 保持 Default(与修复前一致)。
			const std::string member = "transform.Location = vec3.new(0.0)";
			tokens.clear();
			LuauHighlighter::HighlightLine(member, state, tokens, &engineTypes);
			CHECK(KindAt(tokens, member.find("Location")) == WuiCodeTokenKind::Default);
			CHECK(KindAt(tokens, member.find("transform")) == WuiCodeTokenKind::Default);
			CHECK(KindAt(tokens, member.find("vec3")) == WuiCodeTokenKind::EngineType);

			// Entity 今天在 kGlobals 名单里:集合命中时 EngineType 覆盖 Global;ui 不在集合里 → 保持 Global。
			const std::string globals = "Entity.new() ui.panel()";
			tokens.clear();
			LuauHighlighter::HighlightLine(globals, state, tokens, &engineTypes);
			CHECK(KindAt(tokens, globals.find("Entity")) == WuiCodeTokenKind::EngineType);
			CHECK(KindAt(tokens, globals.find("ui")) == WuiCodeTokenKind::Global);

			// 字符串/注释里的类名不判定。
			const std::string quoted = "local s = \"vec3\" -- vec3 comment";
			tokens.clear();
			LuauHighlighter::HighlightLine(quoted, state, tokens, &engineTypes);
			CHECK(KindAt(tokens, quoted.find("\"vec3\"") + 1) == WuiCodeTokenKind::String);
			CHECK(KindAt(tokens, quoted.find("comment")) == WuiCodeTokenKind::Comment);

			// 不传集合 = 修复前行为(vec3 是 Default),老调用方零影响。
			tokens.clear();
			LuauHighlighter::HighlightLine(code, state, tokens);
			CHECK(KindAt(tokens, code.find("vec3")) == WuiCodeTokenKind::Default);
		}

		// ---- 9. VEC-H1:文件内符号优先(数据字段/局部不再吃服务表的全局色)----
		{
			LuauFileSymbolSet fileSymbols;
			fileSymbols.Set({ "a" }, { "Level" });
			CHECK(!fileSymbols.Empty());
			CHECK(fileSymbols.ContainsLocal("a") && !fileSymbols.ContainsField("a"));
			CHECK(fileSymbols.ContainsField("Level") && !fileSymbols.ContainsLocal("Level"));
			CHECK(fileSymbols.Hash() != 0);

			LuauHighlightState state;
			std::vector<WuiCodeToken> tokens;

			// 数据字段:表构造键(含跨行表构造的形态 `InferredStats = {\n Level = 3,\n}`)→ Field。
			const std::string key = "        Level = 3,";
			LuauHighlighter::HighlightLine(key, state, tokens, nullptr, &fileSymbols);
			CHECK(KindAt(tokens, key.find("Level")) == WuiCodeTokenKind::Field);
			CHECK(KindAt(tokens, key.find('3')) == WuiCodeTokenKind::Number);

			// 同行表构造键同样走 Field。
			const std::string inlineKey = "local t = { Level = 1 }";
			tokens.clear();
			LuauHighlighter::HighlightLine(inlineKey, state, tokens, nullptr, &fileSymbols);
			CHECK(KindAt(tokens, inlineKey.find("Level")) == WuiCodeTokenKind::Field);

			// 成员位置(self.ExtraInfo.Level)→ Field;接收者链里的名字保持 Default。
			const std::string member = "local x = self.ExtraInfo.Level";
			tokens.clear();
			LuauHighlighter::HighlightLine(member, state, tokens, nullptr, &fileSymbols);
			CHECK(KindAt(tokens, member.find("Level")) == WuiCodeTokenKind::Field);
			CHECK(KindAt(tokens, member.find("ExtraInfo")) == WuiCodeTokenKind::Default);

			// 服务表的**裸用法**不在本文件的字段/局部名单里 → 仍 Global 浅蓝(VEC-H1 核心口径)。
			const std::string service = "local s = Level.Primary()";
			tokens.clear();
			LuauHighlighter::HighlightLine(service, state, tokens, nullptr, &fileSymbols);
			CHECK(KindAt(tokens, service.find("Level")) == WuiCodeTokenKind::Global);
			// 成员名不吃全局色:它是调用点,按既有规则(规则 3)染 Function,与修复前一致。
			CHECK(KindAt(tokens, service.find("Primary")) == WuiCodeTokenKind::Function);

			// 本文件声明的局部名 → Default(`a = 1` 这类名字不再染全局色)。
			const std::string localDecl = "local a = 1";
			tokens.clear();
			LuauHighlighter::HighlightLine(localDecl, state, tokens, nullptr, &fileSymbols);
			CHECK(KindAt(tokens, localDecl.find("a = ")) == WuiCodeTokenKind::Default);
			const std::string localUse = "a = a + 1";
			tokens.clear();
			LuauHighlighter::HighlightLine(localUse, state, tokens, nullptr, &fileSymbols);
			CHECK(KindAt(tokens, 0) == WuiCodeTokenKind::Default);

			// VEC-A7 不回归:引擎类型档与文件内符号同时传 → vec3 仍 EngineType。
			LuauEngineTypeSet engineTypes;
			engineTypes.Set({ "vec3" });
			const std::string engine = "local v = vec3.new(Level.Primary())";
			tokens.clear();
			LuauHighlighter::HighlightLine(engine, state, tokens, &engineTypes, &fileSymbols);
			CHECK(KindAt(tokens, engine.find("vec3")) == WuiCodeTokenKind::EngineType);
			CHECK(KindAt(tokens, engine.find("Level")) == WuiCodeTokenKind::Global);

			// 不传文件符号 = 修复前行为:同一个 `Level = 3,` 仍是 Global,老调用方零影响。
			tokens.clear();
			LuauHighlighter::HighlightLine(key, state, tokens);
			CHECK(KindAt(tokens, key.find("Level")) == WuiCodeTokenKind::Global);
			tokens.clear();
			LuauHighlighter::HighlightLine(member, state, tokens);
			CHECK(KindAt(tokens, member.find("Level")) == WuiCodeTokenKind::Global);
		}

		// ---- 10. VEC-H1 集成:完成索引的文件符号 → 高亮集合(面板的接线口径)----
		{
			LuauCompletionIndex index;
			index.SetFileSource(
				"---@class PlayerScript : WorldScript\n"
				"---@field Speed number 移动速度\n"
				"local PlayerScript = {\n"
				"    Speed = 5.0,\n"
				"    InferredStats = {\n"
				"        Level = 3,\n"
				"    },\n"
				"}\n");
			std::vector<std::string> locals;
			std::vector<std::string> fields;
			index.CollectFileSymbols(locals, fields);
			LuauFileSymbolSet fileSymbols;
			fileSymbols.Set(std::move(locals), std::move(fields));
			CHECK(fileSymbols.ContainsLocal("PlayerScript"));   // `local X = {}` 是文件符号
			CHECK(fileSymbols.ContainsField("Speed"));          // `---@field` 注解字段
			CHECK(fileSymbols.ContainsField("Level"));          // 嵌套表构造键(推断)
			CHECK(!fileSymbols.ContainsLocal("Level"));         // Level 只是字段,不是本文件的局部

			LuauHighlightState state;
			std::vector<WuiCodeToken> tokens;
			const std::string field = "        Level = 3,";
			LuauHighlighter::HighlightLine(field, state, tokens, nullptr, &fileSymbols);
			CHECK(KindAt(tokens, field.find("Level")) == WuiCodeTokenKind::Field);
			const std::string service = "local s = Level.Primary()";
			tokens.clear();
			LuauHighlighter::HighlightLine(service, state, tokens, nullptr, &fileSymbols);
			CHECK(KindAt(tokens, service.find("Level")) == WuiCodeTokenKind::Global);

			// 空文件(没 SetFileSource)→ 集合为空 → 旧行为。
			LuauCompletionIndex empty;
			empty.SetFileSource("");
			std::vector<std::string> emptyLocals;
			std::vector<std::string> emptyFields;
			empty.CollectFileSymbols(emptyLocals, emptyFields);
			CHECK(emptyLocals.empty() && emptyFields.empty());
		}

		// ---- 11. VEC-H1:高亮缓存按"文件符号指纹"失效(面板接线契约)----
		{
			WuiTextBuffer buffer;
			buffer.SetText("Level = 3\n");
			LuauHighlightCache cache;
			cache.Update(buffer);
			CHECK(KindAt(cache.Tokens(0), 0) == WuiCodeTokenKind::Global);   // 空集合 = 旧行为
			cache.SetFileSymbols({}, { "Level" });
			CHECK(cache.FileSymbols().ContainsField("Level"));
			cache.Update(buffer);
			CHECK(KindAt(cache.Tokens(0), 0) == WuiCodeTokenKind::Field);    // 集合变化 → 缓存重建
			cache.SetFileSymbols({}, {});
			cache.Update(buffer);
			CHECK(KindAt(cache.Tokens(0), 0) == WuiCodeTokenKind::Global);   // 清空同样失效
			CHECK(cache.FileSymbols().Empty());
		}

		std::printf("World.LuauHighlighter: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.LuauHighlighter: FAILED: %s\n", error.what());
		return 1;
	}
}
