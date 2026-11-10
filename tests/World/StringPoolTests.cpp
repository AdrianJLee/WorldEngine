// PECS 1.2(2026-10-04):驻留字符串池(World/Core/StringPool)headless 单测。
//
// 验收口径:
//   1. 同串恒同 id;不同串不同 id;空串恒 0(无效);
//   2. 路径归一化:反斜杠 / "./" / 冗余分隔 / 末尾斜杠 折叠到同一个 PathId;
//   3. 名字与路径 id 空间**独立**(同一个字符串分别 InternName / InternPath 不互相顶掉);
//   4. 反查返回原始(路径为归一化后)字符串;未驻留 id 返回空串,不抛;
//   5. 引用稳定性:后续大量插入不会让已取到的引用失效(容器是 deque);
//   6. 进程内单例:两次 Get() 是同一实例(Editor / Runtime / Game 共享同一张表)。
#include "World/Core/StringPool.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
	using World::NameId;
	using World::PathId;
	using World::StringPool;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	void IdentityAndEmptiness()
	{
		StringPool& pool = StringPool::Get();
		CHECK(pool.InternPath("").Value == 0u);
		CHECK(pool.InternName("").Value == 0u);

		const PathId a = pool.InternPath("models/rock.wmodel");
		const PathId b = pool.InternPath("models/rock.wmodel");
		const PathId c = pool.InternPath("models/other.wmodel");
		CHECK(a.IsValid());
		CHECK(a == b);
		CHECK(a != c);

		const NameId n1 = pool.InternName("Player");
		const NameId n2 = pool.InternName("Player");
		CHECK(n1.IsValid() && n1 == n2);
	}

	void PathNormalizationCollapses()
	{
		StringPool& pool = StringPool::Get();
		const PathId base = pool.InternPath("materials/steel.wmat");
		CHECK(pool.InternPath("materials\\steel.wmat") == base);   // 反斜杠
		CHECK(pool.InternPath("./materials/steel.wmat") == base);  // 前导 ./
		CHECK(pool.InternPath("materials//steel.wmat") == base);   // 冗余分隔
		CHECK(pool.InternPath("materials/./steel.wmat") == base);
		CHECK(pool.InternPath("materials/steel.wmat/") == base);   // 末尾斜杠
		CHECK(pool.NormalizePath("materials\\..\\materials/steel.wmat") == "materials/steel.wmat");
	}

	void NameAndPathSpacesAreIndependent()
	{
		StringPool& pool = StringPool::Get();
		const std::string same = "shared-token-spaces-are-independent";
		const PathId p = pool.InternPath(same);
		const NameId n = pool.InternName(same);
		CHECK(p.IsValid() && n.IsValid());
		CHECK(pool.PathOf(p) == same);
		CHECK(pool.NameOf(n) == same);
	}

	void LookupIsSafe()
	{
		StringPool& pool = StringPool::Get();
		CHECK(pool.PathOf(PathId{}).empty());
		CHECK(pool.NameOf(NameId{}).empty());
		CHECK(pool.PathOf(PathId{ 0xFFFFFFFFu }).empty());
		CHECK(pool.NameOf(NameId{ 0xFFFFFFFFu }).empty());
	}

	void ReferencesAreStableAcrossGrowth()
	{
		StringPool& pool = StringPool::Get();
		const PathId id = pool.InternPath("stability/probe.wmodel");
		const std::string& ref = pool.PathOf(id);
		for (int i = 0; i < 512; ++i)
			pool.InternPath("stability/filler-" + std::to_string(i) + ".wmodel");
		CHECK(pool.PathOf(id) == "stability/probe.wmodel");
		CHECK(&pool.PathOf(id) == &ref);   // deque:元素地址不因插入而改变
	}

	void SingleInstanceAndThreadedReads()
	{
		CHECK(&StringPool::Get() == &StringPool::Get());
		StringPool& pool = StringPool::Get();
		const PathId id = pool.InternPath("threads/probe.wmodel");
		std::vector<std::thread> readers;
		std::vector<bool> ok(4, false);
		for (int i = 0; i < 4; ++i)
		{
			readers.emplace_back([&pool, id, &ok, i]
			{
				bool all = true;
				for (int k = 0; k < 2000; ++k)
					all = all && (pool.PathOf(id) == "threads/probe.wmodel");
				ok[static_cast<std::size_t>(i)] = all;
			});
		}
		for (std::thread& reader : readers)
			reader.join();
		for (bool value : ok)
			CHECK(value);
	}
}

int main()
{
	try
	{
		IdentityAndEmptiness();
		PathNormalizationCollapses();
		NameAndPathSpacesAreIndependent();
		LookupIsSafe();
		ReferencesAreStableAcrossGrowth();
		SingleInstanceAndThreadedReads();
		std::printf("World.StringPool: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.StringPool: FAILED: %s\n", error.what());
		return 1;
	}
}