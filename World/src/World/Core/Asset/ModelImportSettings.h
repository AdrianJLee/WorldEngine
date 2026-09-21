#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace World::Asset
{
	// 模型导入设置(逐源那一份)的**唯一存放地 = 资产的 `.wmodel` meta**(P4-U12)。
	// 早先的 `.wimport` 旁路文件已彻底移除:不存在第二份需要同步的真相。
	//
	// 语义:
	//  - 资产的 meta 读不出来 → 落到项目默认并在 reason 里记原因,**不失败**;
	//  - Hash() 覆盖全部字段,供 .wmodel 的 meta.settingsHash 与 cook 复合指纹使用。
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
		// 语义:已有资产内嵌的设置**逐源优先**;新导入(还没有产物)才用这份项目默认。
		// 存法:`ProjectManifest::ImportDefaults` 是清单里的事实源,`LoadProjectDefaults(manifestPath)`
		// 负责把它读进这里的进程级缓存(与 PhysicsSettings/RenderSettings 同一套"项目设置"模式);
		// 编辑器设置面板改完立刻 SetProjectDefaults + 写清单,新建导入立刻按新默认走。
		static void SetProjectDefaults(const ModelImportSettings& settings);
		static const ModelImportSettings& ProjectDefaults();
		// 读项目清单的 `imports:` 并写入进程级默认;清单缺该块 = 回到 Default()。
		// 返回是否成功读到清单(清单本身缺失/坏掉 → false,并把默认值复位成 Default())。
		static bool LoadProjectDefaults(const std::string& manifestPath);

		// **这次导入该用哪份设置**(唯一解析入口,编辑器 / CLI / cook 共用):
		//   ① 已有 `.wmodel` 的 meta 设置(资产自描述,用户改过的那份)
		//   ② `project.we.yaml` 的 `imports:`(新导入的默认模板)
		//   ③ 引擎默认值
		// P4-U12:早先的 `.wimport` 旁路文件已彻底移除(逐源设置只有一个家 = 资产本身)。
		static ModelImportSettings ResolveForImport(const std::string& existingModelPath = std::string(),
			std::string* reason = nullptr, bool* fromAsset = nullptr);
		// P4-U11:在 sourceDirectory(绝对或相对路径)里找"由 sourceLogicalPath 产出的 .wmodel"。
		// 判定靠 meta.SourcePath(不信文件名),找不到返回空串。编辑器/cook 用它把逐源设置
		// 从资产里读回来。
		static std::string FindProducedModel(const std::string& sourceDirectory,
			const std::string& sourceLogicalPath);
		// 设置的确定哈希(FNV-1a 64,字段顺序固定);两次相同设置必得同一个值。
		static uint64_t Hash(const ModelImportSettings& settings);
	};
}
