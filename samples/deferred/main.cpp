// ============================================================
//  samples/deferred/main.cpp  (ディファードレンダリングのサンプル)
//
//  コマンドバッファの使い方を 3 パターン示す:
//
//  Pattern A — 単一コマンドバッファ (シンプルなフォワードパス)
//    allocCommandBuffer() × 1、記録して submit
//
//  Pattern B — 複数コマンドバッファを独立して記録・一括 submit
//    Shadow パス用 / GBuffer パス用 / Lighting パス用を
//    それぞれ allocCommandBuffer() で確保し、並列に記録してから
//    まとめて submitCommandBuffers() に渡す。
//
//  Pattern C — Bundle (DX12 Bundle / Vulkan Secondary) の再利用
//    静的なジオメトリ描画を Bundle に記録しておき、
//    毎フレーム Direct コマンドバッファから executeBundle() で呼ぶ。
//    記録コストを削減できる。
// ============================================================
#include "SampleUtils.h"
#include "ShaderFiles.h"
#include "Win32Window.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <chrono>
#include <fstream>
#include <timeapi.h>   // timeBeginPeriod / timeEndPeriod
#pragma comment(lib, "winmm.lib")

int main()
{
	// Windows タイマー精度を 1ms に設定してスケジューリングスパイクを軽減する
	timeBeginPeriod(1);
	try
	{
		// ---------------------------------------------------
		//  1. デバイス初期化
		// ---------------------------------------------------
		DeviceConfig devCfg;
		devCfg.api        = GraphicsAPI::Auto;
		// GFX_VALIDATION が定義されているときのみ D3D12 Debug Layer を有効にする
		// CMake の -DGFX_ENABLE_VALIDATION=ON で制御する (デフォルト OFF)
#ifdef GFX_VALIDATION
		devCfg.validation = true;
#else
		devCfg.validation = false;
#endif

		GraphicsEngine engine;
		if (!engine.init(devCfg))
			throw std::runtime_error("[Error] engine.init() failed");

		std::cout << "Adapter: " << engine.adapterName() << "\n";

		// ---------------------------------------------------
		//  2. ウィンドウ作成
		//     nativeWindowHandle に実際の HWND を渡すこと。
		//     nullptr のままではバックバッファが存在しないため
		//     getRenderTarget() が nullptr を返す。
		// ---------------------------------------------------
		// Win32 ネイティブウィンドウを作成して HWND を取得する
#if defined(_WIN32)
		HINSTANCE hInst = GetModuleHandleW(nullptr);
		HWND hwnd = createNativeWindow(hInst, L"GfxExample", 1280, 720);
		void* nativeHandle = hwnd;

		// WndContext を作成してウィンドウに関連付ける
		WndContext wndCtx;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&wndCtx));
#else
		void* nativeHandle = nullptr; // TODO: 他プラットフォームのサーフェスハンドルを設定する
