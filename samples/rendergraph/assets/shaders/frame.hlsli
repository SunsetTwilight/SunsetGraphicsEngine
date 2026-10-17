// フレームの定数 (アプリが constants("frame") で渡す)。すべてのパイプラインで b1 につなぐ
#ifndef FRAME_HLSLI
#define FRAME_HLSLI

struct PointLight
{
	float3 pos;
	float  range;
	float3 color;
	float  intensity;
};

cbuffer Frame : register(b1)
{
	float4x4   gViewProj;
	float4x4   gInvViewProj;
	float4     gCameraPos;
	float4     gSunDir;       // xyz: 光の進む向き (正規化済み)
	float4     gSunColor;     // rgb: 色 x 強さ
	float4     gAmbient;      // rgb: 環境光
	uint       gLightCount;
	float      gTime;
	float      gExposure;
	float      gPad;
	PointLight gLights[8];
};

#endif
