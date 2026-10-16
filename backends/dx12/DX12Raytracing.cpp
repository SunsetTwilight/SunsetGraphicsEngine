// ============================================================
//  backends/dx12/DX12Raytracing.cpp
//  DXR: BLAS / TLAS と RT パイプライン
// ============================================================

#include "DX12Device.h"

// ============================================================
//  BLAS (Bottom-Level Acceleration Structure)
// ============================================================
BLASHandle DX12DeviceImpl::createBLAS(const GfxBLASDesc* desc)
{
	if (!supportsRaytracing())
	{
		OutputDebugStringA("[GfxEngine] createBLAS: DXR not supported\n");
		return {};
	}

	const auto* vbRes = getBuffer(desc->vertexBuffer.id);
	if (!vbRes || !vbRes->res) return {};

	D3D12_GPU_VIRTUAL_ADDRESS vbAddr =
		vbRes->res->GetGPUVirtualAddress() + desc->positionOffset;

	D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
	geomDesc.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

	auto& tri = geomDesc.Triangles;
	tri.VertexBuffer.StartAddress  = vbAddr;
	tri.VertexBuffer.StrideInBytes = desc->vertexStride;
	tri.VertexCount                = desc->vertexCount;
	tri.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;

	if (desc->indexBuffer.valid())
	{
		const auto* ibRes = getBuffer(desc->indexBuffer.id);
		if (ibRes && ibRes->res)
		{
			tri.IndexBuffer = ibRes->res->GetGPUVirtualAddress();
			tri.IndexCount  = desc->indexCount;
			tri.IndexFormat = DXGI_FORMAT_R32_UINT;
		}
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	inputs.NumDescs       = 1;
	inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.pGeometryDescs = &geomDesc;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
	m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
	if (prebuild.ResultDataMaxSizeInBytes == 0) return {};

	auto makeASBuffer = [&](uint64_t size, D3D12_RESOURCE_STATES initState) -> ComPtr<ID3D12Resource>
	{
		ComPtr<ID3D12Resource> res;
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width            = size;
		rd.Height           = 1;
		rd.DepthOrArraySize = 1;
		rd.MipLevels        = 1;
		rd.SampleDesc.Count = 1;
		rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
			initState, nullptr, IID_PPV_ARGS(&res));
		return res;
	};

	DX12BLASRes blas;
	// result: RAYTRACING_ACCELERATION_STRUCTURE で作成する (AS バッファの唯一の初期状態)
	// scratch: バッファは初期状態の指定に関わらず COMMON で作られるため COMMON を指定し、下でバリアを張る
	blas.result  = makeASBuffer(prebuild.ResultDataMaxSizeInBytes,
	                            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
	blas.scratch = makeASBuffer(prebuild.ScratchDataSizeInBytes,
	                            D3D12_RESOURCE_STATE_COMMON);
	if (!blas.result || !blas.scratch) return {};

	// GPU コマンドで BLAS を構築する
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList4> cl;
	m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	{
		ComPtr<ID3D12GraphicsCommandList> tmp;
		m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			alloc.Get(), nullptr, IID_PPV_ARGS(&tmp));
		tmp->QueryInterface(IID_PPV_ARGS(&cl));
	}

	// scratch を COMMON → UNORDERED_ACCESS に遷移してから Build を呼ぶ
	// result は RAYTRACING_ACCELERATION_STRUCTURE で作成済みなのでバリア不要
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource   = blas.scratch.Get();
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cl->ResourceBarrier(1, &b);
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
	buildDesc.DestAccelerationStructureData    = blas.result->GetGPUVirtualAddress();
	buildDesc.Inputs                           = inputs;
	buildDesc.ScratchAccelerationStructureData = blas.scratch->GetGPUVirtualAddress();

	cl->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = blas.result.Get();
	cl->ResourceBarrier(1, &barrier);
	executeAndWait(m_cmdQueue.Get(), cl.Get(), m_device.Get());

	if (desc->debugName)
	{
		wchar_t wname[128];
		MultiByteToWideChar(CP_UTF8, 0, desc->debugName, -1, wname, 128);
		blas.result->SetName(wname);
	}

	const uint64_t id = m_nextId++;
	m_blasMap[id] = std::move(blas);
	return { id };
}

