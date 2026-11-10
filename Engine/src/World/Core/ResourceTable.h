#pragma once

#include "World/Core/Export.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace World
{
	// ---------------------------------------------------------------------------
	// 显式 typed 资源表(世界/场景单例的正式归属,2026-10-04)。
	//
	// 为什么不用 entt 的 `registry.ctx()`:
	//   1. `ctx()` 挂在 **registry** 上,而 registry 会被整体复制(CopyScene / Prefab 克隆)——
	//      世界单例必须**不**跟着实体数据被复制;
	//   2. `ctx()` 无类型契约、无显式所有关系,取缺失项口径不清;
	//   3. 跨 DLL 边界(Microsoft ABI)下模板化 ctx 容易踩类型身份/分配器不一致。
	//
	// 契约:
	//   * 每类型至多一个实例;`Get` 缺失 = 抛 `std::logic_error`(不静默造默认值);
	//   * 生命周期与 owner 一致(WorldContext 或 Scene 自身);
	//   * 显式 Emplace / Remove / 枚举,可诊断(内存面板能列出"这个世界有哪些服务");
	//   * 非拷贝(单例语义);分配与析构都发生在 World 模块内(跨 DLL 不交替堆);
	//   * 键 = 类型的**名字**(不是 type_info 地址),因此跨 DLL 的类型身份一致;
	//   * 线程模型:Emplace/Remove 只在初始化/结构提交点;TryGet 为读,可并发。
	// ---------------------------------------------------------------------------
	class WLD_API ResourceTable
	{
	public:
		ResourceTable() = default;
		~ResourceTable() { Clear(); }
		ResourceTable(const ResourceTable&) = delete;
		ResourceTable& operator=(const ResourceTable&) = delete;

		// 已存在则替换(旧实例先析构)。
		template <typename T, typename... Args>
		T& Emplace(Args&&... args)
		{
			static_assert(!std::is_const_v<T> && !std::is_reference_v<T>,
				"ResourceTable stores concrete object types");
			Remove<T>();
			auto* object = new T(std::forward<Args>(args)...);
			m_Entries.emplace(Key<T>(), Entry { object, &Destroy<T> });
			return *object;
		}

		template <typename T>
		T* TryGet() { return static_cast<T*>(Raw(Key<T>())); }

		template <typename T>
		const T* TryGet() const { return static_cast<const T*>(Raw(Key<T>())); }

		template <typename T>
		T& Get()
		{
			T* found = TryGet<T>();
			if (!found)
				throw std::logic_error("ResourceTable: missing resource '" + Key<T>() + "'");
			return *found;
		}

		template <typename T>
		const T& Get() const
		{
			const T* found = TryGet<T>();
			if (!found)
				throw std::logic_error("ResourceTable: missing resource '" + Key<T>() + "'");
			return *found;
		}

		template <typename T>
		bool Has() const { return m_Entries.find(Key<T>()) != m_Entries.end(); }

		template <typename T>
		bool Remove()
		{
			const auto found = m_Entries.find(Key<T>());
			if (found == m_Entries.end())
				return false;
			found->second.Destroy(found->second.Object);
			m_Entries.erase(found);
			return true;
		}

		void Clear()
		{
			for (auto& [key, entry] : m_Entries)
				entry.Destroy(entry.Object);
			m_Entries.clear();
		}

		std::size_t Size() const { return m_Entries.size(); }

		// 诊断:已登记的资源类型名(内存面板/测试)。
		std::vector<std::string> TypeNames() const
		{
			std::vector<std::string> names;
			names.reserve(m_Entries.size());
			for (const auto& [key, entry] : m_Entries)
				names.push_back(key);
			return names;
		}

	private:
		struct Entry
		{
			void* Object = nullptr;
			void (*Destroy)(void*) = nullptr;
		};

		template <typename T>
		static std::string Key() { return std::string(typeid(T).name()); }

		template <typename T>
		static void Destroy(void* object) { delete static_cast<T*>(object); }

		void* Raw(const std::string& key)
		{
			const auto found = m_Entries.find(key);
			return found == m_Entries.end() ? nullptr : found->second.Object;
		}

		const void* Raw(const std::string& key) const
		{
			const auto found = m_Entries.find(key);
			return found == m_Entries.end() ? nullptr : found->second.Object;
		}

		std::unordered_map<std::string, Entry> m_Entries;
	};
}