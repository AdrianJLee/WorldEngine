#include "wldpch.h"

#include "WUI/Common/AssetThumbnailRenderer.h"

#include "World/WUI/WuiWidgets.h"

#include "World/WUI/Widgets/WuiChrome.h"

#include "World/Utils/Paths.h"
#include "World/Asset/WModelIO.h"

#include "World/Math/Curve/CubicBezierCurve.h"



#include <cmath>

#include <fstream>

#include <filesystem>



namespace World::Editor

{

	namespace

	{

		uint32_t HashString(std::string_view str)

		{

			uint32_t hash = 2166136261u;

			for (char c : str)

			{

				hash ^= static_cast<uint8_t>(c);

				hash *= 16777619u;

			}

			return hash;

		}



		struct MatProps
		{
			Wui::WuiColor BaseColor = { 0.85f, 0.85f, 0.85f, 1.0f };
			float Roughness = 0.5f;
			float Metallic = 0.0f;
			float Emissive = 0.0f;
			std::string AlbedoTex;
			bool IsTransparent = false;
		};

		MatProps ParseMaterialProps(const std::string& logicalPath)
		{
			MatProps props;
			std::filesystem::path fullPath = Paths::AssetRoot() / logicalPath;
			std::ifstream file(fullPath);
			if (!file.is_open())
				return props;

			std::string line;
			while (std::getline(file, line))
			{
				if (line.rfind("BaseColor:", 0) == 0)
				{
					size_t ob = line.find('[');
					size_t cb = line.find(']');
					if (ob != std::string::npos && cb != std::string::npos && cb > ob)
					{
						std::string nums = line.substr(ob + 1, cb - ob - 1);
						float r = 0, g = 0, b = 0, a = 1;
						if (std::sscanf(nums.c_str(), "%f,%f,%f,%f", &r, &g, &b, &a) >= 3 ||
						    std::sscanf(nums.c_str(), "%f %f %f %f", &r, &g, &b, &a) >= 3)
						{
							props.BaseColor = { r, g, b, a };
							if (a < 0.95f) props.IsTransparent = true;
						}
					}
				}
				else if (line.find("tint:") != std::string::npos)
				{
					size_t pos = line.find("tint:");
					std::string nums = line.substr(pos + 5);
					float r = 0, g = 0, b = 0, a = 1;
					if (std::sscanf(nums.c_str(), "%f,%f,%f,%f", &r, &g, &b, &a) >= 3 ||
					    std::sscanf(nums.c_str(), "%f %f %f %f", &r, &g, &b, &a) >= 3)
					{
						props.BaseColor = { r, g, b, a };
						if (a < 0.95f) props.IsTransparent = true;
					}
				}
				else if (line.rfind("Roughness:", 0) == 0 || line.find("roughness:") != std::string::npos)
				{
					size_t pos = line.find(':');
					float rough = 0.5f;
					if (std::sscanf(line.c_str() + pos + 1, "%f", &rough) == 1)
						props.Roughness = std::clamp(rough, 0.0f, 1.0f);
				}
				else if (line.rfind("Metallic:", 0) == 0 || line.find("metallic:") != std::string::npos)
				{
					size_t pos = line.find(':');
					float metal = 0.0f;
					if (std::sscanf(line.c_str() + pos + 1, "%f", &metal) == 1)
						props.Metallic = std::clamp(metal, 0.0f, 1.0f);
				}
				else if (line.rfind("Emissive:", 0) == 0 || line.find("emissivePower:") != std::string::npos)
				{
					size_t pos = line.find(':');
					float em = 0.0f;
					if (std::sscanf(line.c_str() + pos + 1, "%f", &em) == 1)
						props.Emissive = em;
				}
				else if (line.rfind("AlbedoTexture:", 0) == 0 || line.find("albedoMap:") != std::string::npos)
				{
					size_t pos = line.find(':');
					std::string val = line.substr(pos + 1);
					while (!val.empty() && (static_cast<unsigned char>(val.front()) <= 32 || val.front() == '"')) val.erase(val.begin());
					while (!val.empty() && (static_cast<unsigned char>(val.back()) <= 32 || val.back() == '"')) val.pop_back();
					props.AlbedoTex = val;
				}
				else if (line.find("Transparent") != std::string::npos)
				{
					props.IsTransparent = true;
				}
			}
			return props;
		}

