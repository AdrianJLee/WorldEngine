#include "wldpch.h"
#include "World/UI/UiDocument.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <system_error>

namespace World::UI
{
	namespace
	{
		// ---------------- 解析 ----------------

		struct ParseCtx
		{
			std::string* Error = nullptr;

			bool Fail(const std::string& message)
			{
				if (Error)
					*Error = message;
				return false;
			}
		};

		std::string NodePath(const std::string& parentPath, const std::string& id)
		{
			return parentPath.empty() ? id : parentPath + "." + id;
		}

		bool ReadVec2(const YAML::Node& map, const char* key, glm::vec2& out, const std::string& path, ParseCtx& ctx)
		{
			const YAML::Node node = map[key];
			if (!node)
				return true;
			if (!node.IsSequence() || node.size() != 2)
				return ctx.Fail(path + ": '" + key + "' must be a 2-number sequence");
			out.x = node[0].as<float>();
			out.y = node[1].as<float>();
			return true;
		}

		bool ReadPadding(const YAML::Node& map, const char* key, float (&out)[4], const std::string& path, ParseCtx& ctx)
		{
			const YAML::Node node = map[key];
			if (!node)
				return true;
			if (!node.IsSequence() || node.size() != 4)
				return ctx.Fail(path + ": '" + key + "' must be a 4-number sequence [left, top, right, bottom]");
			for (std::size_t i = 0; i < 4; ++i)
				out[i] = node[i].as<float>();
			return true;
		}

		bool ReadVec3(const YAML::Node& map, const char* key, glm::vec3& out, const std::string& path, ParseCtx& ctx)
		{
			const YAML::Node node = map[key];
			if (!node)
				return true;
			if (!node.IsSequence() || node.size() != 3)
				return ctx.Fail(path + ": '" + key + "' must be a 3-number sequence");
			out.x = node[0].as<float>();
			out.y = node[1].as<float>();
			out.z = node[2].as<float>();
			return true;
		}

		bool ReadScalarMap(const YAML::Node& map, const char* key,
			std::vector<UiProp>& out, const std::string& path, ParseCtx& ctx)
		{
			const YAML::Node node = map[key];
			if (!node)
				return true;
			if (!node.IsMap())
				return ctx.Fail(path + ": '" + key + "' must be a map of scalar values");
			for (const auto& kv : node)
			{
				UiProp prop;
				prop.Name = kv.first.as<std::string>();
				if (prop.Name.empty())
					return ctx.Fail(path + ": '" + key + "' contains an empty key");
				if (!kv.second.IsScalar())
					return ctx.Fail(path + ": property '" + prop.Name +
						"' must be a scalar (text-encoded, e.g. \"#RRGGBB\" or \"WxH\"); nested structures are not allowed");
				prop.Value = kv.second.Scalar();
				out.push_back(std::move(prop));
			}
			return true;
		}

		bool ReadStringMap(const YAML::Node& map, const char* key,
			std::vector<UiBindingDecl>& out, const std::string& path, ParseCtx& ctx)
		{
			const YAML::Node node = map[key];
			if (!node)
				return true;
			if (!node.IsMap())
				return ctx.Fail(path + ": '" + key + "' must be a map of strings");
			for (const auto& kv : node)
			{
				UiBindingDecl binding;
				binding.Target = kv.first.as<std::string>();
				if (!kv.second.IsScalar())
					return ctx.Fail(path + ": binding '" + binding.Target + "' must be a string source path");
				binding.Source = kv.second.Scalar();
				out.push_back(std::move(binding));
			}
			return true;
		}

		bool ReadCommandMap(const YAML::Node& map, const char* key,
			std::vector<UiCommandDecl>& out, const std::string& path, ParseCtx& ctx)
		{
			const YAML::Node node = map[key];
			if (!node)
				return true;
			if (!node.IsMap())
				return ctx.Fail(path + ": '" + key + "' must be a map of strings");
			for (const auto& kv : node)
			{
				UiCommandDecl command;
				command.Event = kv.first.as<std::string>();
				if (!kv.second.IsScalar())
					return ctx.Fail(path + ": command for event '" + command.Event + "' must be a string");
				command.Command = kv.second.Scalar();
				out.push_back(std::move(command));
			}
			return true;
		}

		bool ParseNode(const YAML::Node& node, const std::string& parentPath, std::size_t index,
			UiNode& out, ParseCtx& ctx);

