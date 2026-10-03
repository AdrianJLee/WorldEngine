// W9.5-1:LuauCompletionIndex(存根符号表 / 光标上下文 / 过滤排序)headless 回归。
// 覆盖:真实入库 WorldEngineAPI.luau 的符号统计与抽样成员、ParseContext 五种形态、
// ui./entity:Get/self./文件符号/未知接收者的查询、前缀+子串+大小写+排序+截断、
// 合成存根的 base 继承与 doc/type、畸形输入容错、1000 次 Query 的耗时。

#include "World/Script/Tooling/LuauCompletion.h"

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
		// PROJ-7/T1:存根样本(入库存根的历史快照)在仓库内测试夹具里,不再读项目内容根。
		const std::filesystem::path assets(WLD_TEST_ASSETPATH);
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

		// ---- 2c. 悬停提示 Describe:按上下文给出 名称/类型/文档 ----
		{
			LuauCompletionItem described;
			CHECK(index.Describe("self:", "OnDestroy", described));
			CHECK(described.Type == "fun(self: WorldScript)");
			CHECK(described.Doc.find("Cleanup callback") != std::string::npos);
			CHECK(index.Describe("ui.", "panel", described));
			CHECK(described.Name == "panel" && !described.Doc.empty());
			LuauCompletionItem missing;
			CHECK(!index.Describe("ui.", "definitely_not_a_symbol", missing));
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

		// ---- 8. V9:注解类型位补全 + 注解字段悬停(用户反馈:「注释中填类型时没有提示」
		//         「鼠标在字段上时没有类型提示」) ----
		{
			CHECK(index.LoadStubFile(StubPath(), nullptr));
			index.SetFileSource(
				"---@class PlayerScript : WorldScript\n"
				"---@field Speed number 移动速度\n"
				"---@field Name string 显示名称\n"
				"local PlayerScript = { Speed = 5.0 }\n"
				"function PlayerScript:OnUpdate(dt)\n"
				"    local s = self.Speed\n"
				"end\n");

			std::vector<LuauCompletionItem> items;
			const auto indexOf = [&items](std::string_view name) -> int
			{
				for (std::size_t i = 0; i < items.size(); ++i)
					if (items[i].Name == name)
						return static_cast<int>(i);
				return -1;
			};

			// ① `---@field <名字> ` ⇒ 常用基础类型 + 引擎类型 + 其余类型/类名
			index.Query("---@field Speed ", 0, items);
			CHECK(HasName(items, "number") && HasName(items, "string") && HasName(items, "integer"));
			CHECK(HasName(items, "Entity"));        // 存根 ---@class
			CHECK(HasName(items, "WorldScript"));   // 存根里的纯注解类(无运行时全局)
			CHECK(HasName(items, "PlayerScript"));  // 当前文件的 ---@class
			// VEC-A1(D1):常用基础(6,固定顺序)在最前,引擎类型(7)紧随其后。
			CHECK(items.size() >= 13);
			CHECK(items[0].Name == "number" && items[1].Name == "string" && items[2].Name == "boolean"
				&& items[3].Name == "integer" && items[4].Name == "table" && items[5].Name == "any");
			CHECK(items[0].Kind == LuauCompletionItem::KindType::Keyword);   // 基础类型档
			CHECK(items[6].Name == "vec2" && items[7].Name == "vec3" && items[8].Name == "vec4"
				&& items[9].Name == "mat3" && items[10].Name == "mat4"
				&& items[11].Name == "Entity" && items[12].Name == "WorldScript");
			CHECK(items[6].Kind == LuauCompletionItem::KindType::Class);     // 引擎类型档
			const LuauCompletionItem* number = FindName(items, "number");
			CHECK(number != nullptr && !number->Doc.empty());   // 类型候选带一句话说明
			const LuauCompletionItem* vec3Type = FindName(items, "vec3");
			CHECK(vec3Type != nullptr && !vec3Type->Doc.empty());   // 存根没写说明时用兜底:悬停可用
			CHECK(indexOf("number") < indexOf("vec3") && indexOf("vec3") < indexOf("PlayerScript"));

			// ② `---@type ` / `---@param <名字> ` 同样是类型位
			index.Query("---@type ", 0, items);
			CHECK(HasName(items, "number") && HasName(items, "Entity"));
			CHECK(!HasName(items, "Speed"));   // 类型位不给字段名(字段不是全局符号)
			index.Query("---@param dt ", 0, items);
			CHECK(HasName(items, "number") && HasName(items, "boolean"));

			// ③ `---@class X : `(继承位)只给类型名,并按前缀过滤
			index.Query("---@class Derived : ", 0, items);
			CHECK(HasName(items, "WorldScript") && HasName(items, "Entity"));
			index.Query("---@class Derived : World", 0, items);
			CHECK(HasName(items, "WorldScript") && !HasName(items, "Entity"));

			// ④ 说明文本位置(第 3 个 token 起)不是类型位:不给类型专属候选
			index.Query("---@field Speed number ", 0, items);
			CHECK(!HasName(items, "never") && !HasName(items, "integer") && !HasName(items, "buffer")
				&& !HasName(items, "vector"));

			// ⑤ 前缀过滤大小写不敏感 + maxItems 截断(截断发生在常用基础档内)
			index.Query("---@field Speed n", 0, items);
			CHECK(HasName(items, "number") && HasName(items, "never") && !HasName(items, "string"));
			index.Query("---@field Speed ", 2, items);
			CHECK(items.size() == 2 && items[0].Name == "number" && items[1].Name == "string");

			// ⑥ 悬停:注解行里的字段名 / 代码里的 self. 与裸名 / 类型名本身
			LuauCompletionItem described;
			CHECK(index.Describe("---@field ", "Speed", described));
			CHECK(described.Name == "Speed" && described.Type == "number" && described.Doc == "移动速度");
			CHECK(index.Describe("self.", "Speed", described));
			CHECK(described.Type == "number" && described.Doc == "移动速度");
			CHECK(index.Describe("local v = ", "Name", described));
			CHECK(described.Type == "string" && described.Doc == "显示名称");
			CHECK(index.Describe("---@field ", "number", described));
			CHECK(described.Name == "number" && !described.Doc.empty());
			// 内核给的前缀是"词之前的整行片段":真实悬停类型名时前缀是 `---@field Speed `(类型位)。
			CHECK(index.Describe("---@field Speed ", "number", described));
			CHECK(described.Name == "number" && !described.Doc.empty());
			LuauCompletionItem missing;
			CHECK(!index.Describe("local v = ", "DefinitelyNotAField", missing));   // 命中不到 → 不弹空框

			// ⑦ 用户实测场景:脚本**没有** `---@class`(只有 ---@field)时也要有类型提示
			index.SetFileSource("---@field Speed number 移动速度\nlocal X = { Speed = 5.0 }\n");
			CHECK(index.Describe("---@field ", "Speed", described));
			CHECK(described.Type == "number" && described.Doc == "移动速度");
			CHECK(index.Describe("self.", "Speed", described));
			CHECK(described.Type == "number" && described.Doc == "移动速度");

			// ⑧ SetFileSource("") 之后注解字段表随之清空(不残留上一份文件的提示)
			index.SetFileSource("");
			CHECK(!index.Describe("---@field ", "Speed", described));
		}

		// ---- 9. VEC-A1(D1):类型位候选分档(常用基础 → 引擎类型 → 其余基础 → 其余类名)----
		//        修复前(13 条基础类型固定在最前 + 类名字典序)实测名次(1-based,共 49 个候选):
		//        Entity=23、mat3=29、mat4=30、vec2=46、vec3=47、vec4=48、WorldScript=49 ——
		//        类型位弹窗首屏(约 10-12 行)看不到任何引擎类型,只能先打 `vec` 前缀。
		{
			CHECK(index.LoadStubFile(StubPath(), nullptr));
			index.SetFileSource("");

			std::vector<LuauCompletionItem> items;
			index.Query("---@field Speed ", 0, items);
			const auto indexOf = [&items](std::string_view name) -> int
			{
				for (std::size_t i = 0; i < items.size(); ++i)
					if (items[i].Name == name)
						return static_cast<int>(i);
				return -1;
			};

			// 分档顺序:常用 6 → 引擎 7 → 其余基础 7 → 其余类名(字典序)。
			const char* const common[] = { "number", "string", "boolean", "integer", "table", "any" };
			const char* const engine[] = { "vec2", "vec3", "vec4", "mat3", "mat4", "Entity", "WorldScript" };
			const char* const other[] = { "buffer", "function", "never", "nil", "thread", "unknown", "vector" };
			constexpr std::size_t kCommonCount = 6;
			constexpr std::size_t kEngineCount = 7;
			constexpr std::size_t kOtherCount = 7;
			for (std::size_t i = 0; i < kCommonCount; ++i)
				CHECK(items[i].Name == common[i] && items[i].Kind == LuauCompletionItem::KindType::Keyword);
			for (std::size_t i = 0; i < kEngineCount; ++i)
				CHECK(items[kCommonCount + i].Name == engine[i]
					&& items[kCommonCount + i].Kind == LuauCompletionItem::KindType::Class);
			for (std::size_t i = 0; i < kOtherCount; ++i)
				CHECK(items[kCommonCount + kEngineCount + i].Name == other[i]);
			// 其余类名档从第 21 条(0-based 20)起:第一个类名按字典序 = AmbientLightComponent。
			CHECK(items[kCommonCount + kEngineCount + kOtherCount].Name == "AmbientLightComponent");

			// 引擎类型全部落在前 13 条内(修复前 23-49 位),且类型位说明非空。
			for (const char* name : engine)
			{
				const int position = indexOf(name);
				CHECK(position >= 0 && position < 13);
				const LuauCompletionItem* item = FindName(items, name);
				CHECK(item != nullptr && !item->Doc.empty());
			}
			std::printf("[type-order] candidates=%zu | before-fix: Entity=23 mat3=29 mat4=30 vec2=46 vec3=47 vec4=48 WorldScript=49"
				" | now: %s\n", items.size(), JoinNames(items, 13).c_str());
		}

		// ---- 10. VEC-D3(用户口径「局部变量/表字段也要能推」):`local a = 1` 之后悬停 a 给 number;
		//         `local t = { x = 1.5, s = "hi" }` 之后 t.x/t.s;嵌套表 t.stats.hp;作用域内赋值;
		//         推不出来 → 保持现状(不弹空框)。纯文本层,不建 VM。----
		{
			CHECK(index.LoadStubFile(StubPath(), nullptr));
			index.SetFileSource(
				"-- 推断用例:局部变量 / 表构造 / 嵌套表 / 作用域内赋值\n"
				"local a = 1\n"
				"local t = { x = 1.5, s = \"hi\", flag = true, stats = { hp = 10, name = 'boss' } }\n"
				"local empty = someCall()\n"
				"local later\n"
				"local f = function() end\n"
				"local origin = vec3.new(1.0, 2.0, 3.0)\n"
				"a = 2.5\n"
				"later = \"filled\"\n"
				"t.extra = false\n"
				"t.stats.hp = 42\n");

			LuauCompletionItem described;
			// 裸名:声明 + 作用域内赋值(赋值是 number,类型不丢)。
			CHECK(index.Describe("    print(", "a", described));
			CHECK(described.Name == "a" && described.Type == "number");
			// 表构造里的字段(字符串/布尔同样要推)。
			CHECK(index.Describe("    print(t.", "x", described));
			CHECK(described.Name == "x" && described.Type == "number");
			CHECK(index.Describe("    print(t.", "s", described));
			CHECK(described.Type == "string");
			CHECK(index.Describe("    print(t.", "flag", described));
			CHECK(described.Type == "boolean");
			// 嵌套表:t.stats.hp / t.stats.name。
			CHECK(index.Describe("    print(t.stats.", "hp", described));
			CHECK(described.Name == "hp" && described.Type == "number");
			CHECK(index.Describe("    print(t.stats.", "name", described));
			CHECK(described.Type == "string");
			// 作用域内字段赋值 / 先声明后赋值的局部。
			CHECK(index.Describe("    print(t.", "extra", described));
			CHECK(described.Type == "boolean");
			CHECK(index.Describe("    print(", "later", described));
			CHECK(described.Type == "string");
			// 函数与引擎类型构造器(存根里真的声明了 vec3)。
			CHECK(index.Describe("    print(", "f", described));
			CHECK(described.Type == "function");
			CHECK(index.Describe("    print(", "origin", described));
			CHECK(described.Type == "vec3");
			// 推不出来 → 保持现状:不弹空框(字段不存在 / 值不是字面量)。
			LuauCompletionItem missing;
			CHECK(!index.Describe("    print(t.", "missing", missing));
			// `local empty = someCall()`:推断不出类型 → 不凭空给类型(既有的"名字命中但没类型"原样保留)。
			CHECK(!index.Describe("    print(", "empty", missing) || missing.Type.empty());
			CHECK(!index.Describe("    print(", "definitely_not_defined", missing));
			std::printf("[infer-hover] a=number t.x=number t.s=string t.stats.hp=number origin=vec3\n");
		}

		// ---- 11. VEC-E1(E2/E3②/E4,2026-09-27 用户口径):函数返回值/成员表达式赋值的局部变量也要有
		//         类型提示;裸 table 的字段(`note`/`level`)给**本字段自己的**推断类型,不再串到存根里
		//         仅大小写不同的 `Level` 全局服务;`self.X` / `ExtraInfo.note` 的接收者链要能解析。----
		{
			CHECK(index.LoadStubFile(StubPath(), nullptr));
			index.SetFileSource(
				"local FX = { ExtraInfo = { note = \"hi\", level = 1 }, Num = 2.5 }\n"
				"local fromMember = FX.Num\n"
				"local fromNested = FX.ExtraInfo.note\n"
				"local fromSelf = self.Speed\n"
				"local fromEntity = self.entity:GetComponent(\"TransformComponent\")\n"
				"---@return number\n"
				"local function probeHelper() end\n"
				"local fromCall = Level.Primary()\n"
				"local fromFunc = probeHelper()\n"
				"---@class E1HoverProbe : WorldScript\n"
				"---@field Speed number 移动速度\n"
				"local E1HoverProbe = { Speed = 5.0, ExtraInfo = { note = \"x\", level = 1 } }\n");

			LuauCompletionItem described;
			const auto describeLine = [&index](const char* prefix, const char* word) -> std::string
			{
				LuauCompletionItem item;
				if (!index.Describe(prefix, word, item))
					return "<none>";
				return item.Name + " type='" + item.Type + "' doc='" + item.Doc + "'";
			};
			std::printf("[e1-hover] fromMember=%s\n", describeLine("    print(", "fromMember").c_str());
			std::printf("[e1-hover] fromNested=%s\n", describeLine("    print(", "fromNested").c_str());
			std::printf("[e1-hover] fromSelf=%s\n", describeLine("    print(", "fromSelf").c_str());
			std::printf("[e1-hover] fromEntity=%s\n", describeLine("    print(", "fromEntity").c_str());
			std::printf("[e1-hover] fromCall=%s\n", describeLine("    print(", "fromCall").c_str());
			std::printf("[e1-hover] fromFunc=%s\n", describeLine("    print(", "fromFunc").c_str());
			std::printf("[e1-hover] note=%s\n", describeLine("    print(", "note").c_str());
			std::printf("[e1-hover] level=%s\n", describeLine("    print(", "level").c_str());
			std::printf("[e1-hover] ExtraInfo.note=%s\n",
				describeLine("    print(ExtraInfo.", "note").c_str());
			std::printf("[e1-hover] self.ExtraInfo.level=%s\n",
				describeLine("    print(self.ExtraInfo.", "level").c_str());
			// E2:成员表达式(`a.b`)/ 接收者链。
			CHECK(index.Describe("    print(", "fromMember", described));
			CHECK(described.Type == "number");
			CHECK(index.Describe("    print(", "fromNested", described));
			CHECK(described.Type == "string");
			// E2:`self.Member`(文件类注解字段)。
			CHECK(index.Describe("    print(", "fromSelf", described));
			CHECK(described.Type == "number");
			// E2:函数调用结果 —— 存根 `---@return userdata|nil` 取第一个非 nil 类型。
			CHECK(index.Describe("    print(", "fromEntity", described));
			CHECK(described.Type == "userdata");
			// E2:存根方法返回类型(`Level:Primary()` → string)。
			CHECK(index.Describe("    print(", "fromCall", described));
			CHECK(described.Type == "string");
			// E2:本文件 `---@return number` 的函数。
			CHECK(index.Describe("    print(", "fromFunc", described));
			CHECK(described.Type == "number");
			// E3②:表构造里的裸字段 → 本文件自己的推断类型。
			CHECK(index.Describe("    print(", "note", described));
			CHECK(described.Type == "string");
			// E4:`level` 必须给 number(修复前命中存根 Level 服务,Doc 是 "level/flow service…")。
			CHECK(index.Describe("    print(", "level", described));
			CHECK(described.Type == "number");
			CHECK(described.Doc.find("level/flow service") == std::string::npos);
			// E3②:接收者链 —— 根是"表的字段"(`ExtraInfo`)或 `self`(文件类)都要能解析。
			CHECK(index.Describe("    print(ExtraInfo.", "note", described));
			CHECK(described.Type == "string");
			CHECK(index.Describe("    print(self.ExtraInfo.", "level", described));
			CHECK(described.Type == "number");
			// 推不出来 → 不弹(未知名字)。
			LuauCompletionItem missing;
			CHECK(!index.Describe("    print(", "definitely_not_defined", missing));
			std::printf("[e1-hover] fromMember=number fromNested=string fromSelf=number "
				"fromEntity=userdata fromCall=string fromFunc=number note=string level=number\n");
		}

		// ---- 12. VEC-F1(2026-09-27 用户口径「Level 也要修复」):本文件推断优先于存根同名符号;
		//         链式访问(`self.InferredStats.Level` / `Config.mp`)按接收者链解析到本字段类型;
		//         文件里没有同名来源时,存根 `Level` 服务表说明与 `Level:Primary()` 的返回类型照旧。----
		{
			CHECK(index.LoadStubFile(StubPath(), nullptr));
			// 夹具形态与示例模板的 assets/scripts/examples/FeatureShowcase.lua 一致:
			// 结构体 `---@class` 声明在脚本类**之前**,脚本类的表里再嵌套未注解的表字段
			// (`InferredStats.Level` / `Config.mp`);E1 的夹具只有一个类,盖不到这个形态。
			index.SetFileSource(
				"---@class F1Stats\n"
				"---@field Damage number 攻击力\n"
				"---@class F1Showcase : WorldScript\n"
				"---@field Speed number 移动速度\n"
				"local F1Showcase = {\n"
				"    Speed = 2.0,\n"
				"    ExtraInfo = { note = \"运行期自用\", level = 1 },\n"
				"    Config = { hp = 10, mp = 20 },\n"
				"    InferredStats = {\n"
				"        Level = 3,\n"
				"        Title = \"自动推导\",\n"
				"        Nested = { Deep = 7 },\n"
				"    },\n"
				"}\n"
				"return F1Showcase\n");

			LuauCompletionItem described;
			const auto describe = [&index](const char* prefix, const char* word)
			{
				LuauCompletionItem item;
				if (!index.Describe(prefix, word, item))
					return std::string("<none>");
				return item.Name + " type='" + item.Type + "' doc='" + item.Doc + "'";
			};
			// 修复前/后的 tooltip 原文(先打印再判定:修复前该用例 FAIL 时也有对比数据)。
			std::printf("[f1-hover] bare Level=%s | self.InferredStats.Level=%s | F1Showcase.chain=%s | "
				"self.InferredStats.Nested.Deep=%s | self.Config.mp=%s | level=%s | note=%s | "
				"self.Speed=%s\n",
				describe("        ", "Level").c_str(),
				describe("    print(self.InferredStats.", "Level").c_str(),
				describe("    print(F1Showcase.InferredStats.", "Level").c_str(),
				describe("    print(self.InferredStats.Nested.", "Deep").c_str(),
				describe("    print(self.Config.", "mp").c_str(),
				describe("    print(", "level").c_str(),
				describe("    print(self.ExtraInfo.", "note").c_str(),
				describe("    print(self.", "Speed").c_str());
			// VEC-F1 核心①:裸 `Level`(表构造字段,行前缀只有缩进)给**本文件**的 number,
			// 不再是存根 `---@class Level` 服务表的说明(修复前 tooltip 标题/内容就是它)。
			CHECK(index.Describe("        ", "Level", described));
			CHECK(described.Name == "Level" && described.Type == "number");
			CHECK(described.Doc.find("level/flow service") == std::string::npos);
			// VEC-F1 核心②:链式 `self.InferredStats.Level` —— `self` 必须绑定"带推断表的脚本类";
			// m_FileClasses 的第一个是 F1Stats(不是脚本类),旧实现因此解析不出这条链。
			CHECK(index.Describe("    print(self.InferredStats.", "Level", described));
			CHECK(described.Name == "Level" && described.Type == "number");
			CHECK(described.Doc.find("level/flow service") == std::string::npos);
			// 无 `self` 前缀的链式与映射值同样按接收者链解析。
			CHECK(index.Describe("    print(F1Showcase.InferredStats.", "Level", described));
			CHECK(described.Type == "number");
			// 三层链(`self.InferredStats.Nested.Deep`):裸名兜底只扫到两层,这条只能靠
			// 接收者链(`self` 绑定带推断表的脚本类)解析 —— 修复前没有提示。
			CHECK(index.Describe("    print(self.InferredStats.Nested.", "Deep", described));
			CHECK(described.Name == "Deep" && described.Type == "number");
			CHECK(index.Describe("    print(self.Config.", "mp", described));
			CHECK(described.Name == "mp" && described.Type == "number");
			// E1 口径回归:裸 `level`(小写)仍是本字段 number;`self.ExtraInfo.note` 是 string;
			// 注解声明的成员仍带注解说明(`self.Speed` → number + "移动速度")。
			CHECK(index.Describe("    print(", "level", described));
			CHECK(described.Type == "number");
			CHECK(index.Describe("    print(self.ExtraInfo.", "note", described));
			CHECK(described.Type == "string");
			CHECK(index.Describe("    print(self.", "Speed", described));
			CHECK(described.Type == "number" && described.Doc == "移动速度");

			// 反证/回归:文件里**没有** `Level` 的任何本地/推断来源时 —— 存根 `Level` 服务表
			// 照旧(说明还是存根那份),`Level.Primary()` 的结果仍是 string(存根 `---@return`)。
			index.SetFileSource(
				"local fromCall = Level.Primary()\n"
				"return fromCall\n");
			std::printf("[f1-hover] no-local-source: Level=%s | fromCall=%s | vec3.new=%s\n",
				describe("    print(", "Level").c_str(),
				describe("    print(", "fromCall").c_str(),
				describe("    print(vec3.", "new").c_str());
			CHECK(index.Describe("    print(", "Level", described));
			CHECK(described.Name == "Level");
			CHECK(described.Doc.find("level/flow service") != std::string::npos);
			CHECK(index.Describe("    print(", "fromCall", described));
			CHECK(described.Type == "string");
			// 服务表/类的成员悬停不受影响(`vec3.new` 仍是存根成员)。
			CHECK(index.Describe("    print(vec3.", "new", described));
			CHECK(described.Name == "new" && !described.Type.empty());
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
