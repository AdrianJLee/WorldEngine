#pragma once

#include <string>

namespace World
{
	// 平台动态库最小封装:不泄漏平台句柄类型给调用方。
	class DynamicLibrary
	{
	public:
		DynamicLibrary() = default;
		~DynamicLibrary();
		DynamicLibrary(const DynamicLibrary&) = delete;
		DynamicLibrary& operator=(const DynamicLibrary&) = delete;
		DynamicLibrary(DynamicLibrary&& other) noexcept;
		DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

		bool Load(const std::string& path);
		void* GetSymbol(const char* name) const;
		bool IsLoaded() const;
		void Unload();
		const std::string& GetLastError() const { return m_LastError; }

	private:
		void* m_Handle = nullptr;
		std::string m_LastError;
	};
}
