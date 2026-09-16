#include "wldpch.h"

#include "../EditorLayer.h"

#include "World/Core/Log.h"
#include "World/Renderer/Renderer.h"
#include "World/Scene/Components.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiScriptedInput.h"

#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace World
{
	namespace
	{
		std::string JoinLines(const std::vector<std::string>& lines)
		{
			std::ostringstream out;
			for (size_t i = 0; i < lines.size(); ++i)
			{
				if (i)
					out << "\n";
				out << lines[i];
			}
			return out.str();
		}

		// JSON 文本转义:面板 id / 材质路径 / 场景标签都可能含 '"' 或 '\'(Windows 路径),
		// 不转义会让 state.dump 变成非法 JSON(脚本侧 json.loads 直接失败)。
		std::string JsonEscape(const std::string& text)
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
					default: escaped += c; break;
				}
			}
			return escaped;
		}

		bool IsUnder(const std::filesystem::path& child, const std::filesystem::path& parent)
		{
			std::error_code ec;
			const std::filesystem::path normalizedChild = std::filesystem::weakly_canonical(child, ec);
			const std::filesystem::path normalizedParent = std::filesystem::weakly_canonical(parent, ec);
			if (normalizedChild.empty() || normalizedParent.empty())
				return false;
			const auto mismatch = std::mismatch(normalizedParent.begin(), normalizedParent.end(),
				normalizedChild.begin(), normalizedChild.end());
			return mismatch.first == normalizedParent.end();
		}

		// 写文件类命令的路径白名单:相对路径落在构建输出目录,绝对路径必须位于
		// 临时目录或当前工作目录之内 —— 控制通道不接受"写到系统任意位置"。
		std::filesystem::path ResolveCapturePath(const std::string& raw, std::string* error)
		{
			if (raw.empty())
			{
				if (error)
					*error = "capture path is empty";
				return {};
			}
			std::error_code ec;
			std::filesystem::path path(raw);
			if (path.is_absolute())
			{
				const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
				const std::filesystem::path cwd = std::filesystem::current_path(ec);
				if (IsUnder(path, temp) || IsUnder(path, cwd))
					return path;
				if (error)
					*error = "absolute capture paths must live under the temp dir or the working dir";
				return {};
			}
			return std::filesystem::path(WLD_OUTPUT_DIR) / path;
		}
	}

	std::string EditorLayer::DescribeAiScene() const
	{
		std::ostringstream out;
		out << "{";
		out << "\"backend\":\"" << Renderer::GetBackendName() << "\"";
		out << ",\"window\":[" << Application::Get().GetWindow().GetWidth() << ","
			<< Application::Get().GetWindow().GetHeight() << "]";
		const std::filesystem::path scenePath = m_Document.GetPath();
		out << ",\"scene\":\"" << JsonEscape(scenePath.generic_string()) << "\"";
		out << ",\"dirty\":" << (m_Document.IsDirty() ? "true" : "false");
		out << ",\"playState\":" << static_cast<int>(m_SceneState)
			<< ",\"paused\":" << (m_ScenePaused ? "true" : "false");
		out << ",\"entities\":[";
		bool first = true;
		if (m_ActiveScene)
		{
			auto& registry = m_ActiveScene->GetRegistry();
			for (auto handle : registry.view<TagComponent>())
			{
				if (!first)
					out << ",";
				first = false;
				const std::string& tag = registry.get<TagComponent>(handle).Tag;
				const uint32_t id = static_cast<uint32_t>(handle);
				out << "{\"handle\":" << id << ",\"name\":\"" << JsonEscape(tag) << "\"";
				if (handle == static_cast<entt::entity>(m_SelectedEntity))
					out << ",\"selected\":true";
				out << "}";
			}
		}
		// 必须是合法 JSON:之前写成 `"renderer":960x282`(裸的 960x282 不是 JSON 值),
		// 脚本侧 json.loads 直接失败。改成数组。
		out << "],\"renderer\":[" << m_SceneRenderer->GetWidth() << "," << m_SceneRenderer->GetHeight() << "]";
		out << ",\"panels\":" << m_Shell.AiDescribeState();
		out << "}";
		return out.str();
	}

	bool EditorLayer::ExecuteAiCommand(const std::string& cmd, const std::map<std::string, std::string>& args,
		std::string& result, std::string& error)
	{
		auto arg = [&](const char* key, const std::string& fallback = std::string()) -> std::string
		{
			const auto found = args.find(key);
			return found == args.end() ? fallback : found->second;
		};
		// 面板 id 便捷写法:materials/xxx.wmat → material:materials/xxx.wmat。
		auto normalizePanel = [](std::string panel)
		{
			if (!panel.empty() && panel.rfind("material:", 0) != 0 && panel.find(".wmat") != std::string::npos)
				panel = "material:" + panel;
			return panel;
		};

		if (cmd == "hello")
		{
			std::ostringstream out;
			out << "{\"app\":\"WorldEngine.Editor\",\"backend\":\"" << Renderer::GetBackendName() << "\""
				<< ",\"window\":[" << Application::Get().GetWindow().GetWidth() << ","
				<< Application::Get().GetWindow().GetHeight() << "]"
				<< ",\"ai\":" << (m_AiServer ? "true" : "false") << "}";
			result = out.str();
			return true;
		}
		if (cmd == "state.dump")
		{
			result = DescribeAiScene();
			return true;
		}
		if (cmd == "log.tail")
		{
			const size_t count = args.count("count") ? static_cast<size_t>(std::strtoul(arg("count").c_str(), nullptr, 10)) : 40;
			result = JoinLines(Log::RecentLines(count ? count : 40));
			return true;
		}
		if (cmd == "ops.tail")
		{
			const size_t count = args.count("count") ? static_cast<size_t>(std::strtoul(arg("count").c_str(), nullptr, 10)) : 40;
			const std::vector<Wui::WuiOpRecord>& records = m_WuiContext.Ops().Records();
			const size_t begin = records.size() > count ? records.size() - count : 0;
			std::vector<std::string> lines;
			for (size_t i = begin; i < records.size(); ++i)
			{
				const Wui::WuiOpRecord& record = records[i];
				lines.push_back("#" + std::to_string(record.Seq) + " f" + std::to_string(record.Frame) + " "
					+ record.Category + "/" + record.Action + " " + record.Target + " " + record.Detail);
			}
			result = JoinLines(lines);
			return true;
		}
		if (cmd == "ui.tree")
		{
			result = Wui::WuiAccessibility::Get().Serialize();
			return true;
		}
		if (cmd == "ui.invoke")
		{
			const Wui::WuiAccessibility& tree = Wui::WuiAccessibility::Get();
			const Wui::WuiAccessNode* node = nullptr;
			if (!arg("id").empty())
				node = tree.Find(static_cast<Wui::WuiId>(std::strtoull(arg("id").c_str(), nullptr, 10)));
			else
				node = tree.FindByLabel(arg("label"), arg("kind"));
			if (!node)
			{
				error = "no matching ui node (id/label); call ui.tree first";
				return false;
			}
			if (!node->Interactive || !node->Enabled)
			{
				error = node->Visible
					? ("ui node is not interactive/enabled: " + node->Kind + " '" + node->Label + "'")
					: ("ui node is off-screen (中心点不在窗口客户区内,窗口太小): " + node->Kind
						+ " '" + node->Label + "' — 先 ui.resize 放大窗口");
				return false;
			}
			const glm::vec2 center { node->Rect.X + node->Rect.W * 0.5f, node->Rect.Y + node->Rect.H * 0.5f };
			Wui::WuiScriptedInput::Get().QueueClick(node->Window, center);
			result = "queued " + node->Kind + " '" + node->Label + "' at ("
				+ std::to_string(static_cast<int>(center.x)) + "," + std::to_string(static_cast<int>(center.y))
				+ ") window=" + node->Window;
			return true;
		}
		if (cmd == "ui.open" || cmd == "ui.toggle" || cmd == "ui.close")
		{
			const std::string panel = normalizePanel(arg("panel"));
			if (panel.empty())
			{
				error = "missing panel";
				return false;
			}
			if (!m_Shell.AiTogglePanel(panel))
			{
				error = "cannot toggle panel '" + panel + "' (undeclared or no UI context yet)";
				return false;
			}
			result = "toggled " + panel;
			return true;
		}
		if (cmd == "ui.focus")
		{
			const std::string panel = normalizePanel(arg("panel"));
			m_Shell.FocusIndependentWindow(panel);
			result = "focused " + panel;
			return true;
		}
		if (cmd == "ui.resize")
		{
			const std::string panel = normalizePanel(arg("panel"));
			const float width = static_cast<float>(std::atof(arg("width", "0").c_str()));
			const float height = static_cast<float>(std::atof(arg("height", "0").c_str()));
			if (panel.empty() || width <= 0.0f || height <= 0.0f)
			{
				error = "ui.resize needs panel, width, height";
				return false;
			}
			if (!m_Shell.AiResizeWindow(panel, width, height))
			{
				error = "no visible independent window for panel '" + panel + "'";
				return false;
			}
			result = "resized " + panel + " to " + std::to_string(static_cast<int>(width)) + "x"
				+ std::to_string(static_cast<int>(height));
			return true;
		}
		if (cmd == "ui.layout.get")
		{
			result = m_Shell.AiLayoutJson();
			return true;
		}
		if (cmd == "capture.screen")
		{
			std::string pathError;
			const std::filesystem::path path = ResolveCapturePath(arg("path"), &pathError);
			if (path.empty())
			{
				error = pathError;
				return false;
			}
			Renderer::CaptureFrame(path);
			result = path.string();
			return true;
		}
		if (cmd == "capture.scene")
		{
			std::string pathError;
			const std::filesystem::path path = ResolveCapturePath(arg("path"), &pathError);
			if (path.empty())
			{
				error = pathError;
				return false;
			}
			if (!m_SceneRenderer)
			{
				error = "no scene renderer";
				return false;
			}
			m_SceneRenderer->CaptureFrame(path);
			result = path.string();
			return true;
		}
		if (cmd == "capture.float")
		{
			std::string pathError;
			const std::filesystem::path path = ResolveCapturePath(arg("path"), &pathError);
			if (path.empty())
			{
				error = pathError;
				return false;
			}
			const std::string panel = normalizePanel(arg("panel"));
			if (!m_Shell.AiRequestFloatCapture(panel, path.string()))
			{
				error = "no visible independent window for panel '" + panel + "'";
				return false;
			}
			result = "queued (written next frame): " + path.string();
			return true;
		}
		if (cmd == "capture.texture")
		{
			std::string pathError;
			const std::filesystem::path path = ResolveCapturePath(arg("path"), &pathError);
			if (path.empty())
			{
				error = pathError;
				return false;
			}
			const std::string kind = arg("kind", "material-preview");
			if (kind != "material-preview")
			{
				error = "unsupported texture kind '" + kind + "' (supported: material-preview)";
				return false;
			}
			const std::string panel = normalizePanel(arg("panel"));
			if (panel.empty())
			{
				error = "missing panel (e.g. material:materials/glass_red.wmat)";
				return false;
			}
			if (!m_Shell.AiRequestPreviewCapture(panel, path.string()))
			{
				error = "panel '" + panel + "' has no material preview";
				return false;
			}
			result = "queued (written next frame): " + path.string();
			return true;
		}
		if (cmd == "quit")
		{
			result = "closing";
			Application::Get().Close();
			return true;
		}
		error = "unknown command '" + cmd + "'";
		return false;
	}
}
