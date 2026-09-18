// W9.5-1:LuauCompletionIndex(存根符号表 / 光标上下文 / 过滤排序)headless 回归。
// 覆盖:真实入库 WorldEngineAPI.luau 的符号统计与抽样成员、ParseContext 五种形态、
// ui./entity:Get/self./文件符号/未知接收者的查询、前缀+子串+大小写+排序+截断、
// 合成存根的 base 继承与 doc/type、畸形输入容错、1000 次 Query 的耗时。

#include "World/Script/LuauCompletion.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using World::LuauCompletionIndex;
	using World::LuauCompletionItem;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	std::string StubPath()
	{
		const std::filesystem::path assets(WLD_ASSETPATH);
		return (assets / "scripts" / "intermediate" / "WorldEngineAPI.luau").string();
	}

	bool HasName(const std::vector<LuauCompletionItem>& items, std::string_view name)
	{
		for (const LuauCompletionItem& item : items)
			if (item.Name == name)
				return true;
		return false;
	}

	const LuauCompletionItem* FindName(const std::vector<LuauCompletionItem>& items,
		std::string_view name)
	{
		for (const LuauCompletionItem& item : items)
			if (item.Name == name)
				return &item;
		return nullptr;
	}

	std::size_t CountKind(const std::vector<LuauCompletionItem>& items,
		LuauCompletionItem::KindType kind)
	{
		std::size_t count = 0;
		for (const LuauCompletionItem& item : items)
			if (item.Kind == kind)
				++count;
		return count;
	}

	bool AllOfKind(const std::vector<LuauCompletionItem>& items, LuauCompletionItem::KindType kind)
	{
		for (const LuauCompletionItem& item : items)
			if (item.Kind != kind)
				return false;
		return true;
	}

	std::string JoinNames(const std::vector<LuauCompletionItem>& items, std::size_t maxItems)
	{
		std::string result;
		for (std::size_t i = 0; i < items.size() && i < maxItems; ++i)
		{
			if (!result.empty())
				result += ", ";
			result += items[i].Name;
		}
		return result;
	}

	char LowerAscii(char c)
	{
		return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	}

	int CompareIgnoreCase(std::string_view left, std::string_view right)
	{
		const std::size_t shared = left.size() < right.size() ? left.size() : right.size();
		for (std::size_t i = 0; i < shared; ++i)
		{
			const char a = LowerAscii(left[i]);
			const char b = LowerAscii(right[i]);
			if (a != b)
				return (a < b) ? -1 : 1;
		}
		if (left.size() == right.size())
			return 0;
		return (left.size() < right.size()) ? -1 : 1;
	}

	int KindRank(LuauCompletionItem::KindType kind)
	{
		switch (kind)
		{
			case LuauCompletionItem::KindType::Field: return 0;
			case LuauCompletionItem::KindType::Method: return 1;
			case LuauCompletionItem::KindType::Global: return 2;
			case LuauCompletionItem::KindType::Class: return 3;
			case LuauCompletionItem::KindType::Keyword: return 4;
		}
		return 5;
	}

	// 排序规则:Field→Method→Global→Class→Keyword,同档按名字大小写不敏感升序。
	bool SortedByRules(const std::vector<LuauCompletionItem>& items)
	{
		for (std::size_t i = 1; i < items.size(); ++i)
		{
			const LuauCompletionItem& previous = items[i - 1];
			const LuauCompletionItem& current = items[i];
			const int previousRank = KindRank(previous.Kind);
			const int currentRank = KindRank(current.Kind);
			if (currentRank < previousRank)
				return false;
			if (currentRank == previousRank
				&& CompareIgnoreCase(previous.Name, current.Name) > 0)
				return false;
		}
		return true;
	}

	struct KindTotals
	{
		std::size_t Fields = 0;
		std::size_t Methods = 0;
		std::size_t Globals = 0;
		std::size_t Classes = 0;
		std::size_t Keywords = 0;
		std::size_t Receivers = 0;
	};

	// 顶层项来自无接收者查询;成员按"每个全局/类名查一次该名字的成员"汇总。
	KindTotals Tally(const LuauCompletionIndex& index)
	{
		KindTotals totals;
		std::vector<LuauCompletionItem> top;
		index.Query("", 0, top);
		for (const LuauCompletionItem& item : top)
		{
			switch (item.Kind)
			{
				case LuauCompletionItem::KindType::Field: ++totals.Fields; break;
				case LuauCompletionItem::KindType::Method: ++totals.Methods; break;
				case LuauCompletionItem::KindType::Global: ++totals.Globals; break;
				case LuauCompletionItem::KindType::Class: ++totals.Classes; break;
				case LuauCompletionItem::KindType::Keyword: ++totals.Keywords; break;
			}
		}
		for (const LuauCompletionItem& item : top)
		{
			if (item.Kind != LuauCompletionItem::KindType::Global
				&& item.Kind != LuauCompletionItem::KindType::Class)
				continue;
			std::vector<LuauCompletionItem> members;
			index.Query(item.Name + ".", 0, members);
			const std::size_t fields = CountKind(members, LuauCompletionItem::KindType::Field);
			const std::size_t methods = CountKind(members, LuauCompletionItem::KindType::Method);
			if (fields + methods == 0)
				continue;
			++totals.Receivers;
			totals.Fields += fields;
			totals.Methods += methods;
		}
		return totals;
	}
}