void DX12DeviceImpl::destroyBLAS(BLASHandle h)
{
	auto it = m_blasMap.find(h.id);
	if (it == m_blasMap.end())
		return;

	// TLAS が GPU アドレスで参照しているため遅延解放する
	deferRelease(it->second.result);
	deferRelease(it->second.scratch);
	m_blasMap.erase(it);
}

// ============================================================
//  TLAS (Top-Level Acceleration Structure)
// ============================================================

// インスタンスバッファを CPU から書き込む共通処理
static void fillInstanceBuffer(
	ID3D12Resource*           buf,
	const GfxTLASInstance*    instances,
	uint32_t                  count,
	const std::unordered_map<uint64_t, DX12BLASRes>& blasMap)
{
	D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
	buf->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
	for (uint32_t i = 0; i < count; ++i)
	{
		const auto& inst = instances[i];
		auto& d = mapped[i];
		memset(&d, 0, sizeof(d));

		// 3x4 変換行列をコピー（行優先）
		memcpy(d.Transform, inst.transform, sizeof(float) * 12);

		d.InstanceID                          = inst.instanceID;
		d.InstanceMask                        = 0xFF;
		d.InstanceContributionToHitGroupIndex = inst.hitGroupIndex;
		d.Flags                               = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

		auto it = blasMap.find(inst.blas.id);
		if (it != blasMap.end())
			d.AccelerationStructure = it->second.result->GetGPUVirtualAddress();
	}
	D3D12_RANGE writeRange = { 0, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * count };
	buf->Unmap(0, &writeRange);
}

TLASHandle DX12DeviceImpl::createTLAS(const GfxTLASInstance* instances, uint32_t count)
{
	if (!supportsRaytracing() || count == 0) return {};

	// インスタンス記述子バッファ（Upload ヒープ）
	const uint64_t instBufSize = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * count;
	ComPtr<ID3D12Resource> instBuf;
	{
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width            = instBufSize;
		rd.Height           = 1;
		rd.DepthOrArraySize = 1;
		rd.MipLevels        = 1;
		rd.SampleDesc.Count = 1;
		rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&instBuf));
	}
	fillInstanceBuffer(instBuf.Get(), instances, count, m_blasMap);

	// TLAS プリビルド情報を取得する
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	inputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
	inputs.NumDescs      = count;
	inputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.InstanceDescs = instBuf->GetGPUVirtualAddress();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
	m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
	if (prebuild.ResultDataMaxSizeInBytes == 0) return {};

	auto makeASBuffer2 = [&](uint64_t size, D3D12_RESOURCE_STATES initState) -> ComPtr<ID3D12Resource>
	{
		ComPtr<ID3D12Resource> res;
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width            = size;
		rd.Height           = 1;
		rd.DepthOrArraySize = 1;
		rd.MipLevels        = 1;
		rd.SampleDesc.Count = 1;
		rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
			initState, nullptr, IID_PPV_ARGS(&res));
		return res;
	};

	DX12TLASRes tlas;
	tlas.result  = makeASBuffer2(prebuild.ResultDataMaxSizeInBytes,
	                             D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
	tlas.scratch = makeASBuffer2(prebuild.ScratchDataSizeInBytes,
	                             D3D12_RESOURCE_STATE_COMMON);   // バッファは COMMON で作成される
	tlas.instanceBuf  = instBuf;
	tlas.maxInstances = count;
	tlas.builtCount   = count;
	if (!tlas.result || !tlas.scratch) return {};

	// GPU コマンドで TLAS を構築する
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList4> cl;
	m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	{
		ComPtr<ID3D12GraphicsCommandList> tmp;
		m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			alloc.Get(), nullptr, IID_PPV_ARGS(&tmp));
		tmp->QueryInterface(IID_PPV_ARGS(&cl));
	}

	// scratch を COMMON → UNORDERED_ACCESS に遷移してから Build を呼ぶ
	// result は RAYTRACING_ACCELERATION_STRUCTURE で作成済みなのでバリア不要
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource   = tlas.scratch.Get();
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cl->ResourceBarrier(1, &b);
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
	buildDesc.DestAccelerationStructureData    = tlas.result->GetGPUVirtualAddress();
	buildDesc.Inputs                           = inputs;
	buildDesc.ScratchAccelerationStructureData = tlas.scratch->GetGPUVirtualAddress();

	cl->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = tlas.result.Get();
	cl->ResourceBarrier(1, &barrier);
	executeAndWait(m_cmdQueue.Get(), cl.Get(), m_device.Get());

	const uint64_t id = m_nextId++;
	m_tlasMap[id] = std::move(tlas);
	return { id };
}

