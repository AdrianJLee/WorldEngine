#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

namespace World
{
	// ---------------------------------------------------------------------------
	// InlineString<N>:容量固定、**无堆、平凡可拷贝**的文本(2026-10-05)。
	//
	// 为什么需要它:`std::string` 放进组件有两个问题 ——
	//   1. 组件**不再平凡可拷贝** ⇒ 复制实体 / Prefab 实例化走堆深拷贝,也不能 memcpy;
	//   2. MSVC x64 下恒占 32B(含 SSO 缓冲)**外加**一次堆分配(超过内联缓冲时)。
	// 组件的契约是"只放 POD"(见 contract.runtime-state-outside-components),所以
	// **有界文本**必须有对应的 POD 类型。这就是本类。
	//
	// 什么时候用哪个(判据):
	//   * `String`(std::string)   : **无界**用户文本 —— 文本框能输入任意长度,不该被截断。
	//   * `Text`  (InlineString<N>): **有界**文本 —— 名字、标签串、备注、简短说明。
	//     容量是设计决定,并且**截断必须可见**(见 Assign)。
	//
	// 语义:
	//   * 容量 = `N - 1` 个字符 + 结尾 NUL(`Data` 占 N 字节);要求 `N` 是 4 的倍数,
	//     这样 `sizeof` 恰好 = `4 + N`(长度字段 + 数据),不产生对齐空洞;
	//   * `Assign` / `Append` 返回 **false 表示"没装下、已截断"** —— 调用方必须自己决定
	//     报错 / 截短 / 换更大的 N;**构造期**从 string_view 构造是隐式截断(false 无处可给),
	//     所以静态初值与成员默认值应当本来就写在下界内;
	//   * 平凡可拷贝 / 平凡可析构 ⇒ 可 memcpy、可进组件、可进 .wd;
	//   * 与 std::string_view 可直接比较,便于替代 `tag == "x"` 这类写法。
	// ---------------------------------------------------------------------------
	template <std::size_t N>
	class InlineString
	{
	public:
		static_assert(N >= 8, "InlineString capacity is too small (need at least a few characters)");
		static_assert(N % 4 == 0, "InlineString<N>: N must be a multiple of 4 so sizeof == 4 + N");
		// 依赖**类完整性**的不变量(sizeof / type traits)不能放类体内:那里面对的是未完成类型,
		// MSVC 报 C2027/C2139。它们由文件末尾的 `Detail::InlineStringInvariants<N>` 检查。

		InlineString() { Data[0] = '\0'; }

		// 隐式截断(见类注释):静态初值 / 成员默认值用。
		InlineString(std::string_view text) { AssignUnchecked(text); }
		InlineString(const char* text) { AssignUnchecked(text ? std::string_view(text) : std::string_view()); }

		// 可装下的最大字符数(不含结尾 NUL)。
		static constexpr std::size_t Capacity() { return N - 1; }

		std::size_t Size() const { return m_Length; }
		bool Empty() const { return m_Length == 0; }
		const char* CStr() const { return Data; }
		std::string_view View() const { return std::string_view(Data, m_Length); }

		// 写入;**返回 false = 文本过长已截断**(调用方必须处理,不静默丢数据)。
		bool Assign(std::string_view text)
		{
			if (text.size() > Capacity())
			{
				AssignUnchecked(text);
				return false;
			}
			AssignUnchecked(text);
			return true;
		}

		// 追加;返回 false = 只装下了一部分(已按剩余容量截断)。
		bool Append(std::string_view text)
		{
			const std::size_t room = Capacity() - m_Length;
			if (text.size() > room)
			{
				AppendUnchecked(text.substr(0, room));
				return false;
			}
			AppendUnchecked(text);
			return true;
		}

		void Clear()
		{
			m_Length = 0;
			Data[0] = '\0';
		}

		operator std::string_view() const { return View(); }

		bool operator==(std::string_view other) const { return View() == other; }
		bool operator!=(std::string_view other) const { return View() != other; }
		bool operator==(const char* other) const { return View() == std::string_view(other ? other : ""); }
		bool operator!=(const char* other) const { return !(*this == other); }

		// 按值取一份 std::string(边界层/日志/需持有副本时用)。
		std::string ToString() const { return std::string(View()); }

	private:
		void AssignUnchecked(std::string_view text)
		{
			const std::size_t count = text.size() < Capacity() ? text.size() : Capacity();
			if (count != 0)
				std::memcpy(Data, text.data(), count);
			m_Length = static_cast<uint32_t>(count);
			Data[count] = '\0';
		}

		void AppendUnchecked(std::string_view text)
		{
			if (!text.empty())
				std::memcpy(Data + m_Length, text.data(), text.size());
			m_Length += static_cast<uint32_t>(text.size());
			Data[m_Length] = '\0';
		}

		uint32_t m_Length = 0;
		char Data[N] {};
	};

	namespace Detail
	{
		// 类**定义之后**才求值的不变量(类内求值面对的是未完成类型)。
		// 显式实例化 InlineStringInvariants<N> 即检查该 N —— 新增别名时一并实例化。
		template <std::size_t N>
		struct InlineStringInvariants
		{
			static_assert(sizeof(InlineString<N>) == 4 + N,
				"InlineString must be exactly (length + N) bytes with no padding");
			static_assert(std::is_trivially_copyable_v<InlineString<N>>,
				"InlineString must stay trivially copyable so components can be memcpy'd");
			static_assert(std::is_trivially_destructible_v<InlineString<N>>,
				"InlineString must stay trivially destructible (no heap)");
			static constexpr bool Value = true;
		};
	}

	// 组件里推荐的档位(按 sizeof 命名,便于一眼估内存):
	//   InlineString<12> = 16B(11 字符)  —— 短标签(类型/标签/枚举名);
	//   InlineString<16> = 20B(15 字符)  —— 短名字(显示名/键名);
	//   InlineString<20> = 24B(19 字符)  —— 比 std::string(32B)省 8B;
	//   InlineString<28> = 32B(27 字符)  —— 与 std::string 同尺寸,但**无堆且平凡可拷贝**。
	using InlineText16 = InlineString<12>;
	using InlineText20 = InlineString<16>;
	using InlineText24 = InlineString<20>;
	using InlineText32 = InlineString<28>;

	// 别名层断言(F5):每个别名都必须真的满足"无堆 / 平凡 / 无填充"。新增别名时一并实例化。
	static_assert(Detail::InlineStringInvariants<12>::Value, "InlineText16 invariants");
	static_assert(Detail::InlineStringInvariants<16>::Value, "InlineText20 invariants");
	static_assert(Detail::InlineStringInvariants<20>::Value, "InlineText24 invariants");
	static_assert(Detail::InlineStringInvariants<28>::Value, "InlineText32 invariants");
	static_assert(sizeof(InlineText16) == 16 && sizeof(InlineText20) == 20, "alias sizes must match their names");
	static_assert(sizeof(InlineText24) == 24 && sizeof(InlineText32) == 32, "alias sizes must match their names");
}
