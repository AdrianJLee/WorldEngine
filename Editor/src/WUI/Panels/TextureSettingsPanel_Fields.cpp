#include "TextureSettingsPanel_Internal.h"

namespace World
{

using namespace TextureSettingsPanelDetail;


float TextureSettingsPanel::DrawStatusLine(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const Wui::WuiRect& rect, float y){
		const float x = rect.X;
		const float width = std::max(0.0f, rect.W);
		Wui::Label(ctx, { x, y }, EllipsizeToWidth(ctx, ArtifactSummary(), width, theme.FontSizeSmall),
			m_Artifact.State == Editor::TextureArtifactState::Fresh ? theme.Text : theme.Warning,
			theme.FontSizeSmall);
		y += theme.FontSizeSmall + 3.0f;

		if (m_Dirty)
		{
			Wui::Label(ctx, { x, y },
				EllipsizeToWidth(ctx,
					Wui::Tr("panel.texture.status.dirty",
						"Edited — Apply saves the .wtex and re-bakes the artifact."),
					width, theme.FontSizeSmall),
				theme.Accent, theme.FontSizeSmall);
			y += theme.FontSizeSmall + 3.0f;
		}
		if (m_DiskChanged)
		{
			Wui::Label(ctx, { x, y },
				EllipsizeToWidth(ctx,
					Wui::Tr("panel.texture.status.disk_changed",
						"The .wtex changed on disk — close and reopen this panel to reload it (your edits "
						"were kept)."),
					width, theme.FontSizeSmall),
				theme.Warning, theme.FontSizeSmall);
			y += theme.FontSizeSmall + 3.0f;
		}
		if (!m_Status.empty())
		{
			Wui::Label(ctx, { x, y },
				EllipsizeToWidth(ctx, m_Status, width, theme.FontSizeSmall),
				m_StatusIsError ? theme.Danger : theme.Success, theme.FontSizeSmall);
			y += theme.FontSizeSmall + 3.0f;
		}
		return y;
	}


void TextureSettingsPanel::DrawActions(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, PanelHost& host, const Wui::WuiRect& rect){
		(void)host;
		Wui::DrawPanelSurface(ctx, rect, theme);
		const float gap = theme.PadSmall;
		const float buttonH = rect.H - theme.Pad * 2.0f;
		const float buttonW = std::max(70.0f, (rect.W - theme.Pad * 2.0f - gap * 2.0f) / 3.0f);
		const float buttonY = rect.Y + theme.Pad;
		const Wui::WuiRect applyRect { rect.X + theme.Pad, buttonY, buttonW, buttonH };
		const Wui::WuiRect reimportRect { applyRect.X + buttonW + gap, buttonY, buttonW, buttonH };
		const Wui::WuiRect resetRect { reimportRect.X + buttonW + gap, buttonY,
			std::max(60.0f, rect.W - theme.Pad * 2.0f - (buttonW + gap) * 2.0f), buttonH };
		if (Wui::ButtonEx(ctx, Wui::HashId("texture.apply"), applyRect,
				Wui::Tr("panel.texture.apply", "Apply"), theme, true, true,
				Wui::Tr("panel.texture.apply.tooltip",
					"Save the .wtex asset and re-bake <stem>.wtexc in the content root (the live artifact "
					"the running game reads), then flush the material texture cache.")))
			SaveAndBake(false);
		if (Wui::ButtonEx(ctx, Wui::HashId("texture.reimport"), reimportRect,
				Wui::Tr("panel.texture.reimport", "Reimport"), theme, true, false,
				Wui::Tr("panel.texture.reimport.tooltip",
					"Force a re-bake from the current settings (same as Apply, ignoring the up-to-date "
					"short-circuit).")))
			SaveAndBake(true);
		if (Wui::ButtonEx(ctx, Wui::HashId("texture.reset"), resetRect,
				Wui::Tr("panel.texture.reset", "Reset to Defaults"), theme, m_AssetFileExists, false,
				m_AssetFileExists
					? Wui::Tr("panel.texture.reset.tooltip",
						"Delete this .wtex asset: the source image goes back to the engine defaults "
						"(usage = color). The artifact is re-baked with those defaults.")
					: Wui::Tr("panel.texture.reset.tooltip.none",
						"Nothing to reset: this asset has no .wtex file (defaults are already in effect).")))
		{
			m_ResetConfirmOpen = true;
			ctx.SetModal(Wui::HashId("texture.reset.modal"));
			host.SetPanelModalOwner(Id());
			ctx.RecordOp("texture", "reset-ask", m_AssetLogical, "confirm delete .wtex");
		}
	}


void TextureSettingsPanel::SaveAndBake(bool force){
		if (m_AssetLogical.empty())
			return;
		EnsureContentRoot();

		// 导入源(可选)先在**这里**校验:非法 / 不存在 → 行内错误 + 不落盘(不改 .wtex、不重烘)。
		// M4-TEX P9:容器不依赖它(留空 = 用内嵌 payload);旧式文件(没有 payload)才必须有源图。
		const std::string trimmedSource = TrimAscii(m_SourceBuffer);
		std::string sourceTextError;
		if (!Editor::ValidateTextureSourceText(m_ContentRoot, trimmedSource, sourceTextError))
		{
			m_SourceError = sourceTextError;
			m_Status = Wui::TrFormat("panel.texture.status.source_invalid",
				"Cannot apply: fix the source path first ({detail})",
				{ { "detail", sourceTextError } });
			m_StatusIsError = true;
			if (m_Ctx)
				m_Ctx->RecordOp("texture", "apply-rejected", m_AssetLogical, sourceTextError);
			return;
		}
		m_SourceError.clear();
		const bool sourceChanged = m_Settings.Source != trimmedSource;
		m_Settings.Source = trimmedSource;
		m_SourceBuffer = trimmedSource;
		if (sourceChanged)
			TouchPreviewRevision();

		// 1) 定 payload(一张纹理 = 一个文件 = 设置头 + 内嵌源字节):
		//    * 已有容器 payload → **逐字节保持**(只改设置不碰 payload);
		//    * 导入源这一行被改过(或还没有 payload)→ 用该文件的原始字节(同一张图重导入 = 同字节);
		//    * 两者都没有 → 没有可内嵌的源,给可读错误(不再写旧式文件)。
		std::vector<uint8_t> payload = m_Payload;
		const bool sourceEdited = trimmedSource != m_LoadedSourceText;
		if (!trimmedSource.empty() && (payload.empty() || sourceEdited))
		{
			const std::filesystem::path imported = m_ContentRoot / std::filesystem::path(trimmedSource);
			std::vector<uint8_t> bytes;
			std::string readError;
			if (!ReadFileBytes(imported, bytes, readError) || bytes.empty())
			{
				m_Status = Wui::TrFormat("panel.texture.status.import_failed",
					"Cannot import '{path}': {detail}",
					{ { "path", trimmedSource }, { "detail", readError.empty()
							? std::string("empty file") : readError } });
				m_StatusIsError = true;
				m_SourceError = readError;
				if (m_Ctx)
					m_Ctx->RecordOp("texture", "apply-failed", m_AssetLogical, readError);
				return;
			}
			payload = std::move(bytes);
		}
		if (payload.empty())
		{
			m_Status = Wui::Tr("panel.texture.status.no_bytes",
				"Cannot apply: this asset has no embedded source bytes yet — set an import source "
				"(.png/.jpg/.jpeg/.tga/.bmp) or re-import the image, then Apply again.");
			m_StatusIsError = true;
			if (m_Ctx)
				m_Ctx->RecordOp("texture", "apply-rejected", m_AssetLogical, "no embedded payload");
			return;
		}

		// 2) 保存资产 = **单文件容器**(头 + `---payload` + 源字节;原子替换)。
		std::string writeError;
		TextureAssetFile container;
		container.Settings = m_Settings;
		container.Payload = payload;
		if (!SaveTextureAssetFile(std::filesystem::path(AssetAbsolutePath()), container, writeError))
		{
			m_Status = Wui::TrFormat("panel.texture.status.save_failed", "Cannot save the asset: {detail}",
				{ { "detail", writeError } });
			m_StatusIsError = true;
			if (m_Ctx)
				m_Ctx->RecordOp("texture", "apply-failed", m_AssetLogical, writeError);
			return;
		}
		m_Payload = std::move(payload);
		m_Container = true;
		m_LegacyAsset = false;
		m_LoadedSourceText = trimmedSource;
		// 字节来源 = 资产自身(容器);外部源图只是可选的导入源。
		m_SourceLogical = m_AssetLogical;
		m_AssetFileExists = true;
		m_Dirty = false;
		m_DiskChanged = false;
		if (const std::filesystem::path assetFile = AssetAbsolutePath(); !assetFile.empty())
		{
			std::error_code stampError;
			const std::filesystem::file_time_type stamp =
				std::filesystem::last_write_time(assetFile, stampError);
			m_AssetStampValid = !stampError;
			if (!stampError)
				m_AssetStamp = stamp;
		}

		// 3) 就地重烘 `<主名>.wtexc`(内容根)+ 失效材质贴图缓存。
		std::string bakeError;
		if (!Editor::BakeTextureArtifactNow(m_ContentRoot, m_AssetLogical, m_Settings, bakeError))
		{
			m_Status = Wui::TrFormat("panel.texture.status.bake_failed", "Bake failed: {detail}",
				{ { "detail", bakeError } });
			m_StatusIsError = true;
			if (m_Ctx)
				m_Ctx->RecordOp("texture", "bake-failed", m_AssetLogical, bakeError);
			RefreshArtifactState(true);
			return;
		}
		m_Status = Wui::TrFormat("panel.texture.status.applied",
			"Saved {asset} and baked {artifact}", { { "asset", m_AssetLogical },
				{ "artifact", std::filesystem::path(ArtifactAbsolutePath()).filename().generic_string() } });
		m_StatusIsError = false;
		// 形态说明 + 盘上的可选导入源(容器本身不依赖它)。
		Editor::TextureSourceResolution resolution;
		m_ImportSourceLogical.clear();
		if (Editor::ResolveTextureSource(m_ContentRoot, m_AssetLogical, m_Settings, resolution))
			m_ImportSourceLogical = resolution.ImportSourceLogical;
		m_AssetFormNote = Wui::TrFormat("panel.texture.form.container",
			"Single-file asset: {bytes} bytes embedded; the import source is optional.",
			{ { "bytes", std::to_string(m_Payload.size()) } });
		RefreshArtifactState(true);
		// 3) 刚写出的产物直接当预览(不必等防抖的第二次烘)。
		if (UploadArtifactFromDisk())
		{
			m_BakedRevision = m_PreviewRevision;
			m_PreviewState = PreviewState::Baked;
			m_PreviewDetail.clear();
		}
		if (m_Ctx)
			m_Ctx->RecordOp("texture", force ? "reimport" : "apply", m_AssetLogical,
				TextureBlockFormatName(m_Artifact.Header.Format));
	}


void TextureSettingsPanel::ResetAssetToDefaults(){
		if (m_AssetLogical.empty())
			return;
		EnsureContentRoot();
		const std::filesystem::path assetFile = AssetAbsolutePath();
		std::error_code removeError;
		const bool removed = std::filesystem::remove(assetFile, removeError);
		m_Settings = TextureImportSettings {};
		m_Payload.clear();
		m_Container = false;
		m_LegacyAsset = false;
		m_ImportSourceLogical.clear();
		m_AssetFormNote.clear();
		m_SourceBuffer.clear();
		m_LoadedSourceText.clear();
		m_SourceError.clear();
		m_AssetFileExists = false;
		m_AssetStampValid = false;
		m_Dirty = false;
		m_DiskChanged = false;
		if (!removed && removeError)
		{
			m_Status = Wui::TrFormat("panel.texture.status.reset_failed", "Cannot delete the asset: {detail}",
				{ { "detail", removeError.message() } });
			m_StatusIsError = true;
			return;
		}
		TouchPreviewRevision();
		std::string resolveError;
		std::string source;
		if (!Editor::ResolveTextureSourceLogical(m_ContentRoot, m_AssetLogical, m_Settings, source,
				resolveError))
		{
			m_SourceLogical.clear();
			m_Status = Wui::TrFormat("panel.texture.status.reset_no_source",
				"Deleted the asset, but no source image: {detail}", { { "detail", resolveError } });
			m_StatusIsError = true;
			RefreshArtifactState(true);
			return;
		}
		m_SourceLogical = source;
		std::string bakeError;
		if (!Editor::BakeTextureArtifactNow(m_ContentRoot, m_SourceLogical, m_Settings, bakeError))
		{
			m_Status = Wui::TrFormat("panel.texture.status.bake_failed", "Bake failed: {detail}",
				{ { "detail", bakeError } });
			m_StatusIsError = true;
			RefreshArtifactState(true);
			return;
		}
		m_Status = Wui::TrFormat("panel.texture.status.reset",
			"Deleted {asset}: the source is back to the default settings and was re-baked.",
			{ { "asset", m_AssetLogical } });
		m_StatusIsError = false;
		RefreshArtifactState(true);
		if (UploadArtifactFromDisk())
		{
			m_BakedRevision = m_PreviewRevision;
			m_PreviewState = PreviewState::Baked;
			m_PreviewDetail.clear();
		}
		if (m_Ctx)
			m_Ctx->RecordOp("texture", "reset", m_AssetLogical, "deleted .wtex + rebaked defaults");
	}


void TextureSettingsPanel::DrawResetConfirmModal(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, PanelHost& host){
		const Wui::WuiId modalId = Wui::HashId("texture.reset.modal");
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("panel.texture.reset.title", "Reset to default settings?");
		frameDesc.Size = { 520.0f, 210.0f };
		Wui::WuiRect frame;
		bool escapePressed = false;
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, theme))
		{
			m_ResetConfirmOpen = false;
			host.SetPanelModalOwner(std::string());
			return;
		}
		const float x = frame.X + 16.0f;
		const float width = frame.W - 32.0f;
		Wui::Label(ctx, { x, frame.Y + 52.0f },
			EllipsizeToWidth(ctx,
				Wui::TrFormat("panel.texture.reset.body", "Delete {asset}?",
					{ { "asset", m_AssetLogical } }),
				width, 14.0f),
			theme.Text, 14.0f);
		Wui::Label(ctx, { x, frame.Y + 78.0f },
			EllipsizeToWidth(ctx,
				Wui::Tr("panel.texture.reset.body2",
					"The source image keeps working with the engine defaults (usage = color, BC7); the "
					"artifact is re-baked and the material texture cache is flushed."),
				width, 12.0f),
			theme.TextMuted, 12.0f);
		const Wui::ModalResult result = Wui::ModalFooter(ctx, frame,
			Wui::Tr("panel.texture.reset.confirm", "Delete .wtex"),
			Wui::Tr("panel.texture.reset.cancel", "Cancel"),
			Wui::HashId("texture.reset.ok"), Wui::HashId("texture.reset.cancel"), true, theme);
		const bool confirmed = result == Wui::ModalResult::Confirm;
		const bool cancelled = result == Wui::ModalResult::Cancel || escapePressed;
		if (confirmed || cancelled)
			m_ResetConfirmOpen = false;
		Wui::EndModalFrame(ctx);
		if (confirmed)
		{
			if (ctx.Modal() == modalId)
				ctx.ClearModal();
			host.SetPanelModalOwner(std::string());
			ResetAssetToDefaults();
		}
		else if (cancelled)
		{
			if (ctx.Modal() == modalId)
				ctx.ClearModal();
			host.SetPanelModalOwner(std::string());
		}
	}

}