#endif

		WindowDesc winDesc;
		winDesc.nativeWindowHandle = nativeHandle;
		winDesc.width  = 1280;
		winDesc.height = 720;
		winDesc.name   = "Main";

		RenderWindow* win = check(engine.createWindow(winDesc), "createWindow failed");
		wndCtx.win    = win;
		wndCtx.engine = &engine;

		// ---------------------------------------------------
		//  3. RT / DS の作成
		// ---------------------------------------------------
		IDepthStencil* fwdDS = check(
			engine.createDepthStencil({ 1280, 720, PixelFormat::D32_FLOAT, 1, 1.0f, 0, "FwdDS" }),
			"createDepthStencil(fwdDS) failed");
		wndCtx.fwdDS = &fwdDS;  // リサイズ時に再作成するため参照を渡す

		IRenderTarget* gbAlbedo = check(
			engine.createRenderTarget({ 1280, 720, PixelFormat::RGBA8_UNORM,  1, false, true, {0,0,0,1}, "GBuf_Albedo" }),
			"createRenderTarget(gbAlbedo) failed");

		IRenderTarget* gbNormal = check(
			engine.createRenderTarget({ 1280, 720, PixelFormat::RGBA16_FLOAT, 1, false, true, {0,0,0,1}, "GBuf_Normal" }),
			"createRenderTarget(gbNormal) failed");

		IRenderTarget* gbEmissive = check(
			engine.createRenderTarget({ 1280, 720, PixelFormat::RGBA16_FLOAT, 1, false, true, {0,0,0,1}, "GBuf_Emissive" }),
			"createRenderTarget(gbEmissive) failed");

		IDepthStencil* gbDS = check(
			engine.createDepthStencil({ 1280, 720, PixelFormat::D32_FLOAT, 1, 1.0f, 0, "GBuf_DS" }),
			"createDepthStencil(gbDS) failed");

		IDepthStencil* shadowDS = check(
			engine.createDepthStencil({ 1024, 1024, PixelFormat::D32_FLOAT, 1, 1.0f, 0, "ShadowMap" }),
			"createDepthStencil(shadowDS) failed");

		// ---------------------------------------------------
		//  4. GPU リソース
		// ---------------------------------------------------

		// 頂点シェーダー: MVP 行列で変換し UV を PS に渡す
		static const std::string k_VS_src = loadShaderSource("forward.vs.hlsl");
		static const char*       k_VS     = k_VS_src.c_str();

		// ピクセルシェーダー: テクスチャをサンプリングして出力する
		// テクスチャが未バインドのときは白を出力する
		static const std::string k_PS_src = loadShaderSource("forward.ps.hlsl");
		static const char*       k_PS     = k_PS_src.c_str();

		// ---------------------------------------------------
		//  G-Buffer 用 VS (法線付き)
		//  Position(float3) + Normal(float3) + UV(float2) = 32 bytes
		// ---------------------------------------------------
		static const std::string k_GBuf_VS_src = loadShaderSource("gbuffer.vs.hlsl");
		static const char*       k_GBuf_VS     = k_GBuf_VS_src.c_str();

		// ---------------------------------------------------
		//  G-Buffer 用 PS: MRT 3 枚 (Albedo / Normal / Emissive)
		// ---------------------------------------------------
		static const std::string k_GBuf_PS_src = loadShaderSource("gbuffer.ps.hlsl");
		static const char*       k_GBuf_PS     = k_GBuf_PS_src.c_str();

		// ---------------------------------------------------
		//  Deferred Lighting 用 PS
		//  G-Buffer 4枚 (t0=Albedo, t1=Normal, t2=Emissive, t3=Depth) を読み込む
		//  ポイントライト最大16個を b0 の定数バッファで受け取る
		// ---------------------------------------------------
		static const std::string k_Light_VS_src = loadShaderSource("lighting.vs.hlsl");
		static const char*       k_Light_VS     = k_Light_VS_src.c_str();

		// ---------------------------------------------------
		//  Grid Line シェーダー
		// ---------------------------------------------------
		static const std::string k_Grid_VS_src = loadShaderSource("grid.vs.hlsl");
		static const char*       k_Grid_VS     = k_Grid_VS_src.c_str();

		static const std::string k_Grid_PS_src = loadShaderSource("grid.ps.hlsl");
		static const char*       k_Grid_PS     = k_Grid_PS_src.c_str();

		// ---------------------------------------------------
		//  RT Composite シェーダー
		//  UAV テクスチャをバックバッファにフルスクリーン合成する
		// ---------------------------------------------------
		static const std::string k_RTComposite_VS_src = loadShaderSource("rt_composite.vs.hlsl");
		static const char*       k_RTComposite_VS     = k_RTComposite_VS_src.c_str();
		static const std::string k_RTComposite_PS_src = loadShaderSource("rt_composite.ps.hlsl");
		static const char*       k_RTComposite_PS     = k_RTComposite_PS_src.c_str();

		// ---------------------------------------------------
		//  Shadow Pass VS: 光源視点の MVP で頂点変換のみ
		//  PS なし (深度書き込みのみ)
		// ---------------------------------------------------
		static const std::string k_Shadow_VS_src = loadShaderSource("shadow.vs.hlsl");
		static const char*       k_Shadow_VS     = k_Shadow_VS_src.c_str();

		static const std::string k_Light_PS_src = loadShaderSource("lighting.ps.hlsl");
		static const char*       k_Light_PS     = k_Light_PS_src.c_str();

		ShaderDesc vsDesc;
		vsDesc.stage        = ShaderStage::Vertex;
		vsDesc.source       = k_VS;
		// Position(float3) + TexCoord0(float2) = 20 bytes
		GfxVertexLayout layoutPU;
		layoutPU.stride = 20;
		layoutPU.add(VertexAttribute::Position,  PixelFormat::R32_FLOAT,  0);
		layoutPU.add(VertexAttribute::TexCoord0, PixelFormat::R32_FLOAT, 12);
		// vertexLayout は GfxPipelineDesc で指定する

		ShaderDesc psDesc;
		psDesc.stage  = ShaderStage::Fragment;
		psDesc.source = k_PS;

		ShaderHandle vs = engine.compileShader(vsDesc);
		checkHandle(vs, "compileShader(vs) failed");

		ShaderHandle ps = engine.compileShader(psDesc);
		checkHandle(ps, "compileShader(ps) failed");

		// G-Buffer 用 VS/PS (Position + Normal + UV = 32 bytes)
		GfxVertexLayout layoutPNU;
		layoutPNU.stride = 32;
		layoutPNU.add(VertexAttribute::Position,  PixelFormat::R32_FLOAT,  0);
		layoutPNU.add(VertexAttribute::Normal,    PixelFormat::R32_FLOAT, 12);
		layoutPNU.add(VertexAttribute::TexCoord0, PixelFormat::R32_FLOAT, 24);

		ShaderDesc gbVSDesc;
		gbVSDesc.stage        = ShaderStage::Vertex;
		gbVSDesc.source       = k_GBuf_VS;
		// vertexLayout は GfxPipelineDesc で指定する

		ShaderDesc gbPSDesc;
		gbPSDesc.stage  = ShaderStage::Fragment;
		gbPSDesc.source = k_GBuf_PS;

		ShaderHandle gbVS = engine.compileShader(gbVSDesc);
		checkHandle(gbVS, "compileShader(gbVS) failed");
		ShaderHandle gbPS = engine.compileShader(gbPSDesc);
		checkHandle(gbPS, "compileShader(gbPS) failed");

		// Lighting 用 VS/PS (SV_VertexID 使用、頂点バッファ不要。InputLayout なし)
		ShaderDesc ltVSDesc;
		ltVSDesc.stage        = ShaderStage::Vertex;
		ltVSDesc.source       = k_Light_VS;

		ShaderDesc ltPSDesc;
		ltPSDesc.stage  = ShaderStage::Fragment;
		ltPSDesc.source = k_Light_PS;

		ShaderHandle ltVS = engine.compileShader(ltVSDesc);
		checkHandle(ltVS, "compileShader(ltVS) failed");
		ShaderHandle ltPS = engine.compileShader(ltPSDesc);
		checkHandle(ltPS, "compileShader(ltPS) failed");


		// Shadow Pass VS (Position + Normal + UV レイアウト、PS なし)
		ShaderDesc shadowVSDesc;
		shadowVSDesc.stage        = ShaderStage::Vertex;
		shadowVSDesc.source       = k_Shadow_VS;

		ShaderHandle shadowVS = engine.compileShader(shadowVSDesc);
		checkHandle(shadowVS, "compileShader(shadowVS) failed");

		// ---------------------------------------------------
		//  パイプラインを事前作成する
		//  bindShader の代わりに bindPipeline を使うことで
		//  PSO がフレームループ外で確定する
		// ---------------------------------------------------

		// フォワード用パイプライン (Position + UV, 深度あり)
		// ---------------------------------------------------
		//  パイプラインごとにルートシグネチャを明示的に設定する
		//  GfxRootSignatureDesc::add(type, visibility, register, count)
		// ---------------------------------------------------

		// ---------------------------------------------------
		//  ABI v2: バインディングレイアウト (ルートシグネチャ) と
		//  パイプラインステートを D3D12 の構造体と同じ粒度で記述する
		// ---------------------------------------------------

		// テクスチャ 1 枚の構成 (フォワード / G-Buffer): SRV テーブル(t0) + Sampler テーブル(s0) + ルート CBV(b0)
		GfxBindingLayoutDesc texLayoutDesc;
		texLayoutDesc
			.beginTable(GfxShaderVisibility::Pixel)
			.addRange(GfxDescriptorRangeType::SRV, 1, 0, 0, GfxDescriptorRangeFlags::DescriptorsVolatile)
			.beginTable(GfxShaderVisibility::Pixel)
			.addRange(GfxDescriptorRangeType::Sampler, 1, 0)
			.addRootDescriptor(GfxRootParameterType::CBV, 0);
		texLayoutDesc.flags     = GfxRootSignatureFlags::AllowInputAssemblerInputLayout;
		texLayoutDesc.debugName = "TexLayout";
		BindingLayoutHandle texLayout = engine.createBindingLayout(texLayoutDesc);
		checkHandle(texLayout, "createBindingLayout(texLayout) failed");

		// Lighting:
		//   SRV テーブル = レンジ 2 つ (t0-t3 = G-Buffer, t4 = シャドウ)
		//   Sampler テーブル (s0 = linear) + 静的サンプラー (s1 = PCF 用の比較サンプラー)
		//   ルート CBV (b0)
		GfxStaticSamplerDesc shadowStaticSampler;
		shadowStaticSampler.filter           = GfxFilter::ComparisonMinMagMipLinear;
		shadowStaticSampler.addressU         = GfxTextureAddressMode::Clamp;
		shadowStaticSampler.addressV         = GfxTextureAddressMode::Clamp;
		shadowStaticSampler.addressW         = GfxTextureAddressMode::Clamp;
		shadowStaticSampler.maxAnisotropy    = 1;
		shadowStaticSampler.comparisonFunc   = GfxCompareFunc::LessEqual;
		shadowStaticSampler.maxLOD           = 1000.0f;
		shadowStaticSampler.shaderRegister   = 1;   // s1
		shadowStaticSampler.shaderVisibility = GfxShaderVisibility::Pixel;

		GfxBindingLayoutDesc lightLayoutDesc;
		lightLayoutDesc
			.beginTable(GfxShaderVisibility::Pixel)
			.addRange(GfxDescriptorRangeType::SRV, 4, 0, 0, GfxDescriptorRangeFlags::DescriptorsVolatile)  // t0-t3
			.addRange(GfxDescriptorRangeType::SRV, 1, 4, 0, GfxDescriptorRangeFlags::DescriptorsVolatile)  // t4
			.beginTable(GfxShaderVisibility::Pixel)
			.addRange(GfxDescriptorRangeType::Sampler, 1, 0)                                               // s0
			.addRootDescriptor(GfxRootParameterType::CBV, 0)                                               // b0
			.addStaticSampler(shadowStaticSampler);                                                        // s1
		lightLayoutDesc.debugName = "LightingLayout";
		BindingLayoutHandle lightLayout = engine.createBindingLayout(lightLayoutDesc);
		checkHandle(lightLayout, "createBindingLayout(lightLayout) failed");

		// 頂点シェーダーだけが CBV(b0) を読む構成 (シャドウ / グリッド)
		GfxBindingLayoutDesc vsCBLayoutDesc;
		vsCBLayoutDesc.addRootDescriptor(GfxRootParameterType::CBV, 0, 0, GfxShaderVisibility::Vertex);
		vsCBLayoutDesc.flags     = GfxRootSignatureFlags::AllowInputAssemblerInputLayout
		                         | GfxRootSignatureFlags::DenyPixelShaderRootAccess;
		vsCBLayoutDesc.debugName = "VSConstantLayout";
		BindingLayoutHandle vsCBLayout = engine.createBindingLayout(vsCBLayoutDesc);
		checkHandle(vsCBLayout, "createBindingLayout(vsCBLayout) failed");

		// v2 のバインディングレイアウトの他の要素 (ルート定数、ルート SRV / UAV、
		// 種類の混在したテーブル、space の指定) も作成できることを確認する (描画では使わない)
		{
			GfxStaticSamplerDesc pointClamp;
			pointClamp.filter        = GfxFilter::MinMagMipPoint;
			pointClamp.addressU      = GfxTextureAddressMode::Clamp;
			pointClamp.addressV      = GfxTextureAddressMode::Clamp;
			pointClamp.addressW      = GfxTextureAddressMode::Clamp;
			pointClamp.registerSpace = 1;

			GfxBindingLayoutDesc coverage;
			coverage
				.addConstants(0, 4, 1)                                       // b0 space1: 32bit x 4
				.addRootDescriptor(GfxRootParameterType::SRV, 0, 1)          // t0 space1
				.addRootDescriptor(GfxRootParameterType::UAV, 0, 1)          // u0 space1
				.beginTable(GfxShaderVisibility::All)
				.addRange(GfxDescriptorRangeType::CBV, 2, 1, 1)              // b1-b2 space1
				.addRange(GfxDescriptorRangeType::SRV, 4, 1, 1, GfxDescriptorRangeFlags::DataStatic)  // t1-t4 space1
				.addRange(GfxDescriptorRangeType::UAV, 1, 1, 1, GfxDescriptorRangeFlags::DataVolatile, 8)  // u1 space1 (offset 8)
				.addStaticSampler(pointClamp);                               // s0 space1
			coverage.debugName = "CoverageLayout";
			BindingLayoutHandle coverageLayout = engine.createBindingLayout(coverage);
			checkHandle(coverageLayout, "createBindingLayout(coverageLayout) failed");
			engine.destroyBindingLayout(coverageLayout);
		}

		// フォワード用: Position(float3) + TexCoord0(float2), 深度あり
		GfxGraphicsPipelineDesc fwdPipeDesc;
		fwdPipeDesc.bindingLayout = texLayout;
		fwdPipeDesc.vs            = vs;
		fwdPipeDesc.ps            = ps;
		fwdPipeDesc.inputLayout
			.add("POSITION", 0, GfxFormat::R32G32B32_Float, 0, 0)
			.add("TEXCOORD", 0, GfxFormat::R32G32_Float,    0, 12);
		fwdPipeDesc.rasterizerState.cullMode = GfxCullMode::None;
		fwdPipeDesc.numRenderTargets         = 1;
		fwdPipeDesc.rtvFormats[0]            = GfxFormat::B8G8R8A8_Unorm;
		fwdPipeDesc.dsvFormat                = GfxFormat::D32_Float;
		fwdPipeDesc.debugName                = "FwdPipeline";
		PipelineHandle fwdPipe = engine.createPipeline(fwdPipeDesc);
		checkHandle(fwdPipe, "createPipeline(fwdPipe) failed");

		// G-Buffer 用: Position + Normal + TexCoord0, MRT 3 枚 (Albedo / Normal / Emissive)
		GfxGraphicsPipelineDesc gbufPipeDesc;
		gbufPipeDesc.bindingLayout = texLayout;
		gbufPipeDesc.vs            = gbVS;
		gbufPipeDesc.ps            = gbPS;
		gbufPipeDesc.inputLayout
			.add("POSITION", 0, GfxFormat::R32G32B32_Float, 0, 0)
			.add("NORMAL",   0, GfxFormat::R32G32B32_Float, 0, 12)
			.add("TEXCOORD", 0, GfxFormat::R32G32_Float,    0, 24);
		gbufPipeDesc.rasterizerState.cullMode = GfxCullMode::None;
		gbufPipeDesc.numRenderTargets         = 3;
		gbufPipeDesc.rtvFormats[0]            = GfxFormat::R8G8B8A8_Unorm;
		gbufPipeDesc.rtvFormats[1]            = GfxFormat::R16G16B16A16_Float;
		gbufPipeDesc.rtvFormats[2]            = GfxFormat::R16G16B16A16_Float;
		gbufPipeDesc.dsvFormat                = GfxFormat::D32_Float;
		gbufPipeDesc.debugName                = "GBufPipeline";
		PipelineHandle gbufPipe = engine.createPipeline(gbufPipeDesc);
		checkHandle(gbufPipe, "createPipeline(gbufPipe) failed");

		// Lighting 用: フルスクリーン (頂点入力なし), 深度なし
		GfxGraphicsPipelineDesc lightPipeDesc;
		lightPipeDesc.bindingLayout                    = lightLayout;
		lightPipeDesc.vs                               = ltVS;
		lightPipeDesc.ps                               = ltPS;
		lightPipeDesc.rasterizerState.cullMode         = GfxCullMode::None;
		lightPipeDesc.depthStencilState.depthEnable    = false;
		lightPipeDesc.depthStencilState.depthWriteMask = GfxDepthWriteMask::Zero;
		lightPipeDesc.numRenderTargets                 = 1;
		lightPipeDesc.rtvFormats[0]                    = GfxFormat::B8G8R8A8_Unorm;
		lightPipeDesc.debugName                        = "LightingPipeline";
		PipelineHandle lightPipe = engine.createPipeline(lightPipeDesc);
		checkHandle(lightPipe, "createPipeline(lightPipe) failed");

		// Shadow Pass パイプライン (深度のみ、PS なし、RT なし)
		GfxGraphicsPipelineDesc shadowPipeDesc;
		shadowPipeDesc.bindingLayout = vsCBLayout;
		shadowPipeDesc.vs            = shadowVS;
		shadowPipeDesc.inputLayout
			.add("POSITION", 0, GfxFormat::R32G32B32_Float, 0, 0)
			.add("NORMAL",   0, GfxFormat::R32G32B32_Float, 0, 12)
			.add("TEXCOORD", 0, GfxFormat::R32G32_Float,    0, 24);
		shadowPipeDesc.rasterizerState.cullMode = GfxCullMode::None;   // テスト: カリングなし
		shadowPipeDesc.numRenderTargets         = 0;
		shadowPipeDesc.dsvFormat                = GfxFormat::D32_Float;
		shadowPipeDesc.debugName                = "ShadowPipeline";
		PipelineHandle shadowPipe = engine.createPipeline(shadowPipeDesc);
		checkHandle(shadowPipe, "createPipeline(shadowPipe) failed");

		// GFX_EXAMPLE_DUMP_SCHEMA=1: スキーマ経由で記述子の中身を出力する
		{
			char   buf[8] = {};
			size_t len    = 0;
			if (getenv_s(&len, buf, sizeof(buf), "GFX_EXAMPLE_DUMP_SCHEMA") == 0 && len > 0 && buf[0] == '1')
			{
				std::cout << "---- " << gfxStructInfo<GfxGraphicsPipelineDesc>()->name << " (LightingPipeline) ----\n";
				dumpBySchema(gfxStructInfo<GfxGraphicsPipelineDesc>(), &lightPipeDesc, 0);
				std::cout << "---- " << gfxStructInfo<GfxBindingLayoutDesc>()->name << " (LightingLayout) ----\n";
				dumpBySchema(gfxStructInfo<GfxBindingLayoutDesc>(), &lightLayoutDesc, 0);
			}
		}

		// フォワード用: Position(xyz) + UV(uv) — 5 floats = 20 bytes
		float verts[] =
		{
		//   x      y     z      u     v
			-0.5f, -0.5f, 0.0f,  0.0f, 1.0f,
			 0.5f, -0.5f, 0.0f,  1.0f, 1.0f,
			 0.0f,  0.5f, 0.0f,  0.5f, 0.0f,
		};
		uint32_t idx[] = { 0, 1, 2 };

		BufferHandle vb = engine.createBuffer({ sizeof(verts), BufferUsage::Vertex, verts, layoutPU.stride });
		checkHandle(vb, "createBuffer(vb) failed");

		BufferHandle ib = engine.createBuffer({ sizeof(idx), BufferUsage::Index, idx, uint32_t(0) });
		checkHandle(ib, "createBuffer(ib) failed");

		// G-Buffer 用頂点: 床(4頂点) + 円柱(N=16, 側面+上下蓋, UV修正済み)
		// 側面: u = 角度に応じて連続, v = 上0/下1
		float gbVerts[] = {
		//  ---- 床 ----
			-0.8f, -0.5f, -0.8f,  0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
			 0.8f, -0.5f, -0.8f,  0.0f, 1.0f, 0.0f, 1.0f, 0.0f,
			 0.8f, -0.5f,  0.8f,  0.0f, 1.0f, 0.0f, 1.0f, 1.0f,
			-0.8f, -0.5f,  0.8f,  0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
		//  ---- 円柱 ----
			0.150000f, -0.500000f, 0.000000f,  1.000000f, 0.000000f, 0.000000f,  0.000000f, 1.000000f,
			0.150000f, 0.500000f, 0.000000f,  1.000000f, 0.000000f, 0.000000f,  0.000000f, 0.000000f,
			0.138582f, -0.500000f, 0.057403f,  0.923880f, 0.000000f, 0.382683f,  0.062500f, 1.000000f,
			0.138582f, 0.500000f, 0.057403f,  0.923880f, 0.000000f, 0.382683f,  0.062500f, 0.000000f,
			0.138582f, -0.500000f, 0.057403f,  0.923880f, 0.000000f, 0.382683f,  0.062500f, 1.000000f,
			0.138582f, 0.500000f, 0.057403f,  0.923880f, 0.000000f, 0.382683f,  0.062500f, 0.000000f,
			0.106066f, -0.500000f, 0.106066f,  0.707107f, 0.000000f, 0.707107f,  0.125000f, 1.000000f,
			0.106066f, 0.500000f, 0.106066f,  0.707107f, 0.000000f, 0.707107f,  0.125000f, 0.000000f,
			0.106066f, -0.500000f, 0.106066f,  0.707107f, 0.000000f, 0.707107f,  0.125000f, 1.000000f,
			0.106066f, 0.500000f, 0.106066f,  0.707107f, 0.000000f, 0.707107f,  0.125000f, 0.000000f,
			0.057403f, -0.500000f, 0.138582f,  0.382683f, 0.000000f, 0.923880f,  0.187500f, 1.000000f,
			0.057403f, 0.500000f, 0.138582f,  0.382683f, 0.000000f, 0.923880f,  0.187500f, 0.000000f,
			0.057403f, -0.500000f, 0.138582f,  0.382683f, 0.000000f, 0.923880f,  0.187500f, 1.000000f,
			0.057403f, 0.500000f, 0.138582f,  0.382683f, 0.000000f, 0.923880f,  0.187500f, 0.000000f,
			0.000000f, -0.500000f, 0.150000f,  0.000000f, 0.000000f, 1.000000f,  0.250000f, 1.000000f,
			0.000000f, 0.500000f, 0.150000f,  0.000000f, 0.000000f, 1.000000f,  0.250000f, 0.000000f,
			0.000000f, -0.500000f, 0.150000f,  0.000000f, 0.000000f, 1.000000f,  0.250000f, 1.000000f,
			0.000000f, 0.500000f, 0.150000f,  0.000000f, 0.000000f, 1.000000f,  0.250000f, 0.000000f,
			-0.057403f, -0.500000f, 0.138582f,  -0.382683f, 0.000000f, 0.923880f,  0.312500f, 1.000000f,
			-0.057403f, 0.500000f, 0.138582f,  -0.382683f, 0.000000f, 0.923880f,  0.312500f, 0.000000f,
			-0.057403f, -0.500000f, 0.138582f,  -0.382683f, 0.000000f, 0.923880f,  0.312500f, 1.000000f,
			-0.057403f, 0.500000f, 0.138582f,  -0.382683f, 0.000000f, 0.923880f,  0.312500f, 0.000000f,
			-0.106066f, -0.500000f, 0.106066f,  -0.707107f, 0.000000f, 0.707107f,  0.375000f, 1.000000f,
			-0.106066f, 0.500000f, 0.106066f,  -0.707107f, 0.000000f, 0.707107f,  0.375000f, 0.000000f,
			-0.106066f, -0.500000f, 0.106066f,  -0.707107f, 0.000000f, 0.707107f,  0.375000f, 1.000000f,
			-0.106066f, 0.500000f, 0.106066f,  -0.707107f, 0.000000f, 0.707107f,  0.375000f, 0.000000f,
			-0.138582f, -0.500000f, 0.057403f,  -0.923880f, 0.000000f, 0.382683f,  0.437500f, 1.000000f,
			-0.138582f, 0.500000f, 0.057403f,  -0.923880f, 0.000000f, 0.382683f,  0.437500f, 0.000000f,
			-0.138582f, -0.500000f, 0.057403f,  -0.923880f, 0.000000f, 0.382683f,  0.437500f, 1.000000f,
			-0.138582f, 0.500000f, 0.057403f,  -0.923880f, 0.000000f, 0.382683f,  0.437500f, 0.000000f,
			-0.150000f, -0.500000f, 0.000000f,  -1.000000f, 0.000000f, 0.000000f,  0.500000f, 1.000000f,
			-0.150000f, 0.500000f, 0.000000f,  -1.000000f, 0.000000f, 0.000000f,  0.500000f, 0.000000f,
			-0.150000f, -0.500000f, 0.000000f,  -1.000000f, 0.000000f, 0.000000f,  0.500000f, 1.000000f,
			-0.150000f, 0.500000f, 0.000000f,  -1.000000f, 0.000000f, 0.000000f,  0.500000f, 0.000000f,
			-0.138582f, -0.500000f, -0.057403f,  -0.923880f, 0.000000f, -0.382683f,  0.562500f, 1.000000f,
			-0.138582f, 0.500000f, -0.057403f,  -0.923880f, 0.000000f, -0.382683f,  0.562500f, 0.000000f,
			-0.138582f, -0.500000f, -0.057403f,  -0.923880f, 0.000000f, -0.382683f,  0.562500f, 1.000000f,
			-0.138582f, 0.500000f, -0.057403f,  -0.923880f, 0.000000f, -0.382683f,  0.562500f, 0.000000f,
			-0.106066f, -0.500000f, -0.106066f,  -0.707107f, 0.000000f, -0.707107f,  0.625000f, 1.000000f,
			-0.106066f, 0.500000f, -0.106066f,  -0.707107f, 0.000000f, -0.707107f,  0.625000f, 0.000000f,
			-0.106066f, -0.500000f, -0.106066f,  -0.707107f, 0.000000f, -0.707107f,  0.625000f, 1.000000f,
			-0.106066f, 0.500000f, -0.106066f,  -0.707107f, 0.000000f, -0.707107f,  0.625000f, 0.000000f,
			-0.057403f, -0.500000f, -0.138582f,  -0.382683f, 0.000000f, -0.923880f,  0.687500f, 1.000000f,
			-0.057403f, 0.500000f, -0.138582f,  -0.382683f, 0.000000f, -0.923880f,  0.687500f, 0.000000f,
			-0.057403f, -0.500000f, -0.138582f,  -0.382683f, 0.000000f, -0.923880f,  0.687500f, 1.000000f,
			-0.057403f, 0.500000f, -0.138582f,  -0.382683f, 0.000000f, -0.923880f,  0.687500f, 0.000000f,
			-0.000000f, -0.500000f, -0.150000f,  -0.000000f, 0.000000f, -1.000000f,  0.750000f, 1.000000f,
			-0.000000f, 0.500000f, -0.150000f,  -0.000000f, 0.000000f, -1.000000f,  0.750000f, 0.000000f,
			-0.000000f, -0.500000f, -0.150000f,  -0.000000f, 0.000000f, -1.000000f,  0.750000f, 1.000000f,
			-0.000000f, 0.500000f, -0.150000f,  -0.000000f, 0.000000f, -1.000000f,  0.750000f, 0.000000f,
			0.057403f, -0.500000f, -0.138582f,  0.382683f, 0.000000f, -0.923880f,  0.812500f, 1.000000f,
			0.057403f, 0.500000f, -0.138582f,  0.382683f, 0.000000f, -0.923880f,  0.812500f, 0.000000f,
			0.057403f, -0.500000f, -0.138582f,  0.382683f, 0.000000f, -0.923880f,  0.812500f, 1.000000f,
			0.057403f, 0.500000f, -0.138582f,  0.382683f, 0.000000f, -0.923880f,  0.812500f, 0.000000f,
			0.106066f, -0.500000f, -0.106066f,  0.707107f, 0.000000f, -0.707107f,  0.875000f, 1.000000f,
			0.106066f, 0.500000f, -0.106066f,  0.707107f, 0.000000f, -0.707107f,  0.875000f, 0.000000f,
			0.106066f, -0.500000f, -0.106066f,  0.707107f, 0.000000f, -0.707107f,  0.875000f, 1.000000f,
			0.106066f, 0.500000f, -0.106066f,  0.707107f, 0.000000f, -0.707107f,  0.875000f, 0.000000f,
			0.138582f, -0.500000f, -0.057403f,  0.923880f, 0.000000f, -0.382683f,  0.937500f, 1.000000f,
			0.138582f, 0.500000f, -0.057403f,  0.923880f, 0.000000f, -0.382683f,  0.937500f, 0.000000f,
			0.138582f, -0.500000f, -0.057403f,  0.923880f, 0.000000f, -0.382683f,  0.937500f, 1.000000f,
			0.138582f, 0.500000f, -0.057403f,  0.923880f, 0.000000f, -0.382683f,  0.937500f, 0.000000f,
			0.150000f, -0.500000f, -0.000000f,  1.000000f, 0.000000f, -0.000000f,  1.000000f, 1.000000f,
			0.150000f, 0.500000f, -0.000000f,  1.000000f, 0.000000f, -0.000000f,  1.000000f, 0.000000f,
			0.000000f, 0.500000f, 0.000000f,  0.000000f, 1.000000f, 0.000000f,  0.500000f, 0.500000f,
			0.150000f, 0.500000f, 0.000000f,  0.000000f, 1.000000f, 0.000000f,  1.000000f, 0.500000f,
			0.138582f, 0.500000f, 0.057403f,  0.000000f, 1.000000f, 0.000000f,  0.961940f, 0.691342f,
			0.106066f, 0.500000f, 0.106066f,  0.000000f, 1.000000f, 0.000000f,  0.853553f, 0.853553f,
			0.057403f, 0.500000f, 0.138582f,  0.000000f, 1.000000f, 0.000000f,  0.691342f, 0.961940f,
			0.000000f, 0.500000f, 0.150000f,  0.000000f, 1.000000f, 0.000000f,  0.500000f, 1.000000f,
			-0.057403f, 0.500000f, 0.138582f,  0.000000f, 1.000000f, 0.000000f,  0.308658f, 0.961940f,
			-0.106066f, 0.500000f, 0.106066f,  0.000000f, 1.000000f, 0.000000f,  0.146447f, 0.853553f,
			-0.138582f, 0.500000f, 0.057403f,  0.000000f, 1.000000f, 0.000000f,  0.038060f, 0.691342f,
			-0.150000f, 0.500000f, 0.000000f,  0.000000f, 1.000000f, 0.000000f,  0.000000f, 0.500000f,
			-0.138582f, 0.500000f, -0.057403f,  0.000000f, 1.000000f, 0.000000f,  0.038060f, 0.308658f,
			-0.106066f, 0.500000f, -0.106066f,  0.000000f, 1.000000f, 0.000000f,  0.146447f, 0.146447f,
			-0.057403f, 0.500000f, -0.138582f,  0.000000f, 1.000000f, 0.000000f,  0.308658f, 0.038060f,
			-0.000000f, 0.500000f, -0.150000f,  0.000000f, 1.000000f, 0.000000f,  0.500000f, 0.000000f,
			0.057403f, 0.500000f, -0.138582f,  0.000000f, 1.000000f, 0.000000f,  0.691342f, 0.038060f,
			0.106066f, 0.500000f, -0.106066f,  0.000000f, 1.000000f, 0.000000f,  0.853553f, 0.146447f,
			0.138582f, 0.500000f, -0.057403f,  0.000000f, 1.000000f, 0.000000f,  0.961940f, 0.308658f,
			0.000000f, -0.500000f, 0.000000f,  0.000000f, -1.000000f, 0.000000f,  0.500000f, 0.500000f,
			0.150000f, -0.500000f, 0.000000f,  0.000000f, -1.000000f, 0.000000f,  1.000000f, 0.500000f,
			0.138582f, -0.500000f, 0.057403f,  0.000000f, -1.000000f, 0.000000f,  0.961940f, 0.691342f,
			0.106066f, -0.500000f, 0.106066f,  0.000000f, -1.000000f, 0.000000f,  0.853553f, 0.853553f,
			0.057403f, -0.500000f, 0.138582f,  0.000000f, -1.000000f, 0.000000f,  0.691342f, 0.961940f,
			0.000000f, -0.500000f, 0.150000f,  0.000000f, -1.000000f, 0.000000f,  0.500000f, 1.000000f,
			-0.057403f, -0.500000f, 0.138582f,  0.000000f, -1.000000f, 0.000000f,  0.308658f, 0.961940f,
			-0.106066f, -0.500000f, 0.106066f,  0.000000f, -1.000000f, 0.000000f,  0.146447f, 0.853553f,
			-0.138582f, -0.500000f, 0.057403f,  0.000000f, -1.000000f, 0.000000f,  0.038060f, 0.691342f,
			-0.150000f, -0.500000f, 0.000000f,  0.000000f, -1.000000f, 0.000000f,  0.000000f, 0.500000f,
			-0.138582f, -0.500000f, -0.057403f,  0.000000f, -1.000000f, 0.000000f,  0.038060f, 0.308658f,
			-0.106066f, -0.500000f, -0.106066f,  0.000000f, -1.000000f, 0.000000f,  0.146447f, 0.146447f,
			-0.057403f, -0.500000f, -0.138582f,  0.000000f, -1.000000f, 0.000000f,  0.308658f, 0.038060f,
			-0.000000f, -0.500000f, -0.150000f,  0.000000f, -1.000000f, 0.000000f,  0.500000f, 0.000000f,
			0.057403f, -0.500000f, -0.138582f,  0.000000f, -1.000000f, 0.000000f,  0.691342f, 0.038060f,
			0.106066f, -0.500000f, -0.106066f,  0.000000f, -1.000000f, 0.000000f,  0.853553f, 0.146447f,
			0.138582f, -0.500000f, -0.057403f,  0.000000f, -1.000000f, 0.000000f,  0.961940f, 0.308658f,
		};
		uint32_t gbIdx[] = {
			0,1,2, 0,2,3,  // 床
			4, 5, 7, 4, 7, 6,
			8, 9, 11, 8, 11, 10,
			12, 13, 15, 12, 15, 14,
			16, 17, 19, 16, 19, 18,
			20, 21, 23, 20, 23, 22,
			24, 25, 27, 24, 27, 26,
			28, 29, 31, 28, 31, 30,
			32, 33, 35, 32, 35, 34,
			36, 37, 39, 36, 39, 38,
			40, 41, 43, 40, 43, 42,
			44, 45, 47, 44, 47, 46,
			48, 49, 51, 48, 51, 50,
			52, 53, 55, 52, 55, 54,
			56, 57, 59, 56, 59, 58,
			60, 61, 63, 60, 63, 62,
			64, 65, 67, 64, 67, 66,
			68, 70, 69, 68, 71, 70,
			68, 72, 71, 68, 73, 72,
			68, 74, 73, 68, 75, 74,
			68, 76, 75, 68, 77, 76,
			68, 78, 77, 68, 79, 78,
			68, 80, 79, 68, 81, 80,
			68, 82, 81, 68, 83, 82,
			68, 84, 83, 68, 69, 84,
			85, 86, 87, 85, 87, 88,
			85, 88, 89, 85, 89, 90,
			85, 90, 91, 85, 91, 92,
			85, 92, 93, 85, 93, 94,
			85, 94, 95, 85, 95, 96,
			85, 96, 97, 85, 97, 98,
			85, 98, 99, 85, 99, 100,
			85, 100, 101, 85, 101, 86,
		};

		BufferHandle gbVB = engine.createBuffer({ sizeof(gbVerts), BufferUsage::Vertex, gbVerts, layoutPNU.stride });
		checkHandle(gbVB, "createBuffer(gbVB) failed");
		BufferHandle gbIB = engine.createBuffer({ sizeof(gbIdx),   BufferUsage::Index,  gbIdx,   0 });
		checkHandle(gbIB, "createBuffer(gbIB) failed");

		// BLAS 構築 (シーン全体: 床 + 円柱)
		GfxBLASDesc blasDesc;
		blasDesc.vertexBuffer   = gbVB;
		blasDesc.vertexCount    = 102;  // 床4 + 円柱98
		blasDesc.vertexStride   = 32;   // layoutPNU stride
		blasDesc.positionOffset = 0;    // Position はオフセット 0
		blasDesc.indexBuffer    = gbIB;
		blasDesc.indexCount     = 198;  // 床6 + 円柱192
		blasDesc.debugName      = "SceneBLAS";
		BLASHandle sceneBLAS = engine.createBLAS(blasDesc);
		if (sceneBLAS.valid())
			std::cout << "[RT] BLAS created successfully\n";
		else
			std::cout << "[RT] BLAS creation failed\n";

		// TLAS 構築 (インスタンス1個: シーン全体)
		TLASHandle sceneTLAS;
		if (sceneBLAS.valid())
		{
			GfxTLASInstance inst;
			inst.blas        = sceneBLAS;
			inst.instanceID  = 0;
			// transform: 単位行列 (3x4 行優先)
			float identity[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 };
			memcpy(inst.transform, identity, sizeof(identity));
			sceneTLAS = engine.createTLAS(&inst, 1);
			if (sceneTLAS.valid())
				std::cout << "[RT] TLAS created successfully\n";
			else
				std::cout << "[RT] TLAS creation failed\n";
		}

		// UAV テクスチャ作成 (レイトレ出力バッファ: 1280x720 RGBA)
		GfxTextureDesc uavDesc;
		uavDesc.width    = 1280;
		uavDesc.height   = 720;
		uavDesc.format   = PixelFormat::RGBA8_UNORM;
		uavDesc.mips     = 1;
		uavDesc.allowUAV = true;
		TextureHandle rtOutput = engine.createTexture(uavDesc);
		if (rtOutput.valid())
			std::cout << "[RT] UAV texture created successfully\n";
		else
			std::cout << "[RT] UAV texture creation failed\n";

		// Step 5: RT シェーダーのコンパイルと RTPSO 作成
		static const std::string k_RayGen_src = loadShaderSource("rt_shadow.raygen.hlsl");
		static const char*       k_RayGen     = k_RayGen_src.c_str();
		static const std::string k_Miss_src = loadShaderSource("rt_shadow.miss.hlsl");
		static const char*       k_Miss     = k_Miss_src.c_str();
		static const std::string k_ClosestHit_src = loadShaderSource("rt_shadow.closesthit.hlsl");
		static const char*       k_ClosestHit     = k_ClosestHit_src.c_str();

		// RTShadowCB: InvViewProj(64) + lightPos(12) + lightRange(4) = 80 bytes
		BufferHandle rtShadowCB = engine.createBuffer({ 80, BufferUsage::Uniform, nullptr, 0 });
		checkHandle(rtShadowCB, "createBuffer(rtShadowCB) failed");

		RTPipelineHandle scenePipe;
		if (sceneTLAS.valid() && rtOutput.valid())
		{
			GfxRTPipelineDesc rtDesc;
			rtDesc.raygenSource     = k_RayGen;
			rtDesc.missSource       = k_Miss;
			rtDesc.closestHitSource = k_ClosestHit;
			rtDesc.maxPayloadSize   = sizeof(float) * 3;
			rtDesc.maxAttributeSize = sizeof(float) * 2;
			rtDesc.debugName        = "SceneRTPipeline";
			scenePipe = engine.createRTPipeline(rtDesc);
			if (scenePipe.valid())
				std::cout << "[RT] RTPipeline created successfully\n";
			else
				std::cout << "[RT] RTPipeline creation failed (DXC required)\n";
		}

		DrawCmd drawCmd;
		drawCmd.vertexBuffer = vb;




		// G-Buffer 用 DrawCmd
		DrawCmd gbDrawCmd;
		gbDrawCmd.vertexBuffer = gbVB;


		// ---------------------------------------------------
		//  Grid Line セットアップ
		// ---------------------------------------------------
		// グリッド頂点 (Position のみ、XZ 平面 Y=-0.5)
		float gridVertsData[] = { -5.00f, 0.00f, -5.00f, -5.00f, 0.00f, 5.00f, -4.50f, 0.00f, -5.00f, -4.50f, 0.00f, 5.00f, -4.00f, 0.00f, -5.00f, -4.00f, 0.00f, 5.00f, -3.50f, 0.00f, -5.00f, -3.50f, 0.00f, 5.00f, -3.00f, 0.00f, -5.00f, -3.00f, 0.00f, 5.00f, -2.50f, 0.00f, -5.00f, -2.50f, 0.00f, 5.00f, -2.00f, 0.00f, -5.00f, -2.00f, 0.00f, 5.00f, -1.50f, 0.00f, -5.00f, -1.50f, 0.00f, 5.00f, -1.00f, 0.00f, -5.00f, -1.00f, 0.00f, 5.00f, -0.50f, 0.00f, -5.00f, -0.50f, 0.00f, 5.00f, 0.00f, 0.00f, -5.00f, 0.00f, 0.00f, 5.00f, 0.50f, 0.00f, -5.00f, 0.50f, 0.00f, 5.00f, 1.00f, 0.00f, -5.00f, 1.00f, 0.00f, 5.00f, 1.50f, 0.00f, -5.00f, 1.50f, 0.00f, 5.00f, 2.00f, 0.00f, -5.00f, 2.00f, 0.00f, 5.00f, 2.50f, 0.00f, -5.00f, 2.50f, 0.00f, 5.00f, 3.00f, 0.00f, -5.00f, 3.00f, 0.00f, 5.00f, 3.50f, 0.00f, -5.00f, 3.50f, 0.00f, 5.00f, 4.00f, 0.00f, -5.00f, 4.00f, 0.00f, 5.00f, 4.50f, 0.00f, -5.00f, 4.50f, 0.00f, 5.00f, 5.00f, 0.00f, -5.00f, 5.00f, 0.00f, 5.00f, -5.00f, 0.00f, -5.00f, 5.00f, 0.00f, -5.00f, -5.00f, 0.00f, -4.50f, 5.00f, 0.00f, -4.50f, -5.00f, 0.00f, -4.00f, 5.00f, 0.00f, -4.00f, -5.00f, 0.00f, -3.50f, 5.00f, 0.00f, -3.50f, -5.00f, 0.00f, -3.00f, 5.00f, 0.00f, -3.00f, -5.00f, 0.00f, -2.50f, 5.00f, 0.00f, -2.50f, -5.00f, 0.00f, -2.00f, 5.00f, 0.00f, -2.00f, -5.00f, 0.00f, -1.50f, 5.00f, 0.00f, -1.50f, -5.00f, 0.00f, -1.00f, 5.00f, 0.00f, -1.00f, -5.00f, 0.00f, -0.50f, 5.00f, 0.00f, -0.50f, -5.00f, 0.00f, 0.00f, 5.00f, 0.00f, 0.00f, -5.00f, 0.00f, 0.50f, 5.00f, 0.00f, 0.50f, -5.00f, 0.00f, 1.00f, 5.00f, 0.00f, 1.00f, -5.00f, 0.00f, 1.50f, 5.00f, 0.00f, 1.50f, -5.00f, 0.00f, 2.00f, 5.00f, 0.00f, 2.00f, -5.00f, 0.00f, 2.50f, 5.00f, 0.00f, 2.50f, -5.00f, 0.00f, 3.00f, 5.00f, 0.00f, 3.00f, -5.00f, 0.00f, 3.50f, 5.00f, 0.00f, 3.50f, -5.00f, 0.00f, 4.00f, 5.00f, 0.00f, 4.00f, -5.00f, 0.00f, 4.50f, 5.00f, 0.00f, 4.50f, -5.00f, 0.00f, 5.00f, 5.00f, 0.00f, 5.00f };

		GfxVertexLayout layoutP;
		layoutP.stride = sizeof(float) * 3;
		layoutP.add(VertexAttribute::Position, PixelFormat::R32_FLOAT, 0);

		BufferHandle gridVB = engine.createBuffer({ sizeof(gridVertsData), BufferUsage::Vertex, gridVertsData, layoutP.stride });
		checkHandle(gridVB, "createBuffer(gridVB) failed");

		// Grid VP 用 CBV (float4x4 = 64 bytes)
		BufferHandle gridVPBuf = engine.createBuffer({ 64, BufferUsage::Uniform, nullptr, 0 });
		checkHandle(gridVPBuf, "createBuffer(gridVPBuf) failed");

		// Grid シェーダーをコンパイル
		ShaderDesc gridVSDesc; gridVSDesc.stage = ShaderStage::Vertex;   gridVSDesc.source = k_Grid_VS;
		ShaderDesc gridPSDesc; gridPSDesc.stage = ShaderStage::Fragment; gridPSDesc.source = k_Grid_PS;
		ShaderHandle gridVS = engine.compileShader(gridVSDesc); checkHandle(gridVS, "compileShader(gridVS) failed");
		ShaderHandle gridPS = engine.compileShader(gridPSDesc); checkHandle(gridPS, "compileShader(gridPS) failed");

		// Grid パイプライン (LineList、バックバッファに直接描画、深度テストあり・書き込みなし)
		GfxGraphicsPipelineDesc gridPipeDesc;
		gridPipeDesc.bindingLayout = vsCBLayout;
		gridPipeDesc.vs            = gridVS;
		gridPipeDesc.ps            = gridPS;
		gridPipeDesc.inputLayout.add("POSITION", 0, GfxFormat::R32G32B32_Float, 0, 0);
		gridPipeDesc.primitiveTopologyType            = GfxPrimitiveTopologyType::Line;
		gridPipeDesc.primitiveTopology                = GfxPrimitiveTopology::LineList;
		gridPipeDesc.rasterizerState.cullMode          = GfxCullMode::None;
		gridPipeDesc.rasterizerState.multisampleEnable = true;   // MSAA の RT に描く (線もマルチサンプルで描く)
		gridPipeDesc.depthStencilState.depthWriteMask  = GfxDepthWriteMask::Zero;
		gridPipeDesc.numRenderTargets                  = 1;
		gridPipeDesc.rtvFormats[0]                     = GfxFormat::R8G8B8A8_Unorm;
		gridPipeDesc.dsvFormat                         = GfxFormat::D32_Float;
		gridPipeDesc.sampleCount                       = 4;
		gridPipeDesc.debugName                         = "GridPipeline";
		PipelineHandle gridPipe = engine.createPipeline(gridPipeDesc);
		checkHandle(gridPipe, "createPipeline(gridPipe) failed");


		// ---------------------------------------------------
		//  RT Composite パイプライン
		//  UAV テクスチャをバックバッファにフルスクリーン描画する
		// ---------------------------------------------------
		ShaderDesc rtVSDesc; rtVSDesc.stage = ShaderStage::Vertex;   rtVSDesc.source = k_RTComposite_VS;
		ShaderDesc rtPSDesc; rtPSDesc.stage = ShaderStage::Fragment; rtPSDesc.source = k_RTComposite_PS;
		ShaderHandle rtCompVS = engine.compileShader(rtVSDesc); checkHandle(rtCompVS, "compileShader(rtCompVS) failed");
		ShaderHandle rtCompPS = engine.compileShader(rtPSDesc); checkHandle(rtCompPS, "compileShader(rtCompPS) failed");

		// v1 の GfxPipelineDesc のまま (DLL 内で v2 に変換される互換レイヤーの確認用)
		GfxPipelineDesc rtCompPipeDesc;
		rtCompPipeDesc.vs             = rtCompVS;
		rtCompPipeDesc.ps             = rtCompPS;
		rtCompPipeDesc.rtvCount       = 1;
		rtCompPipeDesc.rtvFormats[0]  = PixelFormat::BGRA8_UNORM;
		rtCompPipeDesc.depthEnable    = false;
		rtCompPipeDesc.depthWrite     = false;
		rtCompPipeDesc.cullMode       = CullMode::None;
		rtCompPipeDesc.rootSignature.add(RootParamType::SRVTable,     ShaderVisibility::Pixel, 0);
		rtCompPipeDesc.rootSignature.add(RootParamType::SamplerTable, ShaderVisibility::Pixel, 0);
		rtCompPipeDesc.debugName      = "RTCompositePipeline";
		PipelineHandle rtCompPipe = engine.createPipeline(rtCompPipeDesc);
		checkHandle(rtCompPipe, "createPipeline(rtCompPipe) failed");

		// ---------------------------------------------------
		//  Grid の MSAA パス
		//    1. シーンの深度だけを 4x MSAA の DS に描く (グリッドの遮蔽用)
		//    2. グリッドを 4x MSAA の RT (透明でクリア) に描く
		//    3. resolve で 1x のテクスチャにまとめる (色はカバレッジで重み付けされる)
		//    4. バックバッファに乗算済みアルファで合成する
		// ---------------------------------------------------
		constexpr uint32_t GRID_MSAA = 4;

		GfxGraphicsPipelineDesc gridDepthDesc;
		gridDepthDesc.bindingLayout = texLayout;
		gridDepthDesc.vs            = gbVS;   // PS なし (深度のみ)
		gridDepthDesc.inputLayout
			.add("POSITION", 0, GfxFormat::R32G32B32_Float, 0, 0)
			.add("NORMAL",   0, GfxFormat::R32G32B32_Float, 0, 12)
			.add("TEXCOORD", 0, GfxFormat::R32G32_Float,    0, 24);
		gridDepthDesc.rasterizerState.cullMode          = GfxCullMode::None;
		gridDepthDesc.rasterizerState.multisampleEnable = true;
		gridDepthDesc.blendState.renderTarget[0].renderTargetWriteMask = GfxColorWriteMask::None;
		gridDepthDesc.numRenderTargets = 1;
		gridDepthDesc.rtvFormats[0]    = GfxFormat::R8G8B8A8_Unorm;
		gridDepthDesc.dsvFormat        = GfxFormat::D32_Float;
		gridDepthDesc.sampleCount      = GRID_MSAA;
		gridDepthDesc.debugName        = "GridDepthPrepass";
		PipelineHandle gridDepthPipe = engine.createPipeline(gridDepthDesc);
		checkHandle(gridDepthPipe, "createPipeline(gridDepthPipe) failed");

		// 合成: 解決済みテクスチャ (t0) を Sampler テーブル (s0) で読み、乗算済みアルファで重ねる
		static const std::string k_GridComposite_PS_src = loadShaderSource("grid_composite.ps.hlsl");
		static const char*       k_GridComposite_PS     = k_GridComposite_PS_src.c_str();
		ShaderDesc gcPSDesc; gcPSDesc.stage = ShaderStage::Fragment; gcPSDesc.source = k_GridComposite_PS;
		ShaderHandle gridCompPS = engine.compileShader(gcPSDesc);
		checkHandle(gridCompPS, "compileShader(gridCompPS) failed");

		GfxBindingLayoutDesc gridCompLayoutDesc;
		gridCompLayoutDesc
			.beginTable(GfxShaderVisibility::Pixel)
			.addRange(GfxDescriptorRangeType::SRV, 1, 0, 0, GfxDescriptorRangeFlags::DescriptorsVolatile)
			.beginTable(GfxShaderVisibility::Pixel)
			.addRange(GfxDescriptorRangeType::Sampler, 1, 0);
		gridCompLayoutDesc.debugName = "GridCompositeLayout";
		BindingLayoutHandle gridCompLayout = engine.createBindingLayout(gridCompLayoutDesc);
		checkHandle(gridCompLayout, "createBindingLayout(gridCompLayout) failed");

		GfxGraphicsPipelineDesc gridCompDesc;
		gridCompDesc.bindingLayout = gridCompLayout;
		gridCompDesc.vs            = rtCompVS;
		gridCompDesc.ps            = gridCompPS;
		gridCompDesc.rasterizerState.cullMode         = GfxCullMode::None;
		gridCompDesc.depthStencilState.depthEnable    = false;
		gridCompDesc.depthStencilState.depthWriteMask = GfxDepthWriteMask::Zero;
		{
			GfxRenderTargetBlendDesc& b = gridCompDesc.blendState.renderTarget[0];
			b.blendEnable    = true;
			b.srcBlend       = GfxBlend::One;          // 乗算済みアルファ
			b.destBlend      = GfxBlend::InvSrcAlpha;
			b.srcBlendAlpha  = GfxBlend::One;
			b.destBlendAlpha = GfxBlend::InvSrcAlpha;
		}
		gridCompDesc.numRenderTargets = 1;
		gridCompDesc.rtvFormats[0]    = GfxFormat::B8G8R8A8_Unorm;
		gridCompDesc.debugName        = "GridComposite";
		PipelineHandle gridCompPipe = engine.createPipeline(gridCompDesc);
		checkHandle(gridCompPipe, "createPipeline(gridCompPipe) failed");

		// v2 のサンプラー (全メンバーを指定できる)
		GfxSamplerStateDesc pointClampDesc;
		pointClampDesc.filter   = GfxFilter::MinMagMipPoint;
		pointClampDesc.addressU = GfxTextureAddressMode::Border;
		pointClampDesc.addressV = GfxTextureAddressMode::Border;
		pointClampDesc.addressW = GfxTextureAddressMode::Clamp;
		pointClampDesc.borderColor[0] = pointClampDesc.borderColor[1] =
		pointClampDesc.borderColor[2] = pointClampDesc.borderColor[3] = 0.0f;
		pointClampDesc.debugName = "PointBorder";
		SamplerHandle pointBorderSampler = engine.createSampler(pointClampDesc);
		checkHandle(pointBorderSampler, "createSampler(pointBorderSampler) failed");

		// MSAA のターゲット (ウィンドウサイズに合わせて作り直す)
		//   色: v2 のテクスチャ (4x, RTV ビュー)
		//   深度: v1 の DepthStencil (4x) から v2 の DSV ビューを作る
		//   解決先: v2 のテクスチャ (1x, 既定の SRV)
		struct GridTargets
		{
			TextureHandle  color;
			IDepthStencil* depth = nullptr;
			TextureHandle  resolved;
			ViewHandle     rtv;
			ViewHandle     dsv;
		};
		auto createGridTargets = [&](uint32_t w, uint32_t h)
		{
			GridTargets t;
			GfxTextureResourceDesc cd;
			cd.width                 = w;
			cd.height                = h;
			cd.format                = GfxFormat::R8G8B8A8_Unorm;
			cd.sampleCount           = GRID_MSAA;
			cd.usage                 = GfxTextureUsage::RenderTarget;
			cd.useClearValue         = true;
			cd.clearValue.color[3]   = 0.0f;   // 透明 (0,0,0,0)
			cd.debugName             = "GridMSAAColor";
			t.color = engine.createTexture(cd);
			checkHandle(t.color, "createTexture(GridMSAAColor) failed");

			t.depth = check(engine.createDepthStencil({ w, h, PixelFormat::D32_FLOAT, GRID_MSAA, 1.0f, 0, "GridMSAADepth" }),
				"createDepthStencil(GridMSAADepth) failed");

			GfxTextureResourceDesc rd;
			rd.width     = w;
			rd.height    = h;
			rd.format    = GfxFormat::R8G8B8A8_Unorm;
			rd.usage     = GfxTextureUsage::ShaderResource;
			rd.debugName = "GridResolved";
			t.resolved = engine.createTexture(rd);
			checkHandle(t.resolved, "createTexture(GridResolved) failed");

			GfxViewDesc rv;
			rv.type      = GfxViewType::RTV;
			rv.resource  = gfxRef(t.color);
			rv.dimension = GfxViewDimension::Texture2DMS;
			rv.debugName = "GridMSAAColorRTV";
			t.rtv = engine.createView(rv);

			GfxViewDesc dv;
			dv.type      = GfxViewType::DSV;
			dv.resource  = gfxRef(t.depth);
			dv.dimension = GfxViewDimension::Texture2DMS;
			dv.debugName = "GridMSAADepthDSV";
			t.dsv = engine.createView(dv);
			if (!t.rtv.valid() || !t.dsv.valid())
				throw std::runtime_error("[Error] createView(GridMSAA) failed");
			return t;
		};
		auto destroyGridTargets = [&](GridTargets& t)
		{
			engine.destroyView(t.rtv);
			engine.destroyView(t.dsv);
			engine.destroyTexture(t.color);
			engine.destroyTexture(t.resolved);
			engine.destroyDepthStencil(t.depth);
			t = {};
		};
		GridTargets gridTargets = createGridTargets(1280, 720);



		//      32x32 RGBA8 で白黒の市松模様を生成する
		// ---------------------------------------------------
		constexpr uint32_t TEX_W = 32, TEX_H = 32;
		std::vector<uint32_t> texPixels(TEX_W * TEX_H);
		for (uint32_t y = 0; y < TEX_H; ++y)
			for (uint32_t x = 0; x < TEX_W; ++x)
			{
				const bool white = ((x / 4 + y / 4) % 2) == 0;
				texPixels[y * TEX_W + x] = white ? 0xFFFFFFFF : 0xFF404040;
			}

		GfxTextureDesc texDesc;
		texDesc.width       = TEX_W;
		texDesc.height      = TEX_H;
		texDesc.format      = PixelFormat::RGBA8_UNORM;
		texDesc.initialData = texPixels.data();
		texDesc.rowPitch    = TEX_W * 4;
		texDesc.debugName   = "CheckerTex";

		TextureHandle checkerTex = engine.createTexture(texDesc);
		checkHandle(checkerTex, "createTexture(checkerTex) failed");

		// Linear Wrap サンプラーを作成する
		GfxSamplerDesc sampDesc;
		sampDesc.filter   = FilterMode::Linear;
		sampDesc.addressU = AddressMode::Wrap;
		sampDesc.addressV = AddressMode::Wrap;

		SamplerHandle linearSampler = engine.createSampler(sampDesc);
		checkHandle(linearSampler, "createSampler(linearSampler) failed");

		// 比較サンプラー (PCF シャドウ用 s1) は Lighting のバインディングレイアウトの静的サンプラーにした

		// ---------------------------------------------------
		//  4c. MVP 定数バッファを作成する (float4x4 = 64 bytes)
		// ---------------------------------------------------
		// フォワード / G-Buffer 用 MVP バッファ (float4x4 * 2 = 128 bytes)
		BufferHandle mvpBuf = engine.createBuffer({
			128, BufferUsage::Uniform, nullptr, 0
		});
		checkHandle(mvpBuf, "createBuffer(mvpBuf) failed");

		// ---------------------------------------------------
		//  Lighting 定数バッファ
		//  LightCB: InvProj(64) + ViewPos(16) + LightCount(4) + pad(12)
		//           + PointLight[16] * 32 bytes = 608 bytes → 256 アライメントで 768 bytes
		// ---------------------------------------------------
		struct PointLight { float pos[3]; float range; float color[3]; float intensity; };
		struct LightCB {
			float    projParams[4];  // p00, p11, p22, p32
			float    _pad0[4];       // HLSL: float3 _pad0; uint _pad1; = 16 bytes
			float    lightVP[16];
			float    invView[16];
			float    viewPos[4];
			uint32_t lightCount;
			float    _pad[3];
			PointLight lights[16];
		};

		BufferHandle lightCBuf = engine.createBuffer({
			sizeof(LightCB), BufferUsage::Uniform, nullptr, 0
		});
		checkHandle(lightCBuf, "createBuffer(lightCBuf) failed");

		// Shadow Pass 用 CBV (LightMVP 行列)
		struct ShadowCB { float lightMVP[16]; };
		ShadowCB shadowCBData = {};
		BufferHandle shadowCBuf = engine.createBuffer({
			sizeof(ShadowCB), BufferUsage::Uniform, nullptr, 0
		});
		checkHandle(shadowCBuf, "createBuffer(shadowCBuf) failed");

		// ポイントライトを2個設定する
		LightCB lightData = {};
		lightData.lightCount = 1;
		// ライト: 上方向白色 (シャドウ確認用)
		lightData.lights[0].pos[0] = 0.0f;  lightData.lights[0].pos[1] = 2.0f;  lightData.lights[0].pos[2] = -1.0f;
		lightData.lights[0].range = 8.0f;
		lightData.lights[0].color[0] = 1.0f; lightData.lights[0].color[1] = 1.0f; lightData.lights[0].color[2] = 1.0f;
		lightData.lights[0].intensity = 3.0f;
		lightData.viewPos[0] = 1.5f; lightData.viewPos[1] = 2.0f; lightData.viewPos[2] = -2.5f; lightData.viewPos[3] = 1.0f;
		// InvProj: 単位行列 (簡易版)
		// InvProj: FOV45, aspect 1280/720, near=0.1, far=100 のカメラ逆射影行列
		// Proj パラメータ (p00, p11, p22, p32)
		lightData.projParams[0] = 1.357995f;  // p00 = fh/asp
		lightData.projParams[1] = 2.414214f;  // p11 = fh
		lightData.projParams[2] = 1.001001f;  // p22 = f/(f-n)
		lightData.projParams[3] = -0.100100f; // p32 = -fn/(f-n)
		for (int i = 0; i < 4; ++i) lightData._pad0[i] = 0.0f;
		// 光源 LightVP: 位置(0,2,-1) からの正射影行列
		// LightVP (column-major, lookAt)
		const float lightVPArr[16] = { -0.500000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.223607f, -0.149071f, 0.000000f, 0.000000f, 0.447214f, 0.074536f, 0.000000f, 0.000000f, 0.000000f, 0.372678f, 1.000000f };
		for (int i = 0; i < 16; ++i) lightData.lightVP[i] = lightVPArr[i];
		// InvView: カメラ (0,0.5,-3) の逆行列 (列優先)
		// InvView (column-major): lookAt カメラ (0,0.5,-3) の逆行列
		// InvView: カメラ (1.5, 2.0, -2.5) の逆行列
		const float invViewArr[16] = { -0.800000f,0.600000f,0.000000f,0.000000f, 0.424264f,0.565685f,0.707107f,0.000000f, -0.424264f,-0.565685f,0.707107f,0.000000f, 1.500000f,2.000000f,-2.500000f,1.000000f };
		for (int i = 0; i < 16; ++i) lightData.invView[i] = invViewArr[i];
		// shadowCBData も同じ LightVP 行列を使う
		for (int i = 0; i < 16; ++i) shadowCBData.lightMVP[i] = lightVPArr[i];

		drawCmd.indexBuffer  = ib;
		drawCmd.indexCount   = 3;
		gbDrawCmd.indexBuffer  = gbIB;
		gbDrawCmd.indexCount   = 198;  // 床(6) + 円柱(192) = 198

		// ---------------------------------------------------
		//  5. Bundle を一度だけ記録 (静的ジオメトリ用)
		// ---------------------------------------------------
		ICommandBuffer* bundle = check(
			engine.allocCommandBuffer(CommandBufferType::Bundle),
			"allocCommandBuffer(Bundle) failed");

		bundle->begin();
		bundle->bindPipeline(fwdPipe);
		bundle->draw(drawCmd);
		bundle->end();

		std::cout << "[Init] Bundle recorded\n";

		// ---------------------------------------------------
		//  5b. ABI v2 の明示的なコマンドの確認 (起動時に 1 回だけ実行する)
		//      Compute (ルート定数 / ルート UAV / テーブル)、dispatchIndirect、
		//      手動バリア、copyBuffer / copyTexture、範囲指定のクリア
		// ---------------------------------------------------
		{
			static const std::string k_SmokeCS_src = loadShaderSource("smoke.cs.hlsl");
			static const char*       k_SmokeCS     = k_SmokeCS_src.c_str();

			ShaderDesc csDesc;
			csDesc.stage  = ShaderStage::Compute;
			csDesc.source = k_SmokeCS;
			ShaderHandle smokeCS = engine.compileShader(csDesc);
			checkHandle(smokeCS, "compileShader(smokeCS) failed");

			// [0] ルート定数 b0 (2 個), [1] ルート UAV u0, [2] テーブル (t0)
			GfxBindingLayoutDesc csLayoutDesc;
			csLayoutDesc
				.addConstants(0, 2)
				.addRootDescriptor(GfxRootParameterType::UAV, 0)
				.beginTable(GfxShaderVisibility::All)
				.addRange(GfxDescriptorRangeType::SRV, 1, 0, 0, GfxDescriptorRangeFlags::DescriptorsVolatile);
			csLayoutDesc.debugName = "SmokeCSLayout";
			BindingLayoutHandle csLayout = engine.createBindingLayout(csLayoutDesc);
			checkHandle(csLayout, "createBindingLayout(csLayout) failed");

			GfxComputePipelineDesc csPipeDesc;
			csPipeDesc.bindingLayout = csLayout;
			csPipeDesc.cs            = smokeCS;
			csPipeDesc.debugName     = "SmokeCSPipeline";
			PipelineHandle csPipe = engine.createPipeline(csPipeDesc);
			checkHandle(csPipe, "createPipeline(csPipe) failed");

			BufferHandle argsBuf = engine.createBuffer({ 16, BufferUsage::Storage, nullptr, 0 });
			BufferHandle dataBuf = engine.createBuffer({ 32, BufferUsage::Storage, nullptr, 0 });
			BufferHandle copyBuf = engine.createBuffer({ 32, BufferUsage::Storage, nullptr, 0 });
			checkHandle(argsBuf, "createBuffer(argsBuf) failed");
			checkHandle(dataBuf, "createBuffer(dataBuf) failed");
			checkHandle(copyBuf, "createBuffer(copyBuf) failed");

			TextureDesc copyTexDesc;
			copyTexDesc.width     = 16;
			copyTexDesc.height    = 16;
			copyTexDesc.format    = PixelFormat::RGBA8_UNORM;
			copyTexDesc.debugName = "SmokeCopyTex";
			TextureHandle copyTex = engine.createTexture(copyTexDesc);
			checkHandle(copyTex, "createTexture(copyTex) failed");

			ICommandBuffer* scb = check(engine.allocCommandBuffer(), "allocCommandBuffer(smoke) failed");
			scb->begin();
			scb->beginEvent("SmokeTest");

			scb->bindPipeline(csPipe);
			const GfxDescriptor texTable[] = { gfxSRV(checkerTex) };
			scb->setDescriptorTable(2, texTable, 1);

			// 1 回目: 間接実行の引数を書く
			const uint32_t writeArgs[2] = { 0, 0 };
			scb->setRootConstants(0, 2, writeArgs);
			scb->setRootDescriptor(1, argsBuf);
			scb->dispatch(1);

			// 引数バッファ: UAV → 間接引数 (before を明示した手動バリア)
			scb->barrier(gfxTransition(gfxRef(argsBuf), GfxResourceState::IndirectArgument,
			                           GfxResourceState::UnorderedAccess));

			// 2 回目: dispatchIndirect (2 グループ x 4 スレッド) でデータを書く
			const uint32_t writeData[2] = { 100, 1 };
			scb->setRootConstants(0, 2, writeData);
			scb->setRootDescriptor(1, dataBuf);
			GfxIndirectDesc ind;
			ind.argumentBuffer = argsBuf;
			scb->dispatchIndirect(ind);

			// データを別のバッファへコピーする (状態は自動追跡: UAV → COPY_SOURCE)
			scb->barrier(gfxUAVBarrier(gfxRef(dataBuf)));
			scb->copyBuffer(copyBuf, 0, dataBuf, 0, 32);

			// テクスチャの部分コピー (16x16)
			GfxBox box;
			box.right = 16; box.bottom = 16; box.back = 1;
			scb->copyTexture(gfxRef(copyTex), 0, 0, 0, 0, gfxRef(checkerTex), 0, &box);

			// 範囲指定のクリア
			GfxRect clearRect;
			clearRect.left = 0; clearRect.top = 0; clearRect.right = 64; clearRect.bottom = 64;
			const float clearColor[4] = { 0, 0, 0, 1 };
			scb->clearRenderTarget(gbEmissive, clearColor, 1, &clearRect);
			scb->clearDepthStencil(fwdDS, GfxClearFlags::Depth, 1.0f, 0, 1, &clearRect);

			scb->endEvent();
			scb->end();
			engine.submitCommandBuffers({ scb });
			scb->release();

			// GPU が使っている間に破棄しても遅延解放される
			engine.destroyPipeline(csPipe);
			engine.destroyBindingLayout(csLayout);
			engine.destroyShader(smokeCS);
			engine.destroyBuffer(argsBuf);
			engine.destroyBuffer(dataBuf);
			engine.destroyBuffer(copyBuf);
			engine.destroyTexture(copyTex);
			std::cout << "[Init] v2 explicit command smoke test submitted\n";
		}

		// ---------------------------------------------------
		//  5c. パスごとの GPU 時間 (タイムスタンプ) とオクルージョンクエリ
		//      QUERY_FRAMES フレーム分の領域を順番に使い、同じ領域を次に使う直前に
		//      前回の結果を読む (GPU 完了済みなら readQueryResults が true を返す)
		// ---------------------------------------------------
		enum GpuPass : uint32_t { GP_Shadow, GP_GBuffer, GP_RT, GP_Light, GP_Grid, GP_Count };
		static const char* k_GpuPassNames[GP_Count] = { "shadow", "gbuf", "rt", "light", "grid" };
		constexpr uint32_t QUERY_FRAMES     = 4;
		constexpr uint32_t QUERIES_PER_FRAME = GP_Count * 2;

		GfxQueryPoolDesc tsPoolDesc;
		tsPoolDesc.type      = GfxQueryType::Timestamp;
		tsPoolDesc.count     = QUERY_FRAMES * QUERIES_PER_FRAME;
		tsPoolDesc.debugName = "PassTimestamps";
		QueryPoolHandle tsPool = engine.createQueryPool(tsPoolDesc);
		if (!tsPool.valid())
			throw std::runtime_error("[Error] createQueryPool(timestamp) failed");

		GfxQueryPoolDesc occPoolDesc;
		occPoolDesc.type      = GfxQueryType::Occlusion;
		occPoolDesc.count     = QUERY_FRAMES;
		occPoolDesc.debugName = "GBufferOcclusion";
		QueryPoolHandle occPool = engine.createQueryPool(occPoolDesc);
		if (!occPool.valid())
			throw std::runtime_error("[Error] createQueryPool(occlusion) failed");

		const double tsTicksPerMs = static_cast<double>(engine.getTimestampFrequency()) / 1000.0;
		float    gpuPassMs[GP_Count] = {};
		uint64_t gbufSamples         = 0;

		// グリッドは drawIndirect で描く (引数は UPLOAD ヒープのバッファ)
		const GfxDrawArguments gridArgs = { 84, 1, 0, 0 };
		BufferHandle gridArgsBuf = engine.createBuffer({ sizeof(gridArgs), BufferUsage::Staging, &gridArgs, 0 });
		checkHandle(gridArgsBuf, "createBuffer(gridArgsBuf) failed");

		// ---------------------------------------------------
		//  6. メインループ
		// ---------------------------------------------------
		using Clock     = std::chrono::high_resolution_clock;
		using FloatMs   = std::chrono::duration<float, std::milli>;

		// 直近 60 フレームの移動平均用
		constexpr int   AVG_WINDOW  = 60;
		float           frameTimes[AVG_WINDOW] = {};
		int             ftIndex     = 0;
		float           ftSum       = 0.0f;

		// CSV 計測ログ
		std::ofstream csvLog("frame_log.csv");
		csvLog << "frame,frameMs,shadowMs,gbufMs,lightMs,submitMs\n";

		auto frameStart = Clock::now();

		// 自動テスト用の環境変数
		//   GFX_EXAMPLE_FRAMES=N      : N フレームで自動終了する
		//   GFX_EXAMPLE_RESIZE_TEST=1 : リサイズ / 最小化 / 復帰を自動で繰り返す
		auto readEnvInt = [](const char* name, int def) {
			char   buf[32] = {};
			size_t len     = 0;
			if (getenv_s(&len, buf, sizeof(buf), name) == 0 && len > 0)
				return std::atoi(buf);
			return def;
		};
		const int  maxFrames  = readEnvInt("GFX_EXAMPLE_FRAMES", -1);
		const bool resizeTest = readEnvInt("GFX_EXAMPLE_RESIZE_TEST", 0) != 0;

		for (int frame = 0; maxFrames < 0 || frame < maxFrames; ++frame)
		{
#if defined(_WIN32)
			// リサイズテスト: 100 フレーム周期でサイズ変更 → 最小化 → 復帰 → 元のサイズ
			if (resizeTest)
			{
				auto setClientSize = [hwnd](int w, int h) {
					RECT rc = { 0, 0, w, h };
					AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
					SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
						SWP_NOMOVE | SWP_NOZORDER);
				};
				switch (frame % 100)
				{
				case 20: setClientSize(960, 540);         break;
				case 40: ShowWindow(hwnd, SW_MINIMIZE);   break;
				case 60: ShowWindow(hwnd, SW_RESTORE);    break;
				case 80: setClientSize(1280, 720);        break;
				default: break;
				}
			}
