#include "PluginManager_Internal.h"

namespace World::Plugins
{

using namespace PluginManagerDetail;


	// ---- T3b:编辑器扩展(命令 / 面板)---------------------------------------------------

std::vector<PluginEditorCommand> PluginManager::EditorCommands() const{
		std::vector<PluginEditorCommand> commands;
		for (const Record& record : m_Records)
			for (const RegisteredEditorCommand& item : record.RegisteredEditorCommands)
			{
				PluginEditorCommand command;
				command.PluginId = record.Entry.Manifest.Id;
				command.Id = item.Id;
				command.Label = item.Label;
				command.Tooltip = item.Tooltip;
				command.CommandName = "plugin.command." + command.PluginId + "." + command.Id;
				command.InvokeCount = item.InvokeCount;
				commands.push_back(std::move(command));
			}
		return commands;
	}


bool PluginManager::FindEditorCommand(const std::string& pluginId, const std::string& id, PluginEditorCommand* out) const{
		for (const Record& record : m_Records)
		{
			if (record.Entry.Manifest.Id != pluginId)
				continue;
			for (const RegisteredEditorCommand& item : record.RegisteredEditorCommands)
			{
				if (item.Id != id)
					continue;
				if (out)
				{
					out->PluginId = pluginId;
					out->Id = item.Id;
					out->Label = item.Label;
					out->Tooltip = item.Tooltip;
					out->CommandName = "plugin.command." + pluginId + "." + item.Id;
					out->InvokeCount = item.InvokeCount;
				}
				return true;
			}
		}
		return false;
	}


bool PluginManager::InvokeEditorCommand(const std::string& command, std::string* error){
		// 两种写法:完整命令名 `plugin.command.<pluginId>.<id>` 或裸 id `<id>`(唯一命中才执行)。
		// 插件 id 与命令 id 都允许含点(反域名习惯)⇒ **不按最后一个点拆解**,
		// 而是按 (pluginId, id) 逐条比对完整名 —— 不会把 `test.editorui.ping` 拆错。
		constexpr const char* kPrefix = "plugin.command.";
		const bool fullName = command.rfind(kPrefix, 0) == 0;
		const std::string id = fullName ? command.substr(std::strlen(kPrefix)) : command;
		if (id.empty())
		{
			if (error) *error = "editor command id is empty";
			return false;
		}

		for (Record& record : m_Records)
		{
			for (RegisteredEditorCommand& item : record.RegisteredEditorCommands)
			{
				const std::string expected = record.Entry.Manifest.Id + "." + item.Id;
				if (item.Id != id && expected != id)
					continue;
				if (!item.Callback)
				{
					if (error) *error = "editor command '" + id + "' has no callback";
					return false;
				}
				// 回调不该抛,但不能让异常把命令面(乃至编辑器)带走 —— 与 LogBridge 同口径。
				try
				{
					item.Callback(item.UserData);
				}
				catch (const std::exception& exception)
				{
					Log(WePluginLogError, record.Entry.Manifest.Id + ": editor command '" + id
						+ "' raised an exception: " + exception.what());
				}
				catch (...)
				{
					Log(WePluginLogError, record.Entry.Manifest.Id + ": editor command '" + id
						+ "' raised an unknown exception");
				}
				++item.InvokeCount;
				Log(WePluginLogInfo, record.Entry.Manifest.Id + ": editor command '" + id
					+ "' invoked (count=" + std::to_string(item.InvokeCount) + ")");
				return true;
			}
		}

		if (fullName)
		{
			size_t matches = 0;
			for (const Record& record : m_Records)
				for (const RegisteredEditorCommand& item : record.RegisteredEditorCommands)
					if (record.Entry.Manifest.Id + "." + item.Id == id)
						++matches;
			if (matches > 1)
			{
				if (error) *error = "editor command name '" + command + "' is ambiguous";
				return false;
			}
		}
		if (error) *error = "no editor command matches '" + command + "'";
		return false;
	}


std::vector<PluginEditorPanel> PluginManager::EditorPanels() const{
		std::vector<PluginEditorPanel> panels;
		for (const Record& record : m_Records)
			for (const RegisteredEditorPanel& item : record.RegisteredEditorPanels)
			{
				PluginEditorPanel panel;
				panel.PluginId = record.Entry.Manifest.Id;
				panel.Id = item.Id;
				panel.DisplayId = "plugin.panel." + panel.PluginId + "." + panel.Id;
				panel.Title = item.Title.empty() ? item.Id : item.Title;
				panels.push_back(std::move(panel));
			}
		return panels;
	}


bool PluginManager::FindEditorPanel(const std::string& panelId, PluginEditorPanel* out) const{
		for (const Record& record : m_Records)
			for (const RegisteredEditorPanel& item : record.RegisteredEditorPanels)
			{
				const std::string displayId = "plugin.panel." + record.Entry.Manifest.Id + "." + item.Id;
				if (displayId != panelId)
					continue;
				if (out)
				{
					out->PluginId = record.Entry.Manifest.Id;
					out->Id = item.Id;
					out->DisplayId = displayId;
					out->Title = item.Title.empty() ? item.Id : item.Title;
				}
				return true;
			}
		return false;
	}


int PluginManager::RenderEditorPanel(const std::string& panelId) const{
		if (!m_EditorHost)
			return -1;
		for (const Record& record : m_Records)
			for (const RegisteredEditorPanel& item : record.RegisteredEditorPanels)
			{
				const std::string displayId = "plugin.panel." + record.Entry.Manifest.Id + "." + item.Id;
				if (displayId != panelId)
					continue;
				PluginEditorPanel panel;
				panel.PluginId = record.Entry.Manifest.Id;
				panel.Id = item.Id;
				panel.DisplayId = displayId;
				panel.Title = item.Title.empty() ? item.Id : item.Title;
				return m_EditorHost->RenderEditorPanelSurface(panel, item.Draw, item.UserData);
			}
		return -1;
	}


bool PluginManager::RegisterEditorCommand(Record& record, const WeEditorCommandDesc& desc){
		const std::string& pluginId = record.Entry.Manifest.Id;
		if (desc.StructSize < sizeof(WeEditorCommandDesc) || desc.AbiVersion != WE_PLUGIN_ABI_VERSION)
		{
			Log(WePluginLogWarn, pluginId + ": editor command registration rejected (struct/abi mismatch size="
				+ std::to_string(desc.StructSize) + " abi=" + std::to_string(desc.AbiVersion) + ")");
			return false;
		}
		const std::string id = desc.Id ? desc.Id : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": editor command registration rejected (empty id)");
			return false;
		}
		if (!desc.Callback)
		{
			Log(WePluginLogWarn, pluginId + ": editor command '" + id
				+ "' registration rejected (no callback)");
			return false;
		}
		for (const RegisteredEditorCommand& item : record.RegisteredEditorCommands)
		{
			if (item.Id == id)
			{
				Log(WePluginLogWarn, pluginId + ": editor command '" + id
					+ "' is already registered by this plugin; registration ignored");
				return false;
			}
		}
		if (!m_EditorHost)
		{
			Log(WePluginLogWarn, pluginId + ": editor command '" + id
				+ "' registration rejected (no editor host is wired)");
			return false;
		}
		if (!m_EditorHost->RegisterEditorCommand(pluginId, desc, desc.Callback, desc.UserData))
		{
			Log(WePluginLogWarn, pluginId + ": editor command '" + id
				+ "' registration rejected by the editor");
			return false;
		}

		RegisteredEditorCommand entry;
		entry.Id = id;
		entry.Label = desc.Label ? desc.Label : id;
		entry.Tooltip = desc.Tooltip ? desc.Tooltip : "";
		entry.Callback = desc.Callback;
		entry.UserData = desc.UserData;
		record.RegisteredEditorCommands.push_back(std::move(entry));
		Log(WePluginLogInfo, pluginId + ": registered editor command '" + id + "'");
		return true;
	}