		void DrawMaterialBall(Wui::WuiContext& ctx, const Wui::WuiRect& rect,
			const std::string& logicalPath, const Wui::WuiTheme& theme)
		{
			Wui::GradientFill(ctx, rect, theme.HoverBg, theme.HoverBg, theme.WindowBg, theme.WindowBg);
			Wui::HighlightOutline(ctx, rect, theme.Border, 3.0f, 1.0f);

			const MatProps props = ParseMaterialProps(logicalPath);
			const float cx = rect.X + rect.W * 0.5f;
			const float cy = rect.Y + rect.H * 0.5f;
			const float r = std::min(rect.W, rect.H) * 0.38f;

			Wui::PanelBackground(ctx, { cx - r * 0.75f, cy + r * 0.52f, r * 1.5f, r * 0.34f },
				theme.ModalBackdrop, r * 0.34f);

			uint64_t albedoTexId = 0;
			if (!props.AlbedoTex.empty())
				albedoTexId = Wui::ResolveTextureWithHook(props.AlbedoTex);

			const Wui::WuiRect sphereRect { cx - r, cy - r, r * 2.0f, r * 2.0f };

			if (albedoTexId != 0)
			{
				Wui::Image(ctx, sphereRect, albedoTexId, { 0.0f, 0.0f, 1.0f, 1.0f }, theme);
				Wui::PanelBackground(ctx, { cx - r * 0.65f, cy - r * 0.65f, r * 1.25f, r * 1.25f },
					theme.TextBright, r * 0.625f);
				Wui::HighlightOutline(ctx, sphereRect, theme.BorderStrong, r, 2.0f);
			}
			else if (props.IsTransparent)
			{
				const Wui::WuiColor glassCol = { props.BaseColor.R, props.BaseColor.G, props.BaseColor.B, 0.45f };
				Wui::PanelBackground(ctx, sphereRect, glassCol, r);
				const Wui::WuiColor causticCol = {
					std::min(1.0f, props.BaseColor.R * 1.3f),
					std::min(1.0f, props.BaseColor.G * 1.3f),
					std::min(1.0f, props.BaseColor.B * 1.3f),
					0.60f
				};
				Wui::PanelBackground(ctx, { cx - r * 0.25f, cy + r * 0.15f, r * 0.90f, r * 0.65f },
					causticCol, r * 0.35f);
				Wui::HighlightOutline(ctx, sphereRect, theme.TextBright, r, 1.5f);
			}
			else
			{
				Wui::PanelBackground(ctx, sphereRect, props.BaseColor, r);
				const Wui::WuiColor highlightCol = {
					std::min(1.0f, props.BaseColor.R * 1.35f + 0.12f),
					std::min(1.0f, props.BaseColor.G * 1.35f + 0.12f),
					std::min(1.0f, props.BaseColor.B * 1.35f + 0.12f),
					0.65f
				};
				Wui::PanelBackground(ctx, { cx - r * 0.72f, cy - r * 0.72f, r * 1.28f, r * 1.28f },
					highlightCol, r * 0.64f);
				Wui::HighlightOutline(ctx, sphereRect, theme.Border, r, 1.0f);
			}

			const float specR = r * 0.30f * (1.0f - props.Roughness * 0.65f);
			const Wui::WuiColor specCol = props.Metallic > 0.5f ? props.BaseColor : theme.TextBright;
			Wui::PanelBackground(ctx, { cx - r * 0.40f, cy - r * 0.40f, specR, specR },
				specCol, specR * 0.5f);

			if (props.Emissive > 0.1f)
			{
				Wui::HighlightOutline(ctx, sphereRect, theme.Warning, r, 2.0f);
			}

			if (rect.W >= 36.0f)
			{
				Wui::Badge(ctx, { rect.X + rect.W - 30.0f, rect.Y + 2.0f, 28.0f, 13.0f },
					"MAT", theme.Accent, theme.TextBright, theme, 9.0f);
			}
		}

		void DrawAudioWaveform(Wui::WuiContext& ctx, const Wui::WuiRect& rect,

			const std::string& logicalPath, const Wui::WuiTheme& theme)

