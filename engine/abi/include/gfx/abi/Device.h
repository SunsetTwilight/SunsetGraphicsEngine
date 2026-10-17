#pragma once
// ============================================================
//  gfx/abi/Device.h
//  デバイス vtable と IBackendPlugin
//  ※ GfxBackendABI.h の一部。単独で include してもよい (前のパートを読み込む)
// ============================================================

#include "Interfaces.h"
// ============================================================
//  GfxDeviceVTable
// ============================================================
struct GfxDeviceVTable {
	bool (*init)    (void* self, const GfxDeviceConfig* cfg);
	void (*shutdown)(void* self);

	// ---- SwapChain / RT / DS 作成 ----
	ISwapChain*    (*createSwapChain)   (void* self, const GfxSwapChainDesc*    desc);
	IRenderTarget* (*createRenderTarget)(void* self, const GfxRenderTargetDesc* desc);
	IDepthStencil* (*createDepthStencil)(void* self, const GfxDepthStencilDesc* desc);

	// ---- コマンドバッファ管理 ----
	// allocCommandBuffer: アプリが任意のタイミングで確保できる
	// → release() で解放するまで有効
	ICommandBuffer* (*allocCommandBuffer)(void* self, CommandBufferType type);

	// submitCommandBuffers: Direct コマンドバッファを GPU キューにサブミット
	// cbs[count] の配列を渡す。記録済み(end済み)であること。
	void (*submitCommandBuffers)(void* self,
								 ICommandBuffer* const* cbs,
								 uint32_t count);

	// ---- GPU リソース ----
	BufferHandle  (*createBuffer)  (void* self, const GfxBufferDesc*  desc);
	TextureHandle (*createTexture) (void* self, const GfxTextureDesc* desc);
	ShaderHandle  (*compileShader) (void* self, const GfxShaderDesc*  desc);
	SamplerHandle (*createSampler) (void* self, const GfxSamplerDesc* desc);
	void (*destroyBuffer)  (void* self, BufferHandle  h);
	void (*destroyTexture) (void* self, TextureHandle h);
	void (*destroyShader)  (void* self, ShaderHandle  h);
	void (*destroySampler) (void* self, SamplerHandle h);

	// UPLOAD ヒープのバッファに CPU からデータを書き込む
	// Uniform / Staging バッファ用。size は書き込むバイト数。
	bool (*updateBuffer)   (void* self, BufferHandle h, const void* data, uint64_t size);

	// ---- 描画 (現在アクティブなコマンドバッファへ記録) ----
	void (*bindTexture)    (void* self, TextureHandle h, uint32_t slot);
	void (*draw)           (void* self, const GfxDrawCmd* cmd);
	void (*drawFullscreen) (void* self);

	// ---- クエリ ----
	GraphicsAPI (*getAPI)        (const void* self);
	const char* (*getAdapterName)(const void* self);
	uint64_t    (*getVRAMUsageMB)(const void* self);

	// ---- Pipeline ----
	PipelineHandle (*createPipeline)  (void* self, const GfxPipelineDesc* desc);
	void           (*destroyPipeline) (void* self, PipelineHandle h);
	BLASHandle     (*createBLAS)      (void* self, const GfxBLASDesc* desc);
	void           (*destroyBLAS)     (void* self, BLASHandle h);
	TLASHandle     (*createTLAS)      (void* self, const GfxTLASInstance* instances, uint32_t count);
	void           (*destroyTLAS)     (void* self, TLASHandle h);
	void           (*updateTLAS)      (void* self, TLASHandle h, const GfxTLASInstance* instances, uint32_t count);
	RTPipelineHandle (*createRTPipeline)  (void* self, const GfxRTPipelineDesc* desc);
	void             (*destroyRTPipeline) (void* self, RTPipelineHandle h);

	// ---- ABI v2: バインディングレイアウト / パイプラインステート ----
	// desc->structSize までしか読まない。作成したパイプラインは destroyPipeline で破棄する。
	// バインディングレイアウトは、それを使うパイプラインより先に破棄してもよい
	// (パイプラインはルートシグネチャへの参照を自分で保持する)。
	BindingLayoutHandle (*createBindingLayout)   (void* self, const GfxBindingLayoutDesc* desc);
	void                (*destroyBindingLayout)  (void* self, BindingLayoutHandle h);
	PipelineHandle      (*createGraphicsPipeline)(void* self, const GfxGraphicsPipelineDesc* desc);

	// ---- ABI v2 (Phase 2b): Compute パイプライン / クエリ ----
	PipelineHandle  (*createComputePipeline)(void* self, const GfxComputePipelineDesc* desc);
	QueryPoolHandle (*createQueryPool)      (void* self, const GfxQueryPoolDesc* desc);
	void            (*destroyQueryPool)     (void* self, QueryPoolHandle h);
	// resolveQueries で書き出した結果を読む。対象の resolve を含む submit が GPU で
	// 完了していなければ false を返す (待たない)。
	bool            (*readQueryResults)     (void* self, QueryPoolHandle h, uint32_t first, uint32_t count, uint64_t* out);
	// タイムスタンプの 1 秒あたりのティック数
	uint64_t        (*getTimestampFrequency)(const void* self);

