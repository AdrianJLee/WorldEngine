#pragma once

#include "EditorPanel.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/Mesh.h"
#include "World/Renderer/Renderer.h"
#include "World/RHI/Rhi.h"

#include <string>
#include <vector>

namespace World
{
	// D3:材质编辑器(独立窗口形态,与 Widget Gallery / Input Map / 模型预览同级)。
	//
	// U21-M1(用户 2026-09-22 确认的方案 §1.A + §1.C):把"参数表"做成编辑器——
	//  - 头部:材质名 + 来源逻辑路径 + 脏标记 + Save / Reveal / Revert(全部有悬停说明);
	//  - 参数区:按物理意义分六组(基础外观/表面细节/自发光/透明度与混合/贴图采样/高级),
	//    可折叠、可搜索、每字段"恢复默认";
	//    U24(用户 2026-09-22 反馈②③④):数值字段按 `WuiWidgets.h` 的分类规则选控件 ——
	//    感知型归一化区间(金属度/粗糙度/预览光照强度与角度)用 `Wui::DragBarFloat`
	//    (值区常显、点值区可输入、↑/↓ 步进);"恢复默认"改用固定占位的
	//    `Wui::ResetDefaultButton`(偏离/等于默认两态共用同一 rect,行布局零位移);
	//  - 校验区:缺贴图 / 越界 / 引用不在内容根,每条可点击定位到字段(无问题时整块不占位);
	//  - 预览区(U23,用户 2026-09-22「预览这个大分类应该和其他的分开」):预览设置
	//    (网格/背景/光照/显示)住在**预览区自己的卡片与标签条**里,参数列只留会影响材质
	//    本身的字段 —— 预览只影响"看",不写进 .wmat。离屏目标 = **预览区物理像素**
	//    (与 render_scale 脱钩)。
	//
	// 相机手感与模型/预制体面板一致:左键轨道旋转、滚轮推拉、双击或 F 取景、上下方向已翻正。
	// 每个材质一个面板/窗口(id = "material:<path>"),可同时打开多个。
	class MaterialEditorPanel final : public EditorPanel
	{
	public:
		MaterialEditorPanel();
		explicit MaterialEditorPanel(std::string materialPath);
		~MaterialEditorPanel() override;

		const char* Id() const override { return m_PanelId.c_str(); }
		const char* Title() const override { return m_PanelTitle.c_str(); }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		// 由内容浏览器/Window 菜单调用:打开指定 .wmat(失败时面板显示错误而不是弹窗)。
		void OpenMaterial(const std::string& path);
		bool HasMaterial() const { return m_Material != nullptr; }
		const std::string& GetMaterialPath() const { return m_Path; }
		void SetMaterialPathForPanel(const std::string& path);
		// AI 控制通道:请求抓一张预览纹理(下一帧写盘);材质状态供 state.dump 读取。
		void RequestPreviewCapture(const std::string& path) { m_PendingPreviewCapture = path; }
		const Ref<Material>& GetMaterial() const { return m_Material; }

	private:
		// ---- 预览选项(只影响预览,不写进 .wmat)----
		enum class PreviewMesh : uint8_t { Sphere = 0, Cube = 1, Plane = 2 };
		enum class PreviewBackground : uint8_t { Solid = 0, Gradient = 1 };
		enum class PreviewLighting : uint8_t { ThreePoint = 0, Single = 1, None = 2 };

		// 校验区的一条问题(点击 = 展开并滚到对应字段)。
		struct ValidationEntry
		{
			std::string Field;    // 参数名(base / metallic / albedo / normal / …)
			std::string Text;     // 人类可读说明
			std::string Severity; // "missing" / "range" / "unreferenced"
		};