void DX12DeviceImpl::destroyTLAS(TLASHandle h)
{
	auto it = m_tlasMap.find(h.id);
	if (it == m_tlasMap.end())
		return;

	deferRelease(it->second.result);
	deferRelease(it->second.scratch);
	deferRelease(it->second.instanceBuf);
	m_tlasMap.erase(it);
}

void DX12DeviceImpl::updateTLAS(TLASHandle h, const GfxTLASInstance* instances, uint32_t count)
{
	auto it = m_tlasMap.find(h.id);
	if (it == m_tlasMap.end()) return;

	auto& tlas = it->second;
	if (count == 0 || count > tlas.maxInstances) return;

	// インスタンスバッファを更新 (CPU 書き込み)
	// 前回のビルドは executeAndWait で完了済みのため、上書きしても競合しない
	fillInstanceBuffer(tlas.instanceBuf.Get(), instances, count, m_blasMap);

	// PERFORM_UPDATE はインスタンス数が前回のビルドと同じ場合のみ有効。
	// 数が変わった場合はフルリビルドする (サイズは作成時の maxInstances 分を確保済み)
	const bool canUpdate = (count == tlas.builtCount);

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	inputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
	if (canUpdate)
		inputs.Flags    |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
	inputs.NumDescs      = count;
	inputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.InstanceDescs = tlas.instanceBuf->GetGPUVirtualAddress();

	// 同期実行する (executeAndWait)。毎フレームの非同期化は Phase 2 で行う
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList4> cl;
	m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	{
		ComPtr<ID3D12GraphicsCommandList> tmp;
		m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			alloc.Get(), nullptr, IID_PPV_ARGS(&tmp));
		tmp->QueryInterface(IID_PPV_ARGS(&cl));
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
	buildDesc.DestAccelerationStructureData    = tlas.result->GetGPUVirtualAddress();
	buildDesc.SourceAccelerationStructureData  = canUpdate ? tlas.result->GetGPUVirtualAddress() : 0;
	buildDesc.Inputs                           = inputs;
	buildDesc.ScratchAccelerationStructureData = tlas.scratch->GetGPUVirtualAddress();

	cl->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = tlas.result.Get();
	cl->ResourceBarrier(1, &barrier);
	executeAndWait(m_cmdQueue.Get(), cl.Get(), m_device.Get());
	tlas.builtCount = count;
}