		bool ParseChildren(const YAML::Node& node, const std::string& selfPath, std::vector<UiNode>& out, ParseCtx& ctx)
		{
			const YAML::Node children = node["Children"];
			if (!children)
				return true;
			if (!children.IsSequence())
				return ctx.Fail(selfPath + ": 'Children' must be a sequence");
			std::size_t index = 0;
			for (const YAML::Node& child : children)
			{
				UiNode parsed;
				if (!ParseNode(child, selfPath, index, parsed, ctx))
					return false;
				out.push_back(std::move(parsed));
				++index;
			}
			return true;
		}

		bool ParseNode(const YAML::Node& node, const std::string& parentPath, std::size_t index,
			UiNode& out, ParseCtx& ctx)
		{
			if (!node.IsMap())
				return ctx.Fail(parentPath + ": each node must be a map");

			const YAML::Node typeNode = node["Type"];
			if (!typeNode)
				return ctx.Fail(parentPath + ": node #" + std::to_string(index) + " is missing 'Type'");
			if (!typeNode.IsScalar())
				return ctx.Fail(parentPath + ": 'Type' must be a string");
			out.Type = typeNode.Scalar();
			if (out.Type.empty())
				return ctx.Fail(parentPath + ": 'Type' must not be empty");

			if (const YAML::Node idNode = node["Id"])
			{
				if (!idNode.IsScalar())
					return ctx.Fail(parentPath + ": 'Id' must be a string");
				out.Id = idNode.Scalar();
				if (!IsValidUiNodeId(out.Id))
					return ctx.Fail(parentPath + ": '" + out.Id +
						"' is not a valid node Id (allowed: letters, digits, '_', '-', '.', '#')");
			}
			else
			{
				out.Id = MakeStableId(parentPath, out.Type, index);
				out.IdWasGenerated = true;
			}

			const std::string path = NodePath(parentPath, out.Id);

			if (!ReadScalarMap(node, "Props", out.Props, path, ctx)) return false;
			if (!ReadStringMap(node, "Bind", out.Bind, path, ctx)) return false;
			if (!ReadCommandMap(node, "On", out.On, path, ctx)) return false;

			if (const YAML::Node anchor = node["Anchor"])
			{
				if (!anchor.IsMap())
					return ctx.Fail(path + ": 'Anchor' must be a map");
				if (!ReadVec2(anchor, "Min", out.Anchor.Min, path, ctx)) return false;
				if (!ReadVec2(anchor, "Max", out.Anchor.Max, path, ctx)) return false;
				if (!ReadVec2(anchor, "Pivot", out.Anchor.Pivot, path, ctx)) return false;
				if (!ReadVec2(anchor, "Offset", out.Anchor.Offset, path, ctx)) return false;
				if (!ReadVec2(anchor, "Size", out.Anchor.Size, path, ctx)) return false;
				if (const YAML::Node safe = anchor["RelativeToSafeArea"])
					out.Anchor.RelativeToSafeArea = safe.as<bool>();
			}

			if (const YAML::Node layout = node["Layout"])
			{
				if (!layout.IsMap())
					return ctx.Fail(path + ": 'Layout' must be a map");
				if (const YAML::Node kind = layout["Kind"])
				{
					const std::string text = kind.IsScalar() ? kind.Scalar() : std::string("<non-scalar>");
					if (!kind.IsScalar() || !ParseUiLayoutKind(kind.Scalar(), out.Layout.Kind))
						return ctx.Fail(path + ": unknown Layout Kind '" + text +
							"' (expected Absolute/Column/Row/Overlay/Grid/Flex)");
				}
				if (const YAML::Node gap = layout["Gap"])
					out.Layout.Gap = gap.as<float>();
				if (!ReadPadding(layout, "Padding", out.Layout.Padding, path, ctx)) return false;
				if (const YAML::Node columns = layout["Columns"])
					out.Layout.Columns = columns.as<int>();
				if (const YAML::Node rowMajor = layout["RowMajor"])
					out.Layout.RowMajor = rowMajor.as<bool>();
			}

			if (const YAML::Node world = node["World"])
			{
				if (!world.IsMap())
					return ctx.Fail(path + ": 'World' must be a map");
				out.World.Enabled = true;
				if (const YAML::Node target = world["Target"])
				{
					if (!target.IsScalar())
						return ctx.Fail(path + ": World.Target must be a string");
					out.World.Target = target.Scalar();
				}
				if (out.World.Target.empty())
					return ctx.Fail(path + ": World anchor needs a non-empty 'Target'");
				if (!ReadVec3(world, "Offset", out.World.Offset, path, ctx)) return false;
				if (const YAML::Node keep = world["KeepOnScreen"])
					out.World.KeepOnScreen = keep.as<bool>();
			}

			return ParseChildren(node, path, out.Children, ctx);
		}