		// 参数区的一行(可编辑字段 / 只读信息行 / 动作行)。
		// Key 决定控件 id(material.<key>)、复位 id(material.prop.<key>.reset)与校验定位。
		struct RowPlan
		{
			std::string Group;      // 组 key(preview / base / detail / emissive / blend / sampling / advanced)
			std::string Key;        // 参数名
			std::string ControlId;  // 控件无障碍 id(与 Key 不同:name 用 material.prop.name)
			std::string Label;
			std::string Doc;
			std::string Value;      // 只读行显示值
			bool ReadOnly = false;
			bool Modified = false;
			bool HasReset = false;
			float Height = 26.0f;
		};

		// ---- 文档状态 ----
		std::string m_PanelId = "material:";
		std::string m_PanelTitle = "Material";
		Ref<Material> m_Material;
		std::string m_Path;                 // 当前材质路径(空 = 未落盘新材质)
		std::string m_NewPathBuffer;        // 未落盘时的目标路径输入
		bool m_NewPathAttempted = false;    // U2d:点过 Save 之后才把"路径不能为空"标成行内错误
		std::string m_Status;               // 最近一次操作结果
		bool m_StatusIsError = false;
		std::vector<std::string> m_MaterialPaths;
		std::vector<std::string> m_TexturePaths;
		int m_MaterialPickIndex = -1;
		int m_AlbedoPickIndex = 0;
		int m_NormalPickIndex = 0;
		double m_CatalogRefreshTime = 0.0;

		// ---- U21:参数区(搜索 / 分组折叠 / 定位)----
		std::string m_Search;               // 搜索框内容(参数名 / 分组 / 说明,不区分大小写)
		float m_ScrollY = 0.0f;             // 参数区滚动位置
		// 分组展开态(下标 = 组序 = kGroups 里材质参数分组的顺序):默认全展开,用户折叠后跨帧保持。
		// U23:预览组搬进预览区后,参数列只剩 6 个材质分组(kGroups 与这里必须同步)。
		bool m_SectionOpen[6] = { true, true, true, true, true, true };
		// 校验条目点击后要把目标字段滚进视野(行高是上一帧实测值,所以保持几帧)。
		std::string m_RevealField;
		int m_RevealFrames = 0;
		std::string m_NameBuffer;           // 高级组的"显示名"文本缓冲(回车/失焦提交)
		bool m_SyncNameBuffer = false;      // 打开/重载后把磁盘值重新灌进缓冲

		// ---- U21:校验区 ----
		std::vector<ValidationEntry> m_Validation;
		uint32_t m_ValidationRevision = 0;
		std::string m_ValidationPath;
		double m_ValidationTime = 0.0;

