// ============================================================
//  engine/rendergraph/src/PipelineLibrary.cpp
// ============================================================
#include "gfx/rendergraph/PipelineLibrary.h"

#include <fstream>
#include <sstream>

namespace gfx::rg
{
// ============================================================
//  JSON との相互変換
// ============================================================
namespace
{
// gfxFromJson の診断に、親のパスを付けて移す
void moveDiagnostics(GfxJsonDiagnostics& from, const std::string& prefix, GfxJsonDiagnostics& to)
{
	for (auto& e : from.errors)   to.errors.push_back(prefix + "." + e);
	for (auto& w : from.warnings) to.warnings.push_back(prefix + "." + w);
}

bool readStage(const GfxJson& v, const std::string& path, ShaderStageAsset& out, GfxJsonDiagnostics& d)
{
	if (!v.isObject()) { d.errors.push_back(path + ": オブジェクト ({ \"file\": ... }) が必要です"); return false; }
	bool ok = true;
	for (size_t i = 0; i < v.keys().size(); ++i)
	{
		const std::string& key = v.keys()[i];
		const GfxJson&     val = v.valueAt(i);
		const std::string  kp  = path + "." + key;
		if (key == "file" || key == "entry" || key == "profile")
		{
			if (!val.isString()) { d.errors.push_back(kp + ": 文字列が必要です"); ok = false; continue; }
			(key == "file" ? out.file : key == "entry" ? out.entry : out.profile) = val.asString();
		}
		else if (key == "defines")
		{
			if (!val.isObject()) { d.errors.push_back(kp + ": オブジェクト ({ 名前: 値 }) が必要です"); ok = false; continue; }
			for (size_t k = 0; k < val.keys().size(); ++k)
			{
				const GfxJson& dv = val.valueAt(k);
				std::string value;
				if (dv.isString())      value = dv.asString();
				else if (dv.isNumber()) { std::ostringstream s; s << dv.asNumber(); value = s.str(); }
				else if (dv.isBool())   value = dv.asBool() ? "1" : "0";
				else { d.errors.push_back(kp + "." + val.keys()[k] + ": 文字列か数値が必要です"); ok = false; continue; }
				out.defines.emplace_back(val.keys()[k], value);
			}
		}
		else
			d.warnings.push_back(kp + ": 知らないキーなので無視します");
	}
	if (out.file.empty()) { d.errors.push_back(path + ".file: シェーダーのファイルがありません"); ok = false; }
	return ok;
}

GfxJson writeStage(const ShaderStageAsset& s)
{
	GfxJson j = GfxJson::object();
	j.set("file", s.file);
	if (s.entry != "main") j.set("entry", s.entry);
	if (!s.profile.empty()) j.set("profile", s.profile);
	if (!s.defines.empty())
	{
		GfxJson d = GfxJson::object();
		for (const auto& [name, value] : s.defines) d.set(name, value);
		j.set("defines", std::move(d));
	}
	return j;
}
} // namespace

bool pipelineAssetFromJson(const GfxJson& json, PipelineAsset& out, GfxJsonDiagnostics* diag)
{
	GfxJsonDiagnostics local;
	GfxJsonDiagnostics& d = diag ? *diag : local;
	const size_t errorsBefore = d.errors.size();
	if (!json.isObject())
	{
		d.errors.push_back("(root): オブジェクトが必要です");
		return false;
	}
	PipelineAsset a;
	for (size_t i = 0; i < json.keys().size(); ++i)
	{
		const std::string& key = json.keys()[i];
		const GfxJson&     v   = json.valueAt(i);
		if (key == "version")
		{
			if (!v.isNumber()) d.errors.push_back("version: 数値が必要です");
			else if (v.asNumber() > 1) d.warnings.push_back("version: 新しい形式のアセットです。知らない項目は無視します");
		}
		else if (key == "type")
		{
			if (v.isString() && v.asString() == "graphics")     a.compute = false;
			else if (v.isString() && v.asString() == "compute") a.compute = true;
			else d.errors.push_back("type: \"graphics\" か \"compute\" です");
		}
		else if (key == "shaders" || key == "layout" || key == "state")
			continue;   // type を読んでから
		else
			d.warnings.push_back(key + ": 知らないキーなので無視します");
	}

	if (const GfxJson* shaders = json.find("shaders"))
	{
		if (!shaders->isObject())
			d.errors.push_back("shaders: オブジェクトが必要です");
		else
			for (size_t i = 0; i < shaders->keys().size(); ++i)
			{
				const std::string& stage = shaders->keys()[i];
				const std::string  sp    = "shaders." + stage;
				ShaderStageAsset* target = nullptr;
				if (!a.compute && stage == "vs") target = &a.vs;
				else if (!a.compute && stage == "ps") target = &a.ps;
				else if (a.compute && stage == "cs") target = &a.cs;
				else
				{
					d.errors.push_back(sp + (a.compute ? ": compute のパイプラインは cs だけです" : ": graphics のパイプラインは vs / ps です"));
					continue;
				}
				readStage(shaders->valueAt(i), sp, *target, d);
			}
	}
	if (a.compute ? !a.cs.used() : !a.vs.used())
		d.errors.push_back(a.compute ? "shaders.cs: Compute シェーダーがありません" : "shaders.vs: 頂点シェーダーがありません");

	if (const GfxJson* layout = json.find("layout"))
	{
		GfxJsonDiagnostics ld;
		gfxFromJson(*layout, a.layout, &ld);
		moveDiagnostics(ld, "layout", d);
	}
	if (const GfxJson* state = json.find("state"))
	{
		if (a.compute)
			d.warnings.push_back("state: compute のパイプラインでは使わないので無視します");
		else
		{
			GfxJsonDiagnostics sd;
			gfxFromJson(*state, a.state, &sd);
			moveDiagnostics(sd, "state", d);
		}
	}
	if (d.errors.size() != errorsBefore)
		return false;
	out = std::move(a);
	return true;
}

GfxJson pipelineAssetToJson(const PipelineAsset& a)
{
	GfxJson j = GfxJson::object();
	j.set("version", 1);
	j.set("type", a.compute ? "compute" : "graphics");
	GfxJson shaders = GfxJson::object();
	if (a.compute) shaders.set("cs", writeStage(a.cs));
	else
	{
		shaders.set("vs", writeStage(a.vs));
		if (a.ps.used()) shaders.set("ps", writeStage(a.ps));
	}
	j.set("shaders", std::move(shaders));
	j.set("layout", gfxToJson(a.layout));
	if (!a.compute)
	{
		// 出力のフォーマットはパスから決めるので書かない
		GfxGraphicsPipelineDesc s = a.state;
		s.numRenderTargets = 0;
		for (auto& f : s.rtvFormats) f = GfxFormat::Unknown;
		s.dsvFormat   = GfxFormat::Unknown;
		s.sampleCount = 1;
		j.set("state", gfxToJson(s));
	}
	return j;
}

// ============================================================
//  PipelineLibrary
// ============================================================
namespace
{
std::filesystem::path pathFromUtf8(const std::string& s)
{
	return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

bool readFile(const std::filesystem::path& p, std::string& out)
{
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}
} // namespace

PipelineLibrary::PipelineLibrary(GraphicsEngine& engine, ShaderLibrary& shaders)
	: m_engine(engine), m_shaders(shaders)
{
}

PipelineLibrary::~PipelineLibrary()
{
	clear();
}

void PipelineLibrary::setRoot(const std::filesystem::path& root)
{
	m_root = root;
	m_shaders.addSearchPath(root);
}

void PipelineLibrary::error(const std::string& text)
{
	m_errors.push_back(text);
}

PipelineLibrary::Entry* PipelineLibrary::entry(const std::string& path)
{
	for (auto& e : m_entries)
		if (e->path == path)
			return e.get();

	auto e = std::make_unique<Entry>();
	e->path      = path;
	e->debugName = path;
	e->loaded    = true;

	std::string text;
	const std::filesystem::path file = m_root / pathFromUtf8(path);
	if (!readFile(file, text))
		error(path + ": パイプラインのアセットを読めません");
	else
	{
		GfxJson json;
		std::string parseError;
		if (!gfxJsonParse(text, json, &parseError))
			error(path + ": JSON: " + parseError);
		else
		{
			GfxJsonDiagnostics diag;
			e->ok = pipelineAssetFromJson(json, e->asset, &diag);
			for (const auto& s : diag.errors)   error(path + ": " + s);
			for (const auto& s : diag.warnings) error(path + ": (警告) " + s);
		}
	}
	m_entries.push_back(std::move(e));
	return m_entries.back().get();
}

const PipelineAsset* PipelineLibrary::asset(const std::string& path)
{
	Entry* e = entry(path);
	return e->ok ? &e->asset : nullptr;
}

const GfxBindingLayoutDesc* PipelineLibrary::bindingLayout(const std::string& path)
{
	const PipelineAsset* a = asset(path);
	return a ? &a->layout : nullptr;
}

BindingLayoutHandle PipelineLibrary::layoutHandle(const std::string& path)
{
	Entry* e = entry(path);
	if (!e->ok || e->layoutFailed)
		return {};
	if (!e->layout.valid())
	{
		GfxBindingLayoutDesc d = e->asset.layout;
		d.debugName = e->debugName.c_str();
		e->layout   = m_engine.createBindingLayout(d);
		if (!e->layout.valid())
		{
			e->layoutFailed = true;
			error(path + ": バインディングレイアウトを作れません (詳しくはデバッグメッセージ)");
		}
	}
	return e->layout;
}

ShaderHandle PipelineLibrary::loadShader(Entry& e, const ShaderStageAsset& s, ShaderStage stage, const char* key)
{
	ShaderLibrary::Options o;
	o.entryPoint = s.entry;
	o.profile    = s.profile;
	o.defines    = s.defines;
	const ShaderHandle h = m_shaders.load(pathFromUtf8(s.file), stage, o);
	if (!h.valid())
		error(e.path + ": shaders." + key + ": " + m_shaders.lastError());
	return h;
}

PipelineHandle PipelineLibrary::graphics(const std::string& path, const PipelineTargets& targets)
{
	Entry* e = entry(path);
	if (!e->ok)
		return {};
	for (const auto& v : e->variants)
		if (!v.computeVariant && v.targets == targets)
			return v.handle;

	Variant v;
	v.targets = targets;
	if (e->asset.compute)
	{
		error(path + ": Compute のパイプラインを Raster のパスで使っています");
		v.failed = true;
	}
	else
	{
		const BindingLayoutHandle layout = layoutHandle(path);
		const ShaderHandle vs = loadShader(*e, e->asset.vs, ShaderStage::Vertex, "vs");
		const ShaderHandle ps = e->asset.ps.used() ? loadShader(*e, e->asset.ps, ShaderStage::Fragment, "ps") : ShaderHandle{};
		if (layout.valid() && vs.valid() && (ps.valid() || !e->asset.ps.used()))
		{
			GfxGraphicsPipelineDesc d = e->asset.state;
			d.bindingLayout    = layout;
			d.vs               = vs;
			d.ps               = ps;
			d.numRenderTargets = targets.numRenderTargets;
			for (uint32_t i = 0; i < GFX_MAX_RENDER_TARGETS; ++i)
				d.rtvFormats[i] = i < targets.numRenderTargets ? targets.rtvFormats[i] : GfxFormat::Unknown;
			d.dsvFormat   = targets.dsvFormat;
			d.sampleCount = targets.sampleCount;
			d.debugName   = e->debugName.c_str();
			v.handle = m_engine.createPipeline(d);
			if (!v.handle.valid())
				error(path + ": パイプラインを作れません (詳しくはデバッグメッセージ)");
		}
		v.failed = !v.handle.valid();
	}
	e->variants.push_back(v);
	return v.handle;
}

PipelineHandle PipelineLibrary::compute(const std::string& path)
{
	Entry* e = entry(path);
	if (!e->ok)
		return {};
	for (const auto& v : e->variants)
		if (v.computeVariant)
			return v.handle;

	Variant v;
	v.computeVariant = true;
	if (!e->asset.compute)
		error(path + ": graphics のパイプラインを Compute のパスで使っています");
	else
	{
		const BindingLayoutHandle layout = layoutHandle(path);
		const ShaderHandle cs = loadShader(*e, e->asset.cs, ShaderStage::Compute, "cs");
		if (layout.valid() && cs.valid())
		{
			GfxComputePipelineDesc d;
			d.bindingLayout = layout;
			d.cs            = cs;
			d.debugName     = e->debugName.c_str();
			v.handle = m_engine.createPipeline(d);
			if (!v.handle.valid())
				error(path + ": パイプラインを作れません (詳しくはデバッグメッセージ)");
		}
	}
	v.failed = !v.handle.valid();
	e->variants.push_back(v);
	return v.handle;
}

void PipelineLibrary::clear()
{
	for (auto& e : m_entries)
	{
		for (auto& v : e->variants)
			if (v.handle.valid())
				m_engine.destroyPipeline(v.handle);
		if (e->layout.valid())
			m_engine.destroyBindingLayout(e->layout);
	}
	m_entries.clear();
}
} // namespace gfx::rg
