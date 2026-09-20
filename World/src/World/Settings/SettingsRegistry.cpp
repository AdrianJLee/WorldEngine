#include "wldpch.h"
#include "World/Settings/SettingsRegistry.h"

#include "World/Core/Log.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <utility>

namespace World::Settings
{
	namespace
	{
		std::string ToLower(std::string_view text)
		{
			std::string lowered(text);
			std::transform(lowered.begin(), lowered.end(), lowered.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return lowered;
		}

		bool ParseBool(const std::string& value, bool* out)
		{
			if (value == "true" || value == "1") { *out = true; return true; }
			if (value == "false" || value == "0") { *out = false; return true; }
			return false;
		}
	}

	SettingsRegistry& SettingsRegistry::Get()
	{
		static SettingsRegistry registry;
		return registry;
	}

	void SettingsRegistry::Register(SettingDescriptor descriptor)
	{
		if (descriptor.Id.empty())
		{
			WLD_CORE_WARN("设置注册表:忽略没有 Id 的描述符(label={0})", descriptor.Label);
			return;
		}
		for (SettingDescriptor& existing : m_Items)
			if (existing.Id == descriptor.Id)
			{
				WLD_CORE_WARN("设置注册表:Id 重复,后者覆盖前者({0})", descriptor.Id);
				existing = std::move(descriptor);
				return;
			}
		m_Items.push_back(std::move(descriptor));
	}

	void SettingsRegistry::ClearScope(SettingScope scope)
	{
		m_Items.erase(std::remove_if(m_Items.begin(), m_Items.end(),
			[scope](const SettingDescriptor& item) { return item.Scope == scope; }), m_Items.end());
	}

	void SettingsRegistry::Clear()
	{
		m_Items.clear();
	}

	const SettingDescriptor* SettingsRegistry::Find(std::string_view id) const
	{
		for (const SettingDescriptor& item : m_Items)
			if (item.Id == id)
				return &item;
		return nullptr;
	}

	std::vector<const SettingDescriptor*> SettingsRegistry::OfScope(SettingScope scope, std::string_view group) const
	{
		std::vector<const SettingDescriptor*> result;
		for (const SettingDescriptor& item : m_Items)
			if (item.Scope == scope && (group.empty() || item.Group == group))
				result.push_back(&item);
		return result;
	}

	std::vector<std::string> SettingsRegistry::Groups(SettingScope scope) const
	{
		std::vector<std::string> groups;
		for (const SettingDescriptor& item : m_Items)
			if (item.Scope == scope && std::find(groups.begin(), groups.end(), item.Group) == groups.end())
				groups.push_back(item.Group);
		return groups;
	}

	std::vector<const SettingDescriptor*> SettingsRegistry::Search(
		SettingScope scope, std::string_view query, bool onlyModified) const
	{
		const std::string needle = ToLower(query);
		std::vector<const SettingDescriptor*> result;
		for (const SettingDescriptor& item : m_Items)
		{
			if (item.Scope != scope)
				continue;
			if (onlyModified && item.IsDefault && item.IsDefault())
				continue;
			if (!needle.empty())
			{
				const bool hit = ToLower(item.Id).find(needle) != std::string::npos
					|| ToLower(item.Label).find(needle) != std::string::npos
					|| ToLower(item.Tooltip).find(needle) != std::string::npos;
				if (!hit)
					continue;
			}
			result.push_back(&item);
		}
		return result;
	}

	bool SettingsRegistry::HasModified(SettingScope scope) const
	{
		for (const SettingDescriptor& item : m_Items)
			if (item.Scope == scope && item.IsDefault && !item.IsDefault())
				return true;
		return false;
	}

	bool SettingsRegistry::Set(std::string_view id, const std::string& value, std::string* error)
	{
		const SettingDescriptor* item = Find(id);
		if (!item)
		{
			if (error) *error = "unknown setting: " + std::string(id);
			return false;
		}
		if (!item->Write)
		{
			if (error) *error = "setting is read-only: " + std::string(id);
			return false;
		}
		return item->Write(value, error);
	}

	bool SettingsRegistry::Reset(std::string_view id, std::string* error)
	{
		const SettingDescriptor* item = Find(id);
		if (!item || !item->Reset)
		{
			if (error) *error = "setting has no reset: " + std::string(id);
			return false;
		}
		return item->Reset();
	}

	std::string SettingsRegistry::ReadText(std::string_view id, const std::string& fallback) const
	{
		const SettingDescriptor* item = Find(id);
		if (!item || !item->Read)
			return fallback;
		return item->Read();
	}

	bool SettingsRegistry::ReadBool(std::string_view id, bool fallback) const
	{
		bool parsed = fallback;
		if (!ParseBool(ReadText(id), &parsed))
			return fallback;
		return parsed;
	}

	int64_t SettingsRegistry::ReadInt(std::string_view id, int64_t fallback) const
	{
		const std::string text = ReadText(id);
		if (text.empty())
			return fallback;
		int64_t parsed = fallback;
		const char* begin = text.data();
		const char* end = text.data() + text.size();
		if (std::from_chars(begin, end, parsed).ec != std::errc())
			return fallback;
		return parsed;
	}

	double SettingsRegistry::ReadFloat(std::string_view id, double fallback) const
	{
		const std::string text = ReadText(id);
		if (text.empty())
			return fallback;
		char* parsedEnd = nullptr;
		const double parsed = std::strtod(text.c_str(), &parsedEnd);
		if (parsedEnd == text.c_str())
			return fallback;
		return parsed;
	}
}
