#include "MaterialEditorPanel_Internal.h"

namespace World
{

using namespace MaterialEditorPanelDetail;

namespace MaterialEditorPanelDetail
{
		// MAT-UI3b:参数行的行高 —— Vec4 用 we_engine 的 `Wui::Vec4Field`(2×2:两行高),
		// Vec2/Vec3 与其它类型仍是单行(行高按行类型随行变化,容器高度同步按它累加)。
float ShaderParamRowHeight(ParamType type, float rowHeight){
			return type == ParamType::Vec4 ? rowHeight * 2.0f : rowHeight;
		}


		// MAT-UI7b:注解里的 `doc("…")` 是**用户写的参数说明**(用户 2026-09-25:
		// 「材质编辑器中的参数没法写注释来解释这个参数」)。两个形态(`.wmat` 实例列 /
		// `.slang` 代码形态的参数列)共用这一份口径:
		//  - 悬停 tooltip:有 doc → doc 文本占第一行,后面接原有的类型/范围/单位/来源行;
		//    没有 doc → 与旧口径逐字相同("空则回落")。
std::string ShaderParamRowTooltip(const MaterialParamDecl& decl, const std::string& meta){
			return decl.Doc.empty() ? meta : (decl.Doc + "\n" + meta);
		}


		// MAT-UI7b:说明也要进无障碍树 —— `material.param.<Name>.doc`(只在有 doc 时登记)的
		// Value/Tooltip 都是 doc 文本**原文**,读屏与 AI 通道(ui.tree)按稳定 id 直接读说明。
		// 不覆盖 `.source` 节点的 Value("override"/"parent"/"shader-default" 是三态语义)。
void RegisterShaderParamDocNode(const MaterialParamDecl& decl, const std::string& label, const Wui::WuiRect& rowRect){
			if (decl.Doc.empty())
				return;
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId(("material.param." + decl.Name + ".doc").c_str());
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = label + Wui::Tr("panel.material.shader.param.doc.label", " — description");
			node.Value = decl.Doc;
			node.Tooltip = decl.Doc;
			node.Rect = { rowRect.X, rowRect.Y, 2.0f, rowRect.H };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}


		// U27:预览列宽**会话内跨面板记住**(切材质、关掉再打开都不重置;<= 0 = 还没设过)。
		// 只记用户拖出来的值 —— 窗口变窄时只在本帧夹取,不覆写记忆,窗口再变宽能回到原位置。
float& SessionPreviewColumnWidth(){
			static float width = 0.0f;
			return width;
		}


		// U25-M2:动作按钮(与 U13d 的 ModalActionButton 同一套画法)。
		// 主按钮 = accent 填充;禁用 = 同尺寸弱化绘制,并把"为什么不可用"同时写进
		// 无障碍节点的 Tooltip 与悬停提示 —— 灰按钮不能没有理由。
bool ActionButton(Wui::WuiContext& ctx, Wui::WuiId id, const Wui::WuiRect& rect, const std::string& label, const std::string& tooltip, bool enabled, bool primary, const Wui::WuiTheme& theme){
			const bool hovered = ctx.IsHovered(rect);
			const Wui::WuiColor fill = !enabled ? theme.PanelBg
				: (primary ? theme.Accent : (hovered ? theme.ButtonHover : theme.ButtonBg));
			Wui::PanelBackground(ctx, rect, fill, 3.0f);
			Wui::HighlightOutline(ctx, rect,
				enabled ? (hovered ? theme.Accent : theme.Border) : theme.Border, 3.0f, 1.0f);
			// accent 填充上压深色文字(白字对比度不够);禁用态用 TextDisabled。
			const Wui::WuiColor textColor = !enabled ? theme.TextDisabled
				: (primary ? theme.WindowBg : theme.Text);
			Wui::Label(ctx, { rect.X + 8.0f, rect.Y + (rect.H - 14.0f) * 0.5f }, label, textColor, 13.0f);
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "button";
			node.Label = label;
			node.Value = tooltip;
			node.Tooltip = tooltip;
			node.Rect = rect;
			node.Enabled = enabled;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
			Wui::DrawFocusRing(ctx, rect, id, theme);
			ctx.RegisterFocusable(id, rect);
			if (hovered)
			{
				if (enabled)
					ctx.SetCursor(Wui::WuiCursor::Hand);
				if (!tooltip.empty())
					ctx.SetTooltip(tooltip);
			}
			return enabled && ctx.IsClicked(rect);
		}


		// 逻辑路径的小写扩展名(含点)。
std::string LowerExtension(const std::string& path){
			const size_t dot = path.find_last_of('.');
			if (dot == std::string::npos)
				return {};
			std::string extension = path.substr(dot);
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return extension;
		}


		// M4-TEX-P6a:材质能引用**两种**纹理路径 —— 源图(`textures/Icon.png`)与纹理资产
		// (`textures/Icon.wtex`;运行时按资产里的 `source:`/产物解析,见 TextureData.cpp)。
		// 拖放/下拉/校验全部用这一个判定,不再只认图片扩展名。
bool IsTextureRefExtension(const std::string& extension){
			return extension == ".png" || extension == ".jpg" || extension == ".jpeg"
				|| extension == ".tga" || extension == ".bmp" || extension == ".wtex";
		}