	// ---- ABI v2 (Phase 2b-2): リソース / ビュー / サンプラー ----
	// 破棄は destroyTexture / destroySampler。ビューはリソースへの参照を持つので、
	// リソースを先に破棄してもビューが残っている間は解放されない。
	TextureHandle (*createTextureResource)(void* self, const GfxTextureResourceDesc* desc);
	ViewHandle    (*createView)           (void* self, const GfxViewDesc* desc);
	void          (*destroyView)          (void* self, ViewHandle h);
	SamplerHandle (*createSamplerState)   (void* self, const GfxSamplerStateDesc* desc);

	// ---- ABI 7: シェーダーのリフレクション ----
	// out->structSize までを書く (呼び出し側が structSize を設定する)。リフレクションがなければ false
	bool     (*getShaderReflection)  (void* self, ShaderHandle h, GfxShaderReflection* out);
	// #include で読み込んだファイルのパスを改行 ('\n') 区切りで buffer に書く ('\0' 終端)。
	// 戻り値は必要なバイト数 ('\0' を含む)。bufferSize が足りなければ何も書かない (buffer = nullptr で問い合わせ)
	uint32_t (*getShaderDependencies)(void* self, ShaderHandle h, char* buffer, uint32_t bufferSize);

	// ---- ABI 7: シェーダー / パイプラインの作り直し (ホットリロード、エディターでの変更) ----
	// ハンドルはそのままで中身を入れ替える。全部作れたときだけ入れ替え、失敗したら何も変えずに false。
	// updateShader はそのシェーダーを使うパイプラインも作り直す (ステージは変えられない)。
	// 記録中のコマンドバッファでバインド済みのパイプラインを、その記録中に更新しないこと。
	bool (*updateShader)           (void* self, ShaderHandle h, const GfxShaderDesc* desc);
	bool (*updateGraphicsPipeline) (void* self, PipelineHandle h, const GfxGraphicsPipelineDesc* desc);
	bool (*updateComputePipeline)  (void* self, PipelineHandle h, const GfxComputePipelineDesc* desc);
	// パイプラインの今の記述子 (out->structSize までを書く。debugName はパイプラインが生きている間だけ有効)
	bool (*getGraphicsPipelineDesc)(const void* self, PipelineHandle h, GfxGraphicsPipelineDesc* out);
	bool (*getComputePipelineDesc) (const void* self, PipelineHandle h, GfxComputePipelineDesc* out);

	// ---- ABI 8: リードバック (BufferUsage::Readback のバッファ) ----
	// BufferUsage::Readback のバッファの [offset, offset + size) を out に写す。そのバッファへの copyBuffer を含む
	// 最後の submit が GPU で完了していなければ false (待たない)。まだ一度もコピーしていないときも false
	bool (*readBuffer)(void* self, BufferHandle h, uint64_t offset, uint64_t size, void* out);
};

// ============================================================
//  IBackendPlugin
// ============================================================
struct IBackendPlugin {
	const GfxDeviceVTable* vtable = nullptr;
	void*                  self   = nullptr;

	bool init    (const GfxDeviceConfig& c)  { return vtable->init(self, &c); }
	void shutdown()                           { vtable->shutdown(self); }

	ISwapChain*    createSwapChain   (const GfxSwapChainDesc& d)    { return vtable->createSwapChain(self, &d);    }
	IRenderTarget* createRenderTarget(const GfxRenderTargetDesc& d) { return vtable->createRenderTarget(self, &d); }
	IDepthStencil* createDepthStencil(const GfxDepthStencilDesc& d) { return vtable->createDepthStencil(self, &d); }

	ICommandBuffer* allocCommandBuffer(CommandBufferType type = CommandBufferType::Direct) {
		return vtable->allocCommandBuffer(self, type);
	}
	void submitCommandBuffers(ICommandBuffer* const* cbs, uint32_t count) {
		vtable->submitCommandBuffers(self, cbs, count);
	}

