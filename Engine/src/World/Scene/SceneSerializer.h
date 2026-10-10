#pragma once
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	// 场景文档(.wd)读写。Scene 里实体 → YAML 的**唯一**事实源。
	//
	// Level 2.0(OFPA)复用同一条实体级读写口径:`.entity` 单实体文件由
	// SerializeEntityDocument 写出、由 AppendEntityDocument 读回,字段形状与 .wd
	// 里每个实体块逐字段同源 —— 两处各写一份序列化正是"编辑器能读、流送丢组件"
	// 这类偏差的成因。
	class SceneSerializer
	{
	public:
		SceneSerializer(const Ref<Scene>& scene);
		bool Serialize(const std::string& filepath);
		bool Deserialize(const std::string& filepath);

		// Level 2.0:把一个实体**全部组件**写成独立文档(`.entity`)。
		static bool SerializeEntityDocument(const Entity& entity, const std::filesystem::path& filepath,
			std::string* error = nullptr);

		// 同上,但文档内容回吐到内存(关卡包/迁移工具逐实体生成用)。
		static bool SerializeEntityDocumentToYaml(const Entity& entity, std::string* outYaml,
			std::string* error = nullptr);

		// Level 2.0:读一个 `.entity` 文档,把其中实体并入 scene(保留 UUID/名字/全部组件),
		// 返回并入的实体数;失败返回 0 并填 error。已存在同 UUID 的实体则跳过。
		static size_t AppendEntityDocument(Scene& scene, const std::filesystem::path& filepath,
			std::string* error = nullptr);

		// 同上,但文档内容已经在内存里(关卡包逐实体读取用)。documentYaml 必须是
		// `Entities: [ {...} ]` 形状的单实体文档。
		static size_t AppendEntityYaml(Scene& scene, const std::string& documentYaml,
			std::string* error = nullptr);

		// 批量并入一组单实体文档(关卡包 / 流送块)。已存在同 UUID 的实体跳过;
		// `skip` 非空时按其逐项跳过(流送块用它过滤 DeltaStateRegistry 里已摧毁的实体)。
		// existing-UUID 集合只构建一次 —— 逐条调 AppendEntityYaml 会是 O(n²)。
		// outApplied 非空时按并入顺序填回实体句柄(调用方据此登记可卸载集合)。
		static size_t AppendEntityDocuments(Scene& scene, const std::vector<std::string>& documents,
			const std::vector<uint8_t>* skip = nullptr, std::vector<Entity>* outApplied = nullptr,
			std::string* error = nullptr);

		// Level 2.0 §16.10:把一组单实体文档并入场景,`overwriteExisting = true` 时同 UUID 的
		// 实体**被覆盖**(而不是跳过)。稀疏放置的关卡加载要用它:实例子树先由"实例化模板"
		// 建好(带盘上 UUID),随后落盘的实体文档(实例根 + 与模板不一致的后代)覆盖上去。
		//
		// 覆盖走"临时实体读一遍 + 按组件 Copy 到既有实体"—— schema 的 Add 是 emplace
		// (重复加会断言),Copy 才是 AddOrReplace;这样"逐组件读写口径"仍然只有一处实现。
		static size_t MergeEntityDocuments(Scene& scene, const std::vector<std::string>& documents,
			bool overwriteExisting = true, std::string* error = nullptr);

		// 解析一份实体文档里的 `Prefab:` 块(模板引用 + 覆盖字段 [+ 稀疏重建映射])。
		// 没有该块 / 形状不对 ⇒ 返回 false(不是错误:绝大多数实体文档都不带块)。
		static bool ReadPrefabBlock(const std::string& documentYaml,
			Gameplay::PendingPrefabInstanceRecord* out);

		// 逐实体并入之后**必须**收尾一次:反序列化只写 Transform 的原始字段,不会重算
		// 缓存矩阵;层级反向索引也只有在全部实体就位后才能重建。缺这一步的表现是
		// "相机在物体内部 / 全部物体堆在原点"。
		static void FinalizeEntityImport(Scene& scene);

		bool SerializeRuntime(const std::string& filepath);
		bool DeserializeRuntime(const std::string& filepath);

		const std::string& GetLastError() const { return m_LastError; }

	private:
		Ref<Scene> m_Scene;
		std::string m_LastError;
	};
}