// ============================================================
//  RT パイプライン (RTPSO)
// ============================================================
RTPipelineHandle DX12DeviceImpl::createRTPipeline(const GfxRTPipelineDesc* desc)
{
	if (!supportsRaytracing()) return {};
	if (!desc->raygenSource || !desc->missSource || !desc->closestHitSource) return {};
	// 3つのシェーダーソースを1つの lib としてコンパイルする
	// 各ソースをそのまま結合する（重複定義は各ソース側で避けること）
	std::string combined =
		std::string(desc->raygenSource) + "\n" +
		std::string(desc->missSource)   + "\n" +
		std::string(desc->closestHitSource);

	DX12ShaderOutput compiled;
	const bool compiledOk = m_shaderCompiler.compileLibrary(combined.c_str(), "lib_6_3", compiled);
	reportShaderMessages("createRTPipeline", nullptr, compiled.messages, !compiledOk);
	if (!compiledOk)
		return {};
	const std::vector<uint8_t>& bytecode = compiled.bytecode;

	// エントリポイント名
	const wchar_t* k_raygen     = L"RayGen";
	const wchar_t* k_miss       = L"Miss";
	const wchar_t* k_closestHit = L"ClosestHit";
	const wchar_t* k_hitGroup   = L"HitGroup";

	// StateObject サブオブジェクト
	std::vector<D3D12_STATE_SUBOBJECT> subs;
	subs.reserve(16);

	// 1. DXIL ライブラリ
	D3D12_EXPORT_DESC exports[] = {
		{ k_raygen,     nullptr, D3D12_EXPORT_FLAG_NONE },
		{ k_miss,       nullptr, D3D12_EXPORT_FLAG_NONE },
		{ k_closestHit, nullptr, D3D12_EXPORT_FLAG_NONE },
	};
	D3D12_DXIL_LIBRARY_DESC libDesc = {};
	libDesc.DXILLibrary.pShaderBytecode = bytecode.data();
	libDesc.DXILLibrary.BytecodeLength  = bytecode.size();
	libDesc.NumExports   = _countof(exports);
	libDesc.pExports     = exports;
	subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDesc });

	// 2. Hit Group
	D3D12_HIT_GROUP_DESC hitGroupDesc = {};
	hitGroupDesc.HitGroupExport         = k_hitGroup;
	hitGroupDesc.Type                   = D3D12_HIT_GROUP_TYPE_TRIANGLES;
	hitGroupDesc.ClosestHitShaderImport = k_closestHit;
	subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroupDesc });

	// 3. Shader Config (payload / attribute サイズ)
	D3D12_RAYTRACING_SHADER_CONFIG shaderCfg = {};
	shaderCfg.MaxPayloadSizeInBytes   = desc->maxPayloadSize;
	shaderCfg.MaxAttributeSizeInBytes = desc->maxAttributeSize;
	subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderCfg });

	// 4. Global Root Signature
	// UAV(u0): 出力テクスチャ, SRV(t0): TLAS, CBV(b0): カメラ定数
	// UAV テーブル: u0（RT 出力）
	D3D12_DESCRIPTOR_RANGE1 uavRange = {};
	uavRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	uavRange.NumDescriptors   = 1;
	uavRange.BaseShaderRegister = 0;
	uavRange.OffsetInDescriptorsFromTableStart = 0;
	uavRange.Flags            = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

	// SRV テーブル: t1（深度テクスチャ）
	D3D12_DESCRIPTOR_RANGE1 srvRange = {};
	srvRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors   = 1;
	srvRange.BaseShaderRegister = 1; // t1
	srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	srvRange.Flags            = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

	D3D12_ROOT_PARAMETER1 rtParams[4] = {};
	// param 0: UAV テーブル (u0) = RT 出力
	rtParams[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rtParams[0].DescriptorTable.NumDescriptorRanges = 1;
	rtParams[0].DescriptorTable.pDescriptorRanges   = &uavRange;
	rtParams[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
	// param 1: SRV (t0) = TLAS（AS は直接 GPU アドレスで渡す）
	rtParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_SRV;
	rtParams[1].Descriptor.ShaderRegister           = 0;
	rtParams[1].Descriptor.RegisterSpace            = 0;
	rtParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
	// param 2: SRV テーブル (t1) = 深度テクスチャ
	rtParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rtParams[2].DescriptorTable.NumDescriptorRanges = 1;
	rtParams[2].DescriptorTable.pDescriptorRanges   = &srvRange;
	rtParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
	// param 3: CBV (b0) = RTShadowCB（InvViewProj / LightPos）
	rtParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rtParams[3].Descriptor.ShaderRegister           = 0;
	rtParams[3].Descriptor.RegisterSpace            = 0;
	rtParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

	// Static Sampler (s0) = リニアサンプラー
	D3D12_STATIC_SAMPLER_DESC staticSampler = {};
	staticSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	staticSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	staticSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	staticSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	staticSampler.ShaderRegister   = 0;
	staticSampler.RegisterSpace    = 0;
	staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd = {};
	rsd.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
	rsd.Desc_1_1.NumParameters     = 4;
	rsd.Desc_1_1.pParameters       = rtParams;
	rsd.Desc_1_1.NumStaticSamplers = 1;
	rsd.Desc_1_1.pStaticSamplers   = &staticSampler;
	rsd.Desc_1_1.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ComPtr<ID3DBlob> rsSer, rsErr;
	D3D12SerializeVersionedRootSignature(&rsd, &rsSer, &rsErr);
	ComPtr<ID3D12RootSignature> globalRS;
	m_device->CreateRootSignature(0,
		rsSer->GetBufferPointer(), rsSer->GetBufferSize(),
		IID_PPV_ARGS(&globalRS));

	subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, globalRS.GetAddressOf() });
	// 5. Pipeline Config
	D3D12_RAYTRACING_PIPELINE_CONFIG pipelineCfg = {};
	pipelineCfg.MaxTraceRecursionDepth = desc->maxRecursionDepth;
	subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineCfg });

	// StateObject 作成
	D3D12_STATE_OBJECT_DESC soDesc = {};
	soDesc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
	soDesc.NumSubobjects = static_cast<UINT>(subs.size());
	soDesc.pSubobjects   = subs.data();

	DX12RTPipelineRes rt;
	if (FAILED(m_device->CreateStateObject(&soDesc, IID_PPV_ARGS(&rt.pso))))
	{
		OutputDebugStringA("[GfxEngine] createRTPipeline: CreateStateObject failed\n");
		return {};
	}
	rt.pso->QueryInterface(IID_PPV_ARGS(&rt.props));
	rt.globalRS = globalRS;

	// Shader Table 作成
	// レイアウト: [RayGen][Miss][HitGroup] それぞれ 1 エントリ
	const uint64_t D3D12_SHADER_IDENTIFIER_SIZE = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
	const uint64_t stride = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
	const uint64_t tableSize = stride * 3;  // raygen / miss / hitgroup

	D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = tableSize;
	rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
	rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&rt.shaderTable));

	uint8_t* mapped = nullptr;
	rt.shaderTable->Map(0, nullptr, reinterpret_cast<void**>(&mapped));

	void* rgId = rt.props->GetShaderIdentifier(k_raygen);
	void* msId = rt.props->GetShaderIdentifier(k_miss);
	void* hgId = rt.props->GetShaderIdentifier(k_hitGroup);

	memcpy(mapped + stride * 0, rgId, D3D12_SHADER_IDENTIFIER_SIZE);
	memcpy(mapped + stride * 1, msId, D3D12_SHADER_IDENTIFIER_SIZE);
	memcpy(mapped + stride * 2, hgId, D3D12_SHADER_IDENTIFIER_SIZE);

	rt.shaderTable->Unmap(0, nullptr);
	rt.tableStride = stride;
	rt.tableSize   = tableSize;

	if (desc->debugName)
	{
		wchar_t wname[128];
		MultiByteToWideChar(CP_UTF8, 0, desc->debugName, -1, wname, 128);
		rt.pso->SetName(wname);
	}

	const uint64_t id = m_nextId++;
	m_rtPipelineMap[id] = std::move(rt);

	OutputDebugStringA("[GfxEngine] createRTPipeline: OK\n");
	return { id };
}

void DX12DeviceImpl::destroyRTPipeline(RTPipelineHandle h)
{
	auto it = m_rtPipelineMap.find(h.id);
	if (it == m_rtPipelineMap.end())
		return;

	// 実行中の DispatchRays が参照している可能性があるため遅延解放する
	deferRelease(it->second.pso);
	deferRelease(it->second.globalRS);
	deferRelease(it->second.shaderTable);
	m_rtPipelineMap.erase(it);
}
