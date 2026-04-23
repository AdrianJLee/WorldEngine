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
// Alteratively we could use the same "default" message for both "WITH_MSG" and "NO_MSG" and
// provide support for custom formatting by concatenating the formatting string instead of having the format inside the default message
#define WLD_INTERNAL_ASSERT_IMPL(type, check, msg, ...) { if(!(check)) { WLD##type##ERROR(msg, __VA_ARGS__); WLD_DEBUGBREAK(); } }
#define WLD_INTERNAL_ASSERT_WITH_MSG(type, check, ...) WLD_INTERNAL_ASSERT_IMPL(type, check, "Assertion failed: {0}", __VA_ARGS__)
#define WLD_INTERNAL_ASSERT_NO_MSG(type, check) WLD_INTERNAL_ASSERT_IMPL(type, check, "Assertion '{0}' failed at {1}:{2}", WLD_STRINGIFY_MACRO(check), std::filesystem::path(__FILE__).filename().string(), __LINE__)

#define WLD_INTERNAL_ASSERT_GET_MACRO_NAME(arg1, arg2, macro, ...) macro
#define WLD_INTERNAL_ASSERT_GET_MACRO(...) WLD_EXPAND_MACRO( WLD_INTERNAL_ASSERT_GET_MACRO_NAME(__VA_ARGS__, WLD_INTERNAL_ASSERT_WITH_MSG, WLD_INTERNAL_ASSERT_NO_MSG) )

// Currently accepts at least the condition and one additional parameter (the message) being optional
#define WLD_ASSERT(...) WLD_EXPAND_MACRO( WLD_INTERNAL_ASSERT_GET_MACRO(__VA_ARGS__)(_, __VA_ARGS__) )
#define WLD_CORE_ASSERT(...) WLD_EXPAND_MACRO( WLD_INTERNAL_ASSERT_GET_MACRO(__VA_ARGS__)(_CORE_, __VA_ARGS__) )
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