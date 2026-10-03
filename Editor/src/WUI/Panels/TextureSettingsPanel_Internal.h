#include "wldpch.h"
#include "WUI/Panels/TextureSettingsPanel.h"

#include "WUI/Common/EditorAssetTypes.h"

#include "World/Core/Sha256.h"
#include "World/RHI/RhiDevice.h"
#include "World/Renderer/Texture/MaterialTextureCache.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Texture/TextureCompiler.h"
#include "World/Renderer/Texture/TextureData.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/Widgets/WuiModal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <shellapi.h>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace World
{
namespace TextureSettingsPanelDetail {}   // 前置声明:下面的 using 必须先见到这个名字
using namespace TextureSettingsPanelDetail;   // 等价于拆分前的文件内匿名命名空间可见性
	namespace TextureSettingsPanelDetail
	{
		// 源图扩展名(与 TextureCompiler::BakeDirectory 的候选顺序一致)。
		const char* const kSourceCandidates[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };

		// M4-TEX P5 预览口径:
		//   * 源图**解码一次**;长边超过 kPreviewSourceMaxEdge 时当场落一次(之后每次改设置都从这份
		//     像素重采样,不再回磁盘/解码器);
		//   * 草稿纹理再压到 kDraftPreviewMaxEdge(即时路径必须便宜;状态行标注 "capped",
		//     真产物烘完就换成全分辨率的产物纹理);
		//   * 需要真编码的改动(compression / mips / usage / 尺寸…)走 kPreviewBakeDebounceSeconds 防抖,
		//     在**工作线程**里跑 `TextureCompiler::BakeFile`。
		constexpr uint32_t kPreviewSourceMaxEdge = 2048;
		constexpr uint32_t kDraftPreviewMaxEdge = 1024;
		constexpr double kPreviewBakeDebounceSeconds = 0.35;
		constexpr float kMaxZoomFactor = 32.0f;
double WallClockSeconds();

std::string TrimAscii(const std::string& text);

bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out, std::string& error);

bool WriteFileBytesAtomic(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, std::string& error);

bool WriteTextFileAtomic(const std::filesystem::path& path, const std::string& text, std::string& error);

std::string EllipsizeToWidth(const Wui::WuiContext& ctx, std::string_view text, float maxWidth, float fontSize);

std::string UpperAscii(std::string text);

void RevealPathInExplorer(const std::filesystem::path& path);


		// ---- 预览图像处理:与 TextureCompiler 的缩放口径一致(线性空间平均),压缩交给 GPU ----

		inline uint8_t ToLinearByte(uint8_t value)
		{
			const float linear = std::pow(static_cast<float>(value) / 255.0f, 2.2f);
			return static_cast<uint8_t>(std::lround(std::clamp(linear, 0.0f, 1.0f) * 255.0f));
		}

		inline uint8_t ToSrgbByte(uint8_t value)
		{
			const float srgb = std::pow(static_cast<float>(value) / 255.0f, 1.0f / 2.2f);
			return static_cast<uint8_t>(std::lround(std::clamp(srgb, 0.0f, 1.0f) * 255.0f));
		}
void DownsampleBox2x(const std::vector<uint8_t>& source, uint32_t sourceWidth, uint32_t sourceHeight, bool srgb, std::vector<uint8_t>& out, uint32_t& outWidth, uint32_t& outHeight);

std::vector<uint8_t> ResampleToMaxEdge(const std::vector<uint8_t>& source, uint32_t sourceWidth, uint32_t sourceHeight, uint32_t maxEdge, bool srgb, uint32_t& outWidth, uint32_t& outHeight);

void FlipRowsInPlace(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height);

void PremultiplyAlphaInPlace(std::vector<uint8_t>& pixels);

void ApplyChannelMask(std::vector<uint8_t>& pixels, int channel);

Rhi::Format PreviewDisplayFormat(TextureBlockFormat format);

const char* PreviewChannelCode(int channel);

const char* PreviewStateCode(int state);

std::filesystem::path ArtifactPathForSource(const std::filesystem::path& sourceFile);

std::filesystem::path LegacyArtifactPathForSource(const std::filesystem::path& sourceFile);


		// 预览缩放的统一口径:`Factor` 是"相对 fit"的倍率(跨草稿/产物换尺寸时取景不跳)。
		// 绝对范围 = [min(fit, 1), max(fit, 32)] —— 小图能退到 1:1(而不是被拉满到 fit),
		// 大图不会比 fit 更小(fit 本来就把整张装进预览)。
		struct PreviewZoomRange
		{
			float Fit = 1.0f;     // fit 的绝对倍率(屏幕像素 / 纹素)
			float MinFactor = 1.0f;
			float MaxFactor = 1.0f;
		};
PreviewZoomRange PreviewZoomFor(const Wui::WuiRect& rect, uint32_t width, uint32_t height);

	}

	// ---- 资产与请求 ----

	namespace Editor
	{
std::string AssetLogicalForTexturePath(const std::string& logicalPath);

bool ResolveDeclaredSource(const std::string& declaredText, std::string& outLogical, std::string& outError);

TextureAssetDocument LoadTextureAssetDocument(const std::filesystem::path& contentRoot, const std::string& logicalPath);

bool ResolveTextureSource(const std::filesystem::path& contentRoot, const std::string& logicalPath, const TextureImportSettings& settings, TextureSourceResolution& out);

bool ResolveTextureSourceLogical(const std::filesystem::path& contentRoot, const std::string& logicalPath, const TextureImportSettings& settings, std::string& outSourceLogical, std::string& outError);

bool ValidateTextureSourceText(const std::filesystem::path& contentRoot, const std::string& text, std::string& outError);

TextureArtifactStatus InspectTextureArtifact(const std::filesystem::path& contentRoot, const std::string& sourceLogical, const TextureImportSettings* settingsOverride);

bool CreateTextureAssetForSource(const std::filesystem::path& contentRoot, const std::string& sourceLogical, std::string* outAssetLogical, std::string& outError);

bool EnsureTextureAssetForSource(const std::filesystem::path& contentRoot, const std::string& sourceLogical, std::string* outAssetLogical, bool* outCreated, std::string& outError);


	}
}