int main()
{
	try
	{
		LuauCompletionIndex index;

		// ---- 0. 空索引 ----
		{
			std::vector<LuauCompletionItem> items;
			index.Query("", 0, items);
			CHECK(index.SymbolCount() == 0);
			CHECK(items.empty());
		}

		// ---- 1. 真实存根:加载 + 符号统计 + 抽样成员 ----
		{
			std::string error = "stale";
			const std::string path = StubPath();
			CHECK(index.LoadStubFile(path, &error));
			CHECK(error.empty());

			const KindTotals totals = Tally(index);
			std::printf("[stub] SymbolCount=%zu | items: globals=%zu classes=%zu keywords=%zu"
				" | members: fields=%zu methods=%zu | receivers=%zu\n",
				index.SymbolCount(), totals.Globals, totals.Classes, totals.Keywords,
				totals.Fields, totals.Methods, totals.Receivers);
			CHECK(index.SymbolCount() >= 200);
			CHECK(totals.Fields >= 60);
			CHECK(totals.Methods >= 70);
			CHECK(totals.Globals >= 30);
			CHECK(totals.Classes >= 10);
			CHECK(totals.Keywords >= 25);

			std::vector<LuauCompletionItem> ui;
			index.Query("ui.", 100, ui);
			CHECK(HasName(ui, "panel") && HasName(ui, "button") && HasName(ui, "text"));
			CHECK(AllOfKind(ui, LuauCompletionItem::KindType::Method));
			std::printf("[stub] ui members: %s\n", JoinNames(ui, 12).c_str());

			const LuauCompletionItem* panel = FindName(ui, "panel");
			CHECK(panel != nullptr);
			CHECK(panel->Doc == "Draw a panel background with a title.");
			CHECK(panel->Type.empty());

			std::vector<LuauCompletionItem> entity;
			index.Query("Entity.", 100, entity);
			CHECK(HasName(entity, "AddComponent") && HasName(entity, "CreateChild"));
			CHECK(AllOfKind(entity, LuauCompletionItem::KindType::Method));
			const LuauCompletionItem* addComponent = FindName(entity, "AddComponent");
			CHECK(addComponent != nullptr);
			CHECK(addComponent->Doc.find("Add a default component") == 0);
			const LuauCompletionItem* getName = FindName(entity, "GetName");
			CHECK(getName != nullptr && getName->Type == "string");
			std::printf("[stub] Entity methods=%zu sample: %s\n",
				entity.size(), JoinNames(entity, 8).c_str());

			std::vector<LuauCompletionItem> worldScript;
			index.Query("WorldScript.", 100, worldScript);
			CHECK(HasName(worldScript, "OnCreate") && HasName(worldScript, "OnUpdate"));
			CHECK(HasName(worldScript, "OnUI") && HasName(worldScript, "OnDestroy"));
			const LuauCompletionItem* onCreate = FindName(worldScript, "OnCreate");
			CHECK(onCreate != nullptr);
			CHECK(onCreate->Kind == LuauCompletionItem::KindType::Field);
			CHECK(onCreate->Type == "fun(self: WorldScript)");
			CHECK(onCreate->Doc == "Called once when the instance starts.");
			std::printf("[stub] WorldScript members: %s\n",
				JoinNames(worldScript, 12).c_str());

			std::vector<LuauCompletionItem> vec2;
			index.Query("vec2.", 100, vec2);
			CHECK(HasName(vec2, "x") && HasName(vec2, "y"));
			const LuauCompletionItem* xField = FindName(vec2, "x");
			CHECK(xField != nullptr);
			CHECK(xField->Kind == LuauCompletionItem::KindType::Field);
			CHECK(xField->Type == "number" && xField->Doc == "Vector coordinate");
			std::printf("[stub] vec2 members: %s\n", JoinNames(vec2, 12).c_str());

			// 只有 ---@class 注解、没有 X = {} 的名字进索引(WorldScript/组件类)。
			std::vector<LuauCompletionItem> top;
			index.Query("WorldScript", 50, top);
			const LuauCompletionItem* worldScriptItem = FindName(top, "WorldScript");
			CHECK(worldScriptItem != nullptr);
			CHECK(worldScriptItem->Kind == LuauCompletionItem::KindType::Class);
		}

		// ---- 2. ParseContext 五种形态 ----
		{
			std::string receiver;
			std::string prefix;
			char separator = '?';

			LuauCompletionIndex::ParseContext("ui.", receiver, separator, prefix);
			CHECK(receiver == "ui" && separator == '.' && prefix.empty());

			LuauCompletionIndex::ParseContext("entity:Get", receiver, separator, prefix);
			CHECK(receiver == "entity" && separator == ':' && prefix == "Get");

			LuauCompletionIndex::ParseContext("local x = en", receiver, separator, prefix);
			CHECK(receiver.empty() && separator == '\0' && prefix == "en");

			LuauCompletionIndex::ParseContext("self.", receiver, separator, prefix);
			CHECK(receiver == "self" && separator == '.' && prefix.empty());

			LuauCompletionIndex::ParseContext("for i = 1, ", receiver, separator, prefix);
			CHECK(receiver.empty() && separator == '\0' && prefix.empty());

			// 缩进/空格/前缀片段
			LuauCompletionIndex::ParseContext("    local ui2 = ui . bu", receiver, separator, prefix);
			CHECK(receiver == "ui" && separator == '.' && prefix == "bu");
		}

		// ---- 2b. `---@` 注解标签:只在注解上下文给候选 ----
		{
			std::vector<LuauCompletionItem> items;
			index.Query("---@fie", 20, items);
			CHECK(HasName(items, "field"));
			index.Query("---@", 20, items);
			CHECK(HasName(items, "class") && HasName(items, "field") && HasName(items, "param")
				&& HasName(items, "return") && HasName(items, "type"));
			// 普通代码上下文不给注解标签(避免噪声)
			index.Query("fie", 100, items);
			CHECK(!HasName(items, "field"));
		}

		// ---- 3. 查询:接收者成员 / 过滤 / 大小写 / 排序 / 截断 ----
		{
			std::vector<LuauCompletionItem> items;

			// 接收者为已知全局表:只回该类成员
			index.Query("ui.", 100, items);
			CHECK(!items.empty());
			CHECK(!HasName(items, "AddComponent"));
			CHECK(CountKind(items, LuauCompletionItem::KindType::Keyword) == 0);
			CHECK(SortedByRules(items));

			// 大小写不敏感的接收者与前缀(entity → Entity;PAn → panel)
			index.Query("entity:Get", 100, items);
			CHECK(!items.empty());
			CHECK(AllOfKind(items, LuauCompletionItem::KindType::Method));
			for (const LuauCompletionItem& item : items)
				CHECK(item.Name.compare(0, 3, "Get") == 0);
			CHECK(HasName(items, "GetComponent"));
			std::printf("[query] entity:Get -> %s\n", JoinNames(items, 8).c_str());

			index.Query("uI.pAn", 100, items);
			CHECK(items.size() == 1 && items.front().Name == "panel");
			CHECK(items.front().Type.empty());

			// 无接收者:关键字与全局
			index.Query("fun", 100, items);
			CHECK(HasName(items, "function"));
			CHECK(FindName(items, "function")->Kind == LuauCompletionItem::KindType::Keyword);

			index.Query("local x = en", 100, items);
			CHECK(HasName(items, "Entity") && HasName(items, "end"));

			index.Query("for i = 1, ", 100, items);
			CHECK(!items.empty());
			CHECK(items.front().Name == "assert");   // Global 档里字典序最前
			CHECK(SortedByRules(items));

			// 前缀零命中时子串兜底
			index.Query("ui.anel", 100, items);
			CHECK(items.size() == 1 && items.front().Name == "panel");

			index.Query("ntity", 100, items);
			CHECK(HasName(items, "Entity"));

			// 截断
			index.Query("ui.", 5, items);
			CHECK(items.size() == 5);
			index.Query("ui.", 200, items);
			CHECK(items.size() == 10);   // ui 一共 10 个成员

			// 排序:vec2 的字段(x/y)在方法(length/new)之前
			index.Query("vec2.", 100, items);
			CHECK(items.size() == 4);
			CHECK(items[0].Name == "x" && items[1].Name == "y");
			CHECK(items[2].Name == "length" && items[3].Name == "new");
			CHECK(SortedByRules(items));

			// 未知接收者:回退全局 + 文件符号,且不给关键字
			index.Query("nope.", 300, items);
			CHECK(HasName(items, "Entity") && HasName(items, "ui"));
			CHECK(CountKind(items, LuauCompletionItem::KindType::Keyword) == 0);
		}

		// ---- 4. 合成存根:---@class X : Base 继承 + doc/type 解析 ----
		{
			LuauCompletionIndex synthetic;
			const std::string stub =
				"---@meta\n"
				"---@class Base\n"
				"---@field A number base field\n"
				"Base = {}\n"
				"---Base method\n"
				"---@return number\n"
				"function Base:Hello() end\n"
				"---@class Derived : Base\n"
				"---@field B number derived field\n"
				"Derived = {}\n"
				"---@param value string ignored\n"
				"---@overload fun(value: number): string\n"
				"---@return string\n"
				"function Derived:World() end\n";
			std::string error = "stale";
			CHECK(synthetic.LoadStub(stub, &error));
			CHECK(error.empty());

			std::vector<LuauCompletionItem> derived;
			synthetic.Query("Derived.", 100, derived);
			CHECK(HasName(derived, "A") && HasName(derived, "B"));
			CHECK(HasName(derived, "Hello") && HasName(derived, "World"));

			const LuauCompletionItem* fieldA = FindName(derived, "A");
			CHECK(fieldA != nullptr);
			CHECK(fieldA->Kind == LuauCompletionItem::KindType::Field);
			CHECK(fieldA->Type == "number" && fieldA->Doc == "base field");
			const LuauCompletionItem* hello = FindName(derived, "Hello");
			CHECK(hello != nullptr);
			CHECK(hello->Kind == LuauCompletionItem::KindType::Method);
			CHECK(hello->Type == "number" && hello->Doc == "Base method");
			const LuauCompletionItem* world = FindName(derived, "World");
			CHECK(world != nullptr && world->Type == "string");

			// 未知 tag 被忽略,不打断注释块/不产生符号
			std::vector<LuauCompletionItem> all;
			synthetic.Query("", 0, all);
			CHECK(!HasName(all, "overload"));
		}

		// ---- 5. 文件内符号 + self. ----
		{
			const std::string source =
				"-- 复制到 scripts 下,并修改类名。\n"
				"---@class ExampleScript : WorldScript\n"
				"---@field Speed number 移动速度\n"
				"local ExampleScript = { Speed = 5.0 }\n"
				"\n"
				"function ExampleScript:OnCreate()\n"
				"    print(\"Created entity\", self.entity:GetID())\n"
				"end\n"
				"\n"
				"---@param dt number 距离上一帧的秒数\n"
				"function ExampleScript:OnUpdate(dt)\n"
				"    local foo = 1\n"
				"end\n"
				"\n"
				"function ExampleScript:OnDestroy() end\n"
				"\n"
				"function bar() end\n"
				"Thing = {}\n"
				"return ExampleScript\n";
			index.SetFileSource(source);

			std::vector<LuauCompletionItem> items;
			index.Query("", 500, items);
			CHECK(HasName(items, "foo"));     // local foo
			CHECK(HasName(items, "bar"));     // function bar
			CHECK(HasName(items, "Thing"));   // Thing = {}
			const LuauCompletionItem* example = FindName(items, "ExampleScript");
			CHECK(example != nullptr);
			CHECK(example->Kind == LuauCompletionItem::KindType::Global);   // local 覆写类项

			// self. = 文件类(含 ---@field/文件方法)+ WorldScript 成员并集
			index.Query("self.", 100, items);
			CHECK(HasName(items, "Speed"));        // 文件 ---@field
			CHECK(HasName(items, "OnUpdate"));     // 文件方法
			CHECK(HasName(items, "OnDestroy"));    // WorldScript 基类成员
			const LuauCompletionItem* onDestroy = FindName(items, "OnDestroy");
			CHECK(onDestroy != nullptr);
			// 文件里裸写的 OnDestroy 没有类型/文档:合并后必须保留 WorldScript 注解里的信息
			// (用户实测:列表里 OnDestroy 曾经没有类型也没有注释——同名成员"先到先得"把它盖掉了)。
			CHECK(onDestroy->Type == "fun(self: WorldScript)");
			CHECK(onDestroy->Doc.find("Cleanup callback") != std::string::npos);
			CHECK(HasName(items, "entity"));       // WorldScript 字段
			const LuauCompletionItem* speed = FindName(items, "Speed");
			CHECK(speed != nullptr);
			CHECK(speed->Kind == LuauCompletionItem::KindType::Field);
			CHECK(speed->Type == "number" && speed->Doc == "移动速度");

			// 文件类名当接收者
			index.Query("ExampleScript.", 100, items);
			CHECK(HasName(items, "Speed") && HasName(items, "OnCreate"));
			CHECK(HasName(items, "OnDestroy"));   // 基类成员也可见

			// 未知接收者回退里带上文件符号
			index.Query("nope.", 500, items);
			CHECK(HasName(items, "foo") && HasName(items, "Entity"));

			index.SetFileSource("");
			index.Query("self.", 100, items);
			CHECK(HasName(items, "OnUpdate") && !HasName(items, "Speed"));
		}

		// ---- 6. 畸形输入:不崩、能跳过/报错 ----
		{
			LuauCompletionIndex malformed;
			std::string error = "stale";
			std::string longLine = "---";
			longLine.append(200000, 'x');
			const std::string text =
				"function Foo:Bar(\n"
				"---@unknown whatever\n"
				"---@class Broken : \n"
				"---@field \n"
				+ longLine + "\n"
				"Broken = {}\n"
				"---@field ok number fine\n";   // 字段先于任何类之前:跳过
			CHECK(malformed.LoadStub(text, &error));
			CHECK(error.empty());
			std::vector<LuauCompletionItem> items;
			malformed.Query("Foo.", 50, items);
			CHECK(HasName(items, "Bar"));   // 缺 end 也按行扫到方法
			malformed.Query("", 100, items);
			CHECK(HasName(items, "Broken"));

			CHECK(!malformed.LoadStub("   \n\t\n", &error));
			CHECK(!error.empty());

			error = "stale";
			CHECK(!malformed.LoadStubFile("does-not-exist/WorldEngineAPI.luau", &error));
			CHECK(!error.empty());

			malformed.SetFileSource("---@class Weird : \nfunction Weird:Only()\nlocal 9bad\n");
			malformed.Query("Weird.", 50, items);
			CHECK(HasName(items, "Only"));
		}

		// ---- 7. 性能:1000 次 Query(含空前缀的最坏情形) ----
		{
			index.SetFileSource("");
			CHECK(index.LoadStubFile(StubPath(), nullptr));

			constexpr int kIterations = 1000;
			std::vector<LuauCompletionItem> items;
			std::size_t seen = 0;
			const auto start = std::chrono::steady_clock::now();
			for (int i = 0; i < kIterations; ++i)
			{
				index.Query((i % 2 == 0) ? "ui.b" : "", 12, items);
				seen += items.size();
			}
			const auto elapsed = std::chrono::steady_clock::now() - start;
			const double totalUs = std::chrono::duration<double, std::micro>(elapsed).count();
			std::printf("[perf] %d queries in %.3f ms -> %.2f us/query (returned %zu items)\n",
				kIterations, totalUs / 1000.0, totalUs / kIterations, seen);
			CHECK(seen > 0);
		}
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "World.LuauCompletion FAILED: %s\n", exception.what());
		return 1;
	}

	std::printf("World.LuauCompletion: ALL CHECKS PASSED\n");
	return 0;
}
