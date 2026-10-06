#include "wldpch.h"
#include "World/UI/UiBinding.h"

#include "World/UI/UiScreen.h"

#include <mutex>
#include <utility>

namespace World::UI
{
	namespace
	{
		constexpr const char* kSchemeConst = "const";
		constexpr const char* kSchemeEcs = "ecs";
		constexpr const char* kSchemeScript = "script";
		constexpr const char* kSchemeService = "service";

		// 内置 const 解析器:字面量原样返回(不需要外部数据,永远可求值)。
		bool ConstResolver(const UiBindingSource& source, void*, std::string& out, std::string*)
		{
			out = source.Text;
			return true;
		}

		using ResolverEntry = std::pair<std::string, UiBindingResolverFn>;

		std::vector<ResolverEntry>& ResolverTable()
		{
			static std::vector<ResolverEntry> table;
			return table;
		}

		std::once_flag& ResolverOnce()
		{
			static std::once_flag flag;
			return flag;
		}

		void EnsureBuiltinResolvers()
		{
			std::call_once(ResolverOnce(), [] {
				ResolverTable().push_back(ResolverEntry { kSchemeConst, &ConstResolver });
			});
		}

		// 按分隔符切分;**任何空段(含首尾)都非法**。
		bool SplitNonEmpty(std::string_view text, char separator, std::vector<std::string>& out,
			std::string* error, const char* what)
		{
			out.clear();
			std::size_t start = 0;
			while (true)
			{
				const std::size_t pos = text.find(separator, start);
				const std::string_view part = pos == std::string_view::npos
					? text.substr(start) : text.substr(start, pos - start);
				if (part.empty())
				{
					if (error)
						*error = std::string("empty segment in ") + what;
					return false;
				}
				out.emplace_back(part);
				if (pos == std::string_view::npos)
					return true;
				start = pos + 1;
			}
		}
	}

	const char* UiBindingSchemeName(UiBindingScheme scheme)
	{
		switch (scheme)
		{
		case UiBindingScheme::Const: return kSchemeConst;
		case UiBindingScheme::Ecs: return kSchemeEcs;
		case UiBindingScheme::Script: return kSchemeScript;
		case UiBindingScheme::Service: return kSchemeService;
		case UiBindingScheme::Unknown: break;
		}
		return "unknown";
	}

	bool ParseUiBindingScheme(std::string_view name, UiBindingScheme& out)
	{
		if (name == kSchemeConst) { out = UiBindingScheme::Const; return true; }
		if (name == kSchemeEcs) { out = UiBindingScheme::Ecs; return true; }
		if (name == kSchemeScript) { out = UiBindingScheme::Script; return true; }
		if (name == kSchemeService) { out = UiBindingScheme::Service; return true; }
		return false;
	}

	bool UiBinding::Parse(std::string_view source, UiBindingSource& out, std::string* error)
	{
		const std::size_t colon = source.find(':');
		if (colon == std::string_view::npos)
		{
			if (error)
				*error = "missing scheme prefix (expected '<scheme>:<source>')";
			return false;
		}

		const std::string_view schemeName = source.substr(0, colon);
		UiBindingScheme scheme = UiBindingScheme::Unknown;
		if (!ParseUiBindingScheme(schemeName, scheme))
		{
			if (error)
				*error = "unknown binding scheme '" + std::string(schemeName) + "'";
			return false;
		}

		const std::string_view rest = source.substr(colon + 1);
		if (rest.empty())
		{
			if (error)
				*error = "empty binding source after '" + std::string(schemeName) + ":'";
			return false;
		}

		UiBindingSource parsed;
		parsed.Scheme = scheme;
		parsed.Text.assign(rest);

		switch (scheme)
		{
		case UiBindingScheme::Ecs:
		{
			std::vector<std::string> parts;
			if (!SplitNonEmpty(rest, '/', parts, error, "ecs source"))
				return false;
			if (parts.size() != 3 && parts.size() != 2)
			{
				if (error)
					*error = "ecs source expects '<Entity>/<Component>/<Field>' or '<Component>/<Field>'";
				return false;
			}
			parsed.Parts = std::move(parts);
			break;
		}
		case UiBindingScheme::Script:
		{
			const std::size_t dot = rest.rfind('.');
			if (dot == std::string_view::npos || dot == 0 || dot + 1 >= rest.size())
			{
				if (error)
					*error = "script source expects '<path>.<field>'";
				return false;
			}
			parsed.Parts.push_back(std::string(rest.substr(0, dot)));
			parsed.Parts.push_back(std::string(rest.substr(dot + 1)));
			break;
		}
		default:   // Const / Service:整段原文即唯一段
			parsed.Parts.emplace_back(rest);
			break;
		}

		out = std::move(parsed);
		return true;
	}

