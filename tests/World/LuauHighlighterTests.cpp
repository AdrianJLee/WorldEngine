// W9-2:LuauHighlighter(逐行 token)+ LuauHighlightCache(按行缓存)headless 回归。
// 覆盖:关键字/已知全局名/数字(0x/0b/指数)/运算符、单双引号字符串与转义、
// 行注释、长括号字符串(含跨行与多等号)、跨行块注释(含多等号)、中文串字节边界、
// 缓存命中/失效与跨行状态传播、10k 行 token 化耗时。

#include "World/Script/LuauHighlighter.h"
#include "World/WUI/WuiTextBuffer.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using World::LuauHighlightCache;
	using World::LuauHighlighter;
	using World::LuauHighlightState;
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

		std::printf("World.LuauHighlighter: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.LuauHighlighter: FAILED: %s\n", error.what());
		return 1;
	}
}
