#pragma once

#include "EditorPanel.h"

#include "World/RHI/Rhi.h"
#include "World/Renderer/TextureArtifact.h"
#include "World/Renderer/TextureImportSettings.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace World
{
	// M4-TEX P4/P5:纹理设置(Texture Settings)检查器 —— 编辑一个**纹理资产**(`.wtex`)。
	//
	// 口径(见 docs/dev/texture-import.md §1/§3/§8):
	//   源(`textures/Icon.png`) → 资产(`textures/Icon.wtex`:设置 + `source:`) → 产物(`<主名>.wtexc`)。
	//   设置的家只有 `.wtex` 一个(没有旁路 sidecar);源图永远不动。
	// 面板职责:改设置 → `Apply`(保存 `.wtex` + 就地重烘内容根里的 `<主名>.wtexc` + 失效材质贴图缓存)
	//   / `Reimport`(强制重烘)/ `Reset to Defaults`(删 `.wtex`,确认后回到默认并重烘)。
	//
	// P5 布局与实时预览(用户 2026-09-25):
	//   * 左主区 = 预览(可滚轮缩放 / 拖拽平移 / `Fit` / `1:1` / RGB·R·G·B·A 通道开关);
	//   * 右窄列 = 属性(源图路径框 + 在资源管理器中显示 + 11 个设置字段;列宽 240–300);
	//   * **预览来自资产 + 设置**,不是磁盘 PNG 直显:
	//       - 立即路径(草稿纹理,CPU):解码一次源图 → 按 max_size 等比缩放 → FlipY / 预乘 /
	//         通道掩码 → RGBA8 上传(长边 ≤ 1024,状态行标注"capped");
	//       - 产物路径(真编码,300–500ms 防抖,**工作线程**烘):`TextureCompiler::BakeFile` →
	//         产物按**原生块格式**(RGBA8/BC7/BC5/BC4/BC1/BC3)直接上 GPU(压缩效果可见);
	//         烘失败保留草稿预览(不变黑、不阻塞 UI)+ 状态行显示失败原因。
	//     通道视图(单通道灰度)天然只能来自解压后的像素,统一走草稿路径并在状态行说明。
	// 陈旧判定:现场算 源 sha256 + 设置 hash,与产物头比对("需重烘"/"已烘焙")。
	namespace Editor
	{
		// 产物状态:面板状态行与内容浏览器徽标共用这一份判定(唯一口径)。
		enum class TextureArtifactState : uint8_t
		{
			NoSource = 0,   // 找不到源图(资产/同主名图片都没有)
			NoArtifact,     // 明细上还没有 `.wtexc`(按默认/资产设置烘一次即可)
			Stale,          // 产物在场,但源字节或设置 hash 与产物头不一致(需重烘)
			Fresh,          // 产物与"这份源 + 这份设置"一致
			Invalid,        // 源/资产/产物读不出来(原因见 Detail)
		};

		struct TextureArtifactStatus
		{
			TextureArtifactState State = TextureArtifactState::NoArtifact;
			TextureArtifactHeader Header {};
			std::string SourceLogical;   // 解析出的源图逻辑路径(空 = 没找到)
			std::string Detail;          // 人话(错误原因;没有错误时为空)
		};

		// 资产/源图逻辑路径 → 源图逻辑路径。口径与 TextureCompiler::BakeDirectory 完全一致:
		// 显式 `source:` 优先(内容根相对、不许绝对路径 / `..`),否则同目录同主名的图片
		// (按 .png/.jpg/.jpeg/.tga/.bmp 找第一个存在的)。
		bool ResolveTextureSourceLogical(const std::filesystem::path& contentRoot,
			const std::string& logicalPath, const TextureImportSettings& settings,
			std::string& outSourceLogical, std::string& outError);

		// 现场判定产物是否与"源 + 设置"一致(读源字节算 sha256,与产物头的 sourceSha256/settingsHash 比对)。
		// settingsOverride 非空 = 用调用方的内存设置(面板的可编辑状态);否则从 `.wtex` 读盘
		// (缺资产 = 默认设置)。
		TextureArtifactStatus InspectTextureArtifact(const std::filesystem::path& contentRoot,
			const std::string& sourceLogical, const TextureImportSettings* settingsOverride);

		// 就地在**内容根**重烘 `<源图主名>.wtexc`(开发态运行时能命中产物;`.wtexc` 已进 .gitignore),
		// 并失效该逻辑路径的材质贴图缓存(下一次 Get 重新读盘上传)。失败填 error(人话)。
		bool BakeTextureArtifactNow(const std::filesystem::path& contentRoot,
			const std::string& sourceLogical, const TextureImportSettings& settings, std::string& outError);

		// 为一张**源图**写一份最小 `.wtex`(内容 = `TextureImportSettings{}.Serialize()`:
		// 注释头 + `usage: color`,没有 `source:` —— 缺省就是同目录同主名)。已存在 = 失败
		// (不覆盖用户已有的资产)。outAssetLogical 回传资产逻辑路径(内容根相对)。
		bool CreateTextureAssetForSource(const std::filesystem::path& contentRoot,
			const std::string& sourceLogical, std::string* outAssetLogical, std::string& outError);

		// M4-TEX P5:双击源图 / 拖放导入 / 面板共用的"确保资产存在"入口。已存在 = 成功
		// (created=false,不覆盖);缺资产则写一份最小 `.wtex`。失败填 error(人话)。
		bool EnsureTextureAssetForSource(const std::filesystem::path& contentRoot,
			const std::string& sourceLogical, std::string* outAssetLogical, bool* outCreated,
			std::string& outError);

		// M4-TEX P5:`source:` 文本框的行内校验(内容根相对、不许 `..`/绝对路径、扩展名受支持、
		// 文件必须存在)。text 为空 = 合法(缺省 = 同目录同主名)。非法时 error 给可读原因,
		// 调用方**不得落盘**(与"非法输入给行内错误"同一口径)。
		bool ValidateTextureSourceText(const std::filesystem::path& contentRoot, const std::string& text,
			std::string& outError);

		// 面板间"打开纹理设置"的窄通道(与 Editor::AssetDropBridge 同一口径:两个面板之间的协作
		// 走编辑器级单例,不为此扩 PanelHost 接口)。内容浏览器写请求,EditorShell 每帧取走一次
		// 并打开面板 —— 与 Window 菜单 / AI 通道 `ui.open` 落到同一条开关路径。
		class TextureSettingsRequests
		{
		public:
			enum class Kind : uint8_t
			{
				Open = 0,        // 打开(选中该资产)
				ResetDefaults,   // 打开并弹"回到默认设置(删 .wtex)"确认框
			};

			static TextureSettingsRequests& Get();

			// 请求打开某个 `.wtex` 资产(或它的源图逻辑路径;两者都接受)。
			void Request(const std::string& logicalPath, Kind kind);
			// 取走一次(取走后清空);没有请求返回 false。
			bool Take(std::string& outLogicalPath, Kind& outKind);

		private:
			std::string m_Logical;
			Kind m_Kind = Kind::Open;
			bool m_Pending = false;
		};
	}

	class TextureSettingsPanel final : public EditorPanel
	{
	public:
		// 预览通道开关:RGB = 合成;R/G/B/A = 单通道灰度(用灰度显示 + 状态行说明)。
		enum class PreviewChannel : uint8_t { Rgb = 0, Red, Green, Blue, Alpha };

		TextureSettingsPanel();
		~TextureSettingsPanel() override;

		const char* Id() const override { return "texture_settings"; }
		const char* Title() const override { return "Texture Settings"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		// 打开一个纹理资产(逻辑路径 = `.wtex`,或它的源图 —— 源图会先换算成资产路径)。
		// resetConfirm = 顺带弹出"回到默认设置(删 .wtex)"确认框(内容浏览器右键入口)。
		void RequestOpenAsset(const std::string& logicalPath, bool resetConfirm = false);
		const std::string& AssetLogicalPath() const { return m_AssetLogical; }

	private:
		// 预览状态(状态行 + 无障碍节点 texture.preview.state 的取值)。
		enum class PreviewState : uint8_t
		{
			Idle = 0,    // 没有需要烘的东西(产物最新)
			Pending,     // 防抖计时中(设置刚改)
			Encoding,    // 工作线程正在烘
			Baked,       // 预览 = 当前设置的产物
			Failed,      // 烘焙失败(继续显示草稿,原因在 Detail)
		};

		// 一张预览纹理槽(草稿 / 产物各一份)。Id 是 WUI 注册表的**稳定**槽位,换内容走 Update。
		struct PreviewSlot
		{
			Rhi::Handle<Rhi::Texture> Texture;
			uint64_t Id = 0;
			uint32_t RegistryGeneration = ~0u;
			uint32_t HostEpoch = ~0u;
			std::string SourceLogical;     // 属于哪张源图
			uint32_t Width = 0;
			uint32_t Height = 0;
			std::string FormatName;        // "rgba8" / "bc7" / "bc5" …
			uint32_t MipCount = 0;
			bool VerticalFlip = false;     // 纹理数据第 0 行 = 图像**底**部(产物 FlipY=true)
			bool Capped = false;           // 草稿被压到 ≤ 1024(状态行据此标注)
			bool FromArtifact = false;     // 这张槽位来自产物(原生块格式)还是 CPU 草稿
			int Channel = 0;               // 这张纹理烘出来的通道(0 = RGB;草稿才有单通道)
			uint64_t Revision = 0;         // 烘它时用的设置版本(产物预览是否跟得上当前设置)
			bool Valid = false;
		};

		// ---- 工作线程请求 / 结果(只经 mutex 交换;工作线程不碰任何 UI/渲染状态)----
		struct PreviewBakeRequest
		{
			uint64_t Serial = 0;           // = 派发时的 m_PreviewRevision
			std::string AbsoluteSource;
			TextureImportSettings Settings;
		};
		struct PreviewBakeResult
		{
			uint64_t Serial = 0;
			bool Success = false;
			std::string Error;
			std::vector<uint8_t> Bytes;
			double ElapsedMs = 0.0;
		};

		// ---- 文档状态 ----
		std::filesystem::path m_ContentRoot;
		std::string m_AssetLogical;      // `textures/Icon.wtex`(空 = 还没打开过资产)
		std::string m_SourceLogical;     // 解析出的源图逻辑路径
		TextureImportSettings m_Settings; // 面板可编辑状态(Apply 才落盘)
		std::string m_SourceBuffer;      // `source:` 文本框缓冲(提交时写进 m_Settings.Source)
		std::string m_SourceError;       // `source:` 行内错误(非空 = 不落盘)
		bool m_Loaded = false;           // 至少有一次成功读盘(缺 `.wtex` 也算:那是默认设置)
		bool m_Dirty = false;            // 字段改过但还没 Apply
		bool m_AssetFileExists = false;  // `.wtex` 在盘上(决定 Reset to Defaults 是否可用)
		std::string m_LoadError;         // 资产读不出来时的可读原因(非空 = 面板进入只读错误态)
		std::string m_Status;            // 最近一次操作结果(Apply / Reimport / Reset)
		bool m_StatusIsError = false;

		// ---- 产物 / 源图读数(旧口径:状态行与徽标)----
		Editor::TextureArtifactStatus m_Artifact;
		std::filesystem::file_time_type m_ArtifactSourceStamp {};
		std::filesystem::file_time_type m_ArtifactAssetStamp {};
		std::filesystem::file_time_type m_ArtifactArtifactStamp {};
		uint64_t m_ArtifactSettingsHash = 0;
		bool m_ArtifactStampsValid = false;
		bool m_ArtifactArtifactExists = false;
		uint32_t m_SourceWidth = 0;         // **真实**源图尺寸(预览可能被压过)
		uint32_t m_SourceHeight = 0;
		std::filesystem::file_time_type m_AssetStamp {};
		bool m_AssetStampValid = false;
		bool m_DiskChanged = false;      // 磁盘上 `.wtex` 变了、但面板有未保存改动(只提示不覆盖)

		// ---- 实时预览:解码一次的源像素(长边 ≤ kPreviewSourceMaxEdge)----
		std::string m_PreviewSourceLogical;
		std::vector<uint8_t> m_PreviewSourcePixels;   // RGBA8,第 0 行 = 图像**顶部**(视觉序)
		uint32_t m_PreviewSourceWidth = 0;
		uint32_t m_PreviewSourceHeight = 0;
		bool m_PreviewSourceValid = false;
		std::filesystem::file_time_type m_PreviewSourceStamp {};
		bool m_PreviewSourceStampValid = false;

		// ---- 预览显示态 ----
		PreviewChannel m_Channel = PreviewChannel::Rgb;
		float m_ZoomFactor = 1.0f;       // 相对 fit(1 = fit)
		float m_ZoomAbsolute = 1.0f;     // 本帧实际"屏幕像素 / 纹素"(状态行与 a11y 读它)
		float m_CenterU = 0.5f;          // 可见区中心(归一化,视觉坐标 V 向下)
		float m_CenterV = 0.5f;
		bool m_PanActive = false;
		glm::vec2 m_PanLast { 0.0f, 0.0f };
		PreviewSlot m_Draft;
		PreviewSlot m_ArtifactPreview;
		PreviewSlot m_Shown;             // 本帧真正画的那张(帧内快照)
		bool m_DraftDirty = true;
		uint32_t m_PreviewGeneration = ~0u;   // WuiTextureRegistry::Generation() 快照
		uint32_t m_PreviewEpoch = ~0u;        // PanelHost::TextureEpoch() 快照(GL 上下文重建)
		uint32_t m_HostTextureEpoch = 0;      // 本帧宿主的 TextureEpoch(登记槽位时记进 slot)

		// ---- 预览重烘(防抖 + 工作线程)----
		uint64_t m_PreviewRevision = 0;   // 任何影响产物的改动 +1
		uint64_t m_BakedRevision = 0;     // 当前产物预览对应的版本(0 = 还没有)
		double m_BakeDueSeconds = 0.0;    // 0 = 没有待烘的防抖计时
		bool m_BakeInFlight = false;      // 主线程视角:工作线程正在跑
		bool m_LoadDiskArtifactRequested = false;   // 打开/重载后先把盘上的新鲜产物读进预览
		PreviewState m_PreviewState = PreviewState::Idle;
		std::string m_PreviewDetail;      // 失败原因 / 说明(状态行 tooltip)
		uint64_t m_BakeSerial = 0;        // 已派发的最后一个序号(日志用)
		std::thread m_BakeThread;
		std::mutex m_BakeMutex;
		std::condition_variable m_BakeCv;
		bool m_BakeThreadStop = false;
		bool m_BakeRequestPending = false;
		PreviewBakeRequest m_BakeRequest;
		PreviewBakeResult m_BakeResult;
		bool m_BakeResultReady = false;

		// ---- 交互状态(旧口径)----
		float m_Scroll = 0.0f;
		bool m_ResetConfirmOpen = false;
		bool m_PendingResetConfirm = false;
		uint64_t m_LastRenderFrame = 0;
		bool m_RenderedOnce = false;
		Wui::WuiContext* m_Ctx = nullptr;

		// ---- 帧内管线:读盘/刷新状态 → 画界面 ----
		void EnsureContentRoot();
		void SyncWithDisk(const Wui::WuiContext& ctx);
		void ReloadFromDisk(bool keepStatus);
		void RefreshArtifactState(bool force = false);
		// 源像素解码(一次)+ mtime 检测;源变了 → 设置版本 +1(产物需重烘)。
		void RefreshPreviewSource(const std::string& absoluteSource);
		// 立即路径:草稿纹理(缩放 / FlipY / 预乘 / 通道掩码 → RGBA8 上传)。
		void EnsureDraftPreview();
		// 防抖 + 工作线程派发 + 结果取回(每帧一次)。
		void PumpPreviewBake(const std::string& absoluteSource);
		void DispatchPreviewBake(const std::string& absoluteSource);
		void PreviewBakeWorkerLoop();
		// 产物字节(内存里的重烘结果 / 盘上产物)→ 原生块格式上 GPU。
		bool UploadArtifactPreview(const std::vector<uint8_t>& bytes, uint64_t revision);
		bool UploadArtifactFromDisk();
		void UploadRgba8Draft(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height,
			bool capped);
		// 把一张新纹理挂到槽位:注册表整表清空(设备重建)时重新登记,否则就地 Update 稳定 id;
		// 旧句柄走 Renderer::QueueRelease(与材质面板同一口径)。
		void PublishPreviewSlot(PreviewSlot& target, const PreviewSlot& incoming);
		void ReleasePreviewSlot(PreviewSlot& slot);
		void InvalidatePreviewTextures();
		void SelectShownPreview();
		// 任何影响产物的改动(字段 / 源图 / 换资产)走这一条:标脏 + 版本 +1 + 重排防抖。
		void TouchPreviewRevision();
		void MarkSettingsDirty(const char* field);
		// Apply / Reimport 共用的唯一落盘 + 重烘路径(force = 忽略"没有变化"的短路)。
		void SaveAndBake(bool force);
		void ResetAssetToDefaults();
		// 状态行文本。
		std::string ArtifactSummary() const;
		std::string PreviewSummary() const;
		std::string PreviewStateText() const;
		std::string SourceAbsolutePath() const;
		std::string ArtifactAbsolutePath() const;
		std::string AssetAbsolutePath() const;

		void DrawHeader(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, PanelHost& host,
			const Wui::WuiRect& rect, float& y);
		// 左主区:通道开关 + Fit/1:1 + 可缩放平移的预览 + 预览/产物状态行。
		void DrawPreviewArea(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, PanelHost& host,
			const Wui::WuiRect& rect);
		void DrawPreviewImage(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const Wui::WuiRect& rect);
		void DrawPreviewStatus(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
			const Wui::WuiRect& rect, float& y);
		// 右窄列:源图路径框(+ 在资源管理器中显示)+ 11 个设置字段 + 状态行(可滚动)。
		void DrawPropertiesColumn(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, PanelHost& host,
			const Wui::WuiRect& rect);
		float DrawSourceRow(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const Wui::WuiRect& rect,
			float y);
		float DrawFields(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const Wui::WuiRect& rect,
			float y);
		float DrawStatusLine(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const Wui::WuiRect& rect,
			float y);
		void DrawActions(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, PanelHost& host,
			const Wui::WuiRect& rect);
		void DrawResetConfirmModal(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, PanelHost& host);
		// 一行"标签 + 控件"的几何(返回本行高度);统一字段列的排版口径。
		float FieldRowHeight(const Wui::WuiTheme& theme) const;
	};
}