		// ---- U21:预览选项与读数 ----
		PreviewMesh m_PreviewMesh = PreviewMesh::Sphere;
		PreviewBackground m_PreviewBackground = PreviewBackground::Solid;
		PreviewLighting m_PreviewLighting = PreviewLighting::ThreePoint;
		float m_LightIntensity = 1.0f;      // 主光强度倍率(0..4)
		float m_LightAzimuth = 35.0f;       // 主光方位角(度,-180..180)
		float m_LightElevation = 45.0f;     // 主光仰角(度,-85..85)
		bool m_ShowWireframe = false;
		bool m_ShowNormals = false;
		bool m_ShowUvChecker = false;
		// 离屏目标 = 预览区物理像素(设计单位 × UiScale,长边 clamp);render_scale 不参与
		// (材质预览自建 framebuffer,不走 SceneRenderer::OnResize,所以天然与它脱钩)。
		uint32_t m_PreviewTargetW = 256;
		uint32_t m_PreviewTargetH = 256;
		float m_PreviewUiScale = 1.0f;
		float m_PreviewViewW = 0.0f;
		float m_PreviewViewH = 0.0f;
		// 预览:上一帧矩形(探针按它核对"目标 = 矩形物理像素")。
		Wui::WuiRect m_PreviewRect;
		// U23:预览区卡片矩形(预览设置与参数列的**分区**依据;探针按它断言归属)。
		Wui::WuiRect m_PreviewZoneRect;
		uint64_t m_PreviewTextureId = 0;
		uint32_t m_UiTextureGeneration = 0;
		// 预览纹理连续抓图(WLD_PREVIEW_TEX_CAPTURE):计数与写出张数,按面板各记一份。
		int m_PreviewCaptureFrame = 0;
		int m_PreviewCaptureWritten = 0;
		// AI 控制通道:请求把**本面板的预览纹理**写到 path(下一帧渲染后执行,
		// 走 RHI 读回,双后端有效)。
		std::string m_PendingPreviewCapture;
		void* m_GpuDevice = nullptr;        // 记录资源所属设备,设备重建时整体失效
		uint32_t m_GpuTargetW = 0;          // 已建 framebuffer 的目标尺寸(0 = 未建)
		uint32_t m_GpuTargetH = 0;
		bool m_Orbiting = false;
		float m_OrbitYaw = 0.6f;            // 弧度:绕 Y
		float m_OrbitPitch = 0.25f;         // 弧度:绕 X
		float m_CameraDistance = 3.0f;      // 预览相机距离(滚轮推拉)
		float m_CameraMinDistance = 0.6f;
		float m_CameraMaxDistance = 30.0f;
		float m_FocusDistance = 3.0f;       // 双击/F 取景距离(按网格尺寸算)
		glm::vec2 m_LastMouse { 0.0f };
		// U22:右下角坐标系指示器的矩形(用于登记 a11y 节点;方案 §5.5)。
		Wui::WuiRect m_AxisRect;
		// U22:预览设置分段标签(0=网格 / 1=背景 / 2=光照 / 3=显示)。会话内记住选中项。
		int m_PreviewTab = 0;

		// ---- GPU 资源(预览专用,惰性创建) ----
		Rhi::Handle<Rhi::RenderPass> m_PreviewPass;
		Rhi::Handle<Rhi::Framebuffer> m_PreviewFramebuffer;
		Rhi::Handle<Rhi::Texture> m_PreviewColor;
		Rhi::Handle<Rhi::Texture> m_PreviewEntityId;
		Rhi::Handle<Rhi::Texture> m_PreviewDepth;
		// P4-4b:rendering.msaa>1 时的多采样附件(颜色 / 实体 id / 深度)。上面三张单采样
		// 纹理保持原角色:WUI 采样纹理、抓图目标与 resolve 目标(m_PreviewColor 也是
		// WLD_PREVIEW_TEX_CAPTURE / capture.texture 读的那张)。
		Rhi::Handle<Rhi::Texture> m_PreviewColorMsaa;
		Rhi::Handle<Rhi::Texture> m_PreviewEntityMsaa;
		Rhi::Handle<Rhi::Texture> m_PreviewDepthMsaa;
		// P4-UX16b:预览命令缓冲按**帧槽位**各一份(索引 = Renderer::FrameSlot())。
		// 单缓冲 + 每帧重新录制会在"上一帧的提交还没完成"时 begin —— 实测开材质窗口后连续触发
		// VUID-vkBeginCommandBuffer-00049 / VUID-vkQueueSubmit-pCommandBuffers-00071,
		// 最终把设备打丢(device lost)。帧槽位复用与 Renderer::BeginFrame 的帧栅栏配套:
		// 同一槽位再次使用前,该帧的栅栏已经被等待过,缓冲必然已完成。
		Rhi::Handle<Rhi::CommandBuffer> m_PreviewCommands[Renderer::FramesInFlight];
		Rhi::Handle<Rhi::Buffer> m_PreviewCameraBuffer;
		Rhi::Handle<Rhi::DescriptorSet> m_PreviewCameraSet;
		// U21:预览灯光 UBO(set0 binding 2 = LightUniforms)。
		Rhi::Handle<Rhi::Buffer> m_PreviewLightBuffer;
		// U21:预览专用网格与覆盖材质(不落盘、不进资产库缓存;只被本面板引用)。
		Ref<Mesh> m_PreviewMeshes[3];
		Ref<Mesh> m_CheckLightMesh;         // UV 棋盘格:亮格
		Ref<Mesh> m_CheckDarkMesh;          // UV 棋盘格:暗格
		Ref<Mesh> m_WireMesh;               // 线框:沿每个三角形边的细带
		Ref<Mesh> m_NormalMesh;             // 法线:每个顶点一根三棱柱
		int m_DerivedMeshFor = -1;          // 派生网格属于哪个预览网格(-1 = 未构建)
		Ref<Material> m_OverrideLight;      // 纯色覆盖材质(棋盘格亮格)
		Ref<Material> m_OverrideDark;       // 纯色覆盖材质(棋盘格暗格)
		Ref<Material> m_OverrideWire;       // 纯色覆盖材质(线框)
		Ref<Material> m_OverrideNormal;     // 纯色覆盖材质(法线)

