// ============================================================
//  samples/rendergraph/main.cpp  (レンダーグラフで描くディファードのサンプル)
//
//  描画の流れはすべて assets/ の JSON に書いてある:
//    graphs/deferred.json   G-Buffer → ライティング → 合成 (リソースとパス、つなぎ方)
//    pipelines/*.json       パイプライン (シェーダー、バインディングレイアウト、ステート)
//    shaders/*.hlsl         シェーダー (保存すると作り直す)
//  このファイルは、シーン (メッシュとオブジェクトごとの定数) とフレームの定数を用意し、
//  IDrawListProvider としてグラフに渡すだけ。
//
//  自動テスト用の環境変数:
//    GFX_EXAMPLE_FRAMES=N      N フレームで終了する
//    GFX_EXAMPLE_RESIZE_TEST=1 リサイズ / 最小化 / 復帰を繰り返す
//  終了コード: D3D12 のエラー / 警告、グラフやアセットのエラーがあれば 1
// ============================================================
#include "gfx/core/GraphicsEngine.h"
#include "gfx/core/ShaderLibrary.h"
#include "gfx/rendergraph/Executor.h"
#include "gfx/rendergraph/GraphJson.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace rg = gfx::rg;

namespace
{
// ============================================================
//  行列 (column-major。HLSL の mul(M, v) と同じ並び。m[列 * 4 + 行])
// ============================================================
struct Vec3 { float x, y, z; };
Vec3  operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
float dot(Vec3 a, Vec3 b)       { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3  cross(Vec3 a, Vec3 b)     { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
Vec3  normalize(Vec3 v)         { const float l = std::sqrt(dot(v, v)); return { v.x / l, v.y / l, v.z / l }; }

struct Mat4
{
	float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
};

Mat4 operator*(const Mat4& a, const Mat4& b)
{
	Mat4 c;
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
		{
			float s = 0;
			for (int k = 0; k < 4; ++k)
				s += a.m[k * 4 + row] * b.m[col * 4 + k];
			c.m[col * 4 + row] = s;
		}
	return c;
}

// 右手系、深度 0〜1
Mat4 perspective(float fovY, float aspect, float zn, float zf)
{
	const float f = 1.0f / std::tan(fovY * 0.5f);
	Mat4 p;
	p.m[0]  = f / aspect;
	p.m[5]  = f;
	p.m[10] = zf / (zn - zf);
	p.m[11] = -1.0f;
	p.m[14] = zn * zf / (zn - zf);
	p.m[15] = 0.0f;
	return p;
}

Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up)
{
	const Vec3 f = normalize(target - eye);
	const Vec3 s = normalize(cross(f, up));
	const Vec3 u = cross(s, f);
	Mat4 v;
	v.m[0] = s.x;  v.m[4] = s.y;  v.m[8]  = s.z;  v.m[12] = -dot(s, eye);
	v.m[1] = u.x;  v.m[5] = u.y;  v.m[9]  = u.z;  v.m[13] = -dot(u, eye);
	v.m[2] = -f.x; v.m[6] = -f.y; v.m[10] = -f.z; v.m[14] = dot(f, eye);
	return v;
}

Mat4 trs(Vec3 t, float rotY, Vec3 s)
{
	const float c = std::cos(rotY), n = std::sin(rotY);
	Mat4 w;
	w.m[0] = c * s.x;  w.m[1] = 0;   w.m[2]  = -n * s.x;
	w.m[4] = 0;        w.m[5] = s.y; w.m[6]  = 0;
	w.m[8] = n * s.z;  w.m[9] = 0;   w.m[10] = c * s.z;
	w.m[12] = t.x;     w.m[13] = t.y; w.m[14] = t.z;
	return w;
}

// 一般の 4x4 の逆行列 (並びに依らない)
Mat4 inverse(const Mat4& a)
{
	const float* m = a.m;
	Mat4 r;
	float* inv = r.m;
	inv[0]  =  m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4]  = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8]  =  m[4] * m[9]  * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9]  * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1]  = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5]  =  m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9]  = -m[0] * m[9]  * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] =  m[0] * m[9]  * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2]  =  m[1] * m[6]  * m[15] - m[1] * m[7]  * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7]  - m[13] * m[3] * m[6];
	inv[6]  = -m[0] * m[6]  * m[15] + m[0] * m[7]  * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7]  + m[12] * m[3] * m[6];
	inv[10] =  m[0] * m[5]  * m[15] - m[0] * m[7]  * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7]  - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5]  * m[14] + m[0] * m[6]  * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6]  + m[12] * m[2] * m[5];
	inv[3]  = -m[1] * m[6]  * m[11] + m[1] * m[7]  * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9]  * m[2] * m[7]  + m[9]  * m[3] * m[6];
	inv[7]  =  m[0] * m[6]  * m[11] - m[0] * m[7]  * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8]  * m[2] * m[7]  - m[8]  * m[3] * m[6];
	inv[11] = -m[0] * m[5]  * m[11] + m[0] * m[7]  * m[9]  + m[4] * m[1] * m[11] - m[4] * m[3] * m[9]  - m[8]  * m[1] * m[7]  + m[8]  * m[3] * m[5];
	inv[15] =  m[0] * m[5]  * m[10] - m[0] * m[6]  * m[9]  - m[4] * m[1] * m[10] + m[4] * m[2] * m[9]  + m[8]  * m[1] * m[6]  - m[8]  * m[2] * m[5];
	float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	det = det != 0.0f ? 1.0f / det : 0.0f;
	for (float& x : r.m) x *= det;
	return r;
}

