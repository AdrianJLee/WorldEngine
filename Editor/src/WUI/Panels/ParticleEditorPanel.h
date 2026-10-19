#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Renderer/ParticleAsset.h"
#include "World/Renderer/VfxModule.h"
#include "World/Renderer/VfxComputeGenerator.h"
#include "World/Renderer/ParticleSystem.h"
#include "World/Renderer/SceneRenderer.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/Math/Curve/ColorGradient.h"
#include "World/Math/Curve/CubicBezierCurve.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <memory>

namespace World
{
	enum class ParticleEditorViewMode : uint8_t
	{
		Visual = 0,         // 可视化模块堆栈与 3D 视口 (Niagara Stack + Viewport)
		CustomModule = 1,   // 项目全局代码自定义模块工作区 (Scratch Pad Studio)
		SlangShader = 2,    // 统一 Slang 计算着色器源码与即时热重载 (Slang Compute Code & Hot Reload)
		DataManifest = 3    // 声明式资产数据清单 (YAML Data Manifest)
	};

	enum class ParticleViewportDisplayMode : uint8_t
	{
		Lit = 0,
		Unlit = 1,
		Heatmap = 2,
		Bounds = 3
	};

	enum class CurveTarget : uint8_t
	{
		Size = 0,
		Alpha = 1,
		Speed = 2
	};

	enum class ParticleGizmoHandleId : int
	{
		None = -1,
		ConeBaseRadius = 1,
		ConeTopAngle = 2,
		ConeHeight = 3,
		SphereRadiusX = 10,
		SphereRadiusY = 11,
		SphereRadiusZ = 12,
		BoxExtentPosX = 20,
		BoxExtentNegX = 21,
		BoxExtentPosY = 22,
		BoxExtentNegY = 23,
		BoxExtentPosZ = 24,
		BoxExtentNegZ = 25
	};

	// 每发射器高级扩展特征状态 (Niagara / VFX Graph 级模块化参数)
	struct EmitterExtendedState
	{
		// 模块 1: 生成与生命周期拓展
		float LifetimeVariance = 0.2f;

		// 模块 2: 空间形状与位置拓展
		float ConeAngle = 30.0f;
		float ConeRadius = 0.5f;
		float ConeHeight = 1.5f;
		float SphereRadius = 1.0f;
		glm::vec3 BoxExtents { 1.0f, 1.0f, 1.0f };

		// 模块 3: 动力学、流体与扰动场拓展
		float CurlNoiseStrength = 0.0f;
		float CurlNoiseFrequency = 1.0f;
		uint32_t CurlNoiseOctaves = 1;

		glm::vec3 PointAttractorCenter { 0.0f, 1.0f, 0.0f };
		float PointAttractorStrength = 0.0f;
		float PointAttractorRadius = 5.0f;

		// 模块 4: 初始旋转与角速度
		float InitialRotationDeg = 0.0f;
		float AngularVelocityDeg = 0.0f;

		// 模块 6: 多元渲染器拓展 (Ribbon, Mesh, Light)
		bool RibbonEnabled = false;
		float RibbonLength = 1.0f;
		float RibbonWidth = 0.1f;

		bool MeshEnabled = false;
		int MeshType = 0; // 0=Cube, 1=Pyramid, 2=Sphere

		bool LightEnabled = false;
		float LightRadius = 3.0f;
		float LightIntensity = 1.0f;

		// 模块挂载与折叠状态
		bool ModuleBurstEnabled = true;
		bool ModuleCurlNoiseEnabled = false;
		bool ModuleAttractorEnabled = false;
		bool ModuleRibbonEnabled = false;
		bool ModuleMeshEnabled = false;
		bool ModuleLightEnabled = false;
		bool OutlinerModulesExpanded = false;
	};

	class ParticleEditorPanel final : public EditorPanel
	{
	public:
		ParticleEditorPanel();
		~ParticleEditorPanel() override;

		const char* Id() const override { return "particle_editor"; }
		const char* Title() const override { return "视觉特效系统 (WorldVFX Studio 2.0)"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt) override;

		static void RequestOpenAsset(const std::string& path);
		void LoadAsset(const std::string& path);
		void SaveAsset();
		void NewAsset();

		// 撤销 / 重做事务历史 (Undo / Redo)
		void PushUndoSnapshot();
		void Undo();
		void Redo();
		bool CanUndo() const noexcept { return !m_UndoStack.empty(); }
		bool CanRedo() const noexcept { return !m_RedoStack.empty(); }

		// 纯 Slang 模块化着色器堆栈与热重载操作 (Slang Modular Pipeline Operations)
		void AddModuleToSelectedEmitter(const VfxModule& module);
		void RemoveModuleFromSelectedEmitter(size_t moduleIndex);
		void MoveModuleUp(size_t moduleIndex);
		void MoveModuleDown(size_t moduleIndex);
		void CompileAndApplySlangShader();
		void SyncSlangShaderFromModules();

