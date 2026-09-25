#pragma once

#include "EditorPanel.h"

#include "World/RHI/Rhi.h"
#include "World/Renderer/TextureArtifact.h"
#include "World/Renderer/TextureImportSettings.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace World
{
	// M4-TEX P4:纹理设置(Texture Settings)检查器 —— 编辑一个**纹理资产**(`.wtex`)。
	//
	// 口径(见 docs/dev/texture-import.md §1/§3/§8):
	//   源(`textures/Icon.png`) → 资产(`textures/Icon.wtex`:设置 + `source:`) → 产物(`<源图>.wtexc`)。
	//   设置的家只有 `.wtex` 一个(没有旁路 sidecar);源图永远不动。
	// 面板职责:改设置 → `Apply`(保存 `.wtex` + 就地重烘内容根里的 `<源图>.wtexc` + 失效材质贴图缓存)
	//   / `Reimport`(强制重烘)/ `Reset to Defaults`(删 `.wtex`,确认后回到默认并重烘)。
	// 预览:源图缩略图(直接读源文件的上传,不看产物 —— 用户要看的是"源长什么样")。
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

		// 就地在**内容根**重烘 `<源图>.wtexc`(开发态运行时能命中产物;`.wtexc` 已进 .gitignore),
		// 并失效该逻辑路径的材质贴图缓存(下一次 Get 重新读盘上传)。失败填 error(人话)。
		bool BakeTextureArtifactNow(const std::filesystem::path& contentRoot,
			const std::string& sourceLogical, const TextureImportSettings& settings, std::string& outError);

		// 为一张**源图**写一份最小 `.wtex`(内容 = `TextureImportSettings{}.Serialize()`:
		// 注释头 + `usage: color`,没有 `source:` —— 缺省就是同目录同主名)。已存在 = 失败
		// (不覆盖用户已有的资产)。outAssetLogical 回传资产逻辑路径(内容根相对)。
		bool CreateTextureAssetForSource(const std::filesystem::path& contentRoot,
			const std::string& sourceLogical, std::string* outAssetLogical, std::string& outError);

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
		// ---- 文档状态 ----
		std::filesystem::path m_ContentRoot;
		std::string m_AssetLogical;      // `textures/Icon.wtex`(空 = 还没打开过资产)
		std::string m_SourceLogical;     // 解析出的源图逻辑路径
		TextureImportSettings m_Settings; // 面板可编辑状态(Apply 才落盘)
		std::string m_SourceBuffer;      // `source:` 文本框缓冲(提交时写进 m_Settings.Source)
		bool m_Loaded = false;           // 至少有一次成功读盘(缺 `.wtex` 也算:那是默认设置)
		bool m_Dirty = false;            // 字段改过但还没 Apply
		bool m_AssetFileExists = false;  // `.wtex` 在盘上(决定 Reset to Defaults 是否可用)
		std::string m_LoadError;         // 资产读不出来时的可读原因(非空 = 面板进入只读错误态)
		std::string m_Status;            // 最近一次操作结果(Apply / Reimport / Reset)
		bool m_StatusIsError = false;

		// ---- 产物 / 源图读数 ----
		Editor::TextureArtifactStatus m_Artifact;
		// 产物判定的输入指纹:只有 (源图 / `.wtex` / `.wtexc`) 的 mtime 或"内存设置 hash"变了才重算 ——
		// 判定要读源字节算 sha256(4K 源几十 MB),每帧算一次不可接受。
		std::filesystem::file_time_type m_ArtifactSourceStamp {};
		std::filesystem::file_time_type m_ArtifactAssetStamp {};
		std::filesystem::file_time_type m_ArtifactArtifactStamp {};
		uint64_t m_ArtifactSettingsHash = 0;
		bool m_ArtifactStampsValid = false;
		bool m_ArtifactArtifactExists = false;
		uint32_t m_SourceWidth = 0;
		uint32_t m_SourceHeight = 0;
		bool m_SourceValid = false;
		std::string m_SourceStatsPath;   // 上面两个数属于哪张源图(换源立刻重读)
		// 源图解码结果(只为尺寸读数解码一次;缩略图上传成功后立刻释放,避免 4K 源长期占内存)。
		std::vector<uint8_t> m_SourcePixels;
		std::string m_SourcePixelsSource;
		std::filesystem::file_time_type m_AssetStamp {};
		bool m_AssetStampValid = false;
		bool m_DiskChanged = false;      // 磁盘上 `.wtex` 变了、但面板有未保存改动(只提示不覆盖)

		// ---- 预览缩略图(源图 → RHI 纹理 → WUI 注册表)----
		Rhi::Handle<Rhi::Texture> m_Thumbnail;
		uint64_t m_ThumbnailId = 0;
		uint32_t m_ThumbnailGeneration = ~0u;   // WuiTextureRegistry::Generation() 快照
		uint32_t m_ThumbnailEpoch = ~0u;        // PanelHost::TextureEpoch() 快照(GL 上下文重建)
		std::string m_ThumbnailSource;          // 当前缩略图属于哪张源图

		// ---- 交互状态 ----
		float m_Scroll = 0.0f;
		bool m_ResetConfirmOpen = false;        // "回到默认设置"确认框
		bool m_PendingResetConfirm = false;     // 由 RequestOpenAsset(…, true) 置位,帧内弹框
		uint64_t m_LastRenderFrame = 0;
		bool m_RenderedOnce = false;
		Wui::WuiContext* m_Ctx = nullptr;       // 本帧上下文(操作记录用;帧内有效)

		// 帧内管线:读盘/刷新状态 → 画界面。
		void EnsureContentRoot();
		// 处理"换资产 / 面板从隐藏回到可见 / 磁盘 `.wtex` 变化"三种重读时机。
		void SyncWithDisk(const Wui::WuiContext& ctx);
		void ReloadFromDisk(bool keepStatus);
		void RefreshArtifactState(bool force = false);
		void RefreshSourceStats(const std::string& absoluteSource);
		void EnsureThumbnail(PanelHost& host, const std::string& absoluteSource);
		void ReleaseThumbnail();
		// Apply / Reimport 共用的唯一落盘 + 重烘路径(force = 忽略"没有变化"的短路)。
		void SaveAndBake(bool force);
		void ResetAssetToDefaults();
		// 状态行文本(源尺寸 / 产物格式尺寸 mip / 需重烘)。
		std::string ArtifactSummary() const;
		std::string SourceAbsolutePath() const;
		std::string ArtifactAbsolutePath() const;
		std::string AssetAbsolutePath() const;

		void DrawHeader(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, PanelHost& host,
			const Wui::WuiRect& rect, float& y);
		void DrawPreview(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const Wui::WuiRect& rect);
		void DrawFields(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const Wui::WuiRect& rect,
			float& y);
		void DrawActions(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, PanelHost& host,
			const Wui::WuiRect& rect);
		void DrawStatusLine(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const Wui::WuiRect& rect,
			float& y);
		void DrawResetConfirmModal(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, PanelHost& host);
		// 一行"标签 + 控件"的几何(返回本行高度);统一字段列的排版口径。
		float FieldRowHeight(const Wui::WuiTheme& theme) const;
	};
}