		{

			// 背景底色 (深冷色)

			Wui::PanelBackground(ctx, rect, { 0.08f, 0.11f, 0.16f, 1.0f }, 3.0f);



			const float cy = rect.Y + rect.H * 0.5f;



			// 中心基准线

			Wui::LineSegment(ctx, { rect.X + 6.0f, cy }, { rect.X + rect.W - 6.0f, cy },

				{ 0.25f, 0.35f, 0.45f, 0.35f }, 1.0f);



			// 16 根波形柱 (基于路径哈希确定性生成)

			const uint32_t hash = HashString(logicalPath);

			constexpr int kBarCount = 16;

			const float availW = rect.W - 16.0f;

			const float stepX = availW / static_cast<float>(kBarCount);



			for (int i = 0; i < kBarCount; ++i)

			{

				float normH = (std::sin(static_cast<float>(i) * 1.35f + static_cast<float>(hash % 29)) * 0.5f + 0.5f);

				normH = normH * normH * 0.8f + 0.15f; // 强调动态范围

				const float barH = std::max(3.0f, normH * (rect.H * 0.65f));

				const float barX = rect.X + 8.0f + static_cast<float>(i) * stepX + stepX * 0.5f;



				Wui::LineSegment(ctx, { barX, cy - barH * 0.5f }, { barX, cy + barH * 0.5f },

					{ 1.0f, 0.60f, 0.18f, 0.85f }, 2.0f);

			}



			Wui::Badge(ctx, { rect.X + rect.W - 34.0f, rect.Y + 2.0f, 32.0f, 13.0f },

				"WAV", { 0.95f, 0.55f, 0.20f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, theme, 9.0f);

		}



		void DrawCurveGraph(Wui::WuiContext& ctx, const Wui::WuiRect& rect,

			const std::string& logicalPath, const Wui::WuiTheme& theme)

		{

			Wui::PanelBackground(ctx, rect, { 0.08f, 0.12f, 0.15f, 1.0f }, 3.0f);



			// 内部网格线

			const float midX = rect.X + rect.W * 0.5f;

			const float midY = rect.Y + rect.H * 0.5f;

			Wui::LineSegment(ctx, { rect.X + 4.0f, midY }, { rect.X + rect.W - 4.0f, midY },

				{ 0.20f, 0.28f, 0.35f, 0.30f }, 1.0f);

			Wui::LineSegment(ctx, { midX, rect.Y + 4.0f }, { midX, rect.Y + rect.H - 4.0f },

				{ 0.20f, 0.28f, 0.35f, 0.30f }, 1.0f);



			// 加载或采样曲线

			CubicBezierCurve curve;

			std::filesystem::path fullPath = Paths::AssetRoot() / logicalPath;

			if (!curve.LoadFromFile(fullPath.string()))

			{

				curve = CubicBezierCurve::EaseInOut();

			}



			constexpr int kSteps = 16;

			const float padX = 8.0f;

			const float padY = 8.0f;

			const float plotW = rect.W - padX * 2.0f;

			const float plotH = rect.H - padY * 2.0f;



			auto mapPoint = [&](float t, float val) -> glm::vec2

			{

				float px = rect.X + padX + t * plotW;

				float py = rect.Y + rect.H - padY - val * plotH;

				return { px, py };

			};



			glm::vec2 prev = mapPoint(0.0f, curve.Evaluate(0.0f));

			for (int i = 1; i <= kSteps; ++i)

			{

				float t = static_cast<float>(i) / static_cast<float>(kSteps);

				glm::vec2 curr = mapPoint(t, curve.Evaluate(t));

				Wui::LineSegment(ctx, prev, curr, { 0.0f, 0.88f, 0.82f, 1.0f }, 2.0f);

				prev = curr;

			}



			// 起止控制点

			glm::vec2 p0 = mapPoint(0.0f, curve.Evaluate(0.0f));

			glm::vec2 p1 = mapPoint(1.0f, curve.Evaluate(1.0f));

			Wui::PanelBackground(ctx, { p0.x - 2.5f, p0.y - 2.5f, 5.0f, 5.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, 2.5f);

			Wui::PanelBackground(ctx, { p1.x - 2.5f, p1.y - 2.5f, 5.0f, 5.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, 2.5f);



			Wui::Badge(ctx, { rect.X + rect.W - 36.0f, rect.Y + 2.0f, 34.0f, 13.0f },

				"CURV", { 0.20f, 0.80f, 0.75f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, theme, 9.0f);

		}



		void DrawPrefabBlueprint(Wui::WuiContext& ctx, const Wui::WuiRect& rect,

			const std::string& logicalPath, const Wui::WuiTheme& theme)

		{

			Wui::PanelBackground(ctx, rect, { 0.12f, 0.09f, 0.18f, 1.0f }, 3.0f);



			const float cx = rect.X + rect.W * 0.5f;

			const float cy = rect.Y + rect.H * 0.5f + 2.0f;

			const float s = std::min(rect.W, rect.H) * 0.28f;

			const Wui::WuiColor wireCol { 0.80f, 0.45f, 0.90f, 0.85f };



			// 等轴测 3D 棱晶盒

			glm::vec2 top { cx, cy - s };

			glm::vec2 right { cx + s * 1.1f, cy - s * 0.4f };

			glm::vec2 left { cx - s * 1.1f, cy - s * 0.4f };

			glm::vec2 center { cx, cy + s * 0.2f };

			glm::vec2 bRight { cx + s * 1.1f, cy + s * 0.8f };

			glm::vec2 bLeft { cx - s * 1.1f, cy + s * 0.8f };

			glm::vec2 bottom { cx, cy + s * 1.4f };



			Wui::LineSegment(ctx, top, right, wireCol, 1.5f);

			Wui::LineSegment(ctx, right, center, wireCol, 1.5f);

			Wui::LineSegment(ctx, center, left, wireCol, 1.5f);

			Wui::LineSegment(ctx, left, top, wireCol, 1.5f);



			Wui::LineSegment(ctx, left, bLeft, wireCol, 1.5f);

			Wui::LineSegment(ctx, center, bottom, wireCol, 1.5f);

			Wui::LineSegment(ctx, right, bRight, wireCol, 1.5f);



			Wui::LineSegment(ctx, bLeft, bottom, wireCol, 1.5f);

			Wui::LineSegment(ctx, bottom, bRight, wireCol, 1.5f);



			// 中心菱形标记

			Wui::PanelBackground(ctx, { center.x - 2.5f, center.y - 2.5f, 5.0f, 5.0f },

				{ 1.0f, 1.0f, 1.0f, 0.95f }, 1.5f);



			Wui::Badge(ctx, { rect.X + rect.W - 36.0f, rect.Y + 2.0f, 34.0f, 13.0f },

				"PREFAB", { 0.75f, 0.40f, 0.85f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, theme, 9.0f);

		}



		void DrawMeshWireframe(Wui::WuiContext& ctx, const Wui::WuiRect& rect,
			const std::string& logicalPath, const Wui::WuiTheme& theme)
		{
			Wui::PanelBackground(ctx, rect, theme.ContentBg, 3.0f);
			Wui::HighlightOutline(ctx, rect, theme.Border, 3.0f, 1.0f);

			const float cx = rect.X + rect.W * 0.5f;
			const float cy = rect.Y + rect.H * 0.52f;
			const float r = std::min(rect.W, rect.H) * 0.36f;

			Wui::PanelBackground(ctx, { cx - r * 0.85f, cy + r * 0.55f, r * 1.70f, r * 0.35f },
				theme.ModalBackdrop, r * 0.35f);

			const std::filesystem::path fullPath = Paths::AssetRoot() / logicalPath;
			Asset::WModelData modelData;
			std::string err;
			bool loaded = Asset::WModelIO::ReadFile(fullPath.string(), modelData, &err);

			glm::vec3 center(0.0f);
			glm::vec3 extents(1.0f);
			if (loaded)
			{
				center = (modelData.Bounds.Min + modelData.Bounds.Max) * 0.5f;
				extents = glm::max(glm::vec3(0.01f), (modelData.Bounds.Max - modelData.Bounds.Min) * 0.5f);
			}

			constexpr float cosYaw = 0.8253356f;
			constexpr float sinYaw = 0.5646425f;
			constexpr float cosPitch = 0.9689124f;
			constexpr float sinPitch = 0.2474039f;

			const float maxDim = std::max({ extents.x, extents.y, extents.z });
			const float scale = (r * 0.95f) / std::max(0.001f, maxDim);

			auto project = [&](const glm::vec3& p) -> glm::vec2
			{
				glm::vec3 rel = p - center;
				float rx = rel.x * cosYaw - rel.z * sinYaw;
				float rz = rel.x * sinYaw + rel.z * cosYaw;
				float ry = rel.y * cosPitch - rz * sinPitch;
				return glm::vec2(cx + rx * scale, cy - ry * scale);
			};

			const glm::vec3 bMin = center - extents;
			const glm::vec3 bMax = center + extents;

			const glm::vec2 p000 = project({ bMin.x, bMin.y, bMin.z });
			const glm::vec2 p001 = project({ bMin.x, bMin.y, bMax.z });
			const glm::vec2 p010 = project({ bMin.x, bMax.y, bMin.z });
			const glm::vec2 p011 = project({ bMin.x, bMax.y, bMax.z });
			const glm::vec2 p100 = project({ bMax.x, bMin.y, bMin.z });
			const glm::vec2 p101 = project({ bMax.x, bMin.y, bMax.z });
			const glm::vec2 p110 = project({ bMax.x, bMax.y, bMin.z });
			const glm::vec2 p111 = project({ bMax.x, bMax.y, bMax.z });

			Wui::LineSegment(ctx, p010, p110, theme.Warning, 1.5f);
			Wui::LineSegment(ctx, p110, p111, theme.Warning, 1.5f);
			Wui::LineSegment(ctx, p111, p011, theme.Warning, 1.5f);
			Wui::LineSegment(ctx, p011, p010, theme.Warning, 1.5f);

			Wui::LineSegment(ctx, p011, p001, theme.Warning, 1.5f);
			Wui::LineSegment(ctx, p111, p101, theme.Warning, 1.5f);
			Wui::LineSegment(ctx, p110, p100, theme.Warning, 1.5f);

			Wui::LineSegment(ctx, p001, p101, theme.Warning, 1.5f);
			Wui::LineSegment(ctx, p101, p100, theme.Warning, 1.5f);

			if (loaded && !modelData.Vertices.empty())
			{
				const size_t vCount = modelData.Vertices.size();
				const size_t step = std::max<size_t>(1, vCount / 12);
				for (size_t i = 0; i + step < vCount && i < step * 10; i += step)
				{
					const glm::vec2 va = project(modelData.Vertices[i].Position);
					const glm::vec2 vb = project(modelData.Vertices[i + step].Position);
					Wui::LineSegment(ctx, va, vb, theme.AxisZ, 1.0f);
				}
			}
			else
			{
				Wui::LineSegment(ctx, (p010 + p011) * 0.5f, (p110 + p111) * 0.5f, theme.AxisZ, 1.0f);
				Wui::LineSegment(ctx, (p011 + p111) * 0.5f, (p001 + p101) * 0.5f, theme.AxisZ, 1.0f);
			}

			if (rect.W >= 36.0f)
			{
				Wui::Badge(ctx, { rect.X + rect.W - 28.0f, rect.Y + 2.0f, 26.0f, 13.0f },
					"3D", theme.Warning, theme.TextBright, theme, 9.0f);
			}
		}

		void DrawGenericThumbnail(Wui::WuiContext& ctx, const Wui::WuiRect& rect,

			const std::string& logicalPath, AssetCategory category, const Wui::WuiTheme& theme)

		{

			Wui::PanelBackground(ctx, rect, { 0.12f, 0.13f, 0.16f, 1.0f }, 3.0f);



			const char* label = "FILE";

			Wui::WuiColor col { 0.6f, 0.6f, 0.6f, 1.0f };



			switch (category)

			{

			case AssetCategory::Script:

				label = "LUA";

				col = { 0.95f, 0.80f, 0.20f, 1.0f };

				break;

			case AssetCategory::Level:

				label = "LEVEL";

				col = { 0.90f, 0.30f, 0.30f, 1.0f };

				break;

			case AssetCategory::Shader:

				label = "SHADER";

				col = { 0.50f, 0.50f, 0.95f, 1.0f };

				break;

			case AssetCategory::UI:

				label = "UI";

				col = { 0.85f, 0.35f, 0.70f, 1.0f };

				break;

			default:

				break;

			}



			// 中心色块卡片

			const float bw = 50.0f;

			const float bh = 22.0f;

			Wui::Badge(ctx, { rect.X + (rect.W - bw) * 0.5f, rect.Y + (rect.H - bh) * 0.5f, bw, bh },

				label, col, { 1.0f, 1.0f, 1.0f, 1.0f }, theme, 11.0f);

		}

	}



	void AssetThumbnailRenderer::DrawThumbnail(Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		const std::string& logicalPath, AssetCategory category, const Wui::WuiTheme& theme)
	{
		if (category == AssetCategory::Unknown)
			category = AssetThumbnailCache::DetectCategory(logicalPath);

		// 0. 优先检测是否存在显式预览图/伴随预览图/GPU已就绪预览纹理
		// (对于有预览图的, 缩略图应该是真实预览图)
		uint64_t previewTexId = AssetThumbnailCache::Get().GetOrCreateTextureId(logicalPath);
		if (previewTexId != 0)
		{
			const ThumbnailInfo* thumb = AssetThumbnailCache::Get().GetThumbnail(logicalPath);
			const Wui::WuiRect uv = (thumb && thumb->IsOffscreenRender)
				? Wui::WuiRect { 0.0f, 1.0f, 1.0f, -1.0f }
				: Wui::WuiRect { 0.0f, 0.0f, 1.0f, 1.0f };

			Wui::PanelBackground(ctx, rect, theme.ContentBg, 3.0f);
			Wui::Image(ctx, rect, previewTexId, uv, theme);
			Wui::HighlightOutline(ctx, rect, theme.BorderStrong, 3.0f, 1.0f);

			const char* tag = (category == AssetCategory::Material) ? "MAT" :
			                  (category == AssetCategory::Mesh) ? "3D" :
			                  (category == AssetCategory::Texture) ? "TEX" : nullptr;
			if (tag && rect.W >= 36.0f)
			{
				const Wui::WuiColor tagCol = (category == AssetCategory::Material) ? theme.Accent :
				                             (category == AssetCategory::Mesh) ? theme.Warning : theme.Info;
				Wui::Badge(ctx, { rect.X + rect.W - 28.0f, rect.Y + 2.0f, 26.0f, 13.0f },
					tag, tagCol, theme.TextBright, theme, 9.0f);
			}
			return;
		}

		// 1. 纹理类资产 (Texture / Atlas) -> 优先使用 GPU 纹理直接呈现真实画面

		if (category == AssetCategory::Texture)

		{

			std::string targetTexture = logicalPath;



			// 如果是图集 .watlas，尝试解析其底层的贴图路径

			if (logicalPath.size() > 7 && logicalPath.rfind(".watlas") == logicalPath.size() - 7)

			{

				std::filesystem::path atlasFull = Paths::AssetRoot() / logicalPath;

				std::ifstream f(atlasFull);

				if (f.is_open())

				{

					std::string line;

					while (std::getline(f, line))

					{

						if (line.rfind("TexturePath:", 0) == 0)

						{

							std::string val = line.substr(12);

							while (!val.empty() && (val.front() == ' ' || val.front() == '"')) val.erase(val.begin());

							while (!val.empty() && (val.back() == ' ' || val.back() == '"' || val.back() == '\r')) val.pop_back();

							if (val.rfind("assets/", 0) == 0 || val.rfind("assets\\", 0) == 0)

								val = val.substr(7);

							targetTexture = val;

							break;

						}

					}

				}

			}



			uint64_t texId = AssetThumbnailCache::Get().GetOrCreateTextureId(targetTexture);

			if (texId != 0)

			{

				// 背景棋盘保护底

				Wui::PanelBackground(ctx, rect, { 0.12f, 0.14f, 0.18f, 1.0f }, 3.0f);



				// 绘制真实图像

				Wui::Image(ctx, rect, texId, { 0.0f, 0.0f, 1.0f, 1.0f }, theme);



				// 1px 细微边界保护

				Wui::HighlightOutline(ctx, rect, { 1.0f, 1.0f, 1.0f, 0.15f }, 3.0f, 1.0f);

				return;

			}

		}



		// 2. 材质类资产 -> 3D 材质球预览

		if (category == AssetCategory::Material)

		{

			DrawMaterialBall(ctx, rect, logicalPath, theme);

			return;

		}



		// 3. 音频类资产 -> 动态波形图预览

		if (category == AssetCategory::Audio)

		{

			DrawAudioWaveform(ctx, rect, logicalPath, theme);

			return;

		}



		// 4. 曲线类资产 -> 动态贝塞尔曲线图预览

		if (category == AssetCategory::Curve)

		{

			DrawCurveGraph(ctx, rect, logicalPath, theme);

			return;

		}



		// 5. 预制体类资产 -> 蓝图实体线框预览

		if (category == AssetCategory::Prefab)

		{

			DrawPrefabBlueprint(ctx, rect, logicalPath, theme);

			return;

		}



		// 6. 3D 模型类资产 -> 3D 网格几何线框预览

		if (category == AssetCategory::Mesh)

		{

			DrawMeshWireframe(ctx, rect, logicalPath, theme);

			return;

		}



		// 7. 其余资产 -> 结构化卡片与徽标

		DrawGenericThumbnail(ctx, rect, logicalPath, category, theme);

	}

}

