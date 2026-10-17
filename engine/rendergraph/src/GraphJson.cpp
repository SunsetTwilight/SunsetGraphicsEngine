// ============================================================
//  engine/rendergraph/src/GraphJson.cpp
// ============================================================
#include "gfx/rendergraph/GraphJson.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <initializer_list>

namespace gfx::rg
{
// ============================================================
//  名前
// ============================================================
const char* nameOf(ResourceKind v)
{
	switch (v)
	{
	case ResourceKind::Texture:  return "texture";
	case ResourceKind::Buffer:   return "buffer";
	case ResourceKind::Imported: return "imported";
	}
	return "?";
}
const char* nameOf(TextureDimension v)
{
	switch (v)
	{
	case TextureDimension::Tex2D: return "2d";
	case TextureDimension::Cube:  return "cube";
	case TextureDimension::Tex3D: return "3d";
	}
	return "?";
}
const char* nameOf(PassType v)
{
	switch (v)
	{
	case PassType::Raster:  return "Raster";
	case PassType::Compute: return "Compute";
	case PassType::Copy:    return "Copy";
	}
	return "?";
}
const char* nameOf(Access v)
{
	switch (v)
	{
	case Access::SRV:          return "SRV";
	case Access::UAV:          return "UAV";
	case Access::RTV:          return "RTV";
	case Access::DSV:          return "DSV";
	case Access::DSVRead:      return "DSVRead";
	case Access::CopySrc:      return "CopySrc";
	case Access::CopyDst:      return "CopyDst";
	case Access::IndirectArgs: return "IndirectArgs";
	case Access::ResolveSrc:   return "ResolveSrc";
	case Access::ResolveDst:   return "ResolveDst";
	}
	return "?";
}
const char* nameOf(LoadOp v)
{
	switch (v)
	{
	case LoadOp::Load:     return "load";
	case LoadOp::Clear:    return "clear";
	case LoadOp::DontCare: return "dontCare";
	}
	return "?";
}
const char* nameOf(ContentKind v)
{
	switch (v)
	{
	case ContentKind::None:             return "none";
	case ContentKind::DrawList:         return "drawList";
	case ContentKind::Fullscreen:       return "fullscreen";
	case ContentKind::Dispatch:         return "dispatch";
	case ContentKind::DrawIndirect:     return "drawIndirect";
	case ContentKind::DispatchIndirect: return "dispatchIndirect";
	case ContentKind::Custom:           return "custom";
	}
	return "?";
}
bool parse(const std::string& s, PassType& out)
{
	for (PassType v : { PassType::Raster, PassType::Compute, PassType::Copy })
		if (s == nameOf(v)) { out = v; return true; }
	return false;
}
bool parse(const std::string& s, Access& out)
{
	for (Access v : { Access::SRV, Access::UAV, Access::RTV, Access::DSV, Access::DSVRead, Access::CopySrc,
	                  Access::CopyDst, Access::IndirectArgs, Access::ResolveSrc, Access::ResolveDst })
		if (s == nameOf(v)) { out = v; return true; }
	return false;
}
bool parse(const std::string& s, LoadOp& out)
{
	for (LoadOp v : { LoadOp::Load, LoadOp::Clear, LoadOp::DontCare })
		if (s == nameOf(v)) { out = v; return true; }
	return false;
}
bool parse(const std::string& s, TextureDimension& out)
{
	for (TextureDimension v : { TextureDimension::Tex2D, TextureDimension::Cube, TextureDimension::Tex3D })
		if (s == nameOf(v)) { out = v; return true; }
	return false;
}

bool parseIndexExpr(const std::string& s, IndexValue& out)
{
	// "3" / "i" / "i+1" / "i-2" (空白は許さない)
	IndexValue v;
	if (s.empty())
		return false;
	const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
	if (isDigit(s[0]))
	{
		int32_t n = 0;
		const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
		if (r.ec != std::errc() || r.ptr != s.data() + s.size())
			return false;
		v.value = n;
		out = v;
		return true;
	}
	size_t i = 0;
	while (i < s.size() && (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_' || (i > 0 && isDigit(s[i]))))
		++i;
	if (i == 0)
		return false;
	v.var = s.substr(0, i);
	if (i < s.size())
	{
		if (s[i] != '+' && s[i] != '-')
			return false;
		const bool neg = s[i] == '-';
		int32_t n = 0;
		const auto r = std::from_chars(s.data() + i + 1, s.data() + s.size(), n);
		if (r.ec != std::errc() || r.ptr != s.data() + s.size() || i + 1 == s.size())
			return false;
		v.add = neg ? -n : n;
	}
	out = v;
	return true;
}

std::string formatIndexExpr(const IndexValue& v)
{
	if (!v.usesVar())
		return std::to_string(v.value);
	if (v.add == 0) return v.var;
	return v.var + (v.add > 0 ? "+" : "-") + std::to_string(v.add > 0 ? v.add : -v.add);
}

// ============================================================
//  書き出し
// ============================================================
namespace
{
GfxJson floatJson(float v)
{
	// float として最短の表記 (0.5f → 0.5)
	char buf[32];
	const auto r = std::to_chars(buf, buf + sizeof(buf), v);
	double d = 0.0;
	std::from_chars(buf, r.ptr, d);
	return GfxJson(d);
}

GfxJson indexJson(const IndexValue& v)
{
	return v.usesVar() ? GfxJson(formatIndexExpr(v)) : GfxJson(static_cast<int64_t>(v.value));
}

GfxJson formatJson(GfxFormat f)
{
	const char* name = gfxEnumName(gfxEnumInfo<GfxFormat>(), static_cast<uint64_t>(f));
	return name ? GfxJson(name) : GfxJson(static_cast<uint32_t>(f));
}

GfxJson bindJson(const Binding& b)
{
	GfxJson j = GfxJson::object();
	j.set("param", static_cast<int64_t>(b.param));
	if (b.offset != 0)
		j.set("offset", b.offset);
	return j;
}

std::string accessName(const PassResourceRef& r)
{
	if (r.as == Access::RTV)
		return "RTV" + std::to_string(r.rtvIndex);
	return nameOf(r.as);
}

GfxJson refJson(const PassResourceRef& r, bool output)
{
	GfxJson j = GfxJson::object();
	j.set("res", r.res);
	j.set("as", accessName(r));
	if (output && r.load != LoadOp::Load)
		j.set("load", nameOf(r.load));
	if (!r.mip.all())
	{
		j.set("mip", indexJson(r.mip));
		if (r.mipCount != 1) j.set("mipCount", r.mipCount);
	}
	if (!r.slice.all())
	{
		j.set("slice", indexJson(r.slice));
		if (r.sliceCount != 1) j.set("sliceCount", r.sliceCount);
	}
	if (r.viewFormat != GfxFormat::Unknown)
		j.set("format", formatJson(r.viewFormat));
	if (r.bind.valid())
		j.set("bind", bindJson(r.bind));
	if (!r.shaderName.empty())
		j.set("shaderName", r.shaderName);
	return j;
}

GfxJson regionJson(const Region& r)
{
	GfxJson j = GfxJson::object();
	if (!r.relative) j.set("relative", false);
	j.set("x", floatJson(r.x));
	j.set("y", floatJson(r.y));
	j.set("width", floatJson(r.width));
	j.set("height", floatJson(r.height));
	return j;
}

GfxJson stringArray(const std::vector<std::string>& v)
{
	GfxJson a = GfxJson::array();
	for (const auto& s : v) a.push(s);
	return a;
}
} // namespace

GfxJson graphToJson(const GraphDesc& g)
{
	GfxJson root = GfxJson::object();
	root.set("version", g.version);

	if (!g.parameters.empty())
	{
		GfxJson params = GfxJson::object();
		for (const auto& p : g.parameters)
			params.set(p.name, p.isBool ? GfxJson(p.value != 0) : GfxJson(p.value));
		root.set("parameters", std::move(params));
	}

	GfxJson resources = GfxJson::object();
	for (const auto& r : g.resources)
	{
		GfxJson j = GfxJson::object();
		if (r.kind != ResourceKind::Texture)
			j.set("kind", nameOf(r.kind));
		if (r.kind == ResourceKind::Texture)
		{
			j.set("format", formatJson(r.format));
			if (r.dimension != TextureDimension::Tex2D)
				j.set("dimension", nameOf(r.dimension));
			GfxJson size = GfxJson::object();
			if (r.size.relative)
				size.set("relative", floatJson(r.size.scale));
			else
			{
				size.set("width", r.size.width);
				size.set("height", r.size.height);
			}
			j.set("size", std::move(size));
			if (r.arraySize != 1)   j.set("array", r.arraySize);
			if (r.depth != 1)       j.set("depth", r.depth);
			if (r.mipLevels != 1)   j.set("mips", r.mipLevels);
			if (r.sampleCount != 1) j.set("samples", r.sampleCount);
		}
		if (r.kind == ResourceKind::Buffer)
			j.set("bytes", r.byteSize);
		if (r.history)
			j.set("history", true);
		if (r.clear.hasColor || r.clear.hasDepth)
		{
			GfxJson c = GfxJson::object();
			if (r.clear.hasColor)
			{
				GfxJson col = GfxJson::array();
				for (float v : r.clear.color)
					col.push(floatJson(v));
				c.set("color", std::move(col));
			}
			if (r.clear.hasDepth)
			{
				c.set("depth", floatJson(r.clear.depth));
				if (r.clear.stencil != 0)
					c.set("stencil", static_cast<uint32_t>(r.clear.stencil));
			}
			j.set("clear", std::move(c));
		}
		resources.set(r.name, std::move(j));
	}
	root.set("resources", std::move(resources));

	GfxJson passes = GfxJson::array();
	for (const auto& p : g.passes)
	{
		GfxJson j = GfxJson::object();
		j.set("name", p.name);
		j.set("type", nameOf(p.type));
		if (!p.pipeline.empty()) j.set("pipeline", p.pipeline);
		if (!p.when.empty())     j.set("when", p.when);
		if (p.repeat.set)
		{
			GfxJson rep = GfxJson::object();
			rep.set("count", p.repeat.countParam.empty() ? GfxJson(p.repeat.count) : GfxJson(p.repeat.countParam));
			if (p.repeat.var != "i")  rep.set("var", p.repeat.var);
			if (p.repeat.start != 0)  rep.set("start", static_cast<int64_t>(p.repeat.start));
			j.set("repeat", std::move(rep));
		}
		if (!p.view.empty())     j.set("view", p.view);
		if (p.viewport.set)      j.set("viewport", regionJson(p.viewport));
		if (p.scissor.set)       j.set("scissor", regionJson(p.scissor));
		if (p.order != 0)        j.set("order", static_cast<int64_t>(p.order));
		if (!p.after.empty())    j.set("after", stringArray(p.after));
		if (p.keep)              j.set("keep", true);
		if (!p.enabled)          j.set("enabled", false);

		if (!p.constants.empty())
		{
			GfxJson arr = GfxJson::array();
			for (const auto& c : p.constants)
			{
				GfxJson cj = GfxJson::object();
				cj.set("source", c.source);
				if (c.bind.valid())
					cj.set("bind", bindJson(c.bind));
				if (!c.values.empty())
				{
					GfxJson v = GfxJson::array();
					for (float f : c.values) v.push(floatJson(f));
					cj.set("values", std::move(v));
				}
				arr.push(std::move(cj));
			}
			j.set("constants", std::move(arr));
		}
		if (!p.inputs.empty())
		{
			GfxJson arr = GfxJson::array();
			for (const auto& r : p.inputs) arr.push(refJson(r, false));
			j.set("inputs", std::move(arr));
		}
		if (!p.outputs.empty())
		{
			GfxJson arr = GfxJson::array();
			for (const auto& r : p.outputs) arr.push(refJson(r, true));
			j.set("outputs", std::move(arr));
		}

		GfxJson content = GfxJson::object();
		switch (p.content.kind)
		{
		case ContentKind::DrawList:
		{
			GfxJson d = GfxJson::object();
			if (!p.content.drawList.layer.empty()) d.set("layer", p.content.drawList.layer);
			if (!p.content.drawList.tags.empty())  d.set("tags", stringArray(p.content.drawList.tags));
			if (!p.content.drawList.sort.empty())  d.set("sort", p.content.drawList.sort);
			content.set("drawList", std::move(d));
			break;
		}
		case ContentKind::Fullscreen:
			content.set("fullscreen", true);
			break;
		case ContentKind::Dispatch:
		{
			GfxJson d = GfxJson::array();
			for (uint32_t v : p.content.dispatch) d.push(v);
			content.set("dispatch", std::move(d));
			break;
		}
		case ContentKind::DrawIndirect:
		case ContentKind::DispatchIndirect:
		{
			const IndirectSpec& s = p.content.indirect;
			GfxJson d = GfxJson::object();
			d.set("args", s.args);
			if (s.offset != 0) d.set("offset", s.offset);
			if (p.content.kind == ContentKind::DrawIndirect)
			{
				if (!s.count.empty())  d.set("count", s.count);
				if (s.maxCount != 1)   d.set("maxCount", s.maxCount);
			}
			content.set(nameOf(p.content.kind), std::move(d));
			break;
		}
		case ContentKind::Custom:
			content.set("custom", p.content.custom);
			break;
		case ContentKind::None:
			break;
		}
		if (content.size() > 0)
			j.set("content", std::move(content));
		passes.push(std::move(j));
	}
	root.set("passes", std::move(passes));
	return root;
}

std::string graphToString(const GraphDesc& graph, int indent)
{
	return gfxJsonStringify(graphToJson(graph), indent);
}

// ============================================================
//  読み込み
// ============================================================
namespace
{
class Reader
{
public:
	explicit Reader(GfxJsonDiagnostics& d) : m_diag(d) {}

	void error(const std::string& path, const std::string& msg)   { m_diag.errors.push_back(path + ": " + msg); }
	void warning(const std::string& path, const std::string& msg) { m_diag.warnings.push_back(path + ": " + msg); }
	bool ok() const { return m_diag.errors.empty(); }

	// 知らないキーを警告する
	void checkKeys(const GfxJson& obj, const std::string& path, std::initializer_list<const char*> known)
	{
		for (const auto& k : obj.keys())
		{
			bool found = false;
			for (const char* n : known)
				found |= (k == n);
			if (!found)
				warning(path + "." + k, "知らないキーなので無視します");
		}
	}

	bool str(const GfxJson& obj, const char* key, const std::string& path, std::string& out, bool required)
	{
		const GfxJson* v = obj.find(key);
		if (!v)
		{
			if (required) error(path + "." + key, "必要な項目がありません");
			return !required;
		}
		if (!v->isString()) { error(path + "." + key, "文字列が必要です"); return false; }
		out = v->asString();
		return true;
	}

	bool boolean(const GfxJson& obj, const char* key, const std::string& path, bool& out)
	{
		const GfxJson* v = obj.find(key);
		if (!v) return true;
		if (!v->isBool()) { error(path + "." + key, "true / false が必要です"); return false; }
		out = v->asBool();
		return true;
	}

	bool integer(const GfxJson& v, const std::string& path, double lo, double hi, double& out)
	{
		if (!v.isNumber() || v.asNumber() != std::floor(v.asNumber()))
		{
			error(path, "整数が必要です");
			return false;
		}
		if (v.asNumber() < lo || v.asNumber() > hi)
		{
			error(path, "値が範囲外です");
			return false;
		}
		out = v.asNumber();
		return true;
	}

	template<typename T>
	bool integerKey(const GfxJson& obj, const char* key, const std::string& path, double lo, double hi, T& out)
	{
		const GfxJson* v = obj.find(key);
		if (!v) return true;
		double d = 0;
		if (!integer(*v, path + "." + key, lo, hi, d)) return false;
		out = static_cast<T>(d);
		return true;
	}

	bool number(const GfxJson& v, const std::string& path, float& out)
	{
		if (!v.isNumber()) { error(path, "数値が必要です"); return false; }
		out = static_cast<float>(v.asNumber());
		return true;
	}

	bool numberKey(const GfxJson& obj, const char* key, const std::string& path, float& out)
	{
		const GfxJson* v = obj.find(key);
		return !v || number(*v, path + "." + key, out);
	}

	bool stringArray(const GfxJson& obj, const char* key, const std::string& path, std::vector<std::string>& out)
	{
		const GfxJson* v = obj.find(key);
		if (!v) return true;
		if (!v->isArray()) { error(path + "." + key, "文字列の配列が必要です"); return false; }
		out.clear();
		bool good = true;
		for (size_t i = 0; i < v->size(); ++i)
		{
			if (!v->at(i).isString()) { error(path + "." + key + "[" + std::to_string(i) + "]", "文字列が必要です"); good = false; continue; }
			out.push_back(v->at(i).asString());
		}
		return good;
	}

	bool format(const GfxJson& v, const std::string& path, GfxFormat& out)
	{
		uint64_t value = 0;
		if (v.isString() && gfxEnumValue(gfxEnumInfo<GfxFormat>(), v.asString().c_str(), value))
		{
			out = static_cast<GfxFormat>(value);
			return true;
		}
		if (v.isNumber())
		{
			out = static_cast<GfxFormat>(static_cast<uint32_t>(v.asNumber()));
			return true;
		}
		error(path, "フォーマットの名前が必要です (例: R8G8B8A8_Unorm)");
		return false;
	}

	// mip / slice: 数値か "i" / "i+1" / "i-1"
	void index(const GfxJson& obj, const char* key, const std::string& path, IndexValue& out)
	{
		const GfxJson* v = obj.find(key);
		if (!v) return;
		const std::string p = path + "." + key;
		if (v->isNumber())
		{
			double d = 0;
			if (integer(*v, p, 0, 65535, d))
				out = IndexValue::fixed(static_cast<int32_t>(d));
		}
		else if (v->isString())
		{
			if (!parseIndexExpr(v->asString(), out))
				error(p, "番号か、repeat の変数を使った式 (\"i\" / \"i+1\" / \"i-1\") が必要です");
		}
		else
			error(p, "番号か、repeat の変数を使った式が必要です");
	}

	void binding(const GfxJson& obj, const std::string& path, Binding& out)
	{
		const GfxJson* v = obj.find("bind");
		if (!v) return;
		const std::string p = path + ".bind";
		if (!v->isObject()) { error(p, "{ \"param\": 番号, \"offset\": 位置 } が必要です"); return; }
		checkKeys(*v, p, { "param", "offset" });
		const GfxJson* param = v->find("param");
		if (!param) { error(p + ".param", "必要な項目がありません"); return; }
		double d = 0;
		if (integer(*param, p + ".param", 0, GFX_MAX_ROOT_PARAMETERS - 1, d))
			out.param = static_cast<int32_t>(d);
		integerKey(*v, "offset", p, 0, 4294967295.0, out.offset);
	}

	void region(const GfxJson& obj, const char* key, const std::string& path, Region& out)
	{
		const GfxJson* v = obj.find(key);
		if (!v) return;
		const std::string p = path + "." + key;
		if (!v->isObject()) { error(p, "{ \"x\", \"y\", \"width\", \"height\" } が必要です"); return; }
		checkKeys(*v, p, { "relative", "x", "y", "width", "height" });
		out.set = true;
		boolean(*v, "relative", p, out.relative);
		numberKey(*v, "x", p, out.x);
		numberKey(*v, "y", p, out.y);
		numberKey(*v, "width", p, out.width);
		numberKey(*v, "height", p, out.height);
	}

	void resourceRef(const GfxJson& v, const std::string& path, bool output, PassResourceRef& out)
	{
		if (!v.isObject()) { error(path, "オブジェクトが必要です"); return; }
		checkKeys(v, path, { "res", "as", "load", "mip", "mipCount", "slice", "sliceCount", "format", "bind", "shaderName" });
		str(v, "res", path, out.res, true);
		std::string as;
		if (str(v, "as", path, as, true) && !as.empty())
		{
			if (as.rfind("RTV", 0) == 0 && as.size() == 4 && as[3] >= '0' && as[3] <= '7')
			{
				out.as       = Access::RTV;
				out.rtvIndex = static_cast<uint32_t>(as[3] - '0');
			}
			else if (!parse(as, out.as) || out.as == Access::RTV)
				error(path + ".as", "'" + as + "' は使い方の名前ではありません (SRV / UAV / RTV0〜RTV7 / DSV / DSVRead / "
				                    "CopySrc / CopyDst / IndirectArgs / ResolveSrc / ResolveDst)");
		}
		std::string load;
		if (str(v, "load", path, load, false) && !load.empty())
		{
			if (!output)
				warning(path + ".load", "入力では使わないので無視します");
			else if (!parse(load, out.load))
				error(path + ".load", "'" + load + "' は load / clear / dontCare のどれかです");
		}
		index(v, "mip", path, out.mip);
		integerKey(v, "mipCount", path, 1, 16, out.mipCount);
		index(v, "slice", path, out.slice);
		integerKey(v, "sliceCount", path, 1, 2048, out.sliceCount);
		if (const GfxJson* f = v.find("format"))
			format(*f, path + ".format", out.viewFormat);
		binding(v, path, out.bind);
		str(v, "shaderName", path, out.shaderName, false);
	}

	void resource(const std::string& name, const GfxJson& v, const std::string& path, ResourceDesc& r)
	{
		r.name = name;
		if (!v.isObject()) { error(path, "オブジェクトが必要です"); return; }
		checkKeys(v, path, { "kind", "format", "dimension", "size", "array", "depth", "mips", "samples", "bytes", "history", "clear" });

		std::string kind;
		if (str(v, "kind", path, kind, false) && !kind.empty())
		{
			if      (kind == "texture")  r.kind = ResourceKind::Texture;
			else if (kind == "buffer")   r.kind = ResourceKind::Buffer;
			else if (kind == "imported") r.kind = ResourceKind::Imported;
			else error(path + ".kind", "'" + kind + "' は texture / buffer / imported のどれかです");
		}
		if (isImportedName(name) && !v.find("kind"))
			r.kind = ResourceKind::Imported;

		if (const GfxJson* f = v.find("format"))
			format(*f, path + ".format", r.format);
		std::string dim;
		if (str(v, "dimension", path, dim, false) && !dim.empty() && !parse(dim, r.dimension))
			error(path + ".dimension", "'" + dim + "' は 2d / cube / 3d のどれかです");
		if (const GfxJson* s = v.find("size"))
		{
			const std::string sp = path + ".size";
			if (!s->isObject())
				error(sp, "{ \"relative\": 比率 } か { \"width\": 幅, \"height\": 高さ } が必要です");
			else
			{
				checkKeys(*s, sp, { "relative", "width", "height" });
				if (const GfxJson* rel = s->find("relative"))
				{
					r.size.relative = true;
					number(*rel, sp + ".relative", r.size.scale);
				}
				else
				{
					r.size.relative = false;
					integerKey(*s, "width", sp, 1, 16384, r.size.width);
					integerKey(*s, "height", sp, 1, 16384, r.size.height);
				}
			}
		}
		integerKey(v, "array", path, 1, 2048, r.arraySize);
		integerKey(v, "depth", path, 1, 2048, r.depth);
		integerKey(v, "mips", path, 1, 16, r.mipLevels);
		integerKey(v, "samples", path, 1, 32, r.sampleCount);
		integerKey(v, "bytes", path, 0, 9007199254740991.0, r.byteSize);
		boolean(v, "history", path, r.history);

		if (const GfxJson* c = v.find("clear"))
		{
			const std::string cp = path + ".clear";
			if (!c->isObject()) { error(cp, "オブジェクトが必要です"); return; }
			checkKeys(*c, cp, { "color", "depth", "stencil" });
			if (const GfxJson* col = c->find("color"))
			{
				if (!col->isArray() || col->size() != 4)
					error(cp + ".color", "4 つの数値の配列が必要です");
				else
				{
					r.clear.hasColor = true;
					for (size_t i = 0; i < 4; ++i)
						number(col->at(i), cp + ".color[" + std::to_string(i) + "]", r.clear.color[i]);
				}
			}
			if (const GfxJson* d = c->find("depth"))
			{
				r.clear.hasDepth = true;
				number(*d, cp + ".depth", r.clear.depth);
			}
			integerKey(*c, "stencil", cp, 0, 255, r.clear.stencil);
		}
	}

	void indirect(const GfxJson& v, const std::string& path, bool draw, IndirectSpec& out)
	{
		if (!v.isObject()) { error(path, "{ \"args\": バッファ, \"offset\": バイト } が必要です"); return; }
		if (draw) checkKeys(v, path, { "args", "offset", "count", "maxCount" });
		else      checkKeys(v, path, { "args", "offset" });
		str(v, "args", path, out.args, true);
		integerKey(v, "offset", path, 0, 9007199254740991.0, out.offset);
		if (draw)
		{
			str(v, "count", path, out.count, false);
			integerKey(v, "maxCount", path, 1, 1048576, out.maxCount);
		}
	}

	void pass(const GfxJson& v, const std::string& basePath, PassDesc& p)
	{
		if (!v.isObject()) { error(basePath, "オブジェクトが必要です"); return; }
		str(v, "name", basePath, p.name, true);
		const std::string path = p.name.empty() ? basePath : "passes[" + p.name + "]";
		checkKeys(v, path, { "name", "type", "pipeline", "when", "repeat", "view", "viewport", "scissor", "order",
		                     "after", "keep", "enabled", "constants", "inputs", "outputs", "content" });

		std::string type;
		if (str(v, "type", path, type, true) && !type.empty() && !parse(type, p.type))
		{
			if (type == "RayTrace" || type == "Subgraph")
				error(path + ".type", type + " のパスは名前と意味だけを決めてあり、まだ使えません");
			else
				error(path + ".type", "'" + type + "' は Raster / Compute / Copy のどれかです");
		}
		str(v, "pipeline", path, p.pipeline, false);
		str(v, "when", path, p.when, false);
		str(v, "view", path, p.view, false);
		region(v, "viewport", path, p.viewport);
		region(v, "scissor", path, p.scissor);
		integerKey(v, "order", path, -2147483648.0, 2147483647.0, p.order);
		stringArray(v, "after", path, p.after);
		boolean(v, "keep", path, p.keep);
		boolean(v, "enabled", path, p.enabled);

		if (const GfxJson* rep = v.find("repeat"))
		{
			const std::string rp = path + ".repeat";
			if (!rep->isObject()) error(rp, "{ \"count\": 回数かパラメーター, \"var\": 変数, \"start\": 最初の値 } が必要です");
			else
			{
				checkKeys(*rep, rp, { "count", "var", "start" });
				p.repeat.set = true;
				const GfxJson* count = rep->find("count");
				if (!count) error(rp + ".count", "必要な項目がありません");
				else if (count->isString()) p.repeat.countParam = count->asString();
				else
				{
					double d = 0;
					if (integer(*count, rp + ".count", 0, 4096, d))
						p.repeat.count = static_cast<uint32_t>(d);
				}
				str(*rep, "var", rp, p.repeat.var, false);
				integerKey(*rep, "start", rp, -65536, 65536, p.repeat.start);
			}
		}

		if (const GfxJson* arr = v.find("constants"))
		{
			if (!arr->isArray()) error(path + ".constants", "配列が必要です");
			else
				for (size_t i = 0; i < arr->size(); ++i)
				{
					const std::string cp = path + ".constants[" + std::to_string(i) + "]";
					const GfxJson&    c  = arr->at(i);
					ConstantRef ref;
					if (!c.isObject()) { error(cp, "オブジェクトが必要です"); continue; }
					checkKeys(c, cp, { "source", "bind", "values" });
					str(c, "source", cp, ref.source, true);
					binding(c, cp, ref.bind);
					if (const GfxJson* vals = c.find("values"))
					{
						if (!vals->isArray()) error(cp + ".values", "数値の配列が必要です");
						else
							for (size_t k = 0; k < vals->size(); ++k)
							{
								float f = 0;
								if (number(vals->at(k), cp + ".values[" + std::to_string(k) + "]", f))
									ref.values.push_back(f);
							}
					}
					p.constants.push_back(std::move(ref));
				}
		}
		for (const char* key : { "inputs", "outputs" })
		{
			const bool output = std::string(key) == "outputs";
			const GfxJson* arr = v.find(key);
			if (!arr) continue;
			if (!arr->isArray()) { error(path + "." + key, "配列が必要です"); continue; }
			for (size_t i = 0; i < arr->size(); ++i)
			{
				PassResourceRef ref;
				resourceRef(arr->at(i), path + "." + key + "[" + std::to_string(i) + "]", output, ref);
				(output ? p.outputs : p.inputs).push_back(std::move(ref));
			}
		}

		if (const GfxJson* c = v.find("content"))
		{
			const std::string cp = path + ".content";
			if (!c->isObject() || c->size() != 1)
			{
				error(cp, "drawList / fullscreen / dispatch / drawIndirect / dispatchIndirect / custom のどれか 1 つを持つオブジェクトが必要です");
				return;
			}
			const std::string& kind = c->keys()[0];
			const GfxJson&     val  = c->valueAt(0);
			if (kind == "drawList")
			{
				p.content.kind = ContentKind::DrawList;
				if (!val.isObject()) { error(cp + ".drawList", "オブジェクトが必要です"); return; }
				checkKeys(val, cp + ".drawList", { "layer", "tags", "sort" });
				str(val, "layer", cp + ".drawList", p.content.drawList.layer, false);
				stringArray(val, "tags", cp + ".drawList", p.content.drawList.tags);
				str(val, "sort", cp + ".drawList", p.content.drawList.sort, false);
			}
			else if (kind == "fullscreen")
			{
				if (!val.isBool() || !val.asBool()) error(cp + ".fullscreen", "true が必要です");
				p.content.kind = ContentKind::Fullscreen;
			}
			else if (kind == "dispatch")
			{
				p.content.kind = ContentKind::Dispatch;
				if (!val.isArray() || val.size() != 3) { error(cp + ".dispatch", "[x, y, z] が必要です"); return; }
				for (size_t i = 0; i < 3; ++i)
				{
					double d = 0;
					if (integer(val.at(i), cp + ".dispatch[" + std::to_string(i) + "]", 1, 65535, d))
						p.content.dispatch[i] = static_cast<uint32_t>(d);
				}
			}
			else if (kind == "drawIndirect" || kind == "dispatchIndirect")
			{
				const bool draw = kind == "drawIndirect";
				p.content.kind = draw ? ContentKind::DrawIndirect : ContentKind::DispatchIndirect;
				indirect(val, cp + "." + kind, draw, p.content.indirect);
			}
			else if (kind == "custom")
			{
				p.content.kind = ContentKind::Custom;
				if (!val.isString()) error(cp + ".custom", "処理の名前 (文字列) が必要です");
				else p.content.custom = val.asString();
			}
			else
				error(cp + "." + kind, "知らない内容です (drawList / fullscreen / dispatch / drawIndirect / dispatchIndirect / custom)");
		}
	}

private:
	GfxJsonDiagnostics& m_diag;
};
} // namespace

bool graphFromJson(const GfxJson& json, GraphDesc& out, GfxJsonDiagnostics* diag)
{
	GfxJsonDiagnostics local;
	GfxJsonDiagnostics& d = diag ? *diag : local;
	Reader r(d);
	out = GraphDesc{};
	if (!json.isObject())
	{
		r.error("(root)", "グラフの JSON はオブジェクトです");
		return false;
	}
	r.checkKeys(json, "(root)", { "version", "parameters", "resources", "passes" });
	r.integerKey(json, "version", "(root)", 1, 1000000, out.version);
	if (out.version > 1)
		r.warning("version", "新しい形式 (version " + std::to_string(out.version) + ") のグラフです。知らない項目は無視します");

	if (const GfxJson* params = json.find("parameters"))
	{
		if (!params->isObject()) r.error("parameters", "オブジェクト ({ 名前: true / false / 整数 }) が必要です");
		else
			for (size_t i = 0; i < params->keys().size(); ++i)
			{
				Parameter p;
				p.name = params->keys()[i];
				const GfxJson& v = params->valueAt(i);
				const std::string pp = "parameters." + p.name;
				if (v.isBool()) { p.isBool = true; p.value = v.asBool() ? 1 : 0; }
				else
				{
					double num = 0;
					if (!r.integer(v, pp, -2147483648.0, 2147483647.0, num)) continue;
					p.value = static_cast<int64_t>(num);
				}
				out.parameters.push_back(std::move(p));
			}
	}

	if (const GfxJson* res = json.find("resources"))
	{
		if (!res->isObject()) r.error("resources", "オブジェクト ({ 名前: 定義 }) が必要です");
		else
			for (size_t i = 0; i < res->keys().size(); ++i)
			{
				ResourceDesc rd;
				r.resource(res->keys()[i], res->valueAt(i), "resources." + res->keys()[i], rd);
				out.resources.push_back(std::move(rd));
			}
	}
	if (const GfxJson* passes = json.find("passes"))
	{
		if (!passes->isArray()) r.error("passes", "配列が必要です");
		else
			for (size_t i = 0; i < passes->size(); ++i)
			{
				PassDesc pd;
				r.pass(passes->at(i), "passes[" + std::to_string(i) + "]", pd);
				out.passes.push_back(std::move(pd));
			}
	}
	return r.ok();
}

bool graphFromString(std::string_view text, GraphDesc& out, GfxJsonDiagnostics* diag)
{
	GfxJson json;
	std::string err;
	if (!gfxJsonParse(text, json, &err))
	{
		if (diag)
			diag->errors.push_back("JSON: " + err);
		return false;
	}
	return graphFromJson(json, out, diag);
}
} // namespace gfx::rg