		void EnsureGpuResources();
		// defer = true:旧句柄交给引擎的延迟释放队列(下一轮该帧槽位开始前回收)。
		// 目标尺寸变化时必须走这一条 —— 在飞命令还引用着旧 framebuffer/renderpass/命令缓冲,
		// 立刻销毁会触发 VUID-vkDestroyFramebuffer-00892 / VUID-vkFreeCommandBuffers-00047
		// 并把设备打丢(实测 detach 后一次尺寸变化 → 0xC0000005)。
		void ReleaseGpuResources(bool defer = false);
		// 无障碍诊断:按 WLD_PREVIEW_TEX_CAPTURE + WLD_SCREEN_CAPTURE_START/_EVERY/_COUNT
		// 把预览纹理连续写成 PPM(Vulkan 下唯一能"看到"预览内容的路径)。
		void CapturePreviewTextureSequence();
		// 预览目标尺寸 = 预览区物理像素(每帧核对,只有真的变了才重建资源)。
		void UpdatePreviewTargetSize(const Wui::WuiRect& view);
		// 渲染预览到离屏目标;返回可交给 WuiImage 的纹理 id(0 = 不可用)。
		uint64_t RenderPreview();
		// 预览用网格 / 派生网格(线框 / 法线 / UV 棋盘格)与纯色覆盖材质。
		const Ref<Mesh>& PreviewMeshFor(PreviewMesh kind);
		void BuildDerivedMeshes();
		void EnsureOverrideMaterials();
		// 头部(材质名 + 路径 + 脏标记 + 动作);返回占用高度。
		float DrawHeader(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		// 参数区(搜索 + 分组 + 每字段控件 + 校验区);返回占用高度。
		float DrawParameters(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		// 一行参数:标签 + 控件 + 恢复默认 + 悬停说明 + 无障碍节点。
		void DrawParameterRow(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const RowPlan& row,
			float x, float y, float width, bool stacked);
		// 预览区(卡片:图像 + 相机操作 + 预览设置标签/控件 + "仅影响预览显示"标注 + 读数);
		// 传入矩形 = 分配到的区域,返回实际占用的高度。
		float DrawPreview(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		// 双击 / F 取景:回到按网格包围半径算出的默认距离。
		void FramePreview();
		// Reveal:在资源管理器里选中当前 .wmat(未落盘时给出可读状态)。
		void RevealMaterialOnDisk();
		bool FieldModified(const std::string& key) const;
		// 预览选项是否偏离内置默认(球 / 纯色 / 三点光 / 强度 1 / 方位 35 / 仰角 45 / 关闭)。
		bool PreviewOptionModified(const std::string& key) const;
		void SetFieldToDefault(const std::string& key);
		void ResetAllMaterialFields();
		int GroupModifiedCount(const std::string& groupKey) const;
		void RefreshValidation(double now);
		void SaveCurrent();
		void RefreshCatalog();
		void RefreshPickerIndices();
	};
}
