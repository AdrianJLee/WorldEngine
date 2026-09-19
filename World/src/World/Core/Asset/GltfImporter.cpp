// D5:节点 matrix → TRS 用 glm::decompose,它属于 gtx 实验扩展,必须先开这个宏再包含。
#define GLM_ENABLE_EXPERIMENTAL

#include "wldpch.h"
#include "World/Core/Asset/GltfImporter.h"

#include "World/Core/Asset/ModelImportSettings.h"
#include "World/Core/Asset/WModelIO.h"
#include "World/Core/Log.h"
#include "World/Renderer/Material.h"

#include <cgltf.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace World::Asset
{
	namespace
	{
		const char* kSupportedRequiredExtensions[] = {
			// 量化属性(cgltf_accessor_read_float 会转回 float)对导入无影响。
			"KHR_mesh_quantization",
		};

		std::string SanitizeName(const std::string& raw, const std::string& fallback)
		{
			std::string out;
			out.reserve(raw.size());
			for (const char character : raw)
			{
				const unsigned char value = static_cast<unsigned char>(character);
				out.push_back((std::isalnum(value) || character == '_' || character == '-') ? character : '_');
			}
			return out.empty() ? fallback : out;
		}

		// 引擎的 MaterialDesc 颜色约定是 sRGB 空间取值(见 Material.h);glTF 的
		// baseColorFactor/emissiveFactor 是线性值,导入时按着色器解码的逆变换转成 sRGB。
		float LinearToSrgb(float value)
		{
			if (!(value > 0.0f))
				return 0.0f;
			return std::pow(value, 1.0f / 2.2f);
		}

		glm::vec4 LinearToSrgb4(const cgltf_float* rgba)
		{
			return { LinearToSrgb(rgba[0]), LinearToSrgb(rgba[1]), LinearToSrgb(rgba[2]), rgba[3] };
		}

		glm::vec3 LinearToSrgb3(const cgltf_float* rgb)
		{
			return { LinearToSrgb(rgb[0]), LinearToSrgb(rgb[1]), LinearToSrgb(rgb[2]) };
		}

		std::string ExtensionFromMime(const char* mime)
		{
			if (!mime)
				return {};
			const std::string value = mime;
			if (value == "image/png")
				return "png";
			if (value == "image/jpeg")
				return "jpg";
			return {};
		}

		std::string ExtensionFromUri(const std::string& uri)
		{
			const size_t dot = uri.find_last_of('.');
			if (dot == std::string::npos)
				return {};
			std::string extension = uri.substr(dot + 1);
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			if (extension == "jpeg")
				extension = "jpg";
			return extension;
		}

		bool WriteFileBytes(const std::filesystem::path& path, const void* bytes, size_t size, std::string* error)
		{
			std::error_code ec;
			if (!path.parent_path().empty())
				std::filesystem::create_directories(path.parent_path(), ec);
			const std::filesystem::path temporary = path.string() + ".tmp";
			{
				std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
				if (!file)
				{
					if (error) *error = "cannot write " + temporary.string();
					return false;
				}
				if (size > 0)
					file.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
				if (!file)
				{
					if (error) *error = "write failed: " + temporary.string();
					return false;
				}
			}
			std::filesystem::rename(temporary, path, ec);
			if (ec)
			{
				// Windows 上目标已存在时 rename 会失败:先删再换。
				std::filesystem::remove(path, ec);
				ec.clear();
				std::filesystem::rename(temporary, path, ec);
			}
			if (ec)
			{
				if (error) *error = "cannot replace " + path.string() + ": " + ec.message();
				return false;
			}
			return true;
		}

		struct ImagePayload
		{
			std::vector<uint8_t> Bytes;
			std::string Extension;
		};

		// 读取一个 glTF image 的原始字节:external uri(相对 glTF 文件解析)或内嵌 bufferView。
		bool ReadImagePayload(const std::filesystem::path& sourceDirectory, const cgltf_image& image,
			ImagePayload& out, std::string& why)
		{
			out = ImagePayload {};
			if (image.buffer_view != nullptr)
			{
				const cgltf_buffer_view& view = *image.buffer_view;
				if (view.buffer == nullptr || view.buffer->data == nullptr)
				{
					why = "embedded image buffer was not loaded";
					return false;
				}
				const auto* begin = static_cast<const uint8_t*>(view.buffer->data) + view.offset;
				out.Bytes.assign(begin, begin + view.size);
				out.Extension = ExtensionFromMime(image.mime_type);
				if (out.Extension.empty())
					out.Extension = ExtensionFromUri(image.name ? image.name : "");
				if (out.Extension.empty())
				{
					why = "embedded image has no supported mime type (expected image/png or image/jpeg)";
					return false;
				}
			}
			else if (image.uri != nullptr)
			{
				std::string uri = image.uri;
				if (uri.rfind("data:", 0) == 0)
				{
					why = "image data URI is not supported (use an external file or a bufferView)";
					return false;
				}
				// glTF uri 允许百分号编码:解码后再按 glTF 文件目录解析。
				std::vector<char> decoded(uri.begin(), uri.end());
				decoded.push_back('\0');
				const cgltf_size length = cgltf_decode_uri(decoded.data());
				uri.assign(decoded.data(), length);

				out.Extension = ExtensionFromUri(uri);
				if (out.Extension.empty())
					out.Extension = ExtensionFromMime(image.mime_type);
				const std::filesystem::path resolved = sourceDirectory / std::filesystem::path(uri);
				std::ifstream file(resolved, std::ios::binary);
				if (!file)
				{
					why = "cannot open image '" + resolved.string() + "'";
					return false;
				}
				out.Bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			}
			else
			{
				why = "image has neither uri nor bufferView";
				return false;
			}

			if (out.Bytes.empty())
			{
				why = "image payload is empty";
				return false;
			}
			if (out.Extension != "png" && out.Extension != "jpg")
			{
				why = "unsupported image format '" + out.Extension + "' (expected png or jpg)";
				return false;
			}
			return true;
		}

		bool IsSupportedRequiredExtension(const char* name)
		{
			for (const char* supported : kSupportedRequiredExtensions)
				if (std::strcmp(name, supported) == 0)
					return true;
			return false;
		}

		bool IsKnownExtension(const char* name)
		{
			return IsSupportedRequiredExtension(name);
		}

		WModelBounds ComputeBounds(const std::vector<glm::vec3>& positions)
		{
			WModelBounds bounds;
			if (positions.empty())
				return bounds;
			glm::vec3 min(std::numeric_limits<float>::max());
			glm::vec3 max(std::numeric_limits<float>::lowest());
			for (const glm::vec3& position : positions)
			{
				min = glm::min(min, position);
				max = glm::max(max, position);
			}
			bounds.Min = min;
			bounds.Max = max;
			return bounds;
		}

		WModelBounds ComputeBounds(const std::vector<WModelVertex>& vertices)
		{
			WModelBounds bounds;
			if (vertices.empty())
				return bounds;
			glm::vec3 min(std::numeric_limits<float>::max());
			glm::vec3 max(std::numeric_limits<float>::lowest());
			for (const WModelVertex& vertex : vertices)
			{
				min = glm::min(min, vertex.Position);
				max = glm::max(max, vertex.Position);
			}
			bounds.Min = min;
			bounds.Max = max;
			return bounds;
		}

		// 缺法线时的面法线生成:按三角形面积加权累积(退化三角形/孤立顶点回退 +Y)。
		void GenerateFaceNormals(const std::vector<glm::vec3>& positions,
			const std::vector<uint32_t>& indices, std::vector<glm::vec3>& normals)
		{
			normals.assign(positions.size(), glm::vec3(0.0f));
			for (size_t index = 0; index + 2 < indices.size(); index += 3)
			{
				const glm::vec3& a = positions[indices[index]];
				const glm::vec3& b = positions[indices[index + 1]];
				const glm::vec3& c = positions[indices[index + 2]];
				const glm::vec3 faceNormal = glm::cross(b - a, c - a);
				normals[indices[index]] += faceNormal;
				normals[indices[index + 1]] += faceNormal;
				normals[indices[index + 2]] += faceNormal;
			}
			for (glm::vec3& normal : normals)
			{
				if (glm::length(normal) > 1e-8f)
					normal = glm::normalize(normal);
				else
					normal = glm::vec3(0.0f, 1.0f, 0.0f);
			}
		}

		const cgltf_accessor* FindAttribute(const cgltf_primitive& primitive,
			cgltf_attribute_type type, cgltf_int index)
		{
			for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
				if (primitive.attributes[i].type == type && primitive.attributes[i].index == index)
					return primitive.attributes[i].data;
			return nullptr;
		}

		bool ReadVec3(const cgltf_accessor& accessor, cgltf_size index, glm::vec3& out)
		{
			cgltf_float values[3] = { 0.0f, 0.0f, 0.0f };
			if (!cgltf_accessor_read_float(&accessor, index, values, 3))
				return false;
			out = { values[0], values[1], values[2] };
			return true;
		}

		bool ReadVec2(const cgltf_accessor& accessor, cgltf_size index, glm::vec2& out)
		{
			cgltf_float values[2] = { 0.0f, 0.0f };
			if (!cgltf_accessor_read_float(&accessor, index, values, 2))
				return false;
			out = { values[0], values[1] };
			return true;
		}

		uint64_t Fnv1a64(const void* data, size_t size)
		{
			uint64_t hash = 14695981039346656037ULL;
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t index = 0; index < size; ++index)
			{
				hash ^= bytes[index];
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		// ---- D10:导入内容去重(材质/贴图复用)----
		//
		// 背景(用户 2026-09-19):同一份源材质被多个模型引用时,过去每个模型都会生成
		// 一份自己的 `<模型名>_<材质名>.wmat`,改一份不影响其它模型 —— 等于没有复用。
		// 这里在落盘前先按**内容哈希**在候选目录里找同内容文件,找到就复用它的路径。
		//
		// 只扫描指定目录里的同扩展名文件(不递归),先比大小再比哈希;目录不存在/读不动
		// 一律当作"没找到"(不影响导入本身)。
		std::string FindReusableFile(const std::filesystem::path& contentRoot,
			const std::vector<std::string>& logicalDirectories, const std::string& extension,
			const std::vector<uint8_t>& data)
		{
			if (contentRoot.empty() || data.empty())
				return {};
			const uint64_t hash = Fnv1a64(data.data(), data.size());
			for (const std::string& directory : logicalDirectories)
			{
				if (directory.empty())
					continue;
				std::error_code ec;
				const std::filesystem::path dir = contentRoot / std::filesystem::path(directory);
				std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
				if (ec)
					continue;
				for (const std::filesystem::directory_entry& entry : it)
				{
					std::error_code typeError;
					if (!entry.is_regular_file(typeError))
						continue;
					if (entry.path().extension().string() != extension)
						continue;
					std::error_code sizeError;
					const uintmax_t size = entry.file_size(sizeError);
					if (sizeError || size != data.size())
						continue;
					// 就地读一遍(与 ReadSourceBytes 同一逻辑,这里内联避免前向声明)。
					std::ifstream file(entry.path(), std::ios::binary);
					if (!file)
						continue;
					const std::vector<uint8_t> candidate { std::istreambuf_iterator<char>(file),
						std::istreambuf_iterator<char>() };
					if (candidate.size() != data.size()
						|| Fnv1a64(candidate.data(), candidate.size()) != hash)
						continue;
					// 命中:转成相对内容根的逻辑路径(统一正斜杠)。
					std::error_code relativeError;
					const std::filesystem::path relative =
						std::filesystem::relative(entry.path(), contentRoot, relativeError);
					if (relativeError || relative.empty())
						continue;
					return relative.generic_string();
				}
			}
			return {};
		}

		bool ReadSourceBytes(const std::string& sourcePath, std::vector<uint8_t>& out)
		{
			std::ifstream file(sourcePath, std::ios::binary);
			if (!file)
				return false;
			out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			return true;
		}
	}

	bool GltfImporter::ImportAsBytes(const std::string& sourcePath,
		const ModelImportSettings& settings, const GltfImportMetadata& metadata,
		GltfImportBytesResult* result, std::string* error)
	{
		if (result)
			*result = GltfImportBytesResult {};
		const auto fail = [error](const std::string& message)
		{
			if (error) *error = message;
			return false;
		};
		const auto warn = [](std::vector<std::string>& warnings, const std::string& message)
		{
			warnings.push_back(message);
			WLD_CORE_WARN("[gltf] {0}", message);
		};

		if (sourcePath.empty())
			return fail("glTF import failed: source path is empty");
		std::error_code ec;
		if (!std::filesystem::is_regular_file(std::filesystem::path(sourcePath), ec))
			return fail("glTF import failed: source file not found: " + sourcePath);

		std::vector<uint8_t> sourceBytes;
		if (!ReadSourceBytes(sourcePath, sourceBytes))
			return fail("glTF import failed: cannot read source file: " + sourcePath);
		const uint64_t sourceFingerprint = Fnv1a64(sourceBytes.data(), sourceBytes.size());

		cgltf_options options {};
		cgltf_data* data = nullptr;
		const cgltf_result parseResult = cgltf_parse_file(&options, sourcePath.c_str(), &data);
		if (parseResult != cgltf_result_success || data == nullptr)
			return fail("glTF import failed: cannot parse '" + sourcePath + "' (cgltf code "
				+ std::to_string(static_cast<int>(parseResult)) + ")");
		struct DataGuard
		{
			cgltf_data* Data;
			~DataGuard() { if (Data) cgltf_free(Data); }
		} guard { data };

		std::vector<std::string> warnings;

		// ---- 不支持特性:硬报错(不写半成品,也不"尽力解析")----
		// 这些检查全部只看解析结果,不依赖 buffer 数据 —— 故意放在 load_buffers 之前:
		// 缺 .bin 的 skin/动画文件也要报"不支持特性",而不是先报一个误导性的 buffer 错误。
		if (data->skins_count > 0)
			return fail("glTF import failed: unsupported feature: skinning (skins count = "
				+ std::to_string(data->skins_count) + "); D5 imports static meshes only");
		if (data->animations_count > 0)
			return fail("glTF import failed: unsupported feature: animation (animations count = "
				+ std::to_string(data->animations_count) + ")");
		for (cgltf_size index = 0; index < data->accessors_count; ++index)
			if (data->accessors[index].is_sparse)
				return fail("glTF import failed: unsupported feature: sparse accessor #" + std::to_string(index));
		for (cgltf_size index = 0; index < data->meshes_count; ++index)
		{
			if (data->meshes[index].weights_count > 0)
				return fail("glTF import failed: unsupported feature: morph targets (mesh #"
					+ std::to_string(index) + " weights)");
			for (cgltf_size primitive = 0; primitive < data->meshes[index].primitives_count; ++primitive)
			{
				const cgltf_primitive& source = data->meshes[index].primitives[primitive];
				if (source.targets_count > 0)
					return fail("glTF import failed: unsupported feature: morph targets (mesh #"
						+ std::to_string(index) + " primitive #" + std::to_string(primitive) + ")");
				if (source.has_draco_mesh_compression)
					return fail("glTF import failed: unsupported feature: Draco compression (mesh #"
						+ std::to_string(index) + " primitive #" + std::to_string(primitive) + ")");
			}
		}
		for (cgltf_size index = 0; index < data->textures_count; ++index)
			if (data->textures[index].has_basisu || data->textures[index].basisu_image != nullptr)
				return fail("glTF import failed: unsupported feature: KTX2/Basis textures (texture #"
					+ std::to_string(index) + ")");
		for (cgltf_size index = 0; index < data->extensions_required_count; ++index)
			if (!IsSupportedRequiredExtension(data->extensions_required[index]))
				return fail(std::string("glTF import failed: unsupported required extension '")
					+ data->extensions_required[index] + "'");
		for (cgltf_size index = 0; index < data->extensions_used_count; ++index)
			if (!IsKnownExtension(data->extensions_used[index]))
				warn(warnings, std::string("extension '") + data->extensions_used[index]
					+ "' is not supported by D5 and was ignored");

		if (data->meshes_count == 0)
			return fail("glTF import failed: the file contains no meshes");

		const cgltf_result bufferResult = cgltf_load_buffers(&options, data, sourcePath.c_str());
		if (bufferResult != cgltf_result_success)
			return fail("glTF import failed: cannot load buffers (missing external .bin or bad data URI; cgltf code "
				+ std::to_string(static_cast<int>(bufferResult)) + ")");

		// ---- 输出命名(相对内容根) ----
		const std::filesystem::path source(sourcePath);
		const std::filesystem::path sourceDirectory = source.parent_path();
		const std::string modelName = SanitizeName(source.stem().string(), "model");
		// ---- 产物目的地(D10 / 用户决定 Q1=方案 A)----
		// 规则:产物落在**调用方指定的目的地目录**里;没指定时退回"源所在目录"(cook 与
		// CLI 的默认语义),再退回 models/。材质/贴图在目的地目录的 materials//textures/ 子目录。
		//
		// 优先级:DestinationLogicalDir(用户选的文件夹)> LogicalModelPath 的目录(兼容旧调用)
		//        > SourceLogicalPath 的目录(源相对,cook 用)> "models"。
		std::string modelDirectory;
		{
			// 先归一化成"目录"(末尾带 '/');入参可能是文件路径(models/x.wmodel)。
			const auto normalizeDirectory = [](std::string text)
			{
				std::replace(text.begin(), text.end(), '\\', '/');
				if (text.empty())
					return std::string();
				if (text.size() >= 7 && text.compare(text.size() - 7, 7, ".wmodel") == 0)
				{
					const size_t slash = text.find_last_of('/');
					text = slash == std::string::npos ? std::string() : text.substr(0, slash + 1);
				}
				if (!text.empty() && text.back() != '/')
					text.push_back('/');
				return text;
			};

			modelDirectory = normalizeDirectory(metadata.DestinationLogicalDir);
			if (modelDirectory.empty())
				modelDirectory = normalizeDirectory(metadata.LogicalModelPath);
			if (modelDirectory.empty())
			{
				// 源在内容根内(相对路径带目录、且没有 "..")才用它推导;源在根外或不带目录
				// (跨盘 relative 的兜底只剩文件名)一律退回 models/ —— 否则会写出
				// "../../../x.wmodel" 或把文件名当成目录。
				std::string sourceText = metadata.SourceLogicalPath;
				std::replace(sourceText.begin(), sourceText.end(), '\\', '/');
				const size_t slash = sourceText.find_last_of('/');
				if (slash != std::string::npos)
				{
					std::string sourceDirectory = sourceText.substr(0, slash + 1);
					if (sourceDirectory.find("..") == std::string::npos)
						modelDirectory = std::move(sourceDirectory);
				}
			}
			if (modelDirectory.empty())
				modelDirectory = "models/";
		}
		// 去重的候选目录:目的地目录 + 可选的共享目录(相对内容根)。
		std::vector<std::string> materialLookupDirs { modelDirectory + "materials/" };
		std::vector<std::string> textureLookupDirs { modelDirectory + "textures/" };
		if (!settings.SharedMaterialFolder.empty())
		{
			std::string shared = settings.SharedMaterialFolder;
			std::replace(shared.begin(), shared.end(), '\\', '/');
			if (!shared.empty() && shared.back() != '/')
				shared.push_back('/');
			materialLookupDirs.insert(materialLookupDirs.begin(), shared);
			textureLookupDirs.insert(textureLookupDirs.begin(), shared);
		}
		const std::filesystem::path dedupRoot = metadata.ContentRootAbsolute.empty()
			? std::filesystem::path() : std::filesystem::path(metadata.ContentRootAbsolute);

		// ---- 材质 + 贴图(全部先在内存里准备,验证通过后才落盘)----
		std::vector<MaterialDesc> materialDescs;
		std::vector<std::string> materialPaths;
		// D10:命名阶段就序列化好的材质字节(去重需要哈希),产物阶段直接复用。
		std::vector<std::vector<uint8_t>> materialBytes;
		materialDescs.reserve(data->materials_count);
		materialPaths.reserve(data->materials_count);
		struct PendingTexture
		{
			std::string RelativePath;
			ImagePayload Payload;
		};
		std::vector<PendingTexture> pendingTextures;
		std::unordered_map<cgltf_size, std::string> writtenImages;
		std::unordered_map<std::string, uint32_t> materialNameCounts;

		const auto resolveImage = [&](const cgltf_texture* texture, const std::string& purpose,
			std::string& outRelative, std::string& why) -> bool
		{
			if (texture == nullptr)
				return true;   // 未引用
			if (!settings.ExportTextures)
			{
				// 设置要求不导出贴图:材质仍可导出,但贴图路径留空(不读源贴图字节)。
				outRelative.clear();
				return true;
			}
			if (texture->has_basisu || texture->basisu_image != nullptr)
			{
				why = "KTX2/Basis texture is not supported (" + purpose + ")";
				return false;
			}
			if (texture->image == nullptr)
			{
				why = "texture has no image (" + purpose + ")";
				return false;
			}
			const cgltf_size imageIndex = static_cast<cgltf_size>(texture->image - data->images);
			const auto existing = writtenImages.find(imageIndex);
			if (existing != writtenImages.end())
			{
				outRelative = existing->second;
				return true;
			}
			ImagePayload payload;
			std::string readError;
			if (!ReadImagePayload(sourceDirectory, *texture->image, payload, readError))
			{
				why = readError + " (" + purpose + ")";
				return false;
			}
			std::string relative = modelDirectory + "textures/" + modelName + "_"
				+ std::to_string(imageIndex) + "." + payload.Extension;
			// D10:同内容贴图复用(两个模型引用同一张贴图 → 只留一份)。
			if (settings.ReuseTextures)
			{
				const std::string reused = FindReusableFile(dedupRoot, textureLookupDirs,
					"." + payload.Extension, payload.Bytes);
				if (!reused.empty())
					relative = reused;
			}
			writtenImages.emplace(imageIndex, relative);
			pendingTextures.push_back({ relative, std::move(payload) });
			outRelative = relative;
			return true;
		};

		for (cgltf_size index = 0; index < data->materials_count; ++index)
		{
			const cgltf_material& source = data->materials[index];
			MaterialDesc desc;
			desc.Name = source.name ? source.name : ("Material " + std::to_string(index));
			desc.BaseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
			if (source.has_pbr_metallic_roughness)
			{
				const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
				desc.BaseColor = LinearToSrgb4(pbr.base_color_factor);
				desc.Metallic = pbr.metallic_factor;
				desc.Roughness = pbr.roughness_factor;
				if (pbr.metallic_roughness_texture.texture != nullptr)
					warn(warnings, "material '" + desc.Name
						+ "': metallicRoughness texture is not supported by MaterialDesc and was ignored");
				std::string texturePath;
				std::string reason;
				if (!resolveImage(pbr.base_color_texture.texture, "baseColorTexture", texturePath, reason))
					return fail("glTF import failed: " + reason);
				desc.AlbedoTexture = texturePath;
			}
			desc.Emissive = LinearToSrgb3(source.emissive_factor);
			if (source.emissive_texture.texture != nullptr)
				warn(warnings, "material '" + desc.Name
					+ "': emissive texture is not supported by MaterialDesc and was ignored");
			if (source.occlusion_texture.texture != nullptr)
				warn(warnings, "material '" + desc.Name
					+ "': occlusion texture is not supported by MaterialDesc and was ignored");
			{
				std::string texturePath;
				std::string reason;
				if (!resolveImage(source.normal_texture.texture, "normalTexture", texturePath, reason))
					return fail("glTF import failed: " + reason);
				desc.NormalTexture = texturePath;
			}
			desc.BlendMode = source.alpha_mode == cgltf_alpha_mode_blend
				? MaterialBlendMode::Transparent : MaterialBlendMode::Opaque;
			if (source.alpha_mode == cgltf_alpha_mode_mask && source.alpha_cutoff < 1.0f)
				warn(warnings, "material '" + desc.Name
					+ "': alphaMode MASK with alphaCutoff is imported as Opaque (D5 does no alpha test)");
			desc.DoubleSided = source.double_sided != 0;
			if (source.has_pbr_specular_glossiness || source.has_clearcoat || source.has_transmission
				|| source.has_volume || source.has_ior || source.has_specular || source.has_sheen
				|| source.has_emissive_strength || source.has_iridescence || source.has_diffuse_transmission
				|| source.has_anisotropy || source.has_dispersion)
				warn(warnings, "material '" + desc.Name + "': KHR material extensions are not supported and were ignored");

			const std::string baseName = SanitizeName(desc.Name, "material_" + std::to_string(index));
			uint32_t& count = materialNameCounts[baseName];
			std::string fileName = modelName + "_" + baseName;
			if (count > 0)
				fileName += "_" + std::to_string(count);
			++count;
			// D10:同内容材质复用。序列化提到这里(去重要哈希字节),产物阶段直接复用这份字节。
			const std::string text = MaterialIO::Serialize(desc);
			std::vector<uint8_t> bytes(text.begin(), text.end());
			std::string materialPath = modelDirectory + "materials/" + fileName + ".wmat";
			if (settings.ReuseMaterials)
			{
				const std::string reused = FindReusableFile(dedupRoot, materialLookupDirs, ".wmat", bytes);
				if (!reused.empty())
					materialPath = reused;
			}
			materialPaths.push_back(materialPath);
			materialBytes.push_back(std::move(bytes));
			materialDescs.push_back(std::move(desc));
		}

		// ---- 几何:meshes → submeshes;缺法线按面法线补齐(唯一允许的降级)----
		WModelData model;
		model.MaterialSlots = materialPaths;
		uint32_t primitivesMissingNormals = 0;
		uint32_t primitivesMissingUv = 0;
		for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; ++meshIndex)
		{
			const cgltf_mesh& sourceMesh = data->meshes[meshIndex];
			WModelMeshRange range;
			range.FirstSubmesh = static_cast<uint32_t>(model.Submeshes.size());
			for (cgltf_size primitiveIndex = 0; primitiveIndex < sourceMesh.primitives_count; ++primitiveIndex)
			{
				const cgltf_primitive& primitive = sourceMesh.primitives[primitiveIndex];
				const std::string label = "mesh " + std::to_string(meshIndex) + " primitive " + std::to_string(primitiveIndex);
				if (primitive.type != cgltf_primitive_type_triangles)
					return fail("glTF import failed: " + label + " is not TRIANGLES (points/lines are not supported)");

				const cgltf_accessor* positions = FindAttribute(primitive, cgltf_attribute_type_position, 0);
				if (positions == nullptr)
					return fail("glTF import failed: " + label + " has no POSITION attribute");
				if (positions->count == 0)
					return fail("glTF import failed: " + label + " has no vertices");

				std::vector<glm::vec3> localPositions(static_cast<size_t>(positions->count));
				for (cgltf_size vertex = 0; vertex < positions->count; ++vertex)
					if (!ReadVec3(*positions, vertex, localPositions[static_cast<size_t>(vertex)]))
						return fail("glTF import failed: cannot read POSITION (" + label + ")");

				// 索引:没有 indices 时按顺序填充(glTF 允许非索引三角形)。
				std::vector<uint32_t> localIndices;
				if (primitive.indices != nullptr)
				{
					localIndices.resize(static_cast<size_t>(primitive.indices->count));
					for (cgltf_size index = 0; index < primitive.indices->count; ++index)
						localIndices[static_cast<size_t>(index)] =
							static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, index));
				}
				else
				{
					localIndices.resize(static_cast<size_t>(positions->count));
					for (size_t index = 0; index < localIndices.size(); ++index)
						localIndices[index] = static_cast<uint32_t>(index);
				}
				if (localIndices.size() % 3 != 0)
					return fail("glTF import failed: " + label + " index count is not a multiple of 3");
				for (const uint32_t index : localIndices)
					if (index >= positions->count)
						return fail("glTF import failed: " + label + " index " + std::to_string(index)
							+ " is out of range (vertex count " + std::to_string(positions->count) + ")");

				std::vector<glm::vec3> localNormals;
				const cgltf_accessor* normals = FindAttribute(primitive, cgltf_attribute_type_normal, 0);
				if (normals != nullptr)
				{
					localNormals.resize(static_cast<size_t>(positions->count));
					for (cgltf_size vertex = 0; vertex < positions->count; ++vertex)
						if (!ReadVec3(*normals, vertex, localNormals[static_cast<size_t>(vertex)]))
							return fail("glTF import failed: cannot read NORMAL (" + label + ")");
				}
				else
				{
					if (!settings.GenerateNormals)
						return fail("glTF import failed: " + label
							+ " has no NORMAL and generateNormals is disabled in the import settings");
					++primitivesMissingNormals;
					GenerateFaceNormals(localPositions, localIndices, localNormals);
				}

				const cgltf_accessor* uvs = FindAttribute(primitive, cgltf_attribute_type_texcoord, 0);
				std::vector<glm::vec2> localUvs;
				if (uvs != nullptr)
				{
					localUvs.resize(static_cast<size_t>(positions->count));
					for (cgltf_size vertex = 0; vertex < positions->count; ++vertex)
						if (!ReadVec2(*uvs, vertex, localUvs[static_cast<size_t>(vertex)]))
							return fail("glTF import failed: cannot read TEXCOORD_0 (" + label + ")");
				}
				else
				{
					++primitivesMissingUv;
					localUvs.assign(static_cast<size_t>(positions->count), glm::vec2(0.0f));
				}

				const uint32_t vertexBase = static_cast<uint32_t>(model.Vertices.size());
				const uint32_t indexBase = static_cast<uint32_t>(model.Indices.size());
				model.Vertices.reserve(model.Vertices.size() + localPositions.size());
				for (size_t vertex = 0; vertex < localPositions.size(); ++vertex)
					model.Vertices.push_back({ localPositions[vertex], localNormals[vertex], localUvs[vertex] });
				model.Indices.reserve(model.Indices.size() + localIndices.size());
				for (const uint32_t index : localIndices)
					model.Indices.push_back(vertexBase + index);

				WModelSubmesh submesh;
				submesh.IndexOffset = indexBase;
				submesh.IndexCount = static_cast<uint32_t>(localIndices.size());
				submesh.MaterialSlot = primitive.material != nullptr
					? static_cast<int32_t>(primitive.material - data->materials) : -1;
				submesh.Bounds = ComputeBounds(localPositions);
				model.Submeshes.push_back(submesh);
			}
			range.SubmeshCount = static_cast<uint32_t>(model.Submeshes.size()) - range.FirstSubmesh;
			if (range.SubmeshCount == 0)
				return fail("glTF import failed: mesh " + std::to_string(meshIndex) + " has no primitives");
			model.Meshes.push_back(range);
		}
		model.Bounds = ComputeBounds(model.Vertices);
		if (primitivesMissingNormals > 0)
			warn(warnings, std::to_string(primitivesMissingNormals)
				+ " primitive(s) have no NORMAL; face normals were generated");
		if (primitivesMissingUv > 0)
			warn(warnings, std::to_string(primitivesMissingUv)
				+ " primitive(s) have no TEXCOORD_0; UVs were set to zero");

		// ---- 导入设置烘焙(plan §D5b-1):scale 与 upAxis 在导入期烘进几何/节点 ----
		// scale:顶点/法线/节点 TRS/包围盒统一 ×Scale;upAxis=Z:绕 X 轴 -90°,法线用同一
		// 旋转矩阵(纯旋转,无需逆转置)。几何已带轴校正,所有节点 TRS 原样保留(否则
		// 渲染时节点会再转一次 → 双旋转);scale 是均匀缩放,烘焙进 TRS 与顶点等价。
		{
			const float scale = settings.Scale;
			glm::mat4 axisMatrix(1.0f);
			if (settings.UpAxis == 1u)
			{
				axisMatrix = glm::rotate(axisMatrix, glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
			}
			const auto transformVec = [&](const glm::vec3& value)
			{
				return glm::vec3(axisMatrix * glm::vec4(value * scale, 0.0f));
			};
			const auto transformBounds = [&](WModelBounds& bounds)
			{
				if (bounds.Min == bounds.Max)
					return;   // 空模型/空子网格的默认值,不参与变换
				const glm::vec3 first = transformVec(bounds.Min);
				const glm::vec3 second = transformVec(bounds.Max);
				bounds.Min = glm::min(first, second);
				bounds.Max = glm::max(first, second);
			};

			for (WModelVertex& vertex : model.Vertices)
			{
				vertex.Position = transformVec(vertex.Position);
				vertex.Normal = glm::vec3(axisMatrix * glm::vec4(vertex.Normal, 0.0f));
			}
			for (WModelSubmesh& submesh : model.Submeshes)
				transformBounds(submesh.Bounds);
			model.Bounds = ComputeBounds(model.Vertices);
		}

		// ---- 节点树 ----
		model.Nodes.reserve(data->nodes_count);
		for (cgltf_size index = 0; index < data->nodes_count; ++index)
		{
			const cgltf_node& source = data->nodes[index];
			WModelNode node;
			node.Parent = source.parent != nullptr
				? static_cast<int32_t>(source.parent - data->nodes) : -1;
			node.MeshIndex = source.mesh != nullptr
				? static_cast<int32_t>(source.mesh - data->meshes) : -1;
			node.Name = source.name ? source.name : "";
			if (source.has_matrix)
			{
				const glm::mat4 matrix = glm::make_mat4(source.matrix);
				glm::vec3 translation { 0.0f };
				glm::vec3 scale { 1.0f };
				glm::vec3 skew { 0.0f };
				glm::vec4 perspective { 0.0f };
				glm::quat rotation { 1.0f, 0.0f, 0.0f, 0.0f };
				if (!glm::decompose(matrix, scale, rotation, translation, skew, perspective))
					return fail("glTF import failed: node " + std::to_string(index)
						+ " matrix cannot be decomposed into TRS (shear is not supported)");
				node.Translation = translation;
				node.Rotation = rotation;
				node.Scale = scale;
			}
			else
			{
				if (source.has_translation)
					node.Translation = { source.translation[0], source.translation[1], source.translation[2] };
				if (source.has_rotation)
					node.Rotation = glm::quat(source.rotation[3], source.rotation[0],
						source.rotation[1], source.rotation[2]);
				if (source.has_scale)
					node.Scale = { source.scale[0], source.scale[1], source.scale[2] };
			}
			model.Nodes.push_back(std::move(node));
		}

		// ---- 产物字节(全部验证通过后):贴图 → 材质 → 模型 ----
		// D5b:不再直接落盘,先在内存准备 ImportOutput 列表(ImportFile 逐项落盘;
		// ModelImporter 直接复用同一批字节,顺序保证 .wmodel 最后写 —— 缺件时不留半成品模型)。
		// D10:模型固定在目的地目录里(与材质/贴图的模型前缀同一规则)。
		const std::string modelRelative = modelDirectory + modelName + ".wmodel";
		std::vector<GltfInMemoryOutput> outputs;
		outputs.reserve(pendingTextures.size() + materialDescs.size() + 1u);

		if (!settings.ExportMaterials)
		{
			// 设置要求不导出材质:不产出 .wmat,材质槽留空(-1 = 渲染走 Color 常量色路径)。
			warn(warnings, "exportMaterials is disabled: no .wmat files were produced; "
				"mesh material slots are empty");
			const size_t materialCount = materialPaths.size();
			model.MaterialSlots.assign(materialCount, std::string());
			materialPaths.clear();
			materialPaths.resize(materialCount);
			for (WModelSubmesh& submesh : model.Submeshes)
				submesh.MaterialSlot = -1;
		}
		for (const PendingTexture& texture : pendingTextures)
		{
			GltfInMemoryOutput output;
			output.LogicalPath = texture.RelativePath;
			output.Data = texture.Payload.Bytes;
			outputs.push_back(std::move(output));
		}
		for (size_t index = 0; index < materialDescs.size(); ++index)
		{
			if (materialPaths[index].empty())
				continue;
			GltfInMemoryOutput output;
			output.LogicalPath = materialPaths[index];
			// D10:命名阶段已经序列化过(去重用),这里直接用同一份字节。
			output.Data = materialBytes[index];
			outputs.push_back(std::move(output));
		}

		// meta 必须由导入器写入(源指纹/导入器版本/设置哈希/轴/缩放)。
		model.Meta.Valid = true;
		model.Meta.SourceFingerprint = sourceFingerprint;
		model.Meta.ImporterVersion = metadata.ImporterVersion;
		model.Meta.SettingsHash = metadata.SettingsHash;
		model.Meta.UpAxis = metadata.UpAxis;
		model.Meta.Scale = metadata.Scale;
		model.Meta.SourcePath = metadata.SourceLogicalPath;
		if (model.Meta.UpAxis > 1u)
			return fail("glTF import failed: invalid upAxis metadata value "
				+ std::to_string(model.Meta.UpAxis));
		if (!std::isfinite(model.Meta.Scale) || model.Meta.Scale <= 0.0f)
			return fail("glTF import failed: invalid scale metadata value "
				+ std::to_string(model.Meta.Scale));
		{
			const std::vector<uint8_t> modelBytes = WModelIO::Serialize(model);
			WModelData verified;
			std::string verifyError;
			if (!WModelIO::Parse(modelBytes.data(), modelBytes.size(), verified, &verifyError))
				return fail("glTF import failed: produced .wmodel is invalid: " + verifyError);
			GltfInMemoryOutput output;
			output.LogicalPath = modelRelative;
			output.Data = modelBytes;
			outputs.push_back(std::move(output));
		}

		if (result)
		{
			result->Summary.WModelPath = modelRelative;
			result->Summary.MaterialPaths = materialPaths;
			result->Summary.TexturePaths.reserve(pendingTextures.size());
			for (const PendingTexture& texture : pendingTextures)
				result->Summary.TexturePaths.push_back(texture.RelativePath);
			result->Summary.Warnings = warnings;
			result->Summary.VertexCount = static_cast<uint32_t>(model.Vertices.size());
			result->Summary.IndexCount = static_cast<uint32_t>(model.Indices.size());
			result->Summary.MeshCount = static_cast<uint32_t>(model.Meshes.size());
			result->Summary.SubmeshCount = static_cast<uint32_t>(model.Submeshes.size());
			result->Summary.NodeCount = static_cast<uint32_t>(model.Nodes.size());
			result->Summary.MaterialSlotCount = static_cast<uint32_t>(model.MaterialSlots.size());
			result->Summary.SourceFingerprint = sourceFingerprint;
			result->Summary.UpAxis = metadata.UpAxis;
			result->Metadata = metadata;
			result->Outputs = std::move(outputs);
		}
		WLD_CORE_INFO("[gltf] imported '{0}' -> {1} (meshes={2} submeshes={3} nodes={4} materials={5} textures={6} warnings={7}, bytes)",
			sourcePath, modelRelative, model.Meshes.size(), model.Submeshes.size(), model.Nodes.size(),
			model.MaterialSlots.size(), pendingTextures.size(), warnings.size());
		return true;
	}

	bool GltfImporter::ImportFile(const std::string& sourcePath, const std::string& outputRoot,
		GltfImportResult* result, std::string* error, const std::string& destinationLogicalDir)
	{
		if (result)
			*result = GltfImportResult {};
		if (outputRoot.empty())
		{
			if (error) *error = "glTF import failed: output root is empty";
			return false;
		}

		// 设置来源与 cook 的 ModelImporter **同源**:与源同目录同名的 `.wimport`
		// (缺失/坏 JSON → 默认值 + warning)。否则"改设置 → Reimport 生效"在编辑器路径不成立,
		// 而且 dev 树里的产物会和打包产物不一致(实测:夹具 sidecar 只有 cook 生效)。
		std::string settingsWarning;
		const ModelImportSettings settings = ModelImportSettings::Load(sourcePath, &settingsWarning);
		if (result && !settingsWarning.empty())
			result->Warnings.push_back(settingsWarning);
		GltfImportMetadata metadata;
		metadata.ImporterVersion = 1;
		metadata.SettingsHash = ModelImportSettings::Hash(settings);
		metadata.UpAxis = settings.UpAxis;
		metadata.Scale = settings.Scale;
		// D10:产物落在**调用方指定的目的地目录**(用户在内容浏览器里选/拖到哪个文件夹);
		// 不指定时内核退回"源所在目录",因此 cook 与编辑器天然同规则。源的位置照旧记进
		// meta.SourcePath,编辑器据此判断"需要重导"。
		metadata.LogicalModelPath.clear();
		metadata.DestinationLogicalDir = destinationLogicalDir;
		metadata.ContentRootAbsolute = std::filesystem::absolute(std::filesystem::path(outputRoot)).string();
		{
			std::error_code ec;
			const std::filesystem::path sourceAbsolute = std::filesystem::absolute(std::filesystem::path(sourcePath), ec);
			std::error_code rootError;
			const std::filesystem::path rootAbsolute = std::filesystem::absolute(std::filesystem::path(outputRoot), rootError);
			std::filesystem::path relative = (!ec && !rootError)
				? std::filesystem::relative(sourceAbsolute, rootAbsolute, ec)
				: std::filesystem::path();
			if (ec || relative.empty() || relative.is_absolute())
			{
				relative = std::filesystem::path(sourcePath).filename();
			}
			metadata.SourceLogicalPath = relative.generic_string();
		}

		GltfImportBytesResult bytes;
		std::string importError;
		if (!ImportAsBytes(sourcePath, settings, metadata, &bytes, &importError))
		{
			if (error) *error = importError;
			return false;
		}

		// 逐项落盘:贴图 → 材质 → 模型(.wmodel 最后写,失败时不留半个模型)。
		const std::filesystem::path root(outputRoot);
		for (const GltfInMemoryOutput& output : bytes.Outputs)
		{
			std::string writeError;
			if (!WriteFileBytes(root / output.LogicalPath, output.Data.data(), output.Data.size(), &writeError))
			{
				if (error)
					*error = "glTF import failed: cannot write " + output.LogicalPath + ": " + writeError;
				return false;
			}
		}
		if (result)
			*result = bytes.Summary;
		if (error)
			error->clear();
		return true;
	}

	bool ImportFile(const std::filesystem::path& sourcePath, const std::filesystem::path& outputRoot,
		GltfImportResult* result, std::string* error, const std::string& destinationLogicalDir)
	{
		return GltfImporter::ImportFile(sourcePath.string(), outputRoot.string(), result, error,
			destinationLogicalDir);
	}
}