// ============================================================
//  定数バッファ (shaders/frame.hlsli と gbuffer.*.hlsl と同じ並び)
// ============================================================
struct PointLightData { float pos[3]; float range; float color[3]; float intensity; };
struct FrameData
{
	Mat4           viewProj;
	Mat4           invViewProj;
	float          cameraPos[4];
	float          sunDir[4];
	float          sunColor[4];
	float          ambient[4];
	uint32_t       lightCount;
	float          time;
	float          exposure;
	float          pad;
	PointLightData lights[8];
};
struct ObjectData
{
	Mat4  world;
	float color[4];
};

// ============================================================
//  シーン (IDrawListProvider)
// ============================================================
struct Vertex { float pos[3]; float nrm[3]; };
struct Rgba   { float r, g, b, a; };

class Scene final : public rg::IDrawListProvider
{
public:
	struct Mesh   { BufferHandle vb, ib; uint32_t indexCount = 0; };
	struct Object { const Mesh* mesh = nullptr; BufferHandle cb; ObjectData data; Vec3 pos; float spin = 0; Vec3 scale{ 1, 1, 1 }; };

	explicit Scene(GraphicsEngine& engine) : m_engine(engine)
	{
		m_cube  = makeCube();
		m_plane = makePlane();
		m_frameCB = engine.createBuffer({ sizeof(FrameData), BufferUsage::Uniform, nullptr, 0 });

		addObject(&m_plane, { 0, 0, 0 }, 0, { 12, 1, 12 }, { 0.55f, 0.55f, 0.58f, 1 });
		const Rgba colors[6] = { { 0.9f, 0.3f, 0.25f, 1 }, { 0.3f, 0.8f, 0.35f, 1 }, { 0.25f, 0.45f, 0.9f, 1 },
		                             { 0.9f, 0.8f, 0.3f, 1 },  { 0.8f, 0.4f, 0.9f, 1 },  { 0.9f, 0.9f, 0.9f, 1 } };
		for (int i = 0; i < 6; ++i)
		{
			const float a = i * 6.2831853f / 6.0f;
			addObject(&m_cube, { 3.0f * std::cos(a), 0.6f + 0.3f * (i % 2), 3.0f * std::sin(a) }, a, { 0.6f, 0.6f + 0.3f * (i % 2), 0.6f }, colors[i]);
		}
		addObject(&m_cube, { 0, 1.0f, 0 }, 0, { 1.0f, 1.0f, 1.0f }, { 0.85f, 0.85f, 0.85f, 1 });
	}