		// ---------------- 序列化 ----------------

		bool AnchorIsDefault(const UiAnchor& a) { return IsDefaultAnchor(a); }

		bool LayoutIsDefault(const UiLayoutSpec& l) { return IsDefaultLayout(l); }

		void EmitVec2(YAML::Emitter& out, const char* key, const glm::vec2& v)
		{
			out << YAML::Key << key << YAML::Value
				<< YAML::Flow << YAML::BeginSeq << v.x << v.y << YAML::EndSeq;
		}

		void EmitVec3(YAML::Emitter& out, const char* key, const glm::vec3& v)
		{
			out << YAML::Key << key << YAML::Value
				<< YAML::Flow << YAML::BeginSeq << v.x << v.y << v.z << YAML::EndSeq;
		}

		void EmitNode(YAML::Emitter& out, const UiNode& node)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "Id" << YAML::Value << node.Id;
			out << YAML::Key << "Type" << YAML::Value << node.Type;

			if (!node.Props.empty())
			{
				out << YAML::Key << "Props" << YAML::Value << YAML::BeginMap;
				for (const UiProp& prop : node.Props)
					out << YAML::Key << prop.Name << YAML::Value << prop.Value;
				out << YAML::EndMap;
			}

			if (!AnchorIsDefault(node.Anchor))
			{
				out << YAML::Key << "Anchor" << YAML::Value << YAML::BeginMap;
				EmitVec2(out, "Min", node.Anchor.Min);
				EmitVec2(out, "Max", node.Anchor.Max);
				EmitVec2(out, "Pivot", node.Anchor.Pivot);
				EmitVec2(out, "Offset", node.Anchor.Offset);
				EmitVec2(out, "Size", node.Anchor.Size);
				if (node.Anchor.RelativeToSafeArea)
					out << YAML::Key << "RelativeToSafeArea" << YAML::Value << true;
				out << YAML::EndMap;
			}

			if (!LayoutIsDefault(node.Layout))
			{
				out << YAML::Key << "Layout" << YAML::Value << YAML::BeginMap;
				out << YAML::Key << "Kind" << YAML::Value << UiLayoutKindName(node.Layout.Kind);
				out << YAML::Key << "Gap" << YAML::Value << node.Layout.Gap;
				out << YAML::Key << "Padding" << YAML::Value << YAML::Flow << YAML::BeginSeq
					<< node.Layout.Padding[0] << node.Layout.Padding[1]
					<< node.Layout.Padding[2] << node.Layout.Padding[3] << YAML::EndSeq;
				if (node.Layout.Kind == UiLayoutKind::Grid)
					out << YAML::Key << "Columns" << YAML::Value << node.Layout.Columns;
				if (node.Layout.Kind == UiLayoutKind::Flex)
					out << YAML::Key << "RowMajor" << YAML::Value << node.Layout.RowMajor;
				out << YAML::EndMap;
			}

			if (node.World.Enabled)
			{
				out << YAML::Key << "World" << YAML::Value << YAML::BeginMap;
				out << YAML::Key << "Target" << YAML::Value << node.World.Target;
				EmitVec3(out, "Offset", node.World.Offset);
				if (node.World.KeepOnScreen)
					out << YAML::Key << "KeepOnScreen" << YAML::Value << true;
				out << YAML::EndMap;
			}

			if (!node.Bind.empty())
			{
				out << YAML::Key << "Bind" << YAML::Value << YAML::BeginMap;
				for (const UiBindingDecl& binding : node.Bind)
					out << YAML::Key << binding.Target << YAML::Value << binding.Source;
				out << YAML::EndMap;
			}

			if (!node.On.empty())
			{
				out << YAML::Key << "On" << YAML::Value << YAML::BeginMap;
				for (const UiCommandDecl& command : node.On)
					out << YAML::Key << command.Event << YAML::Value << command.Command;
				out << YAML::EndMap;
			}

			if (!node.Children.empty())
			{
				out << YAML::Key << "Children" << YAML::Value << YAML::BeginSeq;
				for (const UiNode& child : node.Children)
					EmitNode(out, child);
				out << YAML::EndSeq;
			}