#endif
			// メッセージポンプ: WM_QUIT を受信したらループを抜ける
			if (!pumpMessages())
				break;

			// WM_SIZE でリサイズフラグが立っていたら処理する
			// GPU がアイドル状態（submitCommandBuffers の wait 後）なので安全に呼べる
			if (wndCtx.resized)
			{
				wndCtx.resized = false;
				const uint32_t nw = wndCtx.newW;
				const uint32_t nh = wndCtx.newH;

				// スワップチェーンをリサイズする
				if (!win->resize(nw, nh))
					throw std::runtime_error("[Error] SwapChain resize failed");

				// フォワード用 DS をリサイズに合わせて再作成する
				engine.destroyDepthStencil(fwdDS);
				fwdDS = check(
					engine.createDepthStencil({ nw, nh, PixelFormat::D32_FLOAT, 1, 1.0f, 0, "FwdDS" }),
					"createDepthStencil(fwdDS) resize failed");

				// G-Buffer を新しいサイズで再作成する
				engine.destroyRenderTarget(gbAlbedo);
				engine.destroyRenderTarget(gbNormal);
				engine.destroyRenderTarget(gbEmissive);
				engine.destroyDepthStencil(gbDS);

				gbAlbedo = check(
					engine.createRenderTarget({ nw, nh, PixelFormat::RGBA8_UNORM,  1, false, true, {0,0,0,1}, "GBuf_Albedo" }),
					"createRenderTarget(gbAlbedo) resize failed");
				gbNormal = check(
					engine.createRenderTarget({ nw, nh, PixelFormat::RGBA16_FLOAT, 1, false, true, {0,0,0,1}, "GBuf_Normal" }),
					"createRenderTarget(gbNormal) resize failed");
				gbEmissive = check(
					engine.createRenderTarget({ nw, nh, PixelFormat::RGBA16_FLOAT, 1, false, true, {0,0,0,1}, "GBuf_Emissive" }),
					"createRenderTarget(gbEmissive) resize failed");
				gbDS = check(
					engine.createDepthStencil({ nw, nh, PixelFormat::D32_FLOAT, 1, 1.0f, 0, "GBuf_DS" }),
					"createDepthStencil(gbDS) resize failed");

				destroyGridTargets(gridTargets);
				gridTargets = createGridTargets(nw > 0 ? nw : 1u, nh > 0 ? nh : 1u);

				std::cout << "[Engine] Resized to " << nw << "x" << nh << "\n";
			}

			if (!win->acquireNextBuffer())
				throw std::runtime_error("[Error] acquireNextBuffer failed at frame " + std::to_string(frame));

			// MVP 行列を更新する (column-major, HLSL mul(M,v) = M×v)
			{
				const float angle = 0.0f;  // 静的シーン（シャドウ確認用）
				const float c = std::cos(angle), s = std::sin(angle);

				// ライトを XZ 平面で回転させる
				const float lightAngle = static_cast<float>(frame) * 0.01f;
				const float lc = std::cos(lightAngle), ls = std::sin(lightAngle);
				const float lightRadius = 2.0f;
				const float lightX = lc * lightRadius;
				const float lightY = 2.0f;
				const float lightZ = ls * lightRadius - 1.0f;

				// ライト位置を更新
				lightData.lights[0].pos[0] = lightX;
				lightData.lights[0].pos[1] = lightY;
				lightData.lights[0].pos[2] = lightZ;

				// Proj×View×World (Y軸回転) を column-major で計算する
				// Proj = FOV45, 1280/720, near=0.1, far=100 (D3D LH)
				// View = translate(0,-0.5,3) (カメラ(0,0.5,-3)の逆)
				// World = Y回転: col0=(c,0,s,0), col1=(0,1,0,0), col2=(-s,0,c,0), col3=(0,0,0,1)
				//
				// MVP col-major の各列:
				//   col0 = Proj*View*World_col0 = Proj*View*(c,0,s,0)
				//   col1 = Proj*View*(0,1,0,0)
				//   col2 = Proj*View*(-s,0,c,0)
				//   col3 = Proj*View*(0,0,0,1) = translate 成分
				// MVP (column-major): カメラ斜め上 (1.5, 2.0, -2.5)
				// VP col (固定値)
				const float vp0[4] = {-1.086396f, 1.024264f,-0.424689f,-0.424264f};
				const float vp1[4] = { 0.814797f, 1.365685f,-0.566252f,-0.565685f};
				const float vp2[4] = { 0.000000f, 1.707107f, 0.707815f, 0.707107f};
				const float vp3[4] = { 0.000000f, 0.000000f, 3.438973f, 3.535534f};
				const float mvpWorld[32] =
				{
					vp0[0]*c+vp2[0]*s, vp0[1]*c+vp2[1]*s, vp0[2]*c+vp2[2]*s, vp0[3]*c+vp2[3]*s,
					vp1[0],            vp1[1],            vp1[2],            vp1[3],
				   -vp0[0]*s+vp2[0]*c,-vp0[1]*s+vp2[1]*c,-vp0[2]*s+vp2[2]*c,-vp0[3]*s+vp2[3]*c,
					vp3[0],            vp3[1],            vp3[2],            vp3[3],
					// World (column-major: Y軸回転)
					 c,   0.0f, -s,   0.0f,
					0.0f, 1.0f,  0.0f, 0.0f,
					 s,   0.0f,  c,   0.0f,
					0.0f, 0.0f,  0.0f, 1.0f,
				};
				engine.updateBuffer(mvpBuf, mvpWorld, sizeof(mvpWorld));

				// LightVP を毎フレーム再計算する
				{
					// lookAt: 光源位置から原点を見る
					float fx = 0.0f - lightX, fy = 0.0f - lightY, fz = 0.0f - lightZ;
					float fl = std::sqrt(fx*fx + fy*fy + fz*fz);
					fx /= fl; fy /= fl; fz /= fl;
					// right = cross(forward, up=(0,1,0))
					float rx = fy * 0 - fz * 1, ry = fz * 0 - fx * 0, rz = fx * 1 - fy * 0;
					float rl = std::sqrt(rx*rx + ry*ry + rz*rz);
					rx /= rl; ry /= rl; rz /= rl;
					// up = cross(right, forward)
					float ux = ry*fz - rz*fy, uy = rz*fx - rx*fz, uz = rx*fy - ry*fx;
					float dt_r = rx*lightX + ry*lightY + rz*lightZ;
					float dt_u = ux*lightX + uy*lightY + uz*lightZ;
					float dt_f = fx*lightX + fy*lightY + fz*lightZ;
					// LookAt View (col-major)
					float lv[16] = {
						rx, ux, fx, 0,
						ry, uy, fy, 0,
						rz, uz, fz, 0,
						-dt_r, -dt_u, -dt_f, 1
					};
					// 正射影 (col-major): l=-2,r=2,b=-2,t=2,n=0,f=6
					const float lo[16] = {
						0.5f, 0, 0, 0,
						0, 0.5f, 0, 0,
						0, 0, 1.0f/6.0f, 0,
						0, 0, 0, 1
					};
					// LightVP = lo × lv (col-major 行列積)
					float lvp[16] = {};
					for (int r = 0; r < 4; ++r)
						for (int cc = 0; cc < 4; ++cc)
							for (int k = 0; k < 4; ++k)
								lvp[cc*4+r] += lo[k*4+r] * lv[cc*4+k];

					for (int i = 0; i < 16; ++i)
					{
						lightData.lightVP[i]        = lvp[i];
						shadowCBData.lightMVP[i]    = lvp[i];
					}
				}

				// Lighting CB を毎フレーム更新する（LightVP 計算後）
				engine.updateBuffer(lightCBuf, &lightData, sizeof(lightData));
			}

			// バックバッファ RT を取得する
			// nativeWindowHandle が nullptr (ヘッドレス) の場合は nullptr になる
			IRenderTarget* backRT = win->getRenderTarget();
			if (!backRT)
				throw std::runtime_error(
					"[Error] getRenderTarget() returned nullptr at frame "
					+ std::to_string(frame)
					+ " — nativeWindowHandle が設定されていない可能性があります");

			// ===============================================
			//  useDeferred フラグで Forward / Deferred を切り替える
			// ===============================================
			// このフレームで使うクエリの領域。QUERY_FRAMES フレーム前に同じ領域へ書いた結果を先に読む
			const uint32_t qFrame = static_cast<uint32_t>(frame) % QUERY_FRAMES;
			const uint32_t qBase  = qFrame * QUERIES_PER_FRAME;
			{
				uint64_t ts[2] = {};
				for (uint32_t pass = 0; pass < GP_Count; ++pass)
					if (engine.readQueryResults(tsPool, qBase + pass * 2, 2, ts) && ts[1] >= ts[0])
						gpuPassMs[pass] = static_cast<float>(static_cast<double>(ts[1] - ts[0]) / tsTicksPerMs);
				uint64_t samples = 0;
				if (engine.readQueryResults(occPool, qFrame, 1, &samples))
					gbufSamples = samples;
			}
			// パスの先頭と末尾: PIX のイベントとタイムスタンプ
			auto passBegin = [&](ICommandBuffer* c, GpuPass pass)
			{
				c->beginEvent(k_GpuPassNames[pass]);
				c->writeTimestamp(tsPool, qBase + pass * 2);
			};
			auto passEnd = [&](ICommandBuffer* c, GpuPass pass)
			{
				c->writeTimestamp(tsPool, qBase + pass * 2 + 1);
				c->resolveQueries(tsPool, qBase + pass * 2, 2);
				c->endEvent();
			};

			float shadowRecMs=0, gbufRecMs=0, lightRecMs=0, submitMs=0;
			const bool useDeferred = true; // false でフォワードに切り替え

			if (!useDeferred)
			{
				// ---- Forward パス ----
				ICommandBuffer* cb = check(
					engine.allocCommandBuffer(), "allocCommandBuffer failed");
				cb->begin();
				IRenderTarget* rts[] = { backRT };
				cb->setRenderTargets(1, rts, fwdDS);
				cb->bindPipeline(fwdPipe);
				cb->bindTexture(checkerTex, 0);
				cb->bindSampler(linearSampler, 0);
				cb->bindUniformBuffer(mvpBuf, 0);
				cb->draw(drawCmd);
				cb->end();
				engine.submitCommandBuffers({ cb });
				cb->release();
			}
			else
			{
				// ---- Deferred パス ----

				// Pass 0: Shadow Map (光源視点で深度のみ描画)
				auto t_pass = Clock::now();
				engine.updateBuffer(shadowCBuf, &shadowCBData, sizeof(shadowCBData));
				ICommandBuffer* shadowCB = check(
					engine.allocCommandBuffer(), "allocCommandBuffer(shadowCB) failed");
				shadowCB->begin();
				passBegin(shadowCB, GP_Shadow);
				shadowCB->setRenderTargets(0, nullptr, shadowDS, true, true);
				shadowCB->bindPipeline(shadowPipe);
				shadowCB->bindUniformBuffer(shadowCBuf, 0);
				shadowCB->draw(gbDrawCmd);
				passEnd(shadowCB, GP_Shadow);
				shadowCB->end();
				shadowRecMs = FloatMs(Clock::now() - t_pass).count(); t_pass = Clock::now();

				// Pass 1: G-Buffer (Albedo / Normal / Emissive の MRT 3 枚)
				ICommandBuffer* gbufCB = check(
					engine.allocCommandBuffer(), "allocCommandBuffer(gbufCB) failed");
				gbufCB->begin();
				passBegin(gbufCB, GP_GBuffer);
				IRenderTarget* mrts[] = { gbAlbedo, gbNormal, gbEmissive };
				gbufCB->setRenderTargets(3, mrts, gbDS, true, true);
				gbufCB->bindPipeline(gbufPipe);
				gbufCB->bindTexture(checkerTex, 0);
				gbufCB->bindSampler(linearSampler, 0);
				gbufCB->bindUniformBuffer(mvpBuf, 0);
				gbufCB->beginQuery(occPool, qFrame);   // G-Buffer で深度テストを通ったサンプル数
				gbufCB->draw(gbDrawCmd);
				gbufCB->endQuery(occPool, qFrame);
				gbufCB->resolveQueries(occPool, qFrame, 1);
				passEnd(gbufCB, GP_GBuffer);
				gbufCB->end();
				gbufRecMs = FloatMs(Clock::now() - t_pass).count(); t_pass = Clock::now();

				// Pass 4: Raytracing Shadow
				ICommandBuffer* rtCB = nullptr;
				if (scenePipe.valid() && sceneTLAS.valid() && rtOutput.valid())
				{
					// TLAS を毎フレーム更新する（動くオブジェクトへの対応）
					if (sceneTLAS.valid())
					{
						GfxTLASInstance inst;
						inst.blas       = sceneBLAS;
						inst.instanceID = 0;
						// 現在は単位行列（シーン全体が1つの BLAS）
						float identity[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 };
						memcpy(inst.transform, identity, sizeof(identity));
						engine.updateTLAS(sceneTLAS, &inst, 1);
					}

					// RTShadowCB を更新する (InvViewProj + LightPos + LightRange)
					// InvViewProj = InvProj * InvView (column-major)
					// 簡易版: VP の逆行列を CPU で計算
					struct RTShadowCBData {
						float invViewProj[16];
						float lightPos[3];
						float lightRange;
					};
					RTShadowCBData rtCBData = {};
					// InvViewProj: VP の逆行列 (column-major)
					// カメラ固定なので定数を直接使う
					const float invVP[16] = {
						-0.589106f,  0.441825f,  0.000004f, -0.000001f,
						 0.175733f,  0.234311f,  0.292898f, -0.000002f,
						-14.984645f,-19.979527f, 24.974400f, -9.989760f,
						 14.575391f, 19.433855f,-24.292310f,  9.999767f,
					};
					memcpy(rtCBData.invViewProj, invVP, sizeof(invVP));
					rtCBData.lightPos[0] = lightData.lights[0].pos[0];
					rtCBData.lightPos[1] = lightData.lights[0].pos[1];
					rtCBData.lightPos[2] = lightData.lights[0].pos[2];
					rtCBData.lightRange  = 5.0f;
					engine.updateBuffer(rtShadowCB, &rtCBData, sizeof(rtCBData));

					rtCB = check(engine.allocCommandBuffer(), "allocCommandBuffer(rtCB) failed");
					rtCB->begin();
					passBegin(rtCB, GP_RT);
					rtCB->dispatchRays(scenePipe, sceneTLAS, rtOutput,
						gbDS->asTexture(), rtShadowCB, 1280, 720);
					passEnd(rtCB, GP_RT);
					rtCB->end();
				}
				// Pass 2: Deferred Lighting (バックバッファへ)
				// G-Buffer 4枚 (Albedo=t0, Normal=t1, Emissive=t2, Depth=t3) をバインド
				ICommandBuffer* lightCB = check(
					engine.allocCommandBuffer(), "allocCommandBuffer(lightCB) failed");
				lightCB->begin();
				passBegin(lightCB, GP_Light);
				IRenderTarget* lrts[] = { backRT };
				lightCB->setRenderTargets(1, lrts, nullptr, true, false);
				lightCB->bindPipeline(lightPipe);
				lightCB->bindTexture(gbAlbedo->asTexture(),   0);
				lightCB->bindTexture(gbNormal->asTexture(),   1);
				lightCB->bindTexture(gbEmissive->asTexture(), 2);
				lightCB->bindTexture(gbDS->asTexture(),       3);
				// RT Shadow テクスチャをバインドする（RT Pass が先に実行されている必要あり）
				if (rtCB && rtOutput.valid())
					lightCB->bindTexture(rtOutput, 4);
				else
					lightCB->bindTexture(shadowDS->asTexture(), 4);
				// Sampler テーブル (ルートパラメータ 1) は v2 の setDescriptorTable で直接設定する
				const GfxDescriptor lightSamplers[] = { gfxSamplerDesc(linearSampler) };
				lightCB->setDescriptorTable(1, lightSamplers, 1);
				lightCB->bindUniformBuffer(lightCBuf, 0);
				// drawFullscreen: SV_VertexID で 3 頂点を生成
				lightCB->drawFullscreen();
				passEnd(lightCB, GP_Light);
				lightCB->end();
				lightRecMs = FloatMs(Clock::now() - t_pass).count(); t_pass = Clock::now();

				// Pass 3: Grid Line
				{
					// VP (column-major、カメラ斜め上 (1.5,2.0,-2.5))
					const float gvp[16] = {
						-1.086396f, 1.024264f,-0.424689f,-0.424264f,
						 0.814797f, 1.365685f,-0.566252f,-0.565685f,
						 0.000000f, 1.707107f, 0.707815f, 0.707107f,
						 0.000000f, 0.000000f, 3.438973f, 3.535534f,
					};
					engine.updateBuffer(gridVPBuf, gvp, sizeof(gvp));
				}
				ICommandBuffer* gridCB = check(
					engine.allocCommandBuffer(), "allocCommandBuffer(gridCB) failed");
				gridCB->begin();
				passBegin(gridCB, GP_Grid);
				// MSAA の RT / DS (v2 のビュー) に描く
				gridCB->setRenderTargetViews(1, &gridTargets.rtv, gridTargets.dsv);
				{
					const float transparent[4] = { 0, 0, 0, 0 };
					gridCB->clearRenderTargetView(gridTargets.rtv, transparent);
					gridCB->clearDepthStencilView(gridTargets.dsv, GfxClearFlags::Depth, 1.0f, 0);
				}
				// 動的ステートを明示的に設定する (setRenderTargetViews が設定する値と同じ)
				{
					GfxViewport vp;
					vp.width  = static_cast<float>(backRT->getWidth());
					vp.height = static_cast<float>(backRT->getHeight());
					GfxRect sc;
					sc.right  = static_cast<int32_t>(backRT->getWidth());
					sc.bottom = static_cast<int32_t>(backRT->getHeight());
					const float blendFactor[4] = { 1, 1, 1, 1 };
					gridCB->setViewports(1, &vp);
					gridCB->setScissors(1, &sc);
					gridCB->setStencilRef(0);
					gridCB->setBlendFactor(blendFactor);
				}
				// 遮蔽用にシーンの深度だけを描く
				gridCB->bindPipeline(gridDepthPipe);
				gridCB->bindUniformBuffer(mvpBuf, 0);
				gridCB->draw(gbDrawCmd);

				gridCB->bindPipeline(gridPipe);
				gridCB->bindUniformBuffer(gridVPBuf, 0);
				// v2: 頂点バッファを明示的にバインドし、引数バッファから間接描画する
				GfxVertexBufferBinding gridVBBinding;
				gridVBBinding.buffer = gridVB;
				gridCB->setVertexBuffers(0, 1, &gridVBBinding);
				GfxIndirectDesc gridIndirect;
				gridIndirect.argumentBuffer = gridArgsBuf;
				gridCB->drawIndirect(gridIndirect);

				// 4x → 1x に解決し、バックバッファへ合成する
				gridCB->resolve(gfxRef(gridTargets.resolved), 0, gfxRef(gridTargets.color), 0, GfxFormat::R8G8B8A8_Unorm);
				IRenderTarget* gridRTs[] = { backRT };
				gridCB->setRenderTargets(1, gridRTs, nullptr, false, false);
				gridCB->bindPipeline(gridCompPipe);
				const GfxDescriptor gridCompSRV[]     = { gfxSRV(gridTargets.resolved) };
				const GfxDescriptor gridCompSampler[] = { gfxSamplerDesc(pointBorderSampler) };
				gridCB->setDescriptorTable(0, gridCompSRV, 1);
				gridCB->setDescriptorTable(1, gridCompSampler, 1);
				gridCB->drawFullscreen();
				passEnd(gridCB, GP_Grid);
				gridCB->end();


				// F1: RT Shadow テクスチャをバックバッファに上書き表示（デバッグ）
				ICommandBuffer* debugCB = nullptr;
				if (wndCtx.debugRT && rtCB && rtOutput.valid())
				{
					debugCB = check(engine.allocCommandBuffer(), "allocCommandBuffer(debugCB) failed");
					debugCB->begin();
					IRenderTarget* dbgRTs[] = { backRT };
					debugCB->setRenderTargets(1, dbgRTs, nullptr, false, false);
					debugCB->bindPipeline(rtCompPipe);
					debugCB->bindTexture(rtOutput, 0);
					debugCB->bindSampler(linearSampler, 0);
					debugCB->drawFullscreen();
					debugCB->end();
				}

				// Shadow / G-Buffer / RT Shadow / Lighting / Grid を submit
				if (rtCB && debugCB)
					engine.submitCommandBuffers({ shadowCB, gbufCB, rtCB, lightCB, gridCB, debugCB });
				else if (rtCB)
					engine.submitCommandBuffers({ shadowCB, gbufCB, rtCB, lightCB, gridCB });
				else
					engine.submitCommandBuffers({ shadowCB, gbufCB, lightCB, gridCB });
				submitMs = FloatMs(Clock::now() - t_pass).count();
				shadowCB->release();
				gbufCB->release();
				lightCB->release();
				gridCB->release();
				if (rtCB)    rtCB->release();
				if (debugCB) debugCB->release();
						}

			win->present();

			// フレーム時間を計測する
			const auto  now     = Clock::now();
			const float frameMs = FloatMs(now - frameStart).count();
			frameStart          = now;

			// 移動平均を更新する
			ftSum -= frameTimes[ftIndex];
			frameTimes[ftIndex] = frameMs;
			ftSum += frameMs;
			ftIndex = (ftIndex + 1) % AVG_WINDOW;

			const float avgMs  = ftSum / static_cast<float>(AVG_WINDOW);
			const float avgFps = avgMs > 0.0f ? 1000.0f / avgMs : 0.0f;

			// 60 フレームごとの定期ログ
			// CSV に毎フレーム書き込む
			csvLog << frame << "," << frameMs << "," << shadowRecMs
				   << "," << gbufRecMs << "," << lightRecMs << "," << submitMs << "\n";

			if (frame % 60 == 0)
			{
				std::cout
					<< "[Frame " << frame << "]"
					<< "  cur=" << frameMs << " ms"
					<< "  avg=" << avgMs   << " ms"
					<< "  fps=" << avgFps
					<< "  [shadow=" << shadowRecMs
					<< " gbuf=" << gbufRecMs
					<< " light=" << lightRecMs
					<< " submit=" << submitMs << "] ms\n";

				// GPU 時間 (QUERY_FRAMES フレーム前の結果)
				std::cout << "[GPU   " << frame << "]";
				for (uint32_t pass = 0; pass < GP_Count; ++pass)
					std::cout << "  " << k_GpuPassNames[pass] << "=" << gpuPassMs[pass];
				std::cout << " ms  gbufSamples=" << gbufSamples << "\n";

				// タイトルバーに FPS とモードを表示する
				const char* mode = wndCtx.debugRT ? " [F1: RT Shadow Debug]" : " [F1: Normal]";
				char title[128];
				sprintf_s(title, "GfxExample  FPS: %.0f%s", avgFps, mode);
				SetWindowTextA(hwnd, title);
			}

			// スパイク検出: 平均の 2 倍以上かかったフレームを個別にログ出力する
			// avgMs が 0 に近い起動直後は無視する
			if (avgMs > 1.0f && frameMs > avgMs * 2.0f)
			{
				std::cout
					<< "[SPIKE Frame " << frame << "]"
					<< "  cur=" << frameMs << " ms"
					<< "  avg=" << avgMs   << " ms"
					<< "  ratio=" << (frameMs / avgMs) << "x"
					<< "\n";
			}
		}

		// ---------------------------------------------------
		//  7. 終了
		// ---------------------------------------------------
		bundle->release();
		engine.destroyQueryPool(tsPool);
		destroyGridTargets(gridTargets);
		engine.destroyPipeline(gridDepthPipe);
		engine.destroyPipeline(gridCompPipe);
		engine.destroyBindingLayout(gridCompLayout);
		engine.destroyShader(gridCompPS);
		engine.destroySampler(pointBorderSampler);
		engine.destroyQueryPool(occPool);
		engine.destroyBuffer(gridArgsBuf);

		engine.destroyBuffer(vb);
		engine.destroyBuffer(ib);
		engine.destroyPipeline(fwdPipe);
		engine.destroyPipeline(gbufPipe);
		engine.destroyPipeline(lightPipe);
		engine.destroyPipeline(shadowPipe);
		engine.destroyBindingLayout(texLayout);
		engine.destroyBindingLayout(lightLayout);
		engine.destroyBindingLayout(vsCBLayout);
		engine.destroyShader(vs);
		engine.destroyShader(ps);
		engine.destroyShader(gbVS);
		engine.destroyShader(gbPS);
		engine.destroyShader(ltVS);
		engine.destroyShader(ltPS);
		engine.destroyShader(shadowVS);
		engine.destroyShader(gridVS);
		engine.destroyShader(gridPS);
		engine.destroyPipeline(gridPipe);
		engine.destroyShader(rtCompVS);
		engine.destroyShader(rtCompPS);
		engine.destroyPipeline(rtCompPipe);
		engine.destroyBuffer(gridVB);
		engine.destroyBuffer(gridVPBuf);
		engine.destroyTexture(checkerTex);
		engine.destroySampler(linearSampler);
		engine.destroyBuffer(mvpBuf);
		engine.destroyBuffer(lightCBuf);
		engine.destroyBuffer(shadowCBuf);
		if (scenePipe.valid()) engine.destroyRTPipeline(scenePipe);
		if (rtShadowCB.valid()) engine.destroyBuffer(rtShadowCB);
		if (rtOutput.valid())  engine.destroyTexture(rtOutput);
		if (sceneTLAS.valid()) engine.destroyTLAS(sceneTLAS);
		if (sceneBLAS.valid()) engine.destroyBLAS(sceneBLAS);
		engine.destroyBuffer(gbVB);
		engine.destroyBuffer(gbIB);
		engine.destroyRenderTarget(gbAlbedo);
		engine.destroyRenderTarget(gbNormal);
		engine.destroyRenderTarget(gbEmissive);
		engine.destroyDepthStencil(gbDS);
		engine.destroyDepthStencil(fwdDS);
		engine.destroyDepthStencil(shadowDS);
		engine.destroyWindow(win);
		engine.shutdown();
	}
	catch (const std::exception& e)
	{
		std::cerr << e.what() << "\n";
		return 1;
	}

	timeEndPeriod(1);
	return 0;
}

// ============================================================
//  WinMain エントリーポイント (Windows GUI アプリ向け)
//  コンソールアプリとして使う場合は main() を直接呼べばよい。
// ============================================================
#if defined(_WIN32)
int WINAPI WinMain(
	HINSTANCE /*hInstance*/,
	HINSTANCE /*hPrevInstance*/,
	LPSTR     /*lpCmdLine*/,
	int       /*nCmdShow*/)
{
	return main();
}
#endif
