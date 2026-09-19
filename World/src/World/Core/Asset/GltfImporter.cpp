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
		// 缺 .bin 的 morph/sparse 文件也要报"不支持特性",而不是先报一个误导性的 buffer 错误。
		// D5c-2:skin/动画已从这份清单移除(改为导入);其余仍硬报错。
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

		// ---- D5c-2:蒙皮顶点属性(JOINTS_0 / WEIGHTS_0 → float4)----
		// 关节下标用 float 存(与容器布局 2 一致);权重 u8/u16 的归一化由 cgltf 转成 0..1 的 float。
		// 只在**真的绑到 skin 的网格**上读取与校验:importSkins=false 时这些属性按静态网格忽略。
		// 不重排、不重归一化:权重和偏离 1 超过 0.01 只记 warning,数据原样写出(下游自行解释)。
		struct SkinVertexData
		{
			std::vector<glm::vec4> Joints;
			std::vector<glm::vec4> Weights;
		};
		const auto readSkinVertices = [&fail, &warn](const cgltf_primitive& primitive,
			const std::string& label, SkinVertexData& out,
			std::vector<std::string>& warnings) -> bool
		{
			out = SkinVertexData {};
			const cgltf_accessor* joints = FindAttribute(primitive, cgltf_attribute_type_joints, 0);
			const cgltf_accessor* weights = FindAttribute(primitive, cgltf_attribute_type_weights, 0);
			if (joints == nullptr || weights == nullptr)
				return fail("glTF import failed: " + label
					+ " is bound to a skin but is missing JOINTS_0 or WEIGHTS_0");
			if (joints->type != cgltf_type_vec4)
				return fail("glTF import failed: " + label + " JOINTS_0 must be VEC4");
			if (joints->component_type != cgltf_component_type_r_8u
				&& joints->component_type != cgltf_component_type_r_16u)
				return fail("glTF import failed: " + label
					+ " JOINTS_0 must use unsigned byte or unsigned short components");
			if (weights->type != cgltf_type_vec4 || weights->component_type != cgltf_component_type_r_32f)
			{
				// glTF 只允许 WEIGHTS_0 为 float 或归一化 u8/u16;cgltf 读归一化整数时已转回 0..1。
				const bool normalizedInteger = (weights->component_type == cgltf_component_type_r_8u
					|| weights->component_type == cgltf_component_type_r_16u) && weights->normalized != 0;
				if (weights->type != cgltf_type_vec4 || !normalizedInteger)
					return fail("glTF import failed: " + label
						+ " WEIGHTS_0 must be VEC4 float or normalized u8/u16");
			}
			if (joints->count != weights->count)
				return fail("glTF import failed: " + label
					+ " JOINTS_0 and WEIGHTS_0 have different vertex counts");

			out.Joints.assign(static_cast<size_t>(joints->count), glm::vec4(0.0f));
			out.Weights.assign(static_cast<size_t>(weights->count), glm::vec4(0.0f));
			uint32_t nonCanonicalWeights = 0;
			for (cgltf_size vertex = 0; vertex < joints->count; ++vertex)
			{
				cgltf_float jointValues[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				cgltf_float weightValues[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				if (!cgltf_accessor_read_float(joints, vertex, jointValues, 4)
					|| !cgltf_accessor_read_float(weights, vertex, weightValues, 4))
					return fail("glTF import failed: cannot read JOINTS_0/WEIGHTS_0 (" + label + ")");
				glm::vec4& joint = out.Joints[static_cast<size_t>(vertex)];
				glm::vec4& weight = out.Weights[static_cast<size_t>(vertex)];
				joint = { jointValues[0], jointValues[1], jointValues[2], jointValues[3] };
				weight = { weightValues[0], weightValues[1], weightValues[2], weightValues[3] };
				for (int component = 0; component < 4; ++component)
				{
					// 容器布局 2 用 float 存关节下标,上限 kMaxJointsPerSkin(更大的调色板不成立)。
					if (joint[component] < 0.0f
						|| joint[component] > static_cast<float>(WModelIO::kMaxJointsPerSkin - 1u)
						|| std::floor(joint[component]) != joint[component])
						return fail("glTF import failed: " + label + " JOINTS_0 contains joint index "
							+ std::to_string(joint[component]) + " (must be an integer in 0.."
							+ std::to_string(WModelIO::kMaxJointsPerSkin - 1u) + ")");
				}
				const float weightSum = weight.x + weight.y + weight.z + weight.w;
				if (std::fabs(weightSum - 1.0f) > 0.01f)
					++nonCanonicalWeights;
			}
			if (nonCanonicalWeights > 0)
				warn(warnings, label + ": " + std::to_string(nonCanonicalWeights)
					+ " vertex/vertices have weights summing to != 1 (>0.01 off); "
					"weights are written as authored (no re-normalization)");
			return true;
		};

		// ---- D5c-2:动画烘焙 ----
		// 每条 glTF animation → WModelAnimation:duration = 所有通道输入时间的最大值;
		// 按 settings.AnimationSampleRate 在 t = 0..duration 等间隔采样(运行时不解析插值器)。
		const auto bakeAnimations = [&fail, &warn, &warnings, &settings](
			const cgltf_data& source, std::vector<WModelAnimation>& outAnimations) -> bool
		{
			const float sampleRate = settings.AnimationSampleRate > 0.0f
				? settings.AnimationSampleRate : 30.0f;
			const float scale = std::isfinite(settings.Scale) && settings.Scale > 0.0f
				? settings.Scale : 1.0f;
			outAnimations.clear();
			outAnimations.reserve(source.animations_count);
			for (cgltf_size animationIndex = 0; animationIndex < source.animations_count; ++animationIndex)
			{
				const cgltf_animation& sourceAnimation = source.animations[animationIndex];
				const std::string label = "animation " + std::to_string(animationIndex)
					+ (sourceAnimation.name != nullptr
						? std::string(" ('") + sourceAnimation.name + "')" : std::string());
				WModelAnimation animation;
				animation.Name = sourceAnimation.name ? sourceAnimation.name
					: ("Animation " + std::to_string(animationIndex));

				// 插值器降级只报一次(每条动画/每种插值类型)。
				bool warnedStep = false;
				bool warnedCubic = false;
				float duration = 0.0f;
				for (cgltf_size channelIndex = 0; channelIndex < sourceAnimation.channels_count; ++channelIndex)
				{
					const cgltf_animation_channel& sourceChannel = sourceAnimation.channels[channelIndex];
					if (sourceChannel.sampler == nullptr)
					{
						warn(warnings, label + " channel " + std::to_string(channelIndex)
							+ " has no sampler; channel was skipped");
						continue;
					}
					if (sourceChannel.target_node == nullptr)
					{
						warn(warnings, label + " channel " + std::to_string(channelIndex)
							+ " has no target node; channel was skipped");
						continue;
					}
					if (sourceChannel.target_path != cgltf_animation_path_type_translation
						&& sourceChannel.target_path != cgltf_animation_path_type_rotation
						&& sourceChannel.target_path != cgltf_animation_path_type_scale)
					{
						warn(warnings, label + " channel " + std::to_string(channelIndex)
							+ " targets an unsupported path (morph weights); channel was skipped");
						continue;
					}
					const cgltf_animation_sampler& sampler = *sourceChannel.sampler;
					if (sampler.input == nullptr || sampler.output == nullptr)
						return fail("glTF import failed: " + label + " channel "
							+ std::to_string(channelIndex) + " has no input/output accessor");
					const cgltf_size keyCount = sampler.input->count;
					if (keyCount == 0)
						return fail("glTF import failed: " + label + " channel "
							+ std::to_string(channelIndex) + " has no keyframes");

					const bool cubic = sampler.interpolation == cgltf_interpolation_type_cubic_spline;
					if (sampler.interpolation == cgltf_interpolation_type_step)
					{
						if (!warnedStep)
						{
							warnedStep = true;
							warn(warnings, label + " uses STEP interpolation; "
								"keys are baked at the sample rate");
						}
					}
					else if (cubic && !warnedCubic)
					{
						warnedCubic = true;
						warn(warnings, label + " uses CUBICSPLINE interpolation; the value part of each key "
							"is baked linearly (tangents are ignored)");
					}

					std::vector<float> times(static_cast<size_t>(keyCount), 0.0f);
					std::vector<glm::vec4> values(static_cast<size_t>(keyCount), glm::vec4(0.0f));
					for (cgltf_size key = 0; key < keyCount; ++key)
					{
						cgltf_float time = 0.0f;
						if (!cgltf_accessor_read_float(sampler.input, key, &time, 1))
							return fail("glTF import failed: cannot read " + label
								+ " channel input (key " + std::to_string(key) + ")");
						times[static_cast<size_t>(key)] = time;
						// CUBICSPLINE 的 output 是 3 个元素一组(a, value, b),取中间的 value。
						const cgltf_size outputIndex = cubic ? key * 3u + 1u : key;
						cgltf_float value[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
						if (!cgltf_accessor_read_float(sampler.output, outputIndex, value, 4))
							return fail("glTF import failed: cannot read " + label
								+ " channel output (key " + std::to_string(key) + ")");
						values[static_cast<size_t>(key)] = { value[0], value[1], value[2], value[3] };
					}
					// D5c-2:times 必须与 values **同步**排序 —— 只排 times 会把"时间→取值"
					// 的配对打乱(实测:夹具按降序写关键帧时,最后一个采样取到了第一帧的值)。
					{
						std::vector<size_t> order(times.size());
						for (size_t index = 0; index < order.size(); ++index)
							order[index] = index;
						std::stable_sort(order.begin(), order.end(),
							[&times](size_t left, size_t right) { return times[left] < times[right]; });
						std::vector<float> sortedTimes(times.size());
						std::vector<glm::vec4> sortedValues(values.size());
						for (size_t index = 0; index < order.size(); ++index)
						{
							sortedTimes[index] = times[order[index]];
							sortedValues[index] = values[order[index]];
						}
						times.swap(sortedTimes);
						values.swap(sortedValues);
					}
					duration = std::max(duration, times.back());

					WModelAnimationChannel channel;
					channel.TargetNode = static_cast<uint32_t>(sourceChannel.target_node - source.nodes);
					switch (sourceChannel.target_path)
					{
					case cgltf_animation_path_type_translation:
						channel.Path = WModelIO::WModelAnimationPath::Translation;
						break;
					case cgltf_animation_path_type_rotation:
						channel.Path = WModelIO::WModelAnimationPath::Rotation;
						break;
					default:
						channel.Path = WModelIO::WModelAnimationPath::Scale;
						break;
					}

					const uint32_t sampleCount = static_cast<uint32_t>(duration * sampleRate) + 1u;
					channel.Keys.reserve(sampleCount);
					for (uint32_t sample = 0; sample < sampleCount; ++sample)
					{
						const float time = static_cast<float>(sample) / sampleRate;
						// 采样时间严格递增 → 用 upper_bound 找区间,线性插值(旋转用 slerp)。
						const auto upper = std::upper_bound(times.begin(), times.end(), time);
						const size_t rightIndex = std::min(
							static_cast<size_t>(upper - times.begin()), times.size() - 1u);
						const size_t leftIndex = rightIndex > 0u ? rightIndex - 1u : 0u;
						const glm::vec4& left = values[leftIndex];
						const glm::vec4& right = values[rightIndex];
						float factor = 0.0f;
						if (rightIndex != leftIndex)
						{
							const float span = times[rightIndex] - times[leftIndex];
							if (span > 0.0f)
								factor = std::min(1.0f,
									std::max(0.0f, (time - times[leftIndex]) / span));
						}
						glm::vec4 value = left;
						if (factor > 0.0f)
						{
							if (channel.Path == WModelIO::WModelAnimationPath::Rotation)
							{
								const glm::quat leftRotation { left.w, left.x, left.y, left.z };
								const glm::quat rightRotation { right.w, right.x, right.y, right.z };
								const glm::quat blended = glm::normalize(
									glm::slerp(leftRotation, rightRotation, factor));
								value = { blended.x, blended.y, blended.z, blended.w };
							}
							else
							{
								value = left + (right - left) * factor;
							}
						}
						WModelAnimationKey key;
						key.Time = time;
						key.Value = value;
						if (channel.Path == WModelIO::WModelAnimationPath::Translation)
						{
							// plan §D5b-1:scale 烘焙进几何/节点 TRS,平移关键帧同步缩放。
							key.Value.x *= scale;
							key.Value.y *= scale;
							key.Value.z *= scale;
						}
						else if (channel.Path == WModelIO::WModelAnimationPath::Scale)
						{
							// 均匀缩放与 TRS 的 S 可交换,直接缩放三个分量。
							key.Value.x *= scale;
							key.Value.y *= scale;
							key.Value.z *= scale;
						}
						channel.Keys.push_back(key);
					}
					animation.Channels.push_back(std::move(channel));
				}
				animation.Duration = duration;
				if (animation.Channels.empty())
					warn(warnings, label + " has no importable channels");
				outAnimations.push_back(std::move(animation));
			}
			return true;
		};

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
		// D5c-2:节点带 mesh + skin → 该 mesh 绑到 skin 下标(容器 MeshRange.SkinIndex)。
		// 同一 mesh 被多个节点引用且 skin 不同 → 取第一个并 warn(容器一个 mesh 只有一个 SkinIndex)。
		std::unordered_map<cgltf_size, int32_t> meshSkinBindings;
		if (settings.ImportSkins)
		{
			std::unordered_map<cgltf_size, cgltf_size> meshSkinFirstNode;
			for (cgltf_size nodeIndex = 0; nodeIndex < data->nodes_count; ++nodeIndex)
			{
				const cgltf_node& sourceNode = data->nodes[nodeIndex];
				if (sourceNode.mesh == nullptr || sourceNode.skin == nullptr)
					continue;
				const cgltf_size meshIndex = static_cast<cgltf_size>(sourceNode.mesh - data->meshes);
				const cgltf_size skinIndex = static_cast<cgltf_size>(sourceNode.skin - data->skins);
				const auto existing = meshSkinBindings.find(meshIndex);
				if (existing == meshSkinBindings.end())
				{
					meshSkinBindings.emplace(meshIndex, static_cast<int32_t>(skinIndex));
					meshSkinFirstNode.emplace(meshIndex, nodeIndex);
				}
				else if (existing->second != static_cast<int32_t>(skinIndex))
				{
					warn(warnings, "mesh " + std::to_string(meshIndex)
						+ " is referenced by node " + std::to_string(meshSkinFirstNode[meshIndex])
						+ " (skin " + std::to_string(existing->second) + ") and node "
						+ std::to_string(nodeIndex) + " (skin " + std::to_string(skinIndex)
						+ "); the first skin is used");
				}
			}
		}
		uint32_t primitivesMissingNormals = 0;
		uint32_t primitivesMissingUv = 0;
		for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; ++meshIndex)
		{
			const cgltf_mesh& sourceMesh = data->meshes[meshIndex];
			WModelMeshRange range;
			range.FirstSubmesh = static_cast<uint32_t>(model.Submeshes.size());
			const auto skinBinding = meshSkinBindings.find(meshIndex);
			range.SkinIndex = skinBinding != meshSkinBindings.end() ? skinBinding->second : -1;
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
				// D5c-2:只有绑到 skin 的网格才读 JOINTS_0/WEIGHTS_0(缺一 → 硬报错)。
				SkinVertexData skinVertices;
				if (range.SkinIndex >= 0)
				{
					if (!readSkinVertices(primitive, label, skinVertices, warnings))
						return false;
					if (skinVertices.Joints.size() != localPositions.size())
						return fail("glTF import failed: " + label
							+ " JOINTS_0 vertex count does not match POSITION");
				}
				model.Vertices.reserve(model.Vertices.size() + localPositions.size());
				for (size_t vertex = 0; vertex < localPositions.size(); ++vertex)
					model.Vertices.push_back({ localPositions[vertex], localNormals[vertex], localUvs[vertex] });
				if (range.SkinIndex >= 0)
				{
					model.SkinVertices.reserve(model.SkinVertices.size() + skinVertices.Joints.size());
					for (size_t vertex = 0; vertex < skinVertices.Joints.size(); ++vertex)
						model.SkinVertices.push_back({ skinVertices.Joints[vertex], skinVertices.Weights[vertex] });
				}
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

		// ---- D5c-2:骨架(glTF skin → WModelSkin) ----
		// 关节下标空间 = **本 skin 的关节集合**(JointParents/顶点 Joints 都是这套下标);
		// 绑定姿态取关节节点的局部 TRS(与 Node 的 TRS 同一份值,不含轴/缩放烘焙:
		// 几何与节点都按 upAxis 旋转,关节局部姿态在绑定姿态里不做额外变换)。
		if (!settings.ImportSkins && data->skins_count > 0)
			warn(warnings, "importSkins is disabled: " + std::to_string(data->skins_count)
				+ " skin(s) were skipped and skinned meshes are imported as static");
		else if (settings.ImportSkins)
		{
			model.Skins.reserve(data->skins_count);
			for (cgltf_size skinIndex = 0; skinIndex < data->skins_count; ++skinIndex)
			{
				const cgltf_skin& sourceSkin = data->skins[skinIndex];
				if (sourceSkin.joints_count == 0)
					return fail("glTF import failed: skin " + std::to_string(skinIndex)
						+ " has no joints");
				if (sourceSkin.joints_count > WModelIO::kMaxJointsPerSkin)
					return fail("glTF import failed: skin " + std::to_string(skinIndex) + " has "
						+ std::to_string(sourceSkin.joints_count) + " joints, above the per-skin limit of "
						+ std::to_string(WModelIO::kMaxJointsPerSkin));

				WModelSkin skin;
				skin.Name = sourceSkin.name ? sourceSkin.name : ("Skin " + std::to_string(skinIndex));
				const size_t jointCount = static_cast<size_t>(sourceSkin.joints_count);
				skin.JointNames.resize(jointCount);
				skin.JointParents.assign(jointCount, -1);
				skin.InverseBindMatrices.assign(jointCount, glm::mat4(1.0f));
				// glTF 的 TRS 分量都是**可选**的:缺省 = 单位值(平移 0 / 旋转 identity / 缩放 1)。
				// 不预置就会留下 0,绑定姿态直接坏掉(实测:夹具关节没写 scale → BindScales[0] 为 0)。
				skin.BindTranslations.assign(jointCount, glm::vec3(0.0f));
				skin.BindRotations.assign(jointCount, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
				skin.BindScales.assign(jointCount, glm::vec3(1.0f));

				// 关节节点 → 本 skin 关节集合内的下标(父关节用它换算 JointParents)。
				std::unordered_map<const cgltf_node*, int32_t> jointIndices;
				jointIndices.reserve(jointCount);
				for (size_t joint = 0; joint < jointCount; ++joint)
					jointIndices.emplace(sourceSkin.joints[joint], static_cast<int32_t>(joint));

				if (sourceSkin.inverse_bind_matrices != nullptr)
				{
					for (size_t joint = 0; joint < jointCount; ++joint)
					{
						cgltf_float values[16] = {};
						if (!cgltf_accessor_read_float(sourceSkin.inverse_bind_matrices, joint, values, 16))
							return fail("glTF import failed: cannot read skin "
								+ std::to_string(skinIndex) + " inverseBindMatrices (joint "
								+ std::to_string(joint) + ")");
						skin.InverseBindMatrices[joint] = glm::make_mat4(values);
					}
				}

				for (size_t joint = 0; joint < jointCount; ++joint)
				{
					const cgltf_node& jointNode = *sourceSkin.joints[joint];
					skin.JointNames[joint] = jointNode.name != nullptr
						? jointNode.name : ("joint_" + std::to_string(joint));
					if (jointNode.parent != nullptr)
					{
						const auto parent = jointIndices.find(jointNode.parent);
						if (parent != jointIndices.end())
							skin.JointParents[joint] = parent->second;
					}
					if (jointNode.has_matrix)
					{
						const glm::mat4 matrix = glm::make_mat4(jointNode.matrix);
						glm::vec3 translation { 0.0f };
						glm::vec3 scale { 1.0f };
						glm::vec3 skew { 0.0f };
						glm::vec4 perspective { 0.0f };
						glm::quat rotation { 1.0f, 0.0f, 0.0f, 0.0f };
						if (!glm::decompose(matrix, scale, rotation, translation, skew, perspective))
							return fail("glTF import failed: skin " + std::to_string(skinIndex)
								+ " joint " + std::to_string(joint)
								+ " matrix cannot be decomposed into TRS (shear is not supported)");
						skin.BindTranslations[joint] = translation;
						skin.BindRotations[joint] = rotation;
						skin.BindScales[joint] = scale;
					}
					else
					{
						if (jointNode.has_translation)
							skin.BindTranslations[joint] = { jointNode.translation[0],
								jointNode.translation[1], jointNode.translation[2] };
						if (jointNode.has_rotation)
							skin.BindRotations[joint] = glm::quat(jointNode.rotation[3],
								jointNode.rotation[0], jointNode.rotation[1], jointNode.rotation[2]);
						if (jointNode.has_scale)
							skin.BindScales[joint] = { jointNode.scale[0], jointNode.scale[1],
								jointNode.scale[2] };
					}
				}
				model.Skins.push_back(std::move(skin));
			}
		}

		// ---- D5c-2:动画(glTF animation → WModelAnimation,导入期烘关键帧)----
		if (settings.ImportAnimations)
		{
			if (!bakeAnimations(*data, model.Animations))
				return false;
		}
		else if (data->animations_count > 0)
		{
			warn(warnings, "importAnimations is disabled: " + std::to_string(data->animations_count)
				+ " animation(s) were skipped");
		}

		// 有任何蒙皮网格 → 顶点布局 2;否则保持标准布局 1(静态资产零回归)。
		if (!model.SkinVertices.empty())
			model.VertexLayoutId = WModelIO::kVertexLayoutSkinned;
		if ((!model.SkinVertices.empty()) != (model.VertexLayoutId == WModelIO::kVertexLayoutSkinned))
			return fail("glTF import failed: internal error building the skinned vertex layout");

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