	~Scene() override
	{
		for (auto& o : m_objects) m_engine.destroyBuffer(o.cb);
		for (const Mesh* m : { &m_cube, &m_plane }) { m_engine.destroyBuffer(m->vb); m_engine.destroyBuffer(m->ib); }
		m_engine.destroyBuffer(m_frameCB);
	}

	bool valid() const { return m_frameCB.valid() && m_cube.vb.valid() && m_plane.vb.valid(); }

	void update(float time, uint32_t width, uint32_t height)
	{
		FrameData f{};
		const Vec3 eye{ 7.0f * std::cos(time * 0.15f), 4.0f, 7.0f * std::sin(time * 0.15f) };
		const Mat4 view = lookAt(eye, { 0, 0.6f, 0 }, { 0, 1, 0 });
		const Mat4 proj = perspective(0.9f, static_cast<float>(width) / static_cast<float>(height), 0.1f, 100.0f);
		f.viewProj    = proj * view;
		f.invViewProj = inverse(f.viewProj);
		f.cameraPos[0] = eye.x; f.cameraPos[1] = eye.y; f.cameraPos[2] = eye.z;
		const Vec3 sun = normalize({ -0.4f, -1.0f, -0.3f });
		f.sunDir[0] = sun.x; f.sunDir[1] = sun.y; f.sunDir[2] = sun.z;
		f.sunColor[0] = 0.9f; f.sunColor[1] = 0.85f; f.sunColor[2] = 0.75f;
		f.ambient[0] = 0.12f; f.ambient[1] = 0.13f; f.ambient[2] = 0.16f;
		f.lightCount = 3;
		f.time       = time;
		f.exposure   = 1.6f;
		const float lc[3][3] = { { 1.0f, 0.4f, 0.2f }, { 0.2f, 0.6f, 1.0f }, { 0.4f, 1.0f, 0.4f } };
		for (uint32_t i = 0; i < f.lightCount; ++i)
		{
			const float a = time * (0.6f + 0.2f * i) + i * 2.1f;
			PointLightData& l = f.lights[i];
			l.pos[0] = 2.0f * std::cos(a); l.pos[1] = 1.2f; l.pos[2] = 2.0f * std::sin(a);
			l.range = 5.0f; l.intensity = 2.5f;
			for (int k = 0; k < 3; ++k) l.color[k] = lc[i][k];
		}
		m_engine.updateBuffer(m_frameCB, &f, sizeof(f));

		for (size_t i = 0; i < m_objects.size(); ++i)
		{
			Object& o = m_objects[i];
			const float rot = o.spin + (i == 0 ? 0.0f : time * 0.5f);
			o.data.world = trs(o.pos, rot, o.scale);
			m_engine.updateBuffer(o.cb, &o.data, sizeof(o.data));
		}
	}

	// ---- IDrawListProvider ----
	void draw(const rg::DrawListQuery& query, const rg::DrawContext& ctx) override
	{
		if (query.layer != "Opaque")
			return;
		const int32_t objectParam = ctx.paramOf("object");
		for (const Object& o : m_objects)
		{
			GfxVertexBufferBinding vb;
			vb.buffer = o.mesh->vb;
			ctx.cb->setVertexBuffers(0, 1, &vb);
			GfxIndexBufferBinding ib;
			ib.buffer = o.mesh->ib;
			ib.format = GfxFormat::R16_Uint;
			ctx.cb->setIndexBuffer(ib);
			if (objectParam >= 0)
				ctx.cb->setRootDescriptor(static_cast<uint32_t>(objectParam), o.cb, 0);
			ctx.cb->drawIndexedInstanced(o.mesh->indexCount);
		}
	}