bool PluginManager::UnregisterEditorCommand(Record& record, const char* rawId){
		const std::string& pluginId = record.Entry.Manifest.Id;
		const std::string id = rawId ? rawId : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": editor command unregister rejected (empty id)");
			return false;
		}
		const auto tracked = std::find_if(record.RegisteredEditorCommands.begin(),
			record.RegisteredEditorCommands.end(),
			[&id](const RegisteredEditorCommand& item) { return item.Id == id; });
		if (tracked == record.RegisteredEditorCommands.end())
			return true;   // 幂等:本插件没注册过(也可能已被卸载兜底回收)
		if (m_EditorHost)
			m_EditorHost->UnregisterEditorCommand(pluginId, id);
		record.RegisteredEditorCommands.erase(tracked);
		Log(WePluginLogInfo, pluginId + ": unregistered editor command '" + id + "'");
		return true;
	}


bool PluginManager::RegisterEditorPanel(Record& record, const WeEditorPanelDesc& desc){
		const std::string& pluginId = record.Entry.Manifest.Id;
		if (desc.StructSize < sizeof(WeEditorPanelDesc) || desc.AbiVersion != WE_PLUGIN_ABI_VERSION)
		{
			Log(WePluginLogWarn, pluginId + ": editor panel registration rejected (struct/abi mismatch size="
				+ std::to_string(desc.StructSize) + " abi=" + std::to_string(desc.AbiVersion) + ")");
			return false;
		}
		const std::string id = desc.Id ? desc.Id : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": editor panel registration rejected (empty id)");
			return false;
		}
		if (!desc.Draw)
		{
			Log(WePluginLogWarn, pluginId + ": editor panel '" + id
				+ "' registration rejected (no draw callback)");
			return false;
		}
		for (const RegisteredEditorPanel& item : record.RegisteredEditorPanels)
		{
			if (item.Id == id)
			{
				Log(WePluginLogWarn, pluginId + ": editor panel '" + id
					+ "' is already registered by this plugin; registration ignored");
				return false;
			}
		}
		if (!m_EditorHost)
		{
			Log(WePluginLogWarn, pluginId + ": editor panel '" + id
				+ "' registration rejected (no editor host is wired)");
			return false;
		}
		// 渲染入口的 host 参数 = 本管理器;userData = 该插件记录(卸载前必然注销)。
		if (!m_EditorHost->RegisterEditorPanel(pluginId, desc, &PluginManager::RenderPanelEntry, nullptr))
		{
			Log(WePluginLogWarn, pluginId + ": editor panel '" + id
				+ "' registration rejected by the editor");
			return false;
		}

		RegisteredEditorPanel entry;
		entry.Id = id;
		entry.Title = desc.Title ? desc.Title : id;
		entry.Draw = desc.Draw;
		entry.UserData = desc.UserData;
		record.RegisteredEditorPanels.push_back(std::move(entry));
		Log(WePluginLogInfo, pluginId + ": registered editor panel '" + id + "'");
		return true;
	}


bool PluginManager::UnregisterEditorPanel(Record& record, const char* rawId){
		const std::string& pluginId = record.Entry.Manifest.Id;
		const std::string id = rawId ? rawId : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": editor panel unregister rejected (empty id)");
			return false;
		}
		const auto tracked = std::find_if(record.RegisteredEditorPanels.begin(),
			record.RegisteredEditorPanels.end(),
			[&id](const RegisteredEditorPanel& item) { return item.Id == id; });
		if (tracked == record.RegisteredEditorPanels.end())
			return true;   // 幂等
		if (m_EditorHost)
			m_EditorHost->UnregisterEditorPanel(pluginId, id);
		record.RegisteredEditorPanels.erase(tracked);
		Log(WePluginLogInfo, pluginId + ": unregistered editor panel '" + id + "'");
		return true;
	}

}
