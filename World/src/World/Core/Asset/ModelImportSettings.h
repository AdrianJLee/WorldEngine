#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace World::Asset
{
	// P1b D5b:.wimport 导入设置(JSON,与源文件同目录同名:models/x.gltf → models/x.wimport)。
	//
	// 语义(plan §D5b-1 冻结决定):
	//  - 缺文件 / 坏 JSON / 字段类型不对 → 返回 Default() 并在 reason 里记原因,**不失败**;
	//  - Hash() 覆盖全部字段,供 .wmodel 的 meta.settingsHash 与 cook 复合指纹使用;
	//  - 未知字段忽略(向前兼容 D5c 新增字段)。
	struct ModelImportSettings
	{
		float Scale = 1.0f;                  // 导入期统一放大(烘焙进几何/节点)
		uint8_t UpAxis = 0;                  // 0 = Y(引擎坐标,不做转换);1 = Z(绕 X 轴 -90° 烘焙)
		bool ExportMaterials = true;         // false = 不产出 .wmat(材质槽留空)
		bool ExportTextures = true;          // false = 不产出贴图(材质贴图路径留空)
		bool ImportAnimations = true;        // false = 不导入 glTF animations(模型仍可导入)
		bool ImportSkins = true;             // D5c-2:false = 不导入骨架;蒙皮网格按静态处理
		// D5c-2:动画导入的固定采样率(Hz,clamp 1..120)。导入期按它把 glTF 关键帧
		// 烘成等间隔关键帧,运行时不解析插值器。
		float AnimationSampleRate = 30.0f;
		bool GenerateNormals = true;         // true = 源缺法线时按面法线补齐;false = 缺法线硬报错
		// D10（材质复用）：导出前算内容哈希，目标目录已有**同内容**文件就复用它的路径，
		// 不再为每个模型复制一份材质/贴图（关掉保留旧行为：每个模型一份副本）。
		bool ReuseMaterials = true;
		bool ReuseTextures = true;
		// 复用查找范围（相对**内容根**的逻辑目录，如 "materials/shared"）。空 = 只在本次
		// 导入的目的地目录里找。填了就先在它里面找，找不到再落到目的地目录。
		std::string SharedMaterialFolder;

		static ModelImportSettings Default() { return ModelImportSettings {}; }

		// ---- P4-U4:项目级导入默认值(project.we.yaml 的 `imports:` 区块) ----
		//
		// 语义:`.wimport` 旁路文件仍然**逐源优先**;只有"源没有 .wimport"时才用这份项目默认。
		// 存法:`ProjectManifest::ImportDefaults` 是清单里的事实源,`LoadProjectDefaults(manifestPath)`
		// 负责把它读进这里的进程级缓存(与 PhysicsSettings/RenderSettings 同一套"项目设置"模式);
		// 编辑器设置面板改完立刻 SetProjectDefaults + 写清单,新建导入立刻按新默认走。
		static void SetProjectDefaults(const ModelImportSettings& settings);
		static const ModelImportSettings& ProjectDefaults();
		// 读项目清单的 `imports:` 并写入进程级默认;清单缺该块 = 回到 Default()。
		// 返回是否成功读到清单(清单本身缺失/坏掉 → false,并把默认值复位成 Default())。
		static bool LoadProjectDefaults(const std::string& manifestPath);

		// 读取同目录的 .wimport;缺失 → Default() + reason 为空(正常情况)。
		// P4-U11:这是**旧项目兼容**路径 —— 新导入不再写 .wimport,设置存进 .wmodel 的 meta。
		static ModelImportSettings Load(const std::string& sourcePath, std::string* reason = nullptr);
		// 将设置写到与源同目录同名的 .wimport;失败写 reason。
		// P4-U11:仅保留给"显式导出旧格式"与既有测试;导入器不再调用它。
		static bool Save(const std::string& sourcePath, const ModelImportSettings& settings,
			std::string* reason = nullptr);
		// P4-U11:**这次导入该用哪份设置**(唯一的解析入口,编辑器与 cook 共用):
		//   ① 已有 `.wmodel` 的 meta 设置(资产自描述,用户改过的那份)
		//   ② 旧旁路 `.wimport`(只在旧项目里存在)
		//   ③ `project.we.yaml` 的 `imports:`(新导入的默认模板)
		//   ④ 引擎默认值
		// existingModelPath 为空或读不出来时自动落到下一级;fromAsset 返回①是否命中
		// (面板据此显示"设置来自资产")。
		static ModelImportSettings ResolveForImport(const std::string& sourcePath,
			const std::string& existingModelPath = std::string(), std::string* reason = nullptr,
			bool* fromAsset = nullptr);
		// P4-U11:在 sourceDirectory(绝对或相对路径)里找"由 sourceLogicalPath 产出的 .wmodel"。
		// 判定靠 meta.SourcePath(不信文件名),找不到返回空串。编辑器/cook 用它把逐源设置
		// 从资产里读回来。
		static std::string FindProducedModel(const std::string& sourceDirectory,
			const std::string& sourceLogicalPath);
		// 设置的确定哈希(FNV-1a 64,字段顺序固定);两次相同设置必得同一个值。
		static uint64_t Hash(const ModelImportSettings& settings);
	};
}