	BufferHandle constants(const std::string& source, const rg::DrawContext&) override
	{
		return source == "frame" ? m_frameCB : BufferHandle{};   // "object" は draw の中で設定する
	}

private:
	void addObject(const Mesh* mesh, Vec3 pos, float spin, Vec3 scale, Rgba color)
	{
		Object o;
		o.mesh  = mesh;
		o.pos   = pos;
		o.spin  = spin;
		o.scale = scale;
		o.data.color[0] = color.r; o.data.color[1] = color.g; o.data.color[2] = color.b; o.data.color[3] = color.a;
		o.cb = m_engine.createBuffer({ sizeof(ObjectData), BufferUsage::Uniform, nullptr, 0 });
		m_objects.push_back(o);
	}

	Mesh makeMesh(const std::vector<Vertex>& v, const std::vector<uint16_t>& idx)
	{
		Mesh m;
		m.vb = m_engine.createBuffer({ v.size() * sizeof(Vertex), BufferUsage::Vertex, v.data(), sizeof(Vertex) });
		m.ib = m_engine.createBuffer({ idx.size() * sizeof(uint16_t), BufferUsage::Index, idx.data(), 2 });
		m.indexCount = static_cast<uint32_t>(idx.size());
		return m;
	}

	Mesh makeCube()
	{
		std::vector<Vertex>   v;
		std::vector<uint16_t> idx;
		const float n[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
		for (const auto& f : n)
		{
			// 面の 2 つの接線
			const Vec3 nn{ f[0], f[1], f[2] };
			const Vec3 t = std::fabs(nn.y) > 0.5f ? Vec3{ 1, 0, 0 } : Vec3{ 0, 1, 0 };
			const Vec3 b = cross(nn, t);
			const uint16_t base = static_cast<uint16_t>(v.size());
			const float sgn[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
			for (const auto& s : sgn)
				v.push_back({ { 0.5f * (nn.x + s[0] * t.x + s[1] * b.x), 0.5f * (nn.y + s[0] * t.y + s[1] * b.y), 0.5f * (nn.z + s[0] * t.z + s[1] * b.z) },
				              { nn.x, nn.y, nn.z } });
			for (int k : { 0, 1, 2, 0, 2, 3 })
				idx.push_back(static_cast<uint16_t>(base + k));
		}
		return makeMesh(v, idx);
	}

	Mesh makePlane()
	{
		const std::vector<Vertex> v = { { { -0.5f, 0, -0.5f }, { 0, 1, 0 } }, { { 0.5f, 0, -0.5f }, { 0, 1, 0 } },
		                                { { 0.5f, 0, 0.5f }, { 0, 1, 0 } },   { { -0.5f, 0, 0.5f }, { 0, 1, 0 } } };
		return makeMesh(v, { 0, 1, 2, 0, 2, 3 });
	}

	GraphicsEngine&     m_engine;
	Mesh                m_cube, m_plane;
	BufferHandle        m_frameCB;
	std::vector<Object> m_objects;
};

// ============================================================
//  ウィンドウ
// ============================================================
struct WindowState
{
	bool     resized = false;
	uint32_t width = 0, height = 0;
};

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	switch (msg)
	{
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_SIZE:
		if (state && wp != SIZE_MINIMIZED && LOWORD(lp) > 0 && HIWORD(lp) > 0)
		{
			// フレームの間で処理する (記録の途中でスワップチェーンを作り直さない)
			state->resized = true;
			state->width   = LOWORD(lp);
			state->height  = HIWORD(lp);
		}
		return 0;
	default:
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
}

HWND createWindow(WindowState& state, uint32_t width, uint32_t height)
{
	WNDCLASSEXW wc   = {};
	wc.cbSize        = sizeof(wc);
	wc.style         = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc   = wndProc;
	wc.hInstance     = GetModuleHandleW(nullptr);
	wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
	wc.lpszClassName = L"GfxGraphSampleClass";
	RegisterClassExW(&wc);
	RECT rc = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"GfxGraphSample (render graph)", WS_OVERLAPPEDWINDOW,
	                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
	                            nullptr, nullptr, wc.hInstance, nullptr);
	if (hwnd)
	{
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
		ShowWindow(hwnd, SW_SHOWDEFAULT);
	}
	return hwnd;
}

bool pumpMessages()
{
	MSG msg = {};
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
	{
		if (msg.message == WM_QUIT)
			return false;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return true;
}

int readEnvInt(const char* name, int def)
{
	char   buf[32] = {};
	size_t len     = 0;
	if (getenv_s(&len, buf, sizeof(buf), name) == 0 && len > 0)
		return std::atoi(buf);
	return def;
}

// アセットのフォルダ: ビルドツリーではソースの assets/ (編集をそのまま反映する)、それ以外は exe の横
std::filesystem::path assetRoot()
{
#ifdef GFX_GRAPH_SAMPLE_ASSET_DIR
	const std::filesystem::path src = std::filesystem::path(std::u8string(
		reinterpret_cast<const char8_t*>(GFX_GRAPH_SAMPLE_ASSET_DIR)));
	if (std::filesystem::exists(src / "graphs"))
		return src;
#endif
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	return std::filesystem::path(exe).parent_path() / "rendergraph_assets";
}

bool loadGraph(const std::filesystem::path& file, rg::GraphDesc& graph)
{
	std::ifstream f(file, std::ios::binary);
	if (!f)
	{
		std::cerr << "[Graph] グラフを読めません: " << file.string() << "\n";
		return false;
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	GfxJsonDiagnostics diag;
	const bool ok = rg::graphFromString(ss.str(), graph, &diag);
	for (const auto& e : diag.errors)   std::cerr << "[Graph] error: " << e << "\n";
	for (const auto& w : diag.warnings) std::cerr << "[Graph] warning: " << w << "\n";
	return ok;
}
} // namespace

int main()
{
	int problems = 0;
	try
	{
		DeviceConfig cfg;
		cfg.appName = "GfxGraphSample";
#ifdef GFX_VALIDATION
		cfg.validation = true;
#endif
		cfg.debugMessage = [&problems](GfxMessageSeverity sev, int32_t id, const char* text) {
			const char* name = sev == GfxMessageSeverity::Corruption ? "CORRUPTION"
			                 : sev == GfxMessageSeverity::Error      ? "ERROR"
			                 : sev == GfxMessageSeverity::Warning    ? "WARNING" : "INFO";
			if (sev <= GfxMessageSeverity::Warning)
			{
				++problems;
				std::fprintf(stderr, "[D3D12 %s #%d] %s\n", name, id, text ? text : "");
			}
		};

		GraphicsEngine engine;
		if (!engine.init(cfg))
			throw std::runtime_error("engine.init() failed: " + engine.lastError());
		std::cout << "Adapter: " << engine.adapterName() << "\n";

		WindowState wstate;
		HWND hwnd = createWindow(wstate, 1280, 720);
		if (!hwnd)
			throw std::runtime_error("CreateWindowExW failed");
		WindowDesc wd;
		wd.nativeWindowHandle = hwnd;
		wd.width  = 1280;
		wd.height = 720;
		wd.name   = "Main";
		RenderWindow* win = engine.createWindow(wd);
		if (!win)
			throw std::runtime_error("createWindow failed: " + engine.lastError());

		// ---- アセットとグラフ ----
		const std::filesystem::path root = assetRoot();
		std::cout << "Assets: " << root.string() << "\n";

		ShaderLibrary shaders(engine);
		shaders.setOnReload([](ShaderHandle, const std::string& path, bool ok) {
			std::cout << (ok ? "[HotReload] reloaded: " : "[HotReload] failed (keeping the previous shader): ") << path << "\n";
		});
		rg::PipelineLibrary pipelines(engine, shaders);
		pipelines.setRoot(root);

		rg::GraphDesc graph;
		if (!loadGraph(root / "graphs" / "deferred.json", graph))
			throw std::runtime_error("グラフを読めません");

		rg::CompileOptions options;
		options.bindingLayout = [&](const std::string& p) { return pipelines.bindingLayout(p); };
		const rg::CompiledGraph compiled = rg::compile(graph, options);
		for (const auto& d : compiled.diagnostics)
			std::cerr << "[Graph] " << (d.severity == rg::Diagnostic::Severity::Error ? "error: " : "warning: ") << d.text() << "\n";
		if (!compiled.ok)
			throw std::runtime_error("グラフのコンパイルに失敗しました");
		std::cout << "[Graph] passes:";
		for (const auto& p : compiled.passes) std::cout << " " << p.name;
		std::cout << "\n";

		Scene scene(engine);
		if (!scene.valid())
			throw std::runtime_error("シーンのバッファを作れません");

		rg::Executor executor(engine, pipelines);

		// ---- フレームループ ----
		const int  maxFrames  = readEnvInt("GFX_EXAMPLE_FRAMES", -1);
		const bool resizeTest = readEnvInt("GFX_EXAMPLE_RESIZE_TEST", 0) != 0;
		const auto start      = std::chrono::steady_clock::now();
		size_t     reportedPipelineErrors = 0, reportedExecutorErrors = 0;

		for (int frame = 0; maxFrames < 0 || frame < maxFrames; ++frame)
		{
			if (resizeTest)
			{
				auto setClientSize = [hwnd](int w, int h) {
					RECT rc = { 0, 0, w, h };
					AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
					SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
				};
				switch (frame % 100)
				{
				case 20: setClientSize(960, 540);       break;
				case 40: ShowWindow(hwnd, SW_MINIMIZE); break;
				case 60: ShowWindow(hwnd, SW_RESTORE);  break;
				case 80: setClientSize(1280, 720);      break;
				default: break;
				}
			}
			if (!pumpMessages())
				break;
			if (wstate.resized)
			{
				wstate.resized = false;
				if (!win->resize(wstate.width, wstate.height))
					throw std::runtime_error("SwapChain resize failed");
				std::cout << "[Engine] Resized to " << wstate.width << "x" << wstate.height << "\n";
			}

			shaders.poll();   // 保存された .hlsl を作り直す

			if (!win->acquireNextBuffer())
				throw std::runtime_error("acquireNextBuffer failed");
			IRenderTarget* back = win->getRenderTarget();
			if (!back)
				throw std::runtime_error("getRenderTarget failed");

			const float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
			scene.update(time, back->getWidth(), back->getHeight());

			rg::FrameInputs in;
			in.width    = back->getWidth();
			in.height   = back->getHeight();
			in.provider = &scene;
			in.imports["$backbuffer"] = rg::ImportedResource::fromRenderTarget(back);

			ICommandBuffer* cb = engine.allocCommandBuffer();
			cb->begin();
			executor.record(cb, graph, compiled, in);
			cb->end();
			engine.submitCommandBuffers({ cb });
			cb->release();
			win->present();

			for (; reportedPipelineErrors < pipelines.errors().size(); ++reportedPipelineErrors)
				std::cerr << "[Pipeline] " << pipelines.errors()[reportedPipelineErrors] << "\n";
			for (; reportedExecutorErrors < executor.errors().size(); ++reportedExecutorErrors)
				std::cerr << "[Executor] " << executor.errors()[reportedExecutorErrors] << "\n";

			if (frame % 120 == 119 && !executor.timings().empty())
			{
				std::printf("[GPU] total %.3f ms |", executor.totalGpuMs());
				for (const auto& t : executor.timings())
					std::printf(" %s %.3f ms", t.name.c_str(), t.gpuMs);
				std::printf(" | pooled textures %zu\n", executor.pooledTextures());
			}
		}

		problems += static_cast<int>(pipelines.errors().size() + executor.errors().size());
		std::cout << "[Exit] D3D12 / graph problems: " << problems << "\n";
		// executor / pipelines / scene / shaders は engine より先に破棄される (宣言の逆順)
		executor.clear();
	}
	catch (const std::exception& e)
	{
		std::cerr << "[Error] " << e.what() << "\n";
		return 1;
	}
	return problems == 0 ? 0 : 1;
}
