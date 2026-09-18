#include "wldpch.h"

#include "../EditorLayer.h"

#include "World/Core/Log.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Scene/Components.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiScriptedInput.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

		// P2 W5b:脚本实例状态名(script.status/script.reload 的 JSON 字段)。
		const char* ScriptStateLabel(ScriptInstanceState state)
		{
			switch (state)
			{
				case ScriptInstanceState::Pending: return "Pending";
				case ScriptInstanceState::Creating: return "Creating";
				case ScriptInstanceState::Running: return "Running";
				case ScriptInstanceState::Destroying: return "Destroying";
				case ScriptInstanceState::Stopped: return "Stopped";
				case ScriptInstanceState::Faulted: return "Faulted";
				default: return "?";
			}
		}

		// ui.type 的返回文案用"字符数":统计 UTF-8 码点数(中文按 1 个字符),与控件
		// 收到的 TextInput 数量一致。
		size_t Utf8CodepointCount(const std::string& text)
		{
			size_t count = 0;
			for (char byte : text)
				if ((static_cast<unsigned char>(byte) & 0xC0u) != 0x80u)
					++count;
			return count;
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

		bool ParseFloat3(const std::string& text, glm::vec3* out)
		{
			std::stringstream stream(text);
			std::string part;
			glm::vec3 value { 0.0f };
			int index = 0;
			while (std::getline(stream, part, ',') && index < 3)
				value[index++] = std::strtof(part.c_str(), nullptr);
			if (index < 3)
				return false;
			*out = value;
			return true;
		}
	}

	void EditorLayer::StartAiRecording(const std::string& path)
	{
		m_AiRecordPath = path;
		m_AiRecordCount = 0;
		// 覆盖式开始:录制文件代表"从现在起的会话"。
		std::ofstream file(path, std::ios::trunc);
	}

	size_t EditorLayer::StopAiRecording()
	{
		const size_t count = m_AiRecordCount;
		m_AiRecordPath.clear();
		m_AiRecordCount = 0;
		return count;
	}

	void EditorLayer::AiRecordCommand(const std::string& cmd, const std::map<std::string, std::string>& args)
	{
		if (m_AiRecordPath.empty())
			return;
		// 录制/回放/退出类命令不进脚本:回放时会跳过它们,写进去只会让脚本难以阅读。
		if (cmd == "record.start" || cmd == "record.stop" || cmd == "replay" || cmd == "quit")
			return;
		std::ofstream file(m_AiRecordPath, std::ios::app);
		if (!file)
		{
			WLD_CORE_WARN("[ai] cannot append to record file '{0}'", m_AiRecordPath);
			m_AiRecordPath.clear();
			return;
		}
		file << "{\"cmd\":\"" << cmd << "\"";
		for (const auto& entry : args)
		{
			if (entry.first == "seq" || entry.first == "cmd")
				continue;
			file << ",\"" << entry.first << "\":\"" << entry.second << "\"";
		}
		file << "}\n";
		++m_AiRecordCount;
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
			// 只读枚举必须走 const 视图:Play/Simulate 下活动场景的**非 const** GetRegistry()
			// 会触发"活动场景禁止结构写"断言(实测会让异常逃逸并终止进程)。
			const Scene& scene = *m_ActiveScene;
			const entt::registry& registry = scene.GetRegistry();
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
		// ui.invoke / ui.type 共用的无障碍节点解析(id 优先,其次 label(+kind));
		// 失败时把与 ui.invoke 逐字相同的错误文案写进 message。
		auto resolveNode = [&](std::string& message) -> const Wui::WuiAccessNode*
		{
			const Wui::WuiAccessibility& tree = Wui::WuiAccessibility::Get();
			const Wui::WuiAccessNode* node = nullptr;
			if (!arg("id").empty())
				node = tree.Find(static_cast<Wui::WuiId>(std::strtoull(arg("id").c_str(), nullptr, 10)));
			else
				node = tree.FindByLabel(arg("label"), arg("kind"));
			if (!node)
			{
				message = "no matching ui node (id/label); call ui.tree first";
				return nullptr;
			}
			if (!node->Interactive || !node->Enabled)
			{
				message = node->Visible
					? ("ui node is not interactive/enabled: " + node->Kind + " '" + node->Label + "'")
					: ("ui node is off-screen (中心点不在窗口客户区内,窗口太小): " + node->Kind
						+ " '" + node->Label + "' — 先 ui.resize 放大窗口");
				return nullptr;
			}
			return node;
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
		if (cmd == "stats.scene")
		{
			// P1b D8a:场景渲染统计(剔除/提交规模/CPU 耗时),压力场景脚本与 Stats 面板同源。
			const Renderer3D::SceneStatistics scene = Renderer3D::GetSceneStatistics();
			std::ostringstream out;
			out << "{\"objects\":" << scene.Objects
				<< ",\"visible\":" << scene.Submitted
				<< ",\"culled\":" << scene.Culled
				<< ",\"shadowCasters\":" << scene.ShadowCasters
				<< ",\"drawCalls\":" << scene.DrawCalls
				<< ",\"triangles\":" << scene.Triangles
				<< ",\"droppedObjects\":" << scene.DroppedObjects
				<< ",\"culling\":" << (scene.CullingEnabled ? "true" : "false")
				<< ",\"sceneMs\":" << scene.SceneMilliseconds
				<< ",\"cullMs\":" << scene.CullMilliseconds
				<< ",\"fps\":" << m_WuiContext.Input().FPS << "}";
			result = out.str();
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
			std::string message;
			const Wui::WuiAccessNode* node = resolveNode(message);
			if (!node)
			{
				error = message;
				return false;
			}
			const glm::vec2 center { node->Rect.X + node->Rect.W * 0.5f, node->Rect.Y + node->Rect.H * 0.5f };
			Wui::WuiScriptedInput::Get().QueueClick(node->Window, center);
			result = "queued " + node->Kind + " '" + node->Label + "' at ("
				+ std::to_string(static_cast<int>(center.x)) + "," + std::to_string(static_cast<int>(center.y))
				+ ") window=" + node->Window;
			return true;
		}
		// ---- W9-3:向文本控件注入真实键入(点击聚焦 → 下一帧起逐帧写入字符)----
		if (cmd == "ui.type")
		{
			std::string message;
			const Wui::WuiAccessNode* node = resolveNode(message);
			if (!node)
			{
				error = message;
				return false;
			}
			if (node->Kind != "editor" && node->Kind != "text-field")
			{
				error = "node is not a text input: " + node->Kind;
				return false;
			}
			const std::string text = arg("text");
			if (text.empty())
			{
				error = "ui.type needs text";
				return false;
			}
			const glm::vec2 center { node->Rect.X + node->Rect.W * 0.5f, node->Rect.Y + node->Rect.H * 0.5f };
			// 聚焦点击复用既有 click 队列(与 ui.invoke 同一个节拍);文本挂在它后面:
			// 点击完成后的下一帧开始注入,走 WuiCodeEditor/TextField 的真实输入路径。
			Wui::WuiScriptedInput::Get().QueueClick(node->Window, center);
			Wui::WuiScriptedInput::Get().QueueType(node->Window, text);
			result = "queued type " + std::to_string(Utf8CodepointCount(text)) + " chars into "
				+ node->Kind + " '" + node->Label + "' window=" + node->Window;
			return true;
		}
		// ---- W9.8:注入一次按键(方向键/Enter/Tab/Esc 等),用于复现键路与自动化 ----
		if (cmd == "ui.key")
		{
			std::string message;
			const Wui::WuiAccessNode* node = resolveNode(message);
			if (!node)
			{
				error = message;
				return false;
			}
			const std::string keyName = arg("key");
			uint32_t keyCode = 0;
			if (keyName == "Up") keyCode = KeyCodes::Up;
			else if (keyName == "Down") keyCode = KeyCodes::Down;
			else if (keyName == "Left") keyCode = KeyCodes::Left;
			else if (keyName == "Right") keyCode = KeyCodes::Right;
			else if (keyName == "Home") keyCode = KeyCodes::Home;
			else if (keyName == "End") keyCode = KeyCodes::End;
			else if (keyName == "PageUp") keyCode = KeyCodes::PageUp;
			else if (keyName == "PageDown") keyCode = KeyCodes::PageDown;
			else if (keyName == "Enter") keyCode = KeyCodes::Enter;
			else if (keyName == "Tab") keyCode = KeyCodes::Tab;
			else if (keyName == "Escape") keyCode = KeyCodes::Escape;
			else if (keyName == "Backspace") keyCode = KeyCodes::Backspace;
			else if (keyName == "Delete") keyCode = KeyCodes::Delete;
			else if (keyName == "Space") keyCode = KeyCodes::Space;
			else if (keyName.size() == 1 && keyName[0] >= 'A' && keyName[0] <= 'Z')
				keyCode = static_cast<uint32_t>(KeyCodes::A) + static_cast<uint32_t>(keyName[0] - 'A');
			else if (keyName.size() == 1 && keyName[0] >= '0' && keyName[0] <= '9')
				keyCode = static_cast<uint32_t>(KeyCodes::D0) + static_cast<uint32_t>(keyName[0] - '0');
			else
			{
				error = "unknown key: " + keyName + " (Up/Down/Left/Right/Home/End/PageUp/PageDown/Enter/Tab/Escape/Backspace/Delete/Space/A-Z/0-9)";
				return false;
			}
			Wui::WuiScriptedInput::Get().QueueKey(node->Window, keyCode);
			result = "queued key " + keyName + " to " + node->Kind + " '" + node->Label
				+ "' window=" + node->Window;
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
			// 只登记请求:引擎会在"UI 通道已提交、EndFramePresent 之前"执行抓取
			// (见 Renderer::FlushPresentCaptures 的说明 —— 提前抓会拍到空白并破坏后续布局)。
			Renderer::RequestPresentCapture(nullptr, path,
				Application::Get().GetWindow().GetWidth(), Application::Get().GetWindow().GetHeight());
			result = "queued (written at end of frame): " + path.string();
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
		// ---- 场景(实体树/选中/属性/Play) ----
		if (cmd == "scene.list")
		{
			std::ostringstream out;
			out << "[";
			bool first = true;
			if (m_ActiveScene)
			{
				const Scene& scene = *m_ActiveScene;
				const entt::registry& registry = scene.GetRegistry();
				for (auto handle : registry.view<TagComponent>())
				{
					if (!first)
						out << ",";
					first = false;
					out << "{\"handle\":" << static_cast<uint32_t>(handle)
						<< ",\"name\":\"" << JsonEscape(registry.get<TagComponent>(handle).Tag) << "\""
						<< ",\"selected\":" << (handle == static_cast<entt::entity>(m_SelectedEntity) ? "true" : "false")
						<< "}";
				}
			}
			out << "]";
			result = out.str();
			return true;
		}
		if (cmd == "scene.select")
		{
			if (!m_ActiveScene)
			{
				error = "no active scene";
				return false;
			}
			Entity target;
			if (!arg("handle").empty())
			{
				target = Entity(m_ActiveScene.get(),
					static_cast<entt::entity>(std::strtoul(arg("handle").c_str(), nullptr, 10)));
			}
			else if (!arg("name").empty())
			{
				auto& registry = m_ActiveScene->GetRegistry();
				for (auto handle : registry.view<TagComponent>())
					if (registry.get<TagComponent>(handle).Tag == arg("name"))
					{
						target = Entity(m_ActiveScene.get(), handle);
						break;
					}
			}
			if (!target.IsValid())
			{
				error = "entity not found (need handle=<id> or name=<tag>); call scene.list first";
				return false;
			}
			m_Shell.SetSelectedEntity(target);
			m_SelectedEntity = target;
			result = "selected handle=" + std::to_string(static_cast<uint32_t>(static_cast<entt::entity>(target)));
			return true;
		}
		if (cmd == "scene.get")
		{
			if (!m_ActiveScene)
			{
				error = "no active scene";
				return false;
			}
			Entity target = m_SelectedEntity;
			if (!arg("handle").empty())
				target = Entity(m_ActiveScene.get(),
					static_cast<entt::entity>(std::strtoul(arg("handle").c_str(), nullptr, 10)));
			else if (!arg("name").empty())
			{
				target = Entity {};
				const Scene& scene = *m_ActiveScene;
				const entt::registry& registry = scene.GetRegistry();
				for (auto handle : registry.view<TagComponent>())
					if (registry.get<TagComponent>(handle).Tag == arg("name"))
					{
						target = Entity(m_ActiveScene.get(), handle);
						break;
					}
			}
			if (!target.IsValid())
			{
				error = "entity not found (need handle=/name=, or select one first)";
				return false;
			}
			const Scene& sceneRef = *m_ActiveScene;
			const entt::registry& registry = sceneRef.GetRegistry();
			const entt::entity handle = static_cast<entt::entity>(target);
			std::ostringstream out;
			out << "{\"handle\":" << static_cast<uint32_t>(handle);
			if (const auto* tag = registry.try_get<TagComponent>(handle))
				out << ",\"name\":\"" << JsonEscape(tag->Tag) << "\"";
			if (const auto* transform = registry.try_get<TransformComponent>(handle))
			{
				const glm::vec3 location = transform->Location;
				const glm::vec3 rotation = transform->Rotation;
				const glm::vec3 scale = transform->Scale;
				out << ",\"location\":[" << location.x << "," << location.y << "," << location.z << "]"
					<< ",\"rotation\":[" << rotation.x << "," << rotation.y << "," << rotation.z << "]"
					<< ",\"scale\":[" << scale.x << "," << scale.y << "," << scale.z << "]";
			}
			if (const auto* mesh = registry.try_get<MeshRendererComponent>(handle))
				out << ",\"primitive\":\"" << JsonEscape(mesh->Primitive) << "\""
					<< ",\"material\":\"" << JsonEscape(mesh->MaterialPath) << "\""
					<< ",\"color\":[" << mesh->Color.r << "," << mesh->Color.g << ","
					<< mesh->Color.b << "," << mesh->Color.a << "]";
			out << "}";
			result = out.str();
			return true;
		}
		if (cmd == "scene.set")
		{
			if (!m_ActiveScene)
			{
				error = "no active scene";
				return false;
			}
			// Play/Simulate 是只读查看(与属性面板同一条规则):活动场景的结构写必须走命令提交,
			// 编辑器的 AI 通道不越权改写。
			if (m_SceneState != SceneState::Edit)
			{
				error = "scene is read-only in Play/Simulate; exit Play first";
				return false;
			}
			Entity target = m_SelectedEntity;
			if (!arg("handle").empty())
				target = Entity(m_ActiveScene.get(),
					static_cast<entt::entity>(std::strtoul(arg("handle").c_str(), nullptr, 10)));
			else if (!arg("name").empty())
			{
				auto& registry = m_ActiveScene->GetRegistry();
				target = Entity {};
				for (auto handle : registry.view<TagComponent>())
					if (registry.get<TagComponent>(handle).Tag == arg("name"))
					{
						target = Entity(m_ActiveScene.get(), handle);
						break;
					}
			}
			if (!target.IsValid())
			{
				error = "entity not found (need handle=/name=, or select one first)";
				return false;
			}
			const std::string property = arg("property");
			const std::string value = arg("value");
			auto& registry = m_ActiveScene->GetRegistry();
			const entt::entity handle = static_cast<entt::entity>(target);
			if (property == "Tag")
			{
				if (auto* tag = registry.try_get<TagComponent>(handle))
				{
					tag->Tag = value;
					MarkDocumentDirty();
					result = "Tag='" + value + "'";
					return true;
				}
			}
			if (auto* transform = registry.try_get<TransformComponent>(handle))
			{
				glm::vec3 vector { 0.0f };
				if (property == "Location" || property == "Rotation" || property == "Scale")
				{
					if (!ParseFloat3(value, &vector))
					{
						error = "value must be 'x,y,z'";
						return false;
					}
					if (property == "Location")
						transform->SetLocation(vector);
					else if (property == "Rotation")
						transform->SetRotation(vector);
					else
						transform->SetScale(vector);
					MarkDocumentDirty();
					result = property + "=(" + value + ")";
					return true;
				}
			}
			if (auto* mesh = registry.try_get<MeshRendererComponent>(handle))
			{
				if (property == "Material")
				{
					mesh->MaterialPath = value;
					MarkDocumentDirty();
					result = "Material='" + value + "'";
					return true;
				}
				if (property == "Primitive")
				{
					mesh->Primitive = value;
					MarkDocumentDirty();
					result = "Primitive='" + value + "'";
					return true;
				}
				if (property == "Color")
				{
					glm::vec3 rgb { 1.0f };
					if (!ParseFloat3(value, &rgb))
					{
						error = "value must be 'r,g,b'";
						return false;
					}
					mesh->Color = glm::vec4 { rgb, 1.0f };
					MarkDocumentDirty();
					result = "Color=(" + value + ")";
					return true;
				}
			}
			error = "unsupported property '" + property + "' (Tag/Location/Rotation/Scale/Material/Primitive/Color)";
			return false;
		}
		if (cmd == "scene.open")
		{
			const std::string path = arg("path");
			if (path.empty())
			{
				error = "missing path";
				return false;
			}
			OpenScene(std::filesystem::path(path));
			result = "opened " + path;
			return true;
		}
		if (cmd == "scene.save")
		{
			// 只允许写到白名单路径(默认沿用当前文档路径)。
			if (arg("path").empty())
			{
				if (!SaveScene())
				{
					error = "save failed (no document path?)";
					return false;
				}
				result = "saved current document";
				return true;
			}
			std::string pathError;
			const std::filesystem::path path = ResolveCapturePath(arg("path"), &pathError);
			if (path.empty())
			{
				error = pathError;
				return false;
			}
			if (!m_Document.SaveTo(path))
			{
				error = "save failed: " + m_Document.GetLastError();
				return false;
			}
			result = "saved " + path.string();
			return true;
		}
		if (cmd == "play.enter" || cmd == "play.exit" || cmd == "play.pause" || cmd == "play.resume")
		{
			const bool playing = m_SceneState == SceneState::Play;
			if (cmd == "play.enter" && !playing)
				m_Shell.TogglePlay();
			else if (cmd == "play.exit" && playing)
				m_Shell.TogglePlay();
			else if (cmd == "play.pause" && playing && !m_ScenePaused)
				m_Shell.TogglePause();
			else if (cmd == "play.resume" && playing && m_ScenePaused)
				m_Shell.TogglePause();
			result = "playState=" + std::to_string(static_cast<int>(m_SceneState))
				+ " paused=" + (m_ScenePaused ? "true" : "false");
			return true;
		}
		// ---- P2 W5b:脚本热重载 ----
		// 命令格式(字段全部扁平):
		//   script.status [handle=<id>|name=<tag>|path=<逻辑脚本路径>]   只读状态(缺省=当前选中实体)
		//   script.reload [handle=<id>|name=<tag>|path=<逻辑脚本路径>]   触发一次重载(缺省=当前选中实体)
		// reload 与帧边界轮询、属性面板的 Reload 按钮共用 EditorLayer::ReloadLuaScriptComponent:
		// Running 实例走 ScriptEngine::ReloadScript(失败保留旧版本),Faulted/未加载的实例复位成
		// Pending 交给 Scene 在下一帧重建。单个实例重载失败(语法错误等)不算命令失败:结果 JSON
		// 带着 state/lastError/reloadDiagnostic 供脚本断言。
		if (cmd == "script.status" || cmd == "script.reload")
		{
			if (!m_ActiveScene)
			{
				error = "no active scene";
				return false;
			}
			const bool reload = cmd == "script.reload";
			if (reload && !m_ActiveScene->CanApplyScriptReload())
			{
				error = "not at a script reload safe point (callback/structural commit/stopping); retry next frame";
				return false;
			}
			// 只读枚举必须走 const registry:Play/Simulate 下活动场景的非 const GetRegistry()
			// 会触发"活动场景禁止结构写"断言。
			const Scene& scene = *m_ActiveScene;
			const entt::registry& registry = scene.GetRegistry();
			std::vector<entt::entity> targets;
			if (!arg("handle").empty())
			{
				const entt::entity handle = static_cast<entt::entity>(std::strtoul(arg("handle").c_str(), nullptr, 10));
				if (!registry.valid(handle))
				{
					error = "entity not found: handle=" + arg("handle");
					return false;
				}
				if (!registry.all_of<LuaScriptComponent>(handle))
				{
					error = "entity has no LuaScriptComponent: handle=" + arg("handle");
					return false;
				}
				targets.push_back(handle);
			}
			else if (!arg("name").empty())
			{
				for (const entt::entity handle : registry.view<TagComponent>())
					if (registry.get<TagComponent>(handle).Tag == arg("name") &&
						registry.all_of<LuaScriptComponent>(handle))
						targets.push_back(handle);
				if (targets.empty())
				{
					error = "no entity with a LuaScriptComponent named '" + arg("name") + "'";
					return false;
				}
			}
			else if (!arg("path").empty())
			{
				for (const entt::entity handle : registry.view<LuaScriptComponent>())
					if (registry.get<LuaScriptComponent>(handle).ScriptFilePath == arg("path"))
						targets.push_back(handle);
				if (targets.empty())
				{
					error = "no Lua script component uses path=" + arg("path");
					return false;
				}
			}
			else
			{
				if (!m_SelectedEntity.IsValid() || m_SelectedEntity.GetScene() != m_ActiveScene.get())
				{
					error = "no entity selected; pass handle=/name=/path= (or select one first)";
					return false;
				}
				const entt::entity handle = static_cast<entt::entity>(m_SelectedEntity);
				if (!registry.all_of<LuaScriptComponent>(handle))
				{
					error = "selected entity has no LuaScriptComponent; pass handle=/name=/path=";
					return false;
				}
				targets.push_back(handle);
			}

			size_t succeeded = 0;
			size_t failed = 0;
			std::ostringstream out;
			out << "{\"command\":\"" << cmd << "\""
				<< ",\"playState\":" << static_cast<int>(m_SceneState)
				<< ",\"targets\":" << targets.size()
				<< ",\"components\":[";
			for (size_t index = 0; index < targets.size(); ++index)
			{
				const entt::entity handle = targets[index];
				Entity entity(m_ActiveScene.get(), handle);
				auto* script = static_cast<LuaScriptComponent*>(
					entity.GetComponent(entt::type_id<LuaScriptComponent>().hash()));
				if (index)
					out << ",";
				if (!script)
				{
					++failed;
					out << "{\"handle\":" << static_cast<uint32_t>(handle) << ",\"ok\":false"
						<< ",\"message\":\"component disappeared\"}";
					continue;
				}
				std::string message;
				bool ok = true;
				if (reload)
					ok = EditorLayer::ReloadLuaScriptComponent(*script, m_ActiveScene.get(), &message);
				if (ok)
					++succeeded;
				else
					++failed;
				out << "{\"handle\":" << static_cast<uint32_t>(handle)
					<< ",\"ok\":" << (ok ? "true" : "false")
					<< ",\"name\":\"" << JsonEscape(
						registry.all_of<TagComponent>(handle) ? registry.get<TagComponent>(handle).Tag : std::string())
					<< "\",\"path\":\"" << JsonEscape(script->ScriptFilePath)
					<< "\",\"state\":\"" << ScriptStateLabel(script->State)
					<< "\",\"loaded\":" << (script->IsLoaded ? "true" : "false")
					<< ",\"generation\":" << script->Generation
					<< ",\"reloadDiagnostic\":\"" << JsonEscape(script->ReloadDiagnostic)
					<< "\",\"lastError\":\"" << JsonEscape(script->LastError)
					<< "\",\"message\":\"" << JsonEscape(message) << "\"}";
			}
			out << "],\"succeeded\":" << succeeded << ",\"failed\":" << failed << "}";
			result = out.str();
			return true;
		}
		// ---- W9-2:内置脚本编辑器 ----
		// 打开脚本编辑器(逻辑路径;每个脚本一个 "script:<逻辑路径>" 面板)。
		// 与用户入口(内容浏览器双击 .lua/.luau、Scripts 面板"在引擎内打开")同一条
		// EditorShell::OpenScriptEditor 路径 —— 默认直接附加到主窗口。
		if (cmd == "script.open_editor")
		{
			const std::string path = arg("path");
			if (path.empty())
			{
				error = "script.open_editor needs path";
				return false;
			}
			m_Shell.OpenScriptEditor(path);
			result = "opened script editor for " + path;
			return true;
		}
		// ---- 资产 / 材质 ----
		if (cmd == "asset.open_material")
		{
			const std::string path = arg("path");
			if (path.empty())
			{
				error = "missing path";
				return false;
			}
			m_Shell.OpenMaterialEditor(path);
			result = "opened material editor for " + path;
			return true;
		}
		if (cmd == "asset.import_gltf")
		{
			// P1b D5:与"内容浏览器双击 .gltf"同一条路径(只导入,不改场景),
			// 供自动化验证编辑器内的导入交互;相对路径按内容根解析。
			std::string path = arg("path");
			if (path.empty())
			{
				error = "missing path";
				return false;
			}
			std::filesystem::path resolved(path);
			if (!resolved.is_absolute())
				resolved = std::filesystem::path(WLD_ASSETPATH) / resolved;
			std::string message;
			std::string logicalModel;
			if (!ImportModelFile(resolved.string(), &message, &logicalModel))
			{
				error = message.empty() ? "glTF import failed" : message;
				return false;
			}
			// 与内容浏览器双击同一条:导入后打开模型预览(只读,不改场景)。
			if (!logicalModel.empty())
				m_Shell.OpenModelPreview(logicalModel);
			result = "{\"wmodel\":\"" + JsonEscape(logicalModel) + "\",\"message\":\"" + JsonEscape(message) + "\"}";
			return true;
		}
		if (cmd == "asset.instance_model")
		{
			// P1b D5:与模型预览面板的"放进当前场景"同一条路径(仅编辑态)。
			const std::string path = arg("path");
			if (path.empty())
			{
				error = "missing path";
				return false;
			}
			std::string message;
			if (!InstantiateModelFile(path, &message))
			{
				error = message.empty() ? "instantiate model failed" : message;
				return false;
			}
			result = message;
			return true;
		}
		if (cmd == "material.get")
		{
			const std::string path = arg("path");
			const Ref<Material> material = path.empty() ? nullptr : MaterialLibrary::Get().Load(path);
			if (!material)
			{
				error = "material not found: " + path;
				return false;
			}
			const MaterialDesc& desc = material->GetDesc();
			std::ostringstream out;
			out << "{\"path\":\"" << JsonEscape(material->GetPath()) << "\""
				<< ",\"albedo\":\"" << JsonEscape(desc.AlbedoTexture) << "\""
				<< ",\"normal\":\"" << JsonEscape(desc.NormalTexture) << "\""
				<< ",\"baseColor\":[" << desc.BaseColor.r << "," << desc.BaseColor.g << ","
				<< desc.BaseColor.b << "," << desc.BaseColor.a << "]"
				<< ",\"metallic\":" << desc.Metallic << ",\"roughness\":" << desc.Roughness
				<< ",\"blendMode\":" << static_cast<int>(desc.BlendMode)
				<< ",\"doubleSided\":" << (desc.DoubleSided ? "true" : "false")
				<< ",\"revision\":" << material->GetRevision() << "}";
			result = out.str();
			return true;
		}
		if (cmd == "material.set")
		{
			const std::string path = arg("path");
			const Ref<Material> material = path.empty() ? nullptr : MaterialLibrary::Get().Load(path);
			if (!material)
			{
				error = "material not found: " + path;
				return false;
			}
			std::vector<std::string> applied;
			if (args.count("albedo")) { material->SetAlbedoTexture(arg("albedo")); applied.push_back("albedo"); }
			if (args.count("normal")) { material->SetNormalTexture(arg("normal")); applied.push_back("normal"); }
			if (args.count("baseColor"))
			{
				float rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
				std::stringstream stream(arg("baseColor"));
				std::string part;
				int index = 0;
				while (std::getline(stream, part, ',') && index < 4)
					rgba[index++] = std::strtof(part.c_str(), nullptr);
				material->SetBaseColor(glm::vec4 { rgba[0], rgba[1], rgba[2], rgba[3] });
				applied.push_back("baseColor");
			}
			if (args.count("metallic")) { material->SetMetallic(std::strtof(arg("metallic").c_str(), nullptr)); applied.push_back("metallic"); }
			if (args.count("roughness")) { material->SetRoughness(std::strtof(arg("roughness").c_str(), nullptr)); applied.push_back("roughness"); }
			if (args.count("blendMode"))
				material->SetBlendMode(static_cast<MaterialBlendMode>(std::atoi(arg("blendMode").c_str())));
			if (args.count("doubleSided"))
				material->SetDoubleSided(arg("doubleSided") == "1" || arg("doubleSided") == "true");
			if (applied.empty() && !args.count("blendMode") && !args.count("doubleSided"))
			{
				error = "nothing to set (albedo/normal/baseColor/metallic/roughness/blendMode/doubleSided)";
				return false;
			}
			bool saved = false;
			if (arg("save") == "1" || arg("save") == "true")
				saved = MaterialLibrary::Get().Save(material, path, nullptr);
			result = "revision=" + std::to_string(material->GetRevision()) + " saved=" + (saved ? "true" : "false");
			return true;
		}
		// ---- 录制 / 回放(把一次通道会话变成可复现脚本) ----
		if (cmd == "record.start" || cmd == "record.stop" || cmd == "replay")
		{
			if (cmd == "record.start")
			{
				const std::string path = arg("path");
				if (path.empty())
				{
					error = "missing path";
					return false;
				}
				StartAiRecording(path);
				result = "recording to " + path;
				return true;
			}
			if (cmd == "record.stop")
			{
				const size_t count = StopAiRecording();
				result = "recorded " + std::to_string(count) + " commands";
				return true;
			}
			// replay:按行读取 JSON 脚本并逐条执行(跳过录制/回放/退出类命令,防递归)。
			const std::string path = arg("path");
			std::ifstream file(path);
			if (!file)
			{
				error = "cannot open script: " + path;
				return false;
			}
			size_t executed = 0;
			size_t skipped = 0;
			std::string line;
			while (std::getline(file, line))
			{
				const auto objectStart = line.find('{');
				if (objectStart == std::string::npos)
					continue;
				std::map<std::string, std::string> fields;
				if (!Editor::AiControlServer::ParseFlatJson(line.substr(objectStart), &fields, nullptr))
				{
					++skipped;
					continue;
				}
				const std::string nested = fields.count("cmd") ? fields["cmd"] : std::string();
				if (nested.empty() || nested == "record.start" || nested == "record.stop"
					|| nested == "replay" || nested == "quit")
				{
					++skipped;
					continue;
				}
				std::string nestedResult;
				std::string nestedError;
				if (ExecuteAiCommand(nested, fields, nestedResult, nestedError))
					++executed;
				else
					++skipped;
			}
			result = "replayed executed=" + std::to_string(executed) + " skipped=" + std::to_string(skipped);
			return true;
		}
		error = "unknown command '" + cmd + "'";
		return false;
	}
}
