#include "wldpch.h"
#include "World/Utils/DynamicLibrary.h"

namespace World
{
	DynamicLibrary::~DynamicLibrary()
	{
		Unload();
	}

	DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
		: m_Handle(other.m_Handle), m_LastError(std::move(other.m_LastError))
	{
		other.m_Handle = nullptr;
	}

	DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
	{
		if (this != &other)
		{
			Unload();
			m_Handle = other.m_Handle;
			m_LastError = std::move(other.m_LastError);
			other.m_Handle = nullptr;
		}
		return *this;
	}

	bool DynamicLibrary::Load(const std::string& path)
	{
		Unload();
		m_Handle = LoadLibraryA(path.c_str());
		if (!m_Handle)
		{
			m_LastError = "LoadLibraryA failed with Win32 error ";
			m_LastError += std::to_string(static_cast<unsigned long long>(::GetLastError()));
		}
		return m_Handle != nullptr;
	}

	void* DynamicLibrary::GetSymbol(const char* name) const
	{
		if (!m_Handle)
			return nullptr;
		return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_Handle), name));
	}

	bool DynamicLibrary::IsLoaded() const
	{
		return m_Handle != nullptr;
	}

	void DynamicLibrary::Unload()
	{
		if (m_Handle)
		{
			FreeLibrary(static_cast<HMODULE>(m_Handle));
			m_Handle = nullptr;
		}
	}
}
