#pragma once
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include <filesystem>
#include <string>

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