		void RenderSlangShaderView(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderDataView(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderModuleCard(Wui::WuiContext& ctx, VfxModule& mod, size_t modIdx, float cardX, float& curY, float cardW, const Wui::WuiTheme& theme);

		// 发射器大纲管理核心操作 (Outliner Operations)
		void AddEmitterFromTemplate(const std::string& templateType);
		void CloneSelectedEmitter();
		void DeleteSelectedEmitter();
		void MoveEmitterUp();
		void MoveEmitterDown();

		// 视口相机与空间聚焦操作 (Camera Navigation & Focus)
		void ResetCamera();
		void FocusSelectedEmitter();

		void ApplyCodeToAsset();
		void SyncCodeFromAsset();

		const std::string& GetLoadedPath() const noexcept { return m_LoadedPath; }
		const ParticleAsset& GetAsset() const noexcept { return m_Asset; }
		bool IsDirty() const noexcept { return m_Dirty; }
		ParticleEditorViewMode GetViewMode() const noexcept { return m_ViewMode; }

	private:
		void RefreshAvailableAssets();
		void SyncSimulationFromAsset();
		void StepSimulation(float dt);
		void EnsurePerEmitterState();
		bool IsEmitterActive(size_t index) const;
		bool IsEmitterLocked(size_t index) const;
		void ScrubToTime(float time);

		// 四分区布局子渲染函数
		void RenderTopBar(Wui::WuiContext& ctx, const Wui::WuiRect& barRect, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderOutliner(Wui::WuiContext& ctx, const Wui::WuiRect& outlinerRect, const Wui::WuiTheme& theme);
		void RenderViewport(Wui::WuiContext& ctx, const Wui::WuiRect& vpRect, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderTimeline(Wui::WuiContext& ctx, const Wui::WuiRect& timelineRect, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderInspector(Wui::WuiContext& ctx, const Wui::WuiRect& inspectorRect, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderContextMenu(Wui::WuiContext& ctx, const Wui::WuiTheme& theme);

		// 视口定向万向节与曲线工作台辅助渲染
		void RenderOrientationGimbal(Wui::WuiContext& ctx, const Wui::WuiRect& vpRect, const glm::mat4& viewMat, const Wui::WuiTheme& theme);
		void RenderInteractiveCurveWorkbench(Wui::WuiContext& ctx, const Wui::WuiRect& canvasRect, const Wui::WuiTheme& theme);

		static std::string s_PendingOpenPath;
		std::string m_LoadedPath;
		ParticleAsset m_Asset;
		bool m_Dirty = false;
		std::string m_StatusFeedback;
		std::vector<std::string> m_AvailableAssets;
		size_t m_SelectedEmitterIndex = 0;

		// 事务快照撤销/重做栈
		std::vector<ParticleAsset> m_UndoStack;
		std::vector<ParticleAsset> m_RedoStack;
		std::vector<std::vector<EmitterExtendedState>> m_UndoExtendedStack;
		std::vector<std::vector<EmitterExtendedState>> m_RedoExtendedStack;

		ParticleEditorViewMode m_ViewMode = ParticleEditorViewMode::Visual;
		std::string m_CodeBuffer;
		float m_CodeScrollY = 0.0f;
		float m_SlangScrollY = 0.0f;
		int m_SelectedCodeLine = -1;

		// Slang 计算着色器即时编译与热重载状态
		std::string m_SlangShaderBuffer;
		std::string m_SlangCompileError;
		bool m_SlangCompileSuccess = false;
		bool m_AddModuleMenuOpen = false;
		std::unordered_map<std::string, bool> m_ModuleSourceExpanded;

		// 项目全局代码自定义模块与编写器状态 (Niagara Scratch Pad)
		bool m_CustomModuleMenuOpen = false;
		std::vector<VfxModule> m_ProjectCustomModules;
		int m_SelectedProjectModuleIndex = -1;
		VfxModule m_EditingCustomModule;
		std::string m_NewParamNameBuffer;
		int m_NewParamTypeIndex = 0; // 0=Float, 1=Vec3, 2=Color
		float m_NewParamDefaultFloat = 1.0f;
		std::string m_CustomModuleCompileError;
		bool m_CustomModuleCompileSuccess = false;
		float m_CustomModuleListScrollY = 0.0f;
		float m_CustomModuleEditorScrollY = 0.0f;

		void RefreshProjectCustomModules();
		void OpenCustomModuleEditor(const std::string& defaultName = "");
		void OpenExistingCustomModule(size_t index);
		void RenderCustomModuleWorkspace(Wui::WuiContext& ctx, const Wui::WuiRect& workRect, const Wui::WuiTheme& theme, PanelHost& host);

		// 现代多分区布局与 Splitters 尺寸 (Outliner 拓宽至 270px 消除截字)
		float m_OutlinerWidth = 270.0f;
		float m_TimelineHeight = 220.0f;
		float m_InspectorWidth = 340.0f;

		// 左侧大纲与搜索过滤
		std::string m_FilterText;
		float m_OutlinerScrollY = 0.0f;

		// 就地重命名状态 (Inline Rename)
		int m_RenamingEmitterIndex = -1;
		char m_RenameBuffer[64] = {};
		bool m_NewEmitterMenuOpen = false;

		// 3D 自由轨道相机状态
		float m_CameraDistance = 6.0f;
		float m_OrbitYaw = 0.785f;    // ~45 度
		float m_OrbitPitch = 0.45f;   // ~25 度
		glm::vec3 m_CameraFocus { 0.0f, 1.0f, 0.0f };
		glm::vec3 m_TargetCameraFocus { 0.0f, 1.0f, 0.0f };
		bool m_IsOrbiting = false;
		bool m_IsPanning = false;
		glm::vec2 m_LastMousePos { 0.0f };

		// 视口显示模式与直接操纵 Gizmo
		ParticleViewportDisplayMode m_DisplayMode = ParticleViewportDisplayMode::Lit;
		bool m_HeatmapMode = false;
		bool m_BoundsMode = false;
		int m_HoveredGizmoHandle = -1;
		int m_ActiveGizmoHandle = -1;
		bool m_IsDraggingGizmo = false;
		float m_GizmoDragInitialParam = 0.0f;
		glm::vec2 m_GizmoDragStartPos { 0.0f };

		// 走带控制状态机 & 时间轴 Scrubbing
		bool m_IsPlaying = true;
		float m_PlaybackSpeed = 1.0f;
		float m_SimTime = 0.0f;
		float m_TimelineDuration = 5.0f;
		bool m_IsScrubbing = false;
		bool m_TimelineCurvesExpanded = true; // 默认展开内置贝塞尔工作台

		// 交互式曲线工作台核心状态 (Curve Workbench Interactive State)
		CurveTarget m_ActiveCurveTarget = CurveTarget::Size;
		int m_SelectedKeyframeIndex = 0;
		bool m_IsDraggingKeyframe = false;
		bool m_IsDraggingInHandle = false;
		bool m_IsDraggingOutHandle = false;
		glm::vec2 m_DragStartMouse { 0.0f };
		float m_DragKeyframeStartTime = 0.0f;
		float m_DragKeyframeStartVal = 0.0f;
		bool m_HoveredInHandle = false;
		bool m_HoveredOutHandle = false;
		int m_HoveredKeyframeIndex = -1;

		void EnsurePreviewResources();
		void ReleasePreviewResources();

		Ref<SceneRenderer> m_PreviewRenderer;
		Rhi::Device* m_PreviewGpuDevice = nullptr;
		Ref<Scene> m_PreviewScene;
		Entity m_PreviewEntity;
		uint32_t m_PreviewTextureId = 0;
		uint32_t m_UiTextureGeneration = ~0u;
		Rhi::Handle<Rhi::Texture> m_CurrentRenderedTexture;

		// 内部实时物理推演池 (Live Simulation Pools)
		std::vector<std::unique_ptr<ParticlePool>> m_LivePools;

		// 每发射器独立状态 (Solo/Mute/Lock, ColorGradient, Curves, ExtendedState)
		std::vector<bool> m_EmitterSolo;
		std::vector<bool> m_EmitterMute;
		std::vector<bool> m_EmitterLocked;
		std::vector<ColorGradient> m_EmitterGradients;
		std::vector<CubicBezierCurve> m_EmitterSizeCurves;
		std::vector<CubicBezierCurve> m_EmitterAlphaCurves;
		std::vector<CubicBezierCurve> m_EmitterSpeedCurves;
		std::vector<std::string> m_EmitterCurveType;
		std::vector<EmitterExtendedState> m_EmitterExtended;

		// 右键上下文菜单与剪贴板
		bool m_ContextMenuOpen = false;
		glm::vec2 m_ContextMenuPos { 0.0f };
		size_t m_ContextMenuTargetEmitter = 0;
		int m_ContextMenuTargetCard = -1;
		ParticleEmitterLayer m_CopiedEmitter;
		bool m_HasCopiedEmitter = false;

		// 右侧检视区滚动与模块卡片状态
		float m_InspectorScrollY = 0.0f;
		bool m_CardSpawningExpanded = true;
		bool m_CardShapeExpanded = true;
		bool m_CardForcesExpanded = true;
		bool m_CardSizeRotationExpanded = true;
		bool m_CardColorAlphaExpanded = true;
		bool m_CardRenderersExpanded = true;
		bool m_CardCollisionExpanded = true;
		bool m_AddModulePopupOpen = false;
	};
}