		// Save As / 新建向导共用的"基础名":去首尾空白 + 剥掉用户多打的 .wmat 后缀
		// (后缀在落点回显行里常显),不做其它"善意改写" —— 非法字符由校验行说出来。
std::string MaterialBaseName(const std::string& raw){
			std::string name = raw;
			const auto notSpace = [](unsigned char character) { return std::isspace(character) == 0; };
			name.erase(name.begin(), std::find_if(name.begin(), name.end(), notSpace));
			name.erase(std::find_if(name.rbegin(), name.rend(), notSpace).base(), name.end());
			constexpr size_t kSuffixLength = 5;   // ".wmat"
			if (name.size() > kSuffixLength)
			{
				std::string suffix = name.substr(name.size() - kSuffixLength);
				std::transform(suffix.begin(), suffix.end(), suffix.begin(),
					[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
				if (suffix == ".wmat")
					name = name.substr(0, name.size() - kSuffixLength);
			}
			return name;
		}


		// 预览组的分段标签:行 key → 标签下标(0=网格 1=背景 2=光照 3=显示)。
int PreviewTabFor(const std::string& key){
			if (key == std::string("preview.mesh"))
				return 0;
			if (key == std::string("preview.bg"))
				return 1;
			if (key.rfind("preview.light", 0) == 0)
				return 2;
			return 3;   // preview.wireframe / preview.normals / preview.uvchecker
		}


float QuantizeMaterialValue(float value){
			return std::round(value * kMaterialValueStep) / kMaterialValueStep;
		}


glm::vec4 QuantizeMaterialValue(const glm::vec4& value){
			return { QuantizeMaterialValue(value.x), QuantizeMaterialValue(value.y),
				QuantizeMaterialValue(value.z), QuantizeMaterialValue(value.w) };
		}


glm::vec3 QuantizeMaterialValue(const glm::vec3& value){
			return { QuantizeMaterialValue(value.x), QuantizeMaterialValue(value.y),
				QuantizeMaterialValue(value.z) };
		}


std::string ShortenPath(const std::string& path){
			if (path.size() <= 46)
				return path;
			return "…" + path.substr(path.size() - 45);
		}


std::string ToLowerAscii(const std::string& text){
			std::string result = text;
			std::transform(result.begin(), result.end(), result.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return result;
		}


		// 按 UTF-8 码点边界裁剪到宽度(直接按字节切会切碎中文)。
std::string EllipsizeToWidth(const Wui::WuiContext& ctx, const std::string& text, float width, float fontSize){
			if (text.empty() || width <= 0.0f)
				return {};
			if (ctx.MeasureTextWidth(text, fontSize) <= width)
				return text;
			const std::string dots = "…";
			std::vector<size_t> boundaries;
			for (size_t index = 0; index < text.size(); ++index)
				if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80)
					boundaries.push_back(index);
			for (size_t count = boundaries.size(); count > 0; --count)
			{
				const std::string candidate = text.substr(0, boundaries[count - 1]) + dots;
				if (ctx.MeasureTextWidth(candidate, fontSize) <= width)
					return candidate;
			}
			return dots;
		}


	// U2d:未落盘材质的"另存为"目标路径校验,返回给用户看的具体原因(空字符串 = 可提交)。
		// 与 MaterialLibrary::Save + MaterialIO::WriteFileText 的落盘规则一致:
		//  - 缺 .wmat 后缀会自动补;  - 写入目标为项目内容根。
		// 名字里真正非法的只有 < > : " | ? *(反斜杠/斜杠是路径分隔符,不是非法字符)。
std::string NewMaterialPathError(const std::string& raw){
			if (raw.empty())
				return Wui::Tr("panel.material.newpath.error.empty", "Path cannot be empty");
			for (const char character : raw)
			{
				if (character == '<' || character == '>' || character == ':'
					|| character == '"' || character == '|' || character == '?' || character == '*')
					return Wui::Tr("panel.material.newpath.error.illegal",
						"Path contains illegal characters (< > : \" | ? *)");
			}
			std::string key = MaterialLibrary::NormalizePath(raw);
			if (key.size() < 5 || key.substr(key.size() - 5) != ".wmat")
				key.append(".wmat");
			std::error_code existsError;
			const std::filesystem::path contentRoot =
				World::Paths::AssetRoot();
			if (std::filesystem::exists(contentRoot / key, existsError))
				return Wui::Tr("panel.material.newpath.error.exists", "A file already exists at this path");
			return {};
		}


std::filesystem::path ContentRootPath(){
			return World::Paths::AssetRoot();
		}


		// ---- MAT-FN3:材质函数库与 `#include` 根 ----
		//
		// 库文件 = 内容根下 `shaders/lib/**` 里的 `.slang`(docs/dev/shader-contract.md §9;
		// 与 EditorCooker 的烘焙扫描同一口径)。它不是材质资产:没有 `Evaluate` 入口、
		// 不单独编译/烘焙,只被材质 `#include` 引用 —— 因此**不能**按材质编译预览。
bool IsMaterialLibraryShaderPath(const std::string& logicalPath){
			std::string normalized = MaterialLibrary::NormalizePath(logicalPath);
			std::replace(normalized.begin(), normalized.end(), '\\', '/');
			std::transform(normalized.begin(), normalized.end(), normalized.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			// 唯一扩展名是 `.slang`(Slang-B1)。
			const std::string extension = ".slang";
			if (normalized.size() <= extension.size()
				|| normalized.compare(normalized.size() - extension.size(), extension.size(), extension) != 0)
				return false;
			// 只要路径里出现 `shaders/lib/` 这一段就算库文件(相对内容根的 `shaders/lib/x.slang`
			// 与绝对路径 `<…>/assets/shaders/lib/x.slang` 都命中),别处出现同样片段的不算。
			const std::string marker = "shaders/lib/";
			size_t at = normalized.find(marker);
			while (at != std::string::npos)
			{
				if (at == 0 || normalized[at - 1] == '/')
					return true;
				at = normalized.find(marker, at + 1);
			}
			return false;
		}


		// 表面材质编译器的 include 根(绝对路径)。顺序 = docs/dev/shader-contract.md §9 的解析
		// 顺序:**先材质自身目录、再项目 `assets/shaders` 根**(`#include "lib/pattern.slang"`
		// 命中的就是这一层)。只收集真实存在的目录 —— 还没落盘的新材质推出来的目录可能不存在,
		// 这时命中的只能是项目根,和"没写这个根"等价;顺序固定,根本身不进缓存键。
std::vector<std::filesystem::path> SurfaceIncludeRoots(const std::filesystem::path& contentRoot, const std::string& shaderLogicalPath){
			std::vector<std::filesystem::path> roots;
			const auto add = [&roots](const std::filesystem::path& candidate)
			{
				if (candidate.empty())
					return;
				std::error_code ec;
				if (!std::filesystem::is_directory(candidate, ec))
					return;
				const std::filesystem::path absolute = std::filesystem::absolute(candidate, ec);
				if (ec || absolute.empty())
					return;
				const std::filesystem::path normalized = absolute.lexically_normal();
				if (std::find(roots.begin(), roots.end(), normalized) == roots.end())
					roots.push_back(normalized);
			};
			// 材质自身目录:未落盘时按逻辑路径推(绝对路径直接用)。
			const std::filesystem::path shaderFile(shaderLogicalPath);
			add((shaderFile.is_absolute() ? shaderFile : contentRoot / shaderFile).parent_path());
			add(contentRoot / "shaders");
			return roots;
		}


const GroupDesc* FindGroup(const std::string& key){
			for (const GroupDesc& group : kGroups)
				if (key == group.Key)
					return &group;
			return nullptr;
		}


std::string GroupLabel(const GroupDesc& group){
			return Wui::Tr(std::string("material.group.") + group.Key, group.Label);
		}


std::string ResetIdFor(const std::string& key){
			return "material.prop." + key + ".reset";
		}


RowControl RowControlFor(const std::string& key){
			if (key == "reset_all")
				return RowControl::Button;
			if (key == "base")
				return RowControl::Color;
			if (key == "emissive")
				return RowControl::Vec3;
			if (key == "metallic" || key == "roughness" || key == "preview.light.intensity"
				|| key == "preview.light.azimuth" || key == "preview.light.elevation")
				return RowControl::DragBar;
			if (key == "blend")
				return RowControl::Combo;
			if (key == "preview.mesh" || key == "preview.bg" || key == "preview.light")
				return RowControl::Segmented;
			if (key == "doublesided" || key == "preview.wireframe" || key == "preview.normals"
				|| key == "preview.uvchecker")
				return RowControl::Checkbox;
			if (key == "albedo" || key == "normal")
				return RowControl::Asset;
			if (key == "name")
				return RowControl::Text;
			return RowControl::ReadOnly;
		}


		// ---- .wmat 的字段默认值(MaterialDesc 的默认构造;恢复默认 = 写回这里)----
bool FieldDiffersFromDefault(const std::string& key, const MaterialDesc& desc, const MaterialDesc& defaults){
			if (key == "base")
				return desc.BaseColor != defaults.BaseColor;
			if (key == "metallic")
				return desc.Metallic != defaults.Metallic;
			if (key == "roughness")
				return desc.Roughness != defaults.Roughness;
			if (key == "emissive")
				return desc.Emissive != defaults.Emissive;
			if (key == "blend")
				return desc.BlendMode != defaults.BlendMode;
			if (key == "doublesided")
				return desc.DoubleSided != defaults.DoubleSided;
			if (key == "albedo")
				return desc.AlbedoTexture != defaults.AlbedoTexture;
			if (key == "normal")
				return desc.NormalTexture != defaults.NormalTexture;
			return false;   // 只读行与自由文本(显示名)没有"默认值"概念
		}


		// 读取标准布局(32B stride)的顶点;不是标准布局时返回空表(调用方跳过派生网格)。
std::vector<SourceVertex> ReadStandardVertices(const Mesh& mesh){
			std::vector<SourceVertex> vertices;
			if (mesh.GetVertexLayoutId() != Mesh::kVertexLayoutStandard)
				return vertices;
			const std::vector<uint8_t>& data = mesh.GetDesc().VertexData;
			constexpr size_t stride = 32;
			const size_t count = data.size() / stride;
			vertices.reserve(count);
			for (size_t index = 0; index < count; ++index)
			{
				const float* values = reinterpret_cast<const float*>(data.data() + index * stride);
				SourceVertex vertex;
				vertex.Position = { values[0], values[1], values[2] };
				vertex.Normal = { values[3], values[4], values[5] };
				vertex.Uv = { values[6], values[7] };
				vertices.push_back(vertex);
			}
			return vertices;
		}


glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback ){
			const float length = glm::length(value);
			return length > 1e-6f ? value / length : fallback;
		}


void OrthonormalBasis(const glm::vec3& direction, glm::vec3* u, glm::vec3* v){
			const glm::vec3 axis = std::abs(direction.y) < 0.9f ? glm::vec3 { 0.0f, 1.0f, 0.0f }
				: glm::vec3 { 1.0f, 0.0f, 0.0f };
			*u = SafeNormalize(glm::cross(axis, direction), { 1.0f, 0.0f, 0.0f });
			*v = SafeNormalize(glm::cross(direction, *u), { 0.0f, 0.0f, 1.0f });
		}


		// 线框:把每个三角形的三条边做成"贴着表面的细带",沿法线抬起一点点避免 z-fighting。
		// 为什么不用多边形线模式:3D 管线是冻结的(World/** 不在本批边界内),RHI 也没有
		// FillMode=Line;用几何表示线框不需要改任何共享接口。
Ref<Mesh> BuildWireMesh(const Mesh& source, float thickness, float offset){
			const std::vector<SourceVertex> vertices = ReadStandardVertices(source);
			if (vertices.empty())
				return nullptr;
			const std::vector<uint32_t>& indices = source.GetDesc().Indices;
			MeshBuilder builder;
			for (size_t index = 0; index + 2 < indices.size(); index += 3)
			{
				const SourceVertex& a = vertices[indices[index + 0]];
				const SourceVertex& b = vertices[indices[index + 1]];
				const SourceVertex& c = vertices[indices[index + 2]];
				const SourceVertex* triangle[3] = { &a, &b, &c };
				const glm::vec3 faceNormal = SafeNormalize(glm::cross(b.Position - a.Position,
					c.Position - a.Position));
				for (int edge = 0; edge < 3; ++edge)
				{
					const SourceVertex& start = *triangle[edge];
					const SourceVertex& end = *triangle[(edge + 1) % 3];
					const glm::vec3 edgeDirection = end.Position - start.Position;
					if (glm::length(edgeDirection) < 1e-6f)
						continue;
					const glm::vec3 side = SafeNormalize(glm::cross(SafeNormalize(edgeDirection), faceNormal),
						{ 1.0f, 0.0f, 0.0f }) * (thickness * 0.5f);
					const glm::vec3 normal = SafeNormalize(start.Normal + end.Normal + faceNormal);
					const glm::vec3 startPoint = start.Position + normal * offset;
					const glm::vec3 endPoint = end.Position + normal * offset;
					const uint32_t i0 = builder.Add(startPoint - side, normal, start.Uv);
					const uint32_t i1 = builder.Add(startPoint + side, normal, start.Uv);
					const uint32_t i2 = builder.Add(endPoint + side, normal, end.Uv);
					const uint32_t i3 = builder.Add(endPoint - side, normal, end.Uv);
					builder.Quad(i0, i1, i2, i3);
				}
			}
			return builder.Build("Material.Preview.Wire");
		}


		// 法线可视化:每个顶点一根"三棱柱"细柱(比单面片更抗侧视,不用改着色器)。
Ref<Mesh> BuildNormalMesh(const Mesh& source, float length, float radius, float offset){
			const std::vector<SourceVertex> vertices = ReadStandardVertices(source);
			if (vertices.empty())
				return nullptr;
			MeshBuilder builder;
			for (const SourceVertex& vertex : vertices)
			{
				const glm::vec3 normal = SafeNormalize(vertex.Normal);
				glm::vec3 u;
				glm::vec3 v;
				OrthonormalBasis(normal, &u, &v);
				const glm::vec3 base = vertex.Position + normal * offset;
				const glm::vec3 tip = base + normal * length;
				glm::vec3 ring[3];
				glm::vec3 tipRing[3];
				for (int corner = 0; corner < 3; ++corner)
				{
					const float angle = static_cast<float>(corner) * (2.0f * kPi / 3.0f);
					const glm::vec3 offsetDir = u * std::cos(angle) + v * std::sin(angle);
					ring[corner] = base + offsetDir * radius;
					tipRing[corner] = tip + offsetDir * radius * 0.15f;
				}
				for (int corner = 0; corner < 3; ++corner)
				{
					const int next = (corner + 1) % 3;
					const uint32_t i0 = builder.Add(ring[corner], normal, vertex.Uv);
					const uint32_t i1 = builder.Add(ring[next], normal, vertex.Uv);
					const uint32_t i2 = builder.Add(tipRing[next], normal, vertex.Uv);
					const uint32_t i3 = builder.Add(tipRing[corner], normal, vertex.Uv);
					builder.Quad(i0, i1, i2, i3);
				}
			}
			return builder.Build("Material.Preview.Normals");
		}


		// UV 棋盘格:按 UV 把每个三角形细分,按格子奇偶分成两张网格(亮格 / 暗格)。
		// 细分步长取"半个格子",所以低模(立方体/平面)也能得到 8×8 的棋盘。
void BuildCheckerMeshes(const Mesh& source, Ref<Mesh>* light, Ref<Mesh>* dark){
			const std::vector<SourceVertex> vertices = ReadStandardVertices(source);
			if (vertices.empty())
				return;
			const std::vector<uint32_t>& indices = source.GetDesc().Indices;
			MeshBuilder lightBuilder;
			MeshBuilder darkBuilder;
			const float stepSize = 1.0f / (kUvCheckerCells * 2.0f);
			for (size_t index = 0; index + 2 < indices.size(); index += 3)
			{
				const SourceVertex& a = vertices[indices[index + 0]];
				const SourceVertex& b = vertices[indices[index + 1]];
				const SourceVertex& c = vertices[indices[index + 2]];
				const glm::vec2 minUv = glm::min(glm::min(a.Uv, b.Uv), c.Uv);
				const glm::vec2 maxUv = glm::max(glm::max(a.Uv, b.Uv), c.Uv);
				const int stepsU = std::clamp(
					static_cast<int>(std::ceil((maxUv.x - minUv.x) / stepSize)), 1, 24);
				const int stepsV = std::clamp(
					static_cast<int>(std::ceil((maxUv.y - minUv.y) / stepSize)), 1, 24);
				// UV 域重心坐标(带符号,不做 abs —— 镜像 UV 也要能正确求值);
				// 返回 false = 该点在 UV 三角形之外(不发射,避免"折到边上"的斜向条纹)。
				const float denominator = (b.Uv.y - c.Uv.y) * (a.Uv.x - c.Uv.x)
					+ (c.Uv.x - b.Uv.x) * (a.Uv.y - c.Uv.y);
				if (std::abs(denominator) < 1e-9f)
					continue;   // UV 退化(整条三角形挤成一点):没有棋盘格可看
				const auto barycentric = [&](const glm::vec2& uv, bool* inside, SourceVertex* out)
				{
					const float wA = ((b.Uv.y - c.Uv.y) * (uv.x - c.Uv.x)
						+ (c.Uv.x - b.Uv.x) * (uv.y - c.Uv.y)) / denominator;
					const float wB = ((c.Uv.y - a.Uv.y) * (uv.x - c.Uv.x)
						+ (a.Uv.x - c.Uv.x) * (uv.y - c.Uv.y)) / denominator;
					const float wC = 1.0f - wA - wB;
					if (inside)
						*inside = wA >= -1e-4f && wB >= -1e-4f && wC >= -1e-4f;
					if (!out)
						return;
					out->Position = a.Position * wA + b.Position * wB + c.Position * wC;
					out->Normal = SafeNormalize(a.Normal * wA + b.Normal * wB + c.Normal * wC);
					out->Uv = a.Uv * wA + b.Uv * wB + c.Uv * wC;
				};
				for (int cellV = 0; cellV < stepsV; ++cellV)
				{
					for (int cellU = 0; cellU < stepsU; ++cellU)
					{
						const float u0 = static_cast<float>(cellU) / static_cast<float>(stepsU);
						const float u1 = static_cast<float>(cellU + 1) / static_cast<float>(stepsU);
						const float v0 = static_cast<float>(cellV) / static_cast<float>(stepsV);
						const float v1 = static_cast<float>(cellV + 1) / static_cast<float>(stepsV);
						// 四个角都要落在 UV 三角形内才发射(边界损失 ≤ 一个细分格,肉眼不可见)。
						const glm::vec2 uv[4] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
						SourceVertex corner[4];
						bool allInside = true;
						for (int index = 0; index < 4; ++index)
						{
							bool inside = false;
							barycentric(uv[index], &inside, &corner[index]);
							allInside = allInside && inside;
						}
						if (!allInside)
							continue;
						// 两个子三角形各自按**自己的重心 UV** 取格子奇偶(镜像 UV 也正确)。
						const int triangles[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
						for (const int* triangle : triangles)
						{
							const glm::vec2 centroid = (uv[triangle[0]] + uv[triangle[1]] + uv[triangle[2]])
								/ 3.0f;
							const int cellX = static_cast<int>(std::floor(centroid.x * kUvCheckerCells));
							const int cellY = static_cast<int>(std::floor(centroid.y * kUvCheckerCells));
							MeshBuilder& builder = (((cellX + cellY) & 1) == 0) ? lightBuilder : darkBuilder;
							uint32_t ids[3];
							for (int index = 0; index < 3; ++index)
							{
								const SourceVertex& source = corner[triangle[index]];
								ids[index] = builder.Add(source.Position, source.Normal, source.Uv);
							}
							builder.Triangle(ids[0], ids[1], ids[2]);
						}
					}
				}
			}
			if (light)
				*light = lightBuilder.Build("Material.Preview.CheckerLight");
			if (dark)
				*dark = darkBuilder.Build("Material.Preview.CheckerDark");
		}


		// 预览灯光预设 → LightUniforms(走 Renderer3D::BuildLightRig,与场景同一条打包口径)。
		// 为什么补光/轮廓光用点光:方向光容量 = 2 且 BuildLightRig 只取前 1 个方向光,
		// 三点光只能"1 个主方向光 + 2 个点光"。
LightUniforms BuildPreviewLightUniforms(int preset, float intensity, float azimuthDeg, float elevationDeg){
			const float azimuth = glm::radians(azimuthDeg);
			const float elevation = glm::radians(elevationDeg);
			const glm::vec3 toLight { std::cos(elevation) * std::sin(azimuth), std::sin(elevation),
				std::cos(elevation) * std::cos(azimuth) };
			std::vector<DirectionalLightData> directional;
			std::vector<PointLightData> point;
			AmbientLightData ambient;
			if (preset == 0)
			{
				directional.push_back({ glm::vec3 { 1.0f }, intensity, -toLight, false });
				point.push_back({ glm::vec3 { 1.0f }, intensity * 0.35f, { -1.9f, 0.7f, 1.5f }, 9.0f });
				point.push_back({ glm::vec3 { 1.0f }, intensity * 0.45f, { 0.5f, 1.3f, -2.1f }, 9.0f });
				ambient.Intensity = 0.22f;
			}
			else if (preset == 1)
			{
				directional.push_back({ glm::vec3 { 1.0f }, intensity, -toLight, false });
				ambient.Intensity = 0.12f;
			}
			else
			{
				// 无光:只看自发光(环境光也置 0)。
				ambient.Intensity = 0.0f;
			}
			const bool glDepthConvention = Renderer::GetBackendName() != "vulkan";
			return Renderer3D::BuildLightRig(directional, point, &ambient, glDepthConvention).Uniforms;
		}


		// ---- 无障碍辅助:补标签/悬停说明(值/矩形仍以控件自己登记的为准)----
void AnnotateNode(Wui::WuiContext& ctx, Wui::WuiId id, const std::string& label, const std::string& tooltip){
			const Wui::WuiAccessNode* live = Wui::WuiAccessibility::Get().Find(id);
			Wui::WuiAccessNode node;
			if (live)
				node = *live;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			if (live && !live->Label.empty())
				node.Label = live->Label;
			else
				node.Label = label;
			node.Tooltip = tooltip;
			if (!live)
			{
				node.Kind = "text";
				node.Value = label;
			}
			node.Focused = ctx.Focus() == id;
			Wui::WuiAccessibility::Get().Register(node);
		}


void RegisterReadOnlyNode(Wui::WuiId id, const std::string& label, const std::string& value, const Wui::WuiRect& rect, const std::string& tooltip, const char* kind ){
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = kind;
			node.Label = label;
			node.Value = value;
			node.Tooltip = tooltip;
			node.Rect = rect;
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}

}

MaterialEditorPanel::MaterialEditorPanel() : MaterialEditorPanel(std::string()){
	}


MaterialEditorPanel::MaterialEditorPanel(std::string materialPath){
		// 预览资源属于当前设备:设备释放(后端切换/关闭)前必须把句柄放掉,
		// 否则会在设备之后析构(独立窗口崩溃那次的同类问题)。
		Renderer::RegisterDeviceReleaseHook(this, [this]
		{
			ReleaseGpuResources();
			// 设备重建后注册表整表清空:句柄与注册槽位一起作废。
			m_PreviewTextureId = 0;
			m_UiTextureGeneration = 0;
		});
		SetMaterialPathForPanel(materialPath);
		// M4-S3:代码形态的编译线程随面板起停(空闲时只等条件变量,不占 CPU)。
		m_ShaderCompileThread = std::thread([this] { ShaderCompileWorkerLoop(); });
	}


void MaterialEditorPanel::SetMaterialPathForPanel(const std::string& path){
		const std::string key = path.empty() ? std::string("(unsaved)") : MaterialLibrary::NormalizePath(path);
		m_PanelId = "material:" + key;
		// U2d:换文档(打开/首次保存)后重置"路径校验已被触发"状态 —— 面板一进来不该标红空路径。
		m_NewPathAttempted = false;
		std::filesystem::path file(key);
		std::string name = file.stem().string();
		if (name.empty())
			name = Wui::Tr("panel.material.untitled", "Material");
		m_PanelTitle = Wui::Tr("panel.material.window_title", "Material - ") + name;
		m_Search.clear();
		m_ScrollY = 0.0f;
		m_RevealField.clear();
		m_RevealFrames = 0;
		m_SyncNameBuffer = true;
	}


MaterialEditorPanel::~MaterialEditorPanel(){
		// 先收编译线程:工作线程只读自己的副本、结果写回成员,析构前必须确定它已经退出
		// (单次编译器调用有界,等它跑完即可;detach 会让它写已析构的成员)。
		{
			std::lock_guard<std::mutex> lock(m_ShaderCompileMutex);
			m_ShaderCompileThreadStop = true;
		}
		m_ShaderCompileCv.notify_all();
		if (m_ShaderCompileThread.joinable())
			m_ShaderCompileThread.join();
		Renderer::UnregisterDeviceReleaseHook(this);
		// 面板关闭时设备与帧循环通常还活着:延迟释放,避免销毁在飞命令引用着的资源。
		ReleaseGpuResources(/*defer*/ true);
	}


void MaterialEditorPanel::OpenMaterial(const std::string& path){
		// M4-S2/Slang-B1:同一个编辑器的两种形态 —— 着色器(`.slang`)= 代码形态
		// (预览 | 代码 | 注解参数),`.wmat` = 材质实例形态(左预览 / 右字段,无代码区)。
		// 入口与面板 id 都不变。
		const std::string extension = LowerExtension(std::filesystem::path(path));
		if (extension == ".slang")
		{
			OpenShaderDocument(path);
			return;
		}
		m_ShaderMode = false;
		std::string error;
		Ref<Material> material = MaterialLibrary::Get().Load(path, &error);
		if (!material)
		{
			// M3:加载失败要留下**可读原因**(循环引用 / 父级链坏 / 文件读不到)。
			// 面板已经有一份材质时保留它(只把原因写进状态行);面板还没有材质时记进
			// m_LoadError —— 那种情况下内容区没有别的东西可显示,原因必须自己顶上去。
			const std::string reason = error.empty()
				? Wui::Tr("panel.material.status.load_failed.unknown", "unknown error") : error;
			m_Status = Wui::Tr("panel.material.status.load_failed", "Load failed: ") + reason;
			m_StatusIsError = true;
			if (!m_Material)
				m_LoadError = reason;
			return;
		}
		m_LoadError.clear();
		// 已有未保存改动时换目标:提示并保留原材质(不自动丢弃用户改动)。
		if (m_Material && m_Material->IsDirty() && m_Material->GetPath() != path)
		{
			m_Status = Wui::Tr("panel.material.status.unsaved_switch",
				"Current material has unsaved changes (switched to the new material; the old edits are kept in memory)");
			m_StatusIsError = true;
		}
		else
		{
			m_Status = Wui::Tr("panel.material.status.opened", "Opened ") + path;
			m_StatusIsError = false;
		}
		m_Material = material;
		m_Path = material->GetPath();
		SetMaterialPathForPanel(m_Path);
		// 校验缓存按路径/Revision 失效,换文档要立刻重算。
		m_ValidationRevision = 0;
		m_ValidationPath.clear();
		RefreshCatalog();
		RefreshMaterialPickIndex();
		WLD_CORE_INFO("[material-ui] opened material '{0}'", m_Path);
	}


void MaterialEditorPanel::RefreshCatalog(){
		const double now = std::chrono::duration<double>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		if (now - m_CatalogRefreshTime < 1.5)
			return;
		m_CatalogRefreshTime = now;
		m_MaterialPaths = MaterialLibrary::Get().ScanMaterials();
		RefreshMaterialPickIndex();
		// M4-TEX-P11:纹理候选清单(资产 / 源图 + 徽标)由 `Editor::TextureRefCatalog` 自己按
		// 1.5s TTL 维护 —— 三个调用点(槽位 / `.wmat` 参数行 / `.slang` 参数列)共用同一份缓存。
		// 刚导入/刚赋值那种"盘上变了、要立刻看见"的场合由调用点显式 `InvalidateTextureRefCatalog()`。
	}



bool MaterialEditorPanel::TextureFieldHasIssue(const std::string& key) const{
		for (const ValidationEntry& entry : m_Validation)
			if (entry.Field == key && (entry.Severity == "missing" || entry.Severity == "missing-source"
				|| entry.Severity == "unreferenced"))
				return true;
		return false;
	}


	// 材质名下拉的选中下标(下标表 = m_MaterialPaths;找不到 = -1 = 未落盘的新材质)。
void MaterialEditorPanel::RefreshMaterialPickIndex(){
		if (!m_Material)
			return;
		m_MaterialPickIndex = -1;
		for (size_t i = 0; i < m_MaterialPaths.size(); ++i)
			if (m_MaterialPaths[i] == m_Path)
				m_MaterialPickIndex = static_cast<int>(i);
	}


bool MaterialEditorPanel::FieldModified(const std::string& key) const{
		if (!m_Material)
			return false;
		// M3:可继承字段的"已改"= 本文件显式写了它(覆盖位)。未覆盖的字段跟随父级链,
		// 值可能与引擎默认不同,但那不是"这一行改过"。
		MaterialField field;
		if (FieldForKey(key, &field))
			return m_Material->HasOverride(field);
		const MaterialDesc defaults;
		return FieldDiffersFromDefault(key, m_Material->GetDesc(), defaults);
	}


	// ---- 头部:材质名 + 来源逻辑路径 + 脏标记 + Save / Reveal / Revert ----
float MaterialEditorPanel::DrawHeader(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host){
		const Wui::WuiTheme& theme = host.Theme();
		// U23(用户 2026-09-22「材质编辑器上面有空行,太丑了」):头部高度**按内容算**,
		// 行间距全部取主题令牌(不再出现 kHeaderBaseHeight 这种固定占位造成的空白带)。
		const float gap = theme.PadSmall;
		const float x = rect.X;
		const float y = rect.Y + gap;
		const float buttonH = theme.ControlHeight;
		const float actionGap = 6.0f;   // 动作按钮之间的间距(控件间距,与容器留白无关)
		const bool dirty = m_Material && m_Material->IsDirty();
		const bool readOnly = host.IsReadOnlyMode();

		// ---- U25-M2:头部动作表(唯一落点)----
		// Save / Save As… / Assign to Selection / Undo Assign / Reveal / Revert。
		// 按钮宽度按文案量出来(**不硬编码 76**):中文与英文都放得下;一行放不下就换行,
		// 窄窗因此不会把动作挤到面板之外(它们仍有稳定 a11y id,ui.invoke 永远点得到)。
		struct HeaderAction
		{
			const char* Id = "";
			std::string Label;
			std::string Doc;
			bool Enabled = true;
			bool Primary = false;
		};
		const std::string assignBlockedDoc = Wui::Tr("panel.material.assign.blocked",
			"Assign to Selection: unavailable while Play/Simulate runs (the scene is read-only).");
		const std::string assignDoc = readOnly ? assignBlockedDoc
			: Wui::Tr("panel.material.assign.tooltip",
				"Assign to Selection: write this material into the MeshRenderer of the entity selected in "
				"the Scene Hierarchy (the same field the Properties panel edits).");
		const std::string undoDoc = !m_AssignUndoValid
			? Wui::Tr("panel.material.assign.undo.none",
				"Undo Assign: nothing to undo — this panel has not assigned a material yet.")
			: (readOnly ? assignBlockedDoc
				: Wui::Tr("panel.material.assign.undo.tooltip",
					"Undo Assign: write the previous material path back into the entity this panel "
					"assigned just now (one level: only that write)."));
		const std::vector<HeaderAction> headerActions {
			{ "material.save", Wui::Tr("panel.material.save", "Save"),
				Wui::Tr("panel.material.save.tooltip",
					"Save: write the current values back to the .wmat on disk."), true, true },
			{ "material.saveas", Wui::Tr("panel.material.saveas", "Save As…"),
				Wui::Tr("panel.material.saveas.tooltip",
					"Save As…: write a variant (default name <name>_variant) in a folder under the content "
					"root, then keep editing the variant. The source file is never overwritten."), true, false },
			{ "material.assign", Wui::Tr("panel.material.assign", "Assign to Selection"), assignDoc,
				!readOnly, false },
			{ "material.assign.undo", Wui::Tr("panel.material.assign.undo", "Undo Assign"), undoDoc,
				m_AssignUndoValid && !readOnly, false },
			{ "material.extract", Wui::Tr("panel.material.extract", "Extract from Selection"),
				(!readOnly && host.GetSelectedEntity().IsValid())
					? Wui::Tr("panel.material.extract.tooltip",
						"Extract from Selection: start a new material from the selected entity's current "
						"render setup (its material, or its colour), then assign the result back to it.")
					: (readOnly
						? Wui::Tr("panel.material.extract.blocked_play",
							"Extract from Selection: unavailable while Play/Simulate runs (the scene is read-only).")
						: Wui::Tr("panel.material.extract.blocked_none",
							"Extract from Selection: select an entity in the Scene Hierarchy first.")),
				!readOnly && host.GetSelectedEntity().IsValid(), false },
			{ "material.reveal", Wui::Tr("panel.material.reveal", "Reveal"),
				Wui::Tr("panel.material.reveal.tooltip",
					"Reveal: select the .wmat file in Windows Explorer (needs a saved file)."), true, false },
			{ "material.revert", Wui::Tr("panel.material.revert", "Revert"),
				Wui::Tr("panel.material.revert.tooltip",
					"Revert: drop unsaved edits and read the .wmat from disk again."), true, false },
		};
		float actionX = x;
		float actionY = y;
		float actionsHeight = buttonH;
		for (const HeaderAction& action : headerActions)
		{
			const float measured = ctx.MeasureTextWidth(action.Label, 13.0f) + 18.0f;
			const float width = std::min(std::max(kActionMinWidth, measured), std::max(24.0f, rect.W - 2.0f));
			if (actionX > x && actionX + width > x + rect.W - 2.0f)
			{
				actionX = x;
				actionY += buttonH + actionGap;
			}
			const Wui::WuiRect actionRect { actionX, actionY, width, buttonH };
			const std::string actionId = action.Id;
			if (ActionButton(ctx, Wui::HashId(action.Id), actionRect, action.Label, action.Doc,
				action.Enabled, action.Primary, theme))
			{
				if (actionId == "material.save")
					SaveCurrent(host);
				else if (actionId == "material.saveas")
					OpenSaveAsModal(ctx, host);
				else if (actionId == "material.assign")
					AssignToSelection(host);
				else if (actionId == "material.assign.undo")
					UndoAssign(host);
				else if (actionId == "material.extract")
				{
					// 向导住在内容浏览器面板(模板/名称/目录/落点控件都在那边):
					// 面板只负责把"从选中对象提取"的意图传过去,并回显结果。
					std::string message;
					if (host.OpenNewMaterialWizard(true, &message))
					{
						m_Status = Wui::Tr("panel.material.status.extract_opened",
							"Extract from Selection: the new-material wizard is open in the Content Browser "
							"(it will assign the result back to the selected entity)");
						m_StatusIsError = false;
					}
					else
					{
						m_Status = Wui::Tr("panel.material.status.extract_failed",
							"Extract from Selection failed: ")
							+ (message.empty()
								? Wui::Tr("panel.material.status.no_selection", "no selection") : message);
						m_StatusIsError = true;
					}
				}
				else if (actionId == "material.reveal")
					RevealMaterialOnDisk();
				else if (actionId == "material.revert")
				{
					if (m_Path.empty())
					{
						m_Status = Wui::Tr("panel.material.status.revert_failed",
							"Revert failed: this material has never been saved to disk");
						m_StatusIsError = true;
					}
					else
					{
						std::string error;
						if (MaterialLibrary::Get().Reload(m_Path, &error))
						{
							RefreshMaterialPickIndex();
							m_ValidationRevision = 0;
							m_SyncNameBuffer = true;
							m_Status = Wui::Tr("panel.material.status.reloaded", "Reloaded from disk ") + m_Path;
							m_StatusIsError = false;
						}
						else
						{
							m_Status = Wui::Tr("panel.material.status.reload_failed", "Reload failed: ") + error;
							m_StatusIsError = true;
						}
					}
				}
			}
			actionsHeight = (actionY - y) + buttonH;
			actionX += actionRect.W + actionGap;
		}

		// 脏标记(状态节点,不是按钮):* = 内存与磁盘不一致。
		const std::string dirtyText = dirty ? Wui::Tr("panel.material.dirty", "* Unsaved changes")
			: Wui::Tr("panel.material.clean", "Saved");
		const float dirtyWidth = ctx.MeasureTextWidth(dirtyText, 11.0f);
		const Wui::WuiRect dirtyRect { x + rect.W - dirtyWidth - 6.0f, y + 5.0f, dirtyWidth, 16.0f };
		Wui::Label(ctx, { dirtyRect.X, dirtyRect.Y }, dirtyText,
			dirty ? theme.Warning : theme.TextDisabled, 11.0f);
		const std::string dirtyDoc = Wui::Tr("panel.material.dirty.tooltip",
			"Unsaved changes are kept in memory until you press Save; the file on disk is unchanged.");
		Wui::Tooltip(ctx, dirtyRect, dirtyDoc);
		RegisterReadOnlyNode(Wui::HashId("material.dirty"),
			Wui::Tr("panel.material.dirty.label", "Unsaved changes"), dirty ? "true" : "false",
			dirtyRect, dirtyDoc);

		// ---- M3:父级行(Inherits: <父> [打开父材质])----
		// 位置 = 动作行与名称行之间:头部的"最后一行"仍然是名称/路径(或状态行),
		// 因此 U23 的"标题底 → 首个内容控件 = PadSmall"口径不受影响。
		const std::string parentPath = m_Material->ParentPath();
		const bool parentIsEngineDefault = parentPath.empty();
		const bool parentMissing = m_Material->IsParentMissing();
		const std::string parentText = parentIsEngineDefault
			? Wui::Tr("panel.material.parent.engine", "Engine Default") : parentPath;
		const float parentRowY = y + actionsHeight + gap;
		const float parentRowH = theme.ControlHeight;
		const std::string parentDoc = parentIsEngineDefault
			? Wui::Tr("panel.material.parent.engine.tooltip",
				"Inherits: Engine Default. Every field this file does not write comes from the "
				"engine's built-in material.")
			: (parentMissing
				? Wui::Tr("panel.material.parent.missing.tooltip",
					"Inherits: the declared parent could not be read — this material falls back to "
					"the engine default and stays usable.")
				: Wui::Tr("panel.material.parent.file.tooltip",
					"Inherits: every field this file does not write comes from this parent material. "
					"Open Parent Material switches this window to it (unsaved edits ask first)."));
		const std::string openParentLabel = Wui::Tr("panel.material.parent.open",
			"Open Parent Material");
		const float openParentWidth = std::min(std::max(60.0f, rect.W - 60.0f),
			ctx.MeasureTextWidth(openParentLabel, 12.0f) + 18.0f);
		const Wui::WuiRect openParentRect { x + rect.W - openParentWidth - 4.0f, parentRowY,
			openParentWidth, parentRowH };
		const bool canOpenParent = !parentIsEngineDefault && !parentMissing
			&& m_Material->ResolvedParent() != nullptr;
		if (!parentIsEngineDefault)
		{
			if (ActionButton(ctx, Wui::HashId("material.parent.open"), openParentRect,
				openParentLabel, parentDoc, canOpenParent, false, theme))
				OpenParentMaterial(ctx, host);
		}
		const float parentBudget = parentIsEngineDefault
			? std::max(40.0f, rect.W - 8.0f)
			: std::max(40.0f, openParentRect.X - x - 8.0f);
		const std::string parentLine = Wui::Tr("material.parent.prefix", "Inherits:") + " "
			+ parentText;
		Wui::Label(ctx, { x, parentRowY + 5.0f },
			EllipsizeToWidth(ctx, parentLine, parentBudget, 12.0f),
			parentMissing ? theme.Danger : theme.TextMuted, 12.0f);
		RegisterReadOnlyNode(Wui::HashId("material.parent"),
			Wui::Tr("panel.material.parent", "Inherits"), parentText,
			{ x, parentRowY, parentBudget, parentRowH }, parentDoc);
		float parentWarningHeight = 0.0f;
		if (parentMissing)
		{
			// 父级缺失:退化成引擎默认,但要**明说**(值里带声明路径,AI 通道可直接断言)。
			const std::string warning = Wui::Tr("panel.material.parent.missing",
				"Parent missing — using Engine Default:") + " " + parentPath + " — "
				+ m_Material->ParentWarning();
			const Wui::WuiRect warningRect { x, parentRowY + parentRowH + 2.0f,
				std::max(40.0f, rect.W - 8.0f), kHeaderStatusHeight + 4.0f };
			Wui::Label(ctx, { x, warningRect.Y }, EllipsizeToWidth(ctx, warning, warningRect.W, 11.0f),
				theme.Danger, 11.0f);
			RegisterReadOnlyNode(Wui::HashId("material.parent.warning"),
				Wui::Tr("panel.material.parent.warning", "Parent missing"), warning, warningRect, warning);
			Wui::Tooltip(ctx, warningRect, warning);
			parentWarningHeight = warningRect.H + gap;
		}

		// 第二行:材质名 + 来源逻辑路径(紧贴父级行下方一个 PadSmall)。
		const float lineY = parentRowY + parentRowH + gap + parentWarningHeight;
		const MaterialDesc& desc = m_Material->GetDesc();
		std::filesystem::path file(m_Path.empty() ? std::string() : m_Path);
		std::string fallbackName = file.stem().string();
		if (fallbackName.empty())
			fallbackName = Wui::Tr("panel.material.untitled", "Material");
		const std::string name = desc.Name.empty() ? fallbackName : desc.Name;
		const float nameWidth = std::max(40.0f,
			std::min(rect.W * 0.5f, ctx.MeasureTextWidth(name, 13.0f)));
		Wui::Label(ctx, { x, lineY }, EllipsizeToWidth(ctx, name, nameWidth, 13.0f), theme.Text, 13.0f);
		const std::string nameDoc = Wui::Tr("panel.material.name.tooltip",
			"Material display name (the .wmat Name field). Asset identity is the file path; rename the file "
			"in the Content Browser to change where it lives.");
		Wui::Tooltip(ctx, { x, lineY, nameWidth, kHeaderTextHeight }, nameDoc);
		RegisterReadOnlyNode(Wui::HashId("material.name"),
			Wui::Tr("panel.material.name", "Material name"), name,
			{ x, lineY, nameWidth, kHeaderTextHeight }, nameDoc);
		const float pathX = x + nameWidth + 10.0f;
		if (pathX < x + rect.W - 60.0f)
		{
			const float pathWidth = rect.W - (pathX - x) - 6.0f;
			const std::string pathText = m_Path.empty()
				? Wui::Tr("panel.material.unsaved_new", "(unsaved new material)")
				: ShortenPath(m_Path);
			Wui::Label(ctx, { pathX, lineY + 2.0f },
				EllipsizeToWidth(ctx, pathText, pathWidth, 11.0f), theme.TextMuted, 11.0f);
			const std::string pathDoc = Wui::Tr("panel.material.path.tooltip",
				"Source asset path, relative to the project content root.");
			Wui::Tooltip(ctx, { pathX, lineY, pathWidth, kHeaderTextHeight }, pathDoc);
			RegisterReadOnlyNode(Wui::HashId("material.path"),
				Wui::Tr("panel.material.path", "Source path"), pathText,
				{ pathX, lineY, pathWidth, kHeaderTextHeight }, pathDoc);
		}

		// U25-M2 B:拖 .wmat 到**标题/头部件** = 在本窗口打开该材质(有未保存改动先确认)。
		// 落点 = 名称/路径这一行(用户眼里的"标题");贴图等其它类型到这里给可读反馈,不静默改。
		const Wui::WuiRect titleRect { x, lineY, std::max(60.0f, rect.W - 2.0f), kHeaderTextHeight };
		RegisterHeaderDrop(ctx, host, titleRect);
		TakeHeaderDrop(ctx, host);

		// 头部真实高度 = 最后一行文字的底边(名字/路径行),不留空白带。
		float used = (lineY - rect.Y) + kHeaderTextHeight;
		if (m_Path.empty())
		{
			// 未落盘材质:给出"另存为"路径输入 + 行内校验。
			const Wui::WuiRect field { x, rect.Y + used + gap, rect.W - 8.0f, 22.0f };
			NoteFocusOwningRect(field);   // MAT-UI3b:未落盘材质的路径输入自己接手焦点
			const std::string pathError = NewMaterialPathError(m_NewPathBuffer);
			const std::string shownError = m_NewPathBuffer.empty()
				? (m_NewPathAttempted ? pathError : std::string()) : pathError;
			Wui::TextFieldEx(ctx, Wui::HashId("material.newpath"), field, m_NewPathBuffer, theme, shownError);
			used += gap + 22.0f;
		}
		if (!m_Status.empty())
		{
			const Wui::WuiRect statusRect { x, rect.Y + used + gap,
				std::max(20.0f, rect.W - 8.0f), kHeaderStatusHeight };
			Wui::Label(ctx, { x, statusRect.Y }, EllipsizeToWidth(ctx, m_Status, rect.W - 8.0f, 11.0f),
				m_StatusIsError ? theme.Danger : theme.TextMuted, 11.0f);
			// U23:状态行也登记无障碍节点 —— 它是头部的一部分,"标题底"要按它算
			// (顶部间距的验收口径:标题/路径/动作条整块 → 首个内容控件)。
			RegisterReadOnlyNode(Wui::HashId("material.status"),
				Wui::Tr("panel.material.status", "Status"), m_Status, statusRect,
				Wui::Tr("panel.material.status.tooltip",
					"Result of the most recent material action (open / save / reload / reveal)."));
			used += gap + kHeaderStatusHeight;
		}
		return used;
	}


	// ==== M4-S2/Slang-B1:Material Shader(`.slang`)代码形态 ====
	//
	// 用户口径:「打开着色器:左预览 / 中代码 / 右参数 —— 右栏显示 shader 声明的参数与其默认值」。
	// 形态由**打开的文件扩展名**决定(同一个面板、同一套窗口/停靠机制):
	//   - 代码列复用 Wui::CodeEditor 内核(行号 / 高亮 / 选区 / 撤销 / 滚动 / 诊断行),
	//     这里只提供 Slang/HLSL 语法族的 token 化(SlangHighlight.h);
	//   - 参数列的事实源是**文件里的注解**(M4-S2 内核的 ParseMaterialParams);
	//     改默认值 = 改写注解文本,不是改内存里的影子值;
	//   - 真正的"防抖编译 + 原子换管线"属 M4-S3;本批按需编译只验证源码能过编译器,
	//     预览仍用引擎默认表面材质(注解里认识的名字会映射进去),面板上写明这一点。
void MaterialEditorPanel::OpenShaderDocument(const std::string& path){
		std::string normalized = path;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (normalized.empty())
			return;
		// 面板 id/标题/搜索态复位与 .wmat 同一条路径(面板 id 仍是 material:<逻辑路径>,
		// 所以宿主、布局存档、AI 通道的面板寻址都不用新增一套)。
		SetMaterialPathForPanel(normalized);
		m_ShaderMode = true;
		m_ShaderPath = MaterialLibrary::NormalizePath(normalized);
		// MAT-FN3:`shaders/lib/**` 里的 `.slang` 是材质函数库(不是材质资产)—— 窗口标题与
		// 右栏(参数区)都按"库文件"呈现;面板 id 不变(还是 material:<逻辑路径>)。
		m_ShaderIsLibrary = IsMaterialLibraryShaderPath(m_ShaderPath);
		m_PanelTitle = (m_ShaderIsLibrary
			? Wui::Tr("panel.material.shader.library.window_title", "Material Function - ")
			: Wui::Tr("panel.material.shader.window_title", "Material Shader - "))
			+ std::filesystem::path(m_ShaderPath).stem().string();
		// 代码形态不是材质资产:材质字段/引用者/另存这些路径不参与。
		m_Path.clear();
		m_LoadError.clear();
		m_ShaderGroupOpen.clear();
		m_ShaderParamTextBuffers.clear();
		m_ShaderPendingParamName.clear();
		m_ShaderPendingParamValue.clear();
		// M4-S3:换文档时编译状态/键/磁盘指纹一起复位(上一份文档的键与结果都不再相关)。
		m_ShaderDiagnostics.clear();
		m_ShaderCompileStatus.clear();
		m_ShaderCompileFailed = false;
		m_ShaderInstalledKey.clear();
		m_ShaderInstalledVersion = 0;
		m_ShaderDiskChangedNotice = false;
		m_ShaderDiskStampValid = false;
		m_ShaderDiskText.clear();
		m_ShaderRequestedRevision = ~0ull;
		m_ShaderEditTime = 0.0;
		// 预览替身 = 引擎默认表面材质(SetDesc 在解析成功后按注解默认值映射)。
		m_Material = MaterialLibrary::Get().CreateDefault("Shader Preview");
		RefreshCatalog();   // 贴图下拉的选项(与 .wmat 共用同一份 2s 缓存)
		LoadShaderFromDisk();
	}


void MaterialEditorPanel::LoadShaderFromDisk(){
		// 打开 / Revert / 热重载共用:磁盘内容 = 已保存内容(D2 键分离的事实源)。
		m_ShaderCompileStatus.clear();
		m_ShaderCompileFailed = false;
		m_ShaderDiagnostics.clear();
		m_ShaderForceCompile = false;
		if (m_ShaderPath.empty())
		{
			m_ShaderStatus = Wui::Tr("panel.material.shader.no_path", "No shader path");
			m_ShaderStatusIsError = true;
			m_ShaderDiskText.clear();
			m_ShaderDiskStampValid = false;
			return;
		}
		const std::filesystem::path diskPath = ContentRootPath() / m_ShaderPath;
		std::error_code fileError;
		if (!std::filesystem::is_regular_file(diskPath, fileError))
		{
			m_ShaderStatus = Wui::Tr("panel.material.shader.status.missing", "Shader file not found: ")
				+ m_ShaderPath;
			m_ShaderStatusIsError = true;
			m_ShaderBuffer.SetText(std::string());
			m_ShaderDiskText.clear();
			m_ShaderDiskStampValid = false;
			RefreshShaderParams();
			return;
		}
		std::ifstream input(diskPath, std::ios::binary);
		if (!input.is_open())
		{
			m_ShaderStatus = Wui::Tr("panel.material.shader.status.unreadable", "Cannot read: ") + m_ShaderPath;
			m_ShaderStatusIsError = true;
			m_ShaderBuffer.SetText(std::string());
			m_ShaderDiskText.clear();
			m_ShaderDiskStampValid = false;
			RefreshShaderParams();
			return;
		}
		std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		m_ShaderBuffer.SetText(std::move(source));   // 视为已保存状态
		m_ShaderHighlight.Clear();
		RefreshShaderParams();
		// M4-S3:记录磁盘指纹 + 立刻编译一次已保存内容并 Install(路径键)—— 场景与预览的基线。
		m_ShaderDiskText = m_ShaderBuffer.Text();
		RecordShaderDiskStamp(m_ShaderDiskText);
		m_ShaderDiskChangedNotice = false;
		m_ShaderRequestedRevision = ~0ull;
		m_ShaderSeenRevision = m_ShaderBuffer.Revision();
		// MAT-FN3:普通材质着色器打开即编译一次(场景/预览的基线);材质函数库不编译
		// —— 它没有 `Evaluate`,只随引用它的材质一起烘。
		m_ShaderForceCompile = !m_ShaderIsLibrary;
		m_ShaderStatus = Wui::Tr("panel.material.shader.status.loaded", "Loaded ") + m_ShaderPath;
		m_ShaderStatusIsError = false;
		WLD_CORE_INFO("[material-ui] opened shader '{0}'", m_ShaderPath);
	}

namespace MaterialEditorPanelDetail
{
double ShaderWallClockSeconds(){
			return std::chrono::duration<double>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}


std::string FormatShaderMilliseconds(double value){
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.1f", value > 0.0 ? value : 0.0);
			return std::string(buffer);
		}


bool ShaderBackendIsVulkan(){
			const std::string name = Renderer::GetBackendName();
			std::string lower = name;
			std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c)
			{
				return static_cast<char>(std::tolower(c));
			});
			return lower.find("vulkan") != std::string::npos;
		}

}

std::string MaterialEditorPanel::ShaderPathKey() const{
		return MaterialLibrary::NormalizePath(m_ShaderPath);
	}


std::string MaterialEditorPanel::ShaderPreviewKey() const{
		const std::string base = ShaderPathKey();
		return base.empty() ? std::string() : base + "#preview";
	}


std::string MaterialEditorPanel::FormatShaderDiagnostic(const ShaderDiagnostic& diagnostic){
		std::string text = diagnostic.Severity.empty() ? std::string("error") : diagnostic.Severity;
		text += ": ";
		if (diagnostic.InUserSource && diagnostic.Line > 0)
		{
			char position[64] = {};
			std::snprintf(position, sizeof(position),
				Wui::Tr("panel.material.shader.diagnostic.line", "line %u:%u").c_str(),
				diagnostic.Line, diagnostic.Column);
			text += position;
			text += "  ";
		}
		text += diagnostic.Message;
		return text;
	}

}