	bool UiBinding::RegisterResolver(std::string_view scheme, UiBindingResolverFn fn)
	{
		if (scheme.empty() || fn == nullptr)
			return false;
		EnsureBuiltinResolvers();
		std::vector<ResolverEntry>& table = ResolverTable();
		for (ResolverEntry& entry : table)
		{
			if (entry.first == scheme)
			{
				entry.second = fn;
				return true;
			}
		}
		table.push_back(ResolverEntry { std::string(scheme), fn });
		return true;
	}

	UiBindingResolverFn UiBinding::FindResolver(std::string_view scheme)
	{
		EnsureBuiltinResolvers();
		for (const ResolverEntry& entry : ResolverTable())
		{
			if (entry.first == scheme)
				return entry.second;
		}
		return nullptr;
	}

	// ---- 扁平绑定表 ----

	void UiBindingTable::Attach(const UiScreen& screen)
	{
		m_Entries.clear();
		m_Values.clear();
		m_Resolved.clear();
		m_Warnings.clear();
		m_Version = 0;
		m_HasVersion = false;

		const auto warn = [this](const std::string& nodeId, const std::string& target,
			const std::string& source, std::string message) {
			m_Warnings.push_back(UiBindingWarning { nodeId, target, source, std::move(message) });
		};

		for (const UiNodeInstance& node : screen.Nodes())
		{
			if (node.Source == nullptr)
				continue;
			for (const UiBindingDecl& decl : node.Source->Bind)
			{
				if (decl.Target.empty())
				{
					warn(node.Id, decl.Target, decl.Source, "binding target must not be empty");
					continue;
				}
				UiBindingSource parsed;
				std::string error;
				if (!UiBinding::Parse(decl.Source, parsed, &error))
				{
					warn(node.Id, decl.Target, decl.Source, std::move(error));
					continue;
				}
				UiBinding entry;
				entry.NodeId = node.Id;
				entry.Target = decl.Target;
				entry.Source = std::move(parsed);
				m_Entries.push_back(std::move(entry));
				m_Values.emplace_back();
				m_Resolved.push_back(0);
			}
		}
	}

	std::size_t UiBindingTable::Refresh(const UiBindingDataSource& dataSource)
	{
		if (m_HasVersion && dataSource.Version == m_Version)
			return 0;   // 版本未变:不重复求值

		m_HasVersion = true;
		m_Version = dataSource.Version;
		m_Warnings.clear();

		const auto warn = [this](const UiBinding& entry, std::string message) {
			m_Warnings.push_back(UiBindingWarning {
				entry.NodeId, entry.Target, entry.Source.Text, std::move(message) });
		};

		std::size_t evaluated = 0;
		for (std::size_t i = 0; i < m_Entries.size(); ++i)
		{
			const UiBinding& entry = m_Entries[i];
			const UiBindingResolverFn resolver = UiBinding::FindResolver(UiBindingSchemeName(entry.Source.Scheme));
			if (resolver == nullptr)
			{
				warn(entry, std::string("no resolver registered for scheme '") +
					UiBindingSchemeName(entry.Source.Scheme) + "'");
				continue;   // 保留上一值,不影响绘制
			}
			std::string value;
			std::string error;
			if (resolver(entry.Source, dataSource.Context, value, &error))
			{
				m_Values[i] = std::move(value);
				m_Resolved[i] = 1;
				++evaluated;
			}
			else
			{
				warn(entry, error.empty() ? std::string("resolver failed") : ("resolver failed: " + error));
			}
		}
		return evaluated;
	}

	bool UiBindingTable::Value(std::string_view nodeId, std::string_view target, std::string& out) const
	{
		for (std::size_t i = 0; i < m_Entries.size(); ++i)
		{
			if (m_Entries[i].NodeId == nodeId && m_Entries[i].Target == target)
			{
				if (i >= m_Resolved.size() || m_Resolved[i] == 0)
					return false;
				out = m_Values[i];
				return true;
			}
		}
		return false;
	}

	bool UiBindingTable::HasValue(std::string_view nodeId, std::string_view target) const
	{
		std::string ignored;
		return Value(nodeId, target, ignored);
	}

	void UiBindingTable::Reset()
	{
		m_Entries.clear();
		m_Values.clear();
		m_Resolved.clear();
		m_Warnings.clear();
		m_Version = 0;
		m_HasVersion = false;
	}
}
