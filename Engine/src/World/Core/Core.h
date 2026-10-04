#pragma once

#include <memory>
#include <filesystem>

#ifdef WLD_DEBUG
#if defined(WLD_PLATFORM_WINDOWS)
#define WLD_DEBUGBREAK() __debugbreak()
#elif defined(WLD_PLATFORM_LINUX)
#include <signal.h>
#define WLD_DEBUGBREAK() raise(SIGTRAP)
#else
#error "Platform doesn't support debugbreak yet!"
#endif
#define WLD_ENABLE_ASSERTS
#else
#define WLD_DEBUGBREAK()
#endif

#define WLD_EXPAND_MACRO(x) x
#define WLD_STRINGIFY_MACRO(x) #x


#ifdef WLD_ENABLE_ASSERTS
// 断言:第一个参数是条件,其后是日志格式串与可选参数(所有调用点都带消息)。
// 之前的实现用宏参数个数选择器,3 个及以上参数时会选错宏,已简化。
#define WLD_INTERNAL_ASSERT_IMPL(type, check, ...) \
	{ if (!(check)) { WLD##type##ERROR(__VA_ARGS__); WLD_DEBUGBREAK(); } }

#define WLD_ASSERT(...) WLD_EXPAND_MACRO( WLD_INTERNAL_ASSERT_IMPL(_, __VA_ARGS__) )
#define WLD_CORE_ASSERT(...) WLD_EXPAND_MACRO( WLD_INTERNAL_ASSERT_IMPL(_CORE_, __VA_ARGS__) )
#else
#define WLD_ASSERT(...)
#define WLD_CORE_ASSERT(...)
#endif // WLD_ENABLE_ASSERTS



#define BIT(x) (1 << x)

#define WLD_BIND_EVENT_FN(fn) [this] ( auto&&... args ) -> decltype(auto) { return this->fn( std::forward<decltype(args)>(args)... ); }

namespace World
{
	// ---------------------------------------------------------------------------
	// 所有权契约(2026-10-04,见知识库 decisions/0007-ownership-and-handles)
	//
	// 引擎只保留**一个**共享所有权词汇类型:`Ref<T>` = std::shared_ptr<T>。
	// 它命名的生命周期是:「**无确定性单一所有者的运行时资源**」—— GPU buffer /
	// texture / material / mesh / 运行时服务句柄。
	//
	// 硬约束:
	//   * 禁止把 `Ref<T>` 放进 schema 组件:组件只放 POD(值、或 POD 引用/句柄)。
	//     组件必须平凡可拷贝 ⇒ 任何堆持有对象(shared_ptr/string/vector)都不许进。
	//   * 实体/资产这类"可失效引用"用**世代校验的 POD 句柄**(Entity、AssetId、
	//     PathId/NameId),不用 `weak_ptr` —— weak_ptr 是 16B + 原子控制块跳转 +
	//     不可平凡拷贝,结构上进不了组件。
	//   * 独占所有权直接用 `std::unique_ptr`,不给它起别名:别名不增加任何保证。
	//
	// `CreateRef` 是**唯一**的 Ref 分配收口点,保留它是为了将来能挂分配器/存活标签,
	// 不要在业务代码里直接 `std::make_shared`。
	// ---------------------------------------------------------------------------
	template<typename T>
	using Ref = std::shared_ptr<T>;

	// args的类型是左值,其值的类型是Args&&(万能引用)
	// std::forward<Args>(args)...的作用是将args中的每个元素完美转发到std::make_shared<T>中
	template<typename T, typename... Args>
	constexpr Ref<T> CreateRef(Args&&... args)
	{
		return std::make_shared<T>(std::forward<Args>(args)...);
	}
}
