#pragma once

#include "../Asset/ModelImportWatch.h"
#include "../EngineShaderHotReload.h"
#include "../ShaderHotReload.h"
#include "../Texture/TextureImportWatch.h"

#include <cstddef>
#include <string>

namespace World::Editor
{
	// HOTR-P3-T10:编辑器侧热重载宿主 —— 把四个自包含的 watch 服务收敛成单一
	// Poll/Pump/Shutdown/EnqueueShader 入口:
	//   * `ShaderHotReload`(材质 `.slang`,T1)
	//   * `EngineShaderHotReload`(引擎内建 shader,T3)
	//   * `TextureImportWatch`(`.wtex` 重烘,T5)
	//   * `ModelImportWatch`(glTF 重导入,T9)
	// 生效矩阵与开关:`docs/dev/hot-reload.md`。
	//
	// 语义与收敛前一致(日志文本、开关口径逐条不变):
	//   - `EngineShaderHotReload` 由它自己的 `WLD_SHADER_HOTRELOAD` 把关,与资产热重载
	//     总开关无关 —— 因此宿主 Poll 在"没有活动场景/渲染器"的早退之前调用
	//     (启动器/无项目形态同样生效);调用方把"活动场景 + 渲染器 + 资产热重载开关"
	//     算成 assetHotReloadEnabled 传入,资产侧三个 watcher 的轮询闸门与收敛前一致;
	//   - `ShaderHotReload` / `TextureImportWatch` / `ModelImportWatch` 由调用方传入的
	//     `assetHotReloadEnabled`(`WLD_ASSET_HOTRELOAD` + 偏好)把关;
	//     `TextureImportWatch` 另有 `WLD_TEXTURE_HOTRELOAD`、`ModelImportWatch` 另有
	//     `WLD_MODEL_HOTRELOAD` 子开关,都在宿主内部按收敛前的同一口径判定;
	//   - 线程纪律不变:各服务自带工作线程,编译器/烘焙/导入只跑在工作线程,
	//     Install/写盘/原子替换只在主线程帧边界(资产侧在 PollAssetHotReload 的一次 Pump)。
	// 帧内位置微调(单入口收敛的必然结果,已记档):资产侧三个 watcher 的 Poll 从
	// `PollAssetHotReload` 中段移到宿主 Poll(同一帧、同一道资产闸门之内),它们的 Pump
	// 移到宿主 Pump(原材质 `.slang` Install 的位置);两者不与材质库轮询共享状态。
	class EditorHotReloadHost
	{
	public:
		EditorHotReloadHost() = default;
		~EditorHotReloadHost();

		EditorHotReloadHost(const EditorHotReloadHost&) = delete;
		EditorHotReloadHost& operator=(const EditorHotReloadHost&) = delete;

		// 主线程帧边界:轮询引擎 shader(不受 assetHotReloadEnabled 影响)+ 资产侧三个
		// watcher(assetHotReloadEnabled=false 时跳过资产侧,与收敛前的闸门一致)。
		void Poll(bool assetHotReloadEnabled, double deltaSeconds);
		// 主线程帧边界:装配资产侧在飞产物 —— 材质 `.slang` Install、`.wtex` 重烘落盘 +
		// 缓存失效、glTF 重导入原子替换(assetHotReloadEnabled=false 时 no-op)。
		void Pump(bool assetHotReloadEnabled);
		// 停止并 join 全部工作线程(幂等;OnDetach 与析构都会调)。
		void Shutdown();
		// 材质 `.slang` 的 ChangedShaders 入口(与收敛前同一条链)。
		void EnqueueShader(const std::string& logicalPath);

		// HOTR-P3-T9 冲突门探针的转发:模型预览面板正打开该 `.wmodel` 时跳过重导入
		// (空 = 不做面板门,仅测试/无壳形态)。冻结清单外的新增透传方法:不保留它就会
		// 丢掉"面板打开即跳过、关闭后顺延"的既有行为(T9 探针的一条路径)。
		void SetModelSkipProbe(ModelImportWatch::SkipProbe probe);

		// 供 AI/探针读的计数(可选,便于诊断;不得改变既有 JSON 契约)。
		std::size_t WatchedTextureAssets() const;
		std::size_t WatchedModelAssets() const;

	private:
		ShaderHotReload m_ShaderHotReload;
		EngineShaderHotReload m_EngineShaderHotReload;
		TextureImportWatch m_TextureImportWatch;
		ModelImportWatch m_ModelImportWatch;
	};
}
