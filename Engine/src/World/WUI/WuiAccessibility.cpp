#include "wldpch.h"
#include "WuiAccessibility.h"

#include <algorithm>
#include <sstream>

namespace World::Wui
{
	namespace
	{
		std::string EscapeJson(const std::string& text)
		{
			std::string escaped;
			escaped.reserve(text.size() + 8);
			for (char c : text)
			{
				switch (c)
				{
					case '"': escaped += "\\\""; break;
					case '\\': escaped += "\\\\"; break;
					case '\n': escaped += "\\n"; break;
					case '\r': escaped += "\\r"; break;
					case '\t': escaped += "\\t"; break;
					default:
						if (static_cast<unsigned char>(c) < 0x20)
						{
							char buffer[8] = {};
							std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
							escaped += buffer;
						}
						else
							escaped += c;
						break;
				}
			}
			return escaped;
		}

		std::string Number(float value)
		{
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.2f", value);
			return buffer;
		}
	}

	WuiAccessibility& WuiAccessibility::Get()
	{
		static WuiAccessibility instance;
		return instance;
	}

	void WuiAccessibility::BeginFrame(const std::string& windowKey, glm::vec2 clientSize)
	{
		if (!m_Enabled)
			return;
		m_Window = windowKey;
		m_Panel.clear();
		m_ClientSize = clientSize;
		m_Nodes.erase(std::remove_if(m_Nodes.begin(), m_Nodes.end(),
			[&](const WuiAccessNode& node) { return node.Window == windowKey; }), m_Nodes.end());
	}

	void WuiAccessibility::SetPanel(const std::string& panelId)
	{
		m_Panel = panelId;
	}

	void WuiAccessibility::ClearWindow(const std::string& windowKey)
	{
		m_Nodes.erase(std::remove_if(m_Nodes.begin(), m_Nodes.end(),
			[&](const WuiAccessNode& node) { return node.Window == windowKey; }), m_Nodes.end());
	}

	void WuiAccessibility::Register(const WuiAccessNode& node)
	{
		if (!m_Enabled || node.Id == 0)
			return;
		WuiAccessNode entry = node;
		if (m_ClientSize.x > 0.0f && m_ClientSize.y > 0.0f)
		{
			// 中心点必须在客户区内:注入的点击永远落在用户能看到的位置。
			const float centerX = entry.Rect.X + entry.Rect.W * 0.5f;
			const float centerY = entry.Rect.Y + entry.Rect.H * 0.5f;
			entry.Visible = centerX >= 0.0f && centerY >= 0.0f
				&& centerX <= m_ClientSize.x && centerY <= m_ClientSize.y;
		}
		if (!entry.Visible)
			entry.Interactive = false;
		// 同一控件在同一帧内可能被登记两次(例如菜单同时作为弹出项绘制),
		// 以最后一次为准,避免 ui.invoke 命中陈旧矩形。
		auto existing = std::find_if(m_Nodes.begin(), m_Nodes.end(),
			[&](const WuiAccessNode& candidate)
			{ return candidate.Id == entry.Id && candidate.Window == entry.Window; });
		if (existing != m_Nodes.end())
			*existing = entry;
		else
			m_Nodes.push_back(entry);
	}

	const WuiAccessNode* WuiAccessibility::Find(WuiId id) const
	{
		for (const WuiAccessNode& node : m_Nodes)
			if (node.Id == id)
				return &node;
		return nullptr;
	}

	const WuiAccessNode* WuiAccessibility::FindByLabel(const std::string& label, const std::string& kind) const
	{
		// 允许 "kind:label" 形式,便于脚本无歧义地指定(例如 "button:Save")。
		std::string wantedKind = kind;
		std::string wantedLabel = label;
		if (wantedKind.empty())
		{
			const size_t colon = label.find(':');
			if (colon != std::string::npos)
			{
				wantedKind = label.substr(0, colon);
				wantedLabel = label.substr(colon + 1);
			}
		}
		for (const WuiAccessNode& node : m_Nodes)
		{
			if (!wantedKind.empty() && node.Kind != wantedKind)
				continue;
			if (node.Label == wantedLabel)
				return &node;
		}
		return nullptr;
	}

	void WuiAccessibility::Clear()
	{
		m_Nodes.clear();
		m_Panel.clear();
	}

	std::string WuiAccessibility::Serialize() const
	{
		std::ostringstream out;
		out << "[";
		for (size_t i = 0; i < m_Nodes.size(); ++i)
		{
			const WuiAccessNode& node = m_Nodes[i];
			if (i)
				out << ",";
			out << "{\"id\":" << node.Id
				<< ",\"window\":\"" << EscapeJson(node.Window) << "\""
				<< ",\"panel\":\"" << EscapeJson(node.Panel) << "\""
				<< ",\"kind\":\"" << EscapeJson(node.Kind) << "\""
				<< ",\"label\":\"" << EscapeJson(node.Label) << "\""
				<< ",\"value\":\"" << EscapeJson(node.Value) << "\""
				<< ",\"rect\":[" << Number(node.Rect.X) << "," << Number(node.Rect.Y) << ","
				<< Number(node.Rect.W) << "," << Number(node.Rect.H) << "]"
				<< ",\"enabled\":" << (node.Enabled ? "true" : "false")
				<< ",\"focused\":" << (node.Focused ? "true" : "false")
				<< ",\"visible\":" << (node.Visible ? "true" : "false")
				<< ",\"interactive\":" << (node.Interactive ? "true" : "false")
				<< ",\"tooltip\":\"" << EscapeJson(node.Tooltip) << "\""
				<< "}";
		}
		out << "]";
		return out.str();
	}
}
