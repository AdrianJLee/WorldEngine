#include "PluginManager_Internal.h"

namespace World::Plugins
{

using namespace PluginManagerDetail;

namespace PluginManagerDetail
{
		// 诊断用:插件组件存储 id(保留段)写成 0xXXXXXXXX。
std::string Hex32(uint32_t value){
			char buffer[16] = {};
			std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
			return buffer;
		}


std::string JoinIds(const std::set<std::string>& ids){
			std::string text;
			for (const std::string& id : ids)
			{
				if (!text.empty())
					text += ",";
				text += id;
			}
			return text;
		}


		// PLUG-CLEAN-1:插件组件显示名的本地化约定(契约见 docs/dev/plugin-framework.md):
		//   键 = "plugin.<pluginId>.<typeId>";先按类型全名(WeComponentDesc::Id)查,
		//   未命中再按短名(最后一个 '.' 之后)查一次;都没命中 ⇒ 插件声明的 DisplayName。
		// 解析发生在**加载/重载时**(与组件注册同一生命周期):schema.DisplayName 同时是
		// 编辑期显示文案与 a11y 行 id(`prop.add.<DisplayName>`)的来源,不能每帧改写;
		// 切换语言后由 plugin.reload 或重启编辑器重新解析。
std::string ResolveComponentDisplayName(const std::string& pluginId, const std::string& typeId, const std::string& declared){
			const std::string fallback = declared.empty() ? typeId : declared;
			const Wui::LocalizedLabel full = Wui::TrLabel("plugin." + pluginId + "." + typeId, fallback);
			if (full.Text != fallback)
				return full.Text;
			const size_t separator = typeId.rfind('.');
			if (separator != std::string::npos && separator + 1 < typeId.size())
			{
				const std::string shortName = typeId.substr(separator + 1);
				const Wui::LocalizedLabel shortLabel =
					Wui::TrLabel("plugin." + pluginId + "." + shortName, fallback);
				if (shortLabel.Text != fallback)
					return shortLabel.Text;
			}
			return fallback;
		}

}

	// ---- T2:宿主注册面(WeHostApi 尾部字段的宿主实现)----------------------------------

PluginManager::Record* PluginManager::ResolveHostRecord(HostApiBox& box){
		if (box.Manager != this)
			return nullptr;
		Record* record = FindRecord(box.PluginId);
		if (!record)
			return nullptr;
		// 只接受:本管理器在 Register 调用期间交给插件的那张表,或该条目自己的表(Unregister 期间)。
		if (m_ActiveBox != &box && record->Host.get() != &box)
			return nullptr;
		return record;
	}


bool PluginManager::BridgeRegisterAssetType(void* userData, const WeAssetTypeDesc* desc){
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record || !desc)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": asset type registration rejected ("
				+ (!desc ? "null descriptor" : "invalid host handle") + ")");
			return false;
		}
		return box->Manager->RegisterAssetType(*record, *desc);
	}


bool PluginManager::BridgeUnregisterAssetType(void* userData, const char* id){
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": asset type unregister rejected (invalid host handle)");
			return false;
		}
		return box->Manager->UnregisterAssetType(*record, id);
	}


bool PluginManager::BridgeRegisterAssetImporter(void* userData, const WeAssetImporterDesc* desc){
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record || !desc)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": importer registration rejected ("
				+ (!desc ? "null descriptor" : "invalid host handle") + ")");
			return false;
		}
		return box->Manager->RegisterAssetImporter(*record, *desc);
	}


bool PluginManager::BridgeUnregisterAssetImporter(void* userData, const char* id){
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": importer unregister rejected (invalid host handle)");
			return false;
		}
		return box->Manager->UnregisterAssetImporter(*record, id);
	}


void* PluginManager::BridgeLookupExport(void* userData, const char* pluginId, const char* name, uint32_t minVersion){
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager || !pluginId || !name)
			return nullptr;
		return box->Manager->LookupExport(pluginId, name, minVersion);
	}


bool PluginManager::BridgeRegisterComponent(void* userData, const WeComponentDesc* desc){
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record || !desc)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": component registration rejected ("
				+ (!desc ? "null descriptor" : "invalid host handle") + ")");
			return false;
		}
		return box->Manager->RegisterComponent(*record, box->Schemas, *desc);
	}


bool PluginManager::BridgeUnregisterComponent(void* userData, const char* id){
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": component unregister rejected (invalid host handle)");
			return false;
		}
		return box->Manager->UnregisterComponent(*record, box->Schemas, id, box->Context);
	}


	// ---- T3b:编辑器扩展(命令 / 面板)桥 ------------------------------------------------

bool PluginManager::BridgeRegisterEditorCommand(void* userData, const WeEditorCommandDesc* desc){
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record || !desc)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": editor command registration rejected ("
				+ (!desc ? "null descriptor" : "invalid host handle") + ")");
			return false;
		}
		return box->Manager->RegisterEditorCommand(*record, *desc);
	}


bool PluginManager::BridgeUnregisterEditorCommand(void* userData, const char* id){
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": editor command unregister rejected (invalid host handle)");
			return false;
		}
		return box->Manager->UnregisterEditorCommand(*record, id);
	}


bool PluginManager::BridgeRegisterEditorPanel(void* userData, const WeEditorPanelDesc* desc){
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record || !desc)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": editor panel registration rejected ("
				+ (!desc ? "null descriptor" : "invalid host handle") + ")");
			return false;
		}
		return box->Manager->RegisterEditorPanel(*record, *desc);
	}


bool PluginManager::BridgeUnregisterEditorPanel(void* userData, const char* id){
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": editor panel unregister rejected (invalid host handle)");
			return false;
		}
		return box->Manager->UnregisterEditorPanel(*record, id);
	}


	// ---- T2c:插件组件存储的在册槽位 ------------------------------------------------

uint32_t PluginManager::AllocateComponentSlot(){
		for (uint32_t slot = 0; slot < kPluginComponentSlotCount; ++slot)
		{
			if (m_ComponentSlots[slot])
				continue;
			m_ComponentSlots[slot] = true;
			return slot;
		}
		return kPluginComponentSlotCount;   // 用尽(调用方按可读诊断拒绝注册)
	}


void PluginManager::ReleaseComponentSlot(uint32_t slot){
		if (slot < kPluginComponentSlotCount)
			m_ComponentSlots[slot] = false;
	}

namespace PluginManagerDetail
{
		ComponentFieldSlot g_ComponentFieldSlots[kMaxComponentFieldSlots];
}
}
