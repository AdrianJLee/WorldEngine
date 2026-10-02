#pragma once

namespace World
{
	class Scene;
	struct FrameExtract;

	// ---- 渲染抽取的宿主接口(工业口径:Extract 阶段)----
	//
	// Bevy 把渲染拆成 `Extract`(主世界 → 渲染世界,相机无关地收集"本帧要画什么")与
	// `Prepare`/`Queue`(按视图剔除、排序、下发);Unreal 对应 FScene 的 gather。
	// 本引擎同构:
	//   * **Extract** = 逐实体遍历组件、产出绘制列表与灯光 —— 相机无关,所以能进帧管线
	//     (作为 `render-extract` 帧系统,PreRender 阶段,排在 `animation-system` 之后);
	//   * **Prepare/pass** = 视锥剔除、阴影矩阵、各 pass —— 依赖相机,留在提交侧
	//     (`SceneRenderer::SubmitScene`),因为相机是宿主在提交时才给的
	//     (编辑器视口相机 ≠ 场景主相机)。
	//
	// 抽取结果落在**场景**上(`Scene::RenderExtract()`):编辑器的主渲染器与预览渲染器
	// 提交的是**同一个场景**,缓冲放渲染器里会各自持有一份、互相看到陈旧数据。
	class IRenderExtractSink
	{
	public:
		virtual ~IRenderExtractSink() = default;
		// 把 scene 的本帧渲染数据填进 `scene.RenderExtract()`。必须是只读遍历
		// (不得增删组件/实体);失败(资源读不到)只警告并跳过该实体。
		virtual void ExtractScene(Scene& scene, float deltaSeconds) = 0;

		// sink 是**跨帧**装在场景上的(下一帧的 `render-extract` 帧系统要用),所以场景
		// 析构时必须通知宿主忘掉它 —— 否则宿主里那个 `Scene*` 会悬垂。
		virtual void OnExtractSceneDestroyed(Scene& scene) { (void)scene; }
	};
}
