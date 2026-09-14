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
	template<typename T>
	using Ref = std::shared_ptr<T>;

	// args的类型是左值,其值的类型是Args&&(万能引用)
	// std::forward<Args>(args)...的作用是将args中的每个元素完美转发到std::make_shared<T>中
	template<typename T, typename... Args>
	constexpr Ref<T> CreateRef(Args&&... args)
	{
		return std::make_shared<T>(std::forward<Args>(args)...);
	}


	template<typename T>
	using WeakRef = std::weak_ptr<T>;

	template<typename T, typename... Args>
	constexpr WeakRef<T> CreateWeakRef(Args&&... args)
	{
		return std::weak_ptr<T>(std::forward<Args>(args)...);
	}


	template<typename T>
	using Scope = std::unique_ptr<T>;
	template<typename T, typename... Args>
	constexpr Scope<T> CreateScope(Args&&... args)
	{
		return std::make_unique<T>(std::forward<Args>(args)...);
	}
}