	BufferHandle  createBuffer  (const GfxBufferDesc& d)   { return vtable->createBuffer(self, &d);   }
	TextureHandle createTexture (const GfxTextureDesc& d)  { return vtable->createTexture(self, &d);  }
	ShaderHandle  compileShader (const GfxShaderDesc& d)   { return vtable->compileShader(self, &d);  }
	SamplerHandle createSampler (const GfxSamplerDesc& d)  { return vtable->createSampler(self, &d);  }
	void destroyBuffer (BufferHandle  h) { vtable->destroyBuffer(self, h);  }
	void destroyTexture(TextureHandle h) { vtable->destroyTexture(self, h); }
	void destroyShader (ShaderHandle  h) { vtable->destroyShader(self, h);  }
	void destroySampler(SamplerHandle h) { vtable->destroySampler(self, h); }
	bool updateBuffer  (BufferHandle h, const void* data, uint64_t size)
	{
		return vtable->updateBuffer(self, h, data, size);
	}
	PipelineHandle createPipeline (const GfxPipelineDesc& d) { return vtable->createPipeline(self, &d); }
	void destroyPipeline(PipelineHandle h)                    { vtable->destroyPipeline(self, h);       }
	BLASHandle createBLAS  (const GfxBLASDesc& d) { return vtable->createBLAS(self, &d); }
	void       destroyBLAS (BLASHandle h)          { vtable->destroyBLAS(self, h);        }
	TLASHandle createTLAS  (const GfxTLASInstance* inst, uint32_t n) { return vtable->createTLAS(self, inst, n); }
	void       destroyTLAS (TLASHandle h)                            { vtable->destroyTLAS(self, h);             }
	void       updateTLAS  (TLASHandle h, const GfxTLASInstance* inst, uint32_t n) { vtable->updateTLAS(self, h, inst, n); }
	RTPipelineHandle createRTPipeline  (const GfxRTPipelineDesc& d) { return vtable->createRTPipeline(self, &d); }
	void             destroyRTPipeline (RTPipelineHandle h)          { vtable->destroyRTPipeline(self, h);       }

	BindingLayoutHandle createBindingLayout (const GfxBindingLayoutDesc& d)    { return vtable->createBindingLayout(self, &d);    }
	void                destroyBindingLayout(BindingLayoutHandle h)            { vtable->destroyBindingLayout(self, h);           }
	PipelineHandle      createPipeline      (const GfxGraphicsPipelineDesc& d) { return vtable->createGraphicsPipeline(self, &d); }
	PipelineHandle      createPipeline      (const GfxComputePipelineDesc& d)  { return vtable->createComputePipeline(self, &d);  }
	QueryPoolHandle     createQueryPool     (const GfxQueryPoolDesc& d)        { return vtable->createQueryPool(self, &d);        }
	void                destroyQueryPool    (QueryPoolHandle h)                { vtable->destroyQueryPool(self, h);               }
	bool                readQueryResults    (QueryPoolHandle h, uint32_t first, uint32_t count, uint64_t* out)
	                                        { return vtable->readQueryResults(self, h, first, count, out); }
	uint64_t            getTimestampFrequency() const                         { return vtable->getTimestampFrequency(self);      }
	TextureHandle       createTexture       (const GfxTextureResourceDesc& d)  { return vtable->createTextureResource(self, &d);  }
	ViewHandle          createView          (const GfxViewDesc& d)             { return vtable->createView(self, &d);             }
	void                destroyView         (ViewHandle h)                     { vtable->destroyView(self, h);                    }
	SamplerHandle       createSampler       (const GfxSamplerStateDesc& d)     { return vtable->createSamplerState(self, &d);     }
	bool                getShaderReflection (ShaderHandle h, GfxShaderReflection& out) { return vtable->getShaderReflection(self, h, &out); }
	uint32_t            getShaderDependencies(ShaderHandle h, char* buf, uint32_t size) { return vtable->getShaderDependencies(self, h, buf, size); }
	bool                updateShader        (ShaderHandle h, const GfxShaderDesc& d)              { return vtable->updateShader(self, h, &d); }
	bool                updatePipeline      (PipelineHandle h, const GfxGraphicsPipelineDesc& d)  { return vtable->updateGraphicsPipeline(self, h, &d); }
	bool                updatePipeline      (PipelineHandle h, const GfxComputePipelineDesc& d)   { return vtable->updateComputePipeline(self, h, &d); }
	bool                getPipelineDesc     (PipelineHandle h, GfxGraphicsPipelineDesc& out) const { return vtable->getGraphicsPipelineDesc(self, h, &out); }
	bool                getPipelineDesc     (PipelineHandle h, GfxComputePipelineDesc& out) const  { return vtable->getComputePipelineDesc(self, h, &out); }
	bool                readBuffer          (BufferHandle h, uint64_t offset, uint64_t size, void* out) { return vtable->readBuffer(self, h, offset, size, out); }

	void bindTexture   (TextureHandle h, uint32_t s)  { vtable->bindTexture(self, h, s);  }
	void draw          (const GfxDrawCmd& cmd)        { vtable->draw(self, &cmd);         }
	void drawFullscreen()                              { vtable->drawFullscreen(self);      }

	GraphicsAPI getAPI()         const { return vtable->getAPI(self);         }
	const char* getAdapterName() const { return vtable->getAdapterName(self); }
	uint64_t    getVRAMUsageMB() const { return vtable->getVRAMUsageMB(self); }
};