			out << YAML::EndMap;
		}
	}

	// ---------------- 公开接口 ----------------

	bool UiDocumentIO::Parse(std::string_view text, UiDocument& out, std::string* error)
	{
		ParseCtx ctx { error };
		try
		{
			const YAML::Node root = YAML::Load(std::string(text));
			if (!root || !root.IsMap())
				return ctx.Fail("UI document root must be a map");

			if (const YAML::Node version = root["FormatVersion"])
				out.FormatVersion = version.as<int>();
			else
				out.FormatVersion = 0;

			if (out.FormatVersion != kUiDocumentFormatVersion)
			{
				return ctx.Fail("Unsupported UI document format version " +
					std::to_string(out.FormatVersion) + " (expected " +
					std::to_string(kUiDocumentFormatVersion) +
					"); this build does not migrate older documents");
			}

			const YAML::Node screen = root["Screen"];
			if (!screen || !screen.IsScalar() || screen.Scalar().empty())
				return ctx.Fail("'Screen' is required and must be a non-empty string");
			out.Screen = screen.Scalar();

			if (const YAML::Node design = root["Design"])
			{
				if (!design.IsMap())
					return ctx.Fail("'Design' must be a map");
				if (!ReadVec2(design, "Resolution", out.Design.Resolution, "Design", ctx)) return false;
				if (const YAML::Node mode = design["ScaleMode"])
				{
					const std::string text2 = mode.IsScalar() ? mode.Scalar() : std::string("<non-scalar>");
					if (!mode.IsScalar() || !ParseUiScaleMode(mode.Scalar(), out.Design.ScaleMode))
						return ctx.Fail("Design: unknown ScaleMode '" + text2 +
							"' (expected ConstantPixelSize/ScaleWithScreenSize/Expand/Shrink)");
				}
				if (const YAML::Node match = design["Match"])
					out.Design.Match = match.as<float>();
			}

			if (const YAML::Node safe = root["SafeArea"])
			{
				if (!safe.IsMap())
					return ctx.Fail("'SafeArea' must be a map");
				if (const YAML::Node enabled = safe["Enabled"])
					out.SafeArea.Enabled = enabled.as<bool>();
				if (const YAML::Node insets = safe["Insets"])
				{
					if (!insets.IsSequence() || insets.size() != 4)
						return ctx.Fail("SafeArea: 'Insets' must be [left, top, right, bottom]");
					out.SafeArea.Left = insets[0].as<float>();
					out.SafeArea.Top = insets[1].as<float>();
					out.SafeArea.Right = insets[2].as<float>();
					out.SafeArea.Bottom = insets[3].as<float>();
				}
			}

			if (const YAML::Node theme = root["Theme"])
			{
				if (!theme.IsScalar())
					return ctx.Fail("'Theme' must be a string");
				out.Theme = theme.Scalar();
			}

			if (const YAML::Node params = root["Parameters"])
			{
				if (!params.IsSequence())
					return ctx.Fail("'Parameters' must be a sequence");
				for (const YAML::Node& item : params)
				{
					if (!item.IsMap())
						return ctx.Fail("Parameters: each entry must be a map");
					UiParamDecl decl;
					if (const YAML::Node n = item["Name"]) decl.Name = n.as<std::string>();
					if (const YAML::Node t = item["Type"]) decl.Type = t.as<std::string>();
					if (const YAML::Node d = item["Default"]) decl.Default = d.IsScalar() ? d.Scalar() : std::string();
					out.Parameters.push_back(std::move(decl));
				}
			}

			if (const YAML::Node styles = root["Styles"])
			{
				if (!styles.IsSequence())
					return ctx.Fail("'Styles' must be a sequence");
				std::size_t index = 0;
				for (const YAML::Node& item : styles)
				{
					UiStyleDef def;
					const std::string path = "Styles[" + std::to_string(index) + "]";
					if (!item.IsMap())
						return ctx.Fail(path + ": each style must be a map");
					if (const YAML::Node id = item["Id"])
						def.Id = id.as<std::string>();
					if (!ReadScalarMap(item, "Props", def.Props, path, ctx))
						return false;
					out.Styles.push_back(std::move(def));
					++index;
				}
			}

			const YAML::Node nodes = root["Nodes"];
			if (!nodes)
				return ctx.Fail("'Nodes' is required (may be an empty sequence)");
			if (!nodes.IsSequence())
				return ctx.Fail("'Nodes' must be a sequence");
			std::size_t index = 0;
			for (const YAML::Node& item : nodes)
			{
				UiNode node;
				if (!ParseNode(item, std::string(), index, node, ctx))
					return false;
				out.Nodes.push_back(std::move(node));
				++index;
			}

			std::vector<UiValidationIssue> issues;
			if (!ValidateUiDocument(out, &issues))
			{
				std::string message = "UI document validation failed: ";
				for (std::size_t i = 0; i < issues.size() && i < 3; ++i)
				{
					if (i)
						message += "; ";
					message += (issues[i].Path.empty() ? std::string("<root>") : issues[i].Path) + ": " + issues[i].Message;
				}
				if (issues.size() > 3)
					message += "; ... (" + std::to_string(issues.size() - 3) + " more)";
				return ctx.Fail(message);
			}

			return true;
		}
		catch (const std::exception& e)
		{
			return ctx.Fail(std::string("UI document parse error: ") + e.what());
		}
	}

	bool UiDocumentIO::LoadFile(const std::filesystem::path& path, UiDocument& out, std::string* error)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			if (error)
				*error = "cannot open UI document: " + path.string();
			return false;
		}
		std::ostringstream buffer;
		buffer << file.rdbuf();
		return Parse(buffer.str(), out, error);
	}

	std::string UiDocumentIO::Serialize(const UiDocument& doc)
	{
		YAML::Emitter out;
		out.SetIndent(2);

		out << YAML::BeginMap;
		out << YAML::Key << "FormatVersion" << YAML::Value << doc.FormatVersion;
		out << YAML::Key << "Screen" << YAML::Value << doc.Screen;

		out << YAML::Key << "Design" << YAML::Value << YAML::BeginMap;
		EmitVec2(out, "Resolution", doc.Design.Resolution);
		out << YAML::Key << "ScaleMode" << YAML::Value << UiScaleModeName(doc.Design.ScaleMode);
		out << YAML::Key << "Match" << YAML::Value << doc.Design.Match;
		out << YAML::EndMap;

		if (doc.SafeArea.Enabled || doc.SafeArea.Top != 0.0f || doc.SafeArea.Bottom != 0.0f ||
			doc.SafeArea.Left != 0.0f || doc.SafeArea.Right != 0.0f)
		{
			out << YAML::Key << "SafeArea" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "Enabled" << YAML::Value << doc.SafeArea.Enabled;
			out << YAML::Key << "Insets" << YAML::Value << YAML::Flow << YAML::BeginSeq
				<< doc.SafeArea.Left << doc.SafeArea.Top << doc.SafeArea.Right << doc.SafeArea.Bottom
				<< YAML::EndSeq;
			out << YAML::EndMap;
		}

		if (!doc.Theme.empty())
			out << YAML::Key << "Theme" << YAML::Value << doc.Theme;

		if (!doc.Parameters.empty())
		{
			out << YAML::Key << "Parameters" << YAML::Value << YAML::BeginSeq;
			for (const UiParamDecl& decl : doc.Parameters)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "Name" << YAML::Value << decl.Name;
				out << YAML::Key << "Type" << YAML::Value << decl.Type;
				if (!decl.Default.empty())
					out << YAML::Key << "Default" << YAML::Value << decl.Default;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
		}

		if (!doc.Styles.empty())
		{
			out << YAML::Key << "Styles" << YAML::Value << YAML::BeginSeq;
			for (const UiStyleDef& def : doc.Styles)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "Id" << YAML::Value << def.Id;
				if (!def.Props.empty())
				{
					out << YAML::Key << "Props" << YAML::Value << YAML::BeginMap;
					for (const UiProp& prop : def.Props)
						out << YAML::Key << prop.Name << YAML::Value << prop.Value;
					out << YAML::EndMap;
				}
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
		}

		out << YAML::Key << "Nodes" << YAML::Value << YAML::BeginSeq;
		for (const UiNode& node : doc.Nodes)
			EmitNode(out, node);
		out << YAML::EndSeq;

		out << YAML::EndMap;

		std::string text = out.c_str();
		text.push_back('\n');
		return text;
	}

	bool UiDocumentIO::SaveFile(const std::filesystem::path& path, const UiDocument& doc, std::string* error)
	{
		const std::string text = Serialize(doc);

		std::error_code ec;
		std::filesystem::path tmp = path;
		tmp += ".tmp";
		std::filesystem::path bak = path;
		bak += ".bak";

		{
			std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
			if (!file)
			{
				if (error)
					*error = "cannot write temporary UI document: " + tmp.string();
				return false;
			}
			file << text;
			if (!file.good())
			{
				if (error)
					*error = "failed while writing UI document: " + tmp.string();
				return false;
			}
		}

		if (std::filesystem::exists(path, ec))
		{
			std::filesystem::remove(bak, ec);
			std::filesystem::copy_file(path, bak, std::filesystem::copy_options::overwrite_existing, ec);
		}
		std::filesystem::remove(path, ec);
		std::filesystem::rename(tmp, path, ec);
		if (ec)
		{
			if (error)
				*error = "failed to commit UI document: " + ec.message();
			return false;
		}
		return true;
	}
}
