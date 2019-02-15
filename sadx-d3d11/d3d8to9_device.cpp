/**
 * Copyright (C) 2015 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/d3d8to9#license
 */

// TODO: (long-term) adjust draw queue to draw all opaque geometry first like a SANE GAME

#include "stdafx.h"

#include <d3d11_1.h>
#include <DirectXMath.h>
#include <cassert>
#include <fstream>

#include "d3d8to9.hpp"
#include "SimpleMath.h"
#include "int_multiple.h"
#include "CBufferWriter.h"
#include "Material.h"
#include "globals.h"
#include "ShaderIncluder.h"

using namespace Microsoft::WRL;

static constexpr auto BLEND_DEFAULT = D3DBLEND_ONE | (D3DBLEND_ONE << 4) | (D3DBLENDOP_ADD << 8);

static const D3D_FEATURE_LEVEL FEATURE_LEVELS[2] =
{
	D3D_FEATURE_LEVEL_11_1,
	D3D_FEATURE_LEVEL_11_0
};

std::vector<D3D_SHADER_MACRO> Direct3DDevice8::shader_preprocess(uint32_t flags) const
{
	std::vector<D3D_SHADER_MACRO> result;

	result.push_back({ "MAX_FRAGMENTS", fragments_str.c_str() });

	if (flags & ShaderFlags::fvf_rhw)
	{
		result.push_back({ "FVF_RHW", "1" });
	}

	if (flags & ShaderFlags::fvf_normal)
	{
		result.push_back({ "FVF_NORMAL", "1" });
	}

	if (flags & ShaderFlags::fvf_diffuse)
	{
		result.push_back({ "FVF_DIFFUSE", "1" });
	}

	if (flags & ShaderFlags::fvf_tex1)
	{
		result.push_back({ "FVF_TEX1", "1" });
	}

	if (flags & ShaderFlags::tci_envmap)
	{
		result.push_back({ "TCI_CAMERASPACENORMAL", "1" });
	}

	if (flags & ShaderFlags::rs_lighting)
	{
		result.push_back({ "RS_LIGHTING", "1" });
	}

	if (flags & ShaderFlags::rs_specular)
	{
		result.push_back({ "RS_SPECULAR", "1" });
	}

	if (flags & ShaderFlags::rs_alpha)
	{
		result.push_back({ "RS_ALPHA", "1" });
	}

	if (flags & ShaderFlags::oit)
	{
		result.push_back({ "OIT", "1" });
	}

	if (flags & ShaderFlags::rs_fog)
	{
		result.push_back({ "RS_FOG", "1" });
	}

	result.push_back({});
	return result;
}

const std::vector<uint8_t>& Direct3DDevice8::get_shader_source(const std::string& path)
{
	const auto it = shader_sources.find(path);

	if (it != shader_sources.end())
	{
		return it->second;
	}

	std::ifstream file(path, std::ios::binary | std::ios::ate);
	auto size = static_cast<size_t>(file.tellg());
	file.seekg(0, std::ios::beg);
	std::vector<uint8_t> buffer(size);
	file.read(reinterpret_cast<char*>(buffer.data()), size);
	file.close();

	shader_sources[path] = std::move(buffer);
	return shader_sources[path];
}

VertexShader Direct3DDevice8::get_vs(uint32_t flags)
{
	flags = ShaderFlags::sanitize(flags & ShaderFlags::vs_mask);

	const auto it = vertex_shaders.find(flags);

	if (it != vertex_shaders.end())
	{
		return it->second;
	}

	PrintDebug(__FUNCTION__ " compiling vs: %04X (total: %u)\n", flags, vertex_shaders.size() + 1);

	auto preproc = shader_preprocess(flags);

	ComPtr<ID3DBlob> errors;
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3D11VertexShader> shader;

	ShaderIncluder includer(this);

	auto path = globals::get_system_path("shader.hlsl");
	const auto& src = get_shader_source(path);

	HRESULT hr = D3DCompile(src.data(), src.size(), path.c_str(), preproc.data(), &includer, "vs_main", "vs_5_0", 0, 0, &blob, &errors);

	if (FAILED(hr))
	{
		const std::string str(static_cast<char*>(errors->GetBufferPointer()), 0, errors->GetBufferSize());
		throw std::runtime_error(str);
	}

	hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader);

	if (FAILED(hr))
	{
		throw std::runtime_error("vertex shader creation failed");
	}

	auto result = VertexShader(shader, blob);
	vertex_shaders[flags] = result;
	return result;
}

PixelShader Direct3DDevice8::get_ps(uint32_t flags)
{
	flags = ShaderFlags::sanitize(flags & ShaderFlags::ps_mask);

	const auto it = pixel_shaders.find(flags);

	if (it != pixel_shaders.end())
	{
		return it->second;
	}

	PrintDebug(__FUNCTION__ " compiling ps: %04X (total: %u)\n", flags, pixel_shaders.size() + 1);

	auto preproc = shader_preprocess(flags);

	ComPtr<ID3DBlob> errors;
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3D11PixelShader> shader;

	ShaderIncluder includer(this);

	auto path = globals::get_system_path("shader.hlsl");
	const auto& src = get_shader_source(path);

	HRESULT hr = D3DCompile(src.data(), src.size(), path.c_str(), preproc.data(), &includer, "ps_main", "ps_5_0", 0, 0, &blob, &errors);

	if (FAILED(hr))
	{
		const std::string str(static_cast<char*>(errors->GetBufferPointer()), 0, errors->GetBufferSize());
		throw std::runtime_error(str);
	}

	hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader);

	if (FAILED(hr))
	{
		throw std::runtime_error("pixel shader creation failed");
	}

	auto result = PixelShader(shader, blob);
	pixel_shaders[flags] = result;
	return result;
}

void Direct3DDevice8::create_depth_stencil()
{
	D3D11_TEXTURE2D_DESC depth_tdesc {};

	const DXGI_FORMAT format = to_dxgi(present_params.AutoDepthStencilFormat);

	depth_tdesc.Width      = present_params.BackBufferWidth;
	depth_tdesc.Height     = present_params.BackBufferHeight;
	depth_tdesc.MipLevels  = 1;
	depth_tdesc.ArraySize  = 1;
	depth_tdesc.Format     = to_typeless(format); // set texture format to typeless so we can make an SRV
	depth_tdesc.SampleDesc = { 1, 0 };
	depth_tdesc.BindFlags  = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

	if (FAILED(device->CreateTexture2D(&depth_tdesc, nullptr, &depth_texture)))
	{
		throw std::runtime_error("failed to create depth texture");
	}

	D3D11_DEPTH_STENCIL_DESC depth_desc {};

	depth_desc.DepthEnable    = TRUE;
	depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depth_desc.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
	depth_desc.FrontFace      = {
		D3D11_STENCIL_OP_KEEP,
		D3D11_STENCIL_OP_KEEP,
		D3D11_STENCIL_OP_KEEP,
		D3D11_COMPARISON_ALWAYS
	};
	depth_desc.BackFace = {
		D3D11_STENCIL_OP_KEEP,
		D3D11_STENCIL_OP_KEEP,
		D3D11_STENCIL_OP_KEEP,
		D3D11_COMPARISON_ALWAYS
	};

	if (FAILED(device->CreateDepthStencilState(&depth_desc, &depth_state_rw)))
	{
		throw std::runtime_error("failed to create rw depth stencil state");
	}

	depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;

	if (FAILED(device->CreateDepthStencilState(&depth_desc, &depth_state_ro)))
	{
		throw std::runtime_error("failed to create ro depth stencil state");
	}

	context->OMSetDepthStencilState(depth_state_rw.Get(), 0);

	D3D11_DEPTH_STENCIL_VIEW_DESC depth_vdesc {};
	depth_vdesc.Format        = typeless_to_depth(depth_tdesc.Format);
	depth_vdesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

	if (FAILED(device->CreateDepthStencilView(depth_texture.Get(), &depth_vdesc, &depth_view)))
	{
		throw std::runtime_error("failed to create depth stencil view");
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc {};

	// create a shader resource view with a readable pixel format
	auto srv_format = typeless_to_float(depth_tdesc.Format);

	// if float didn't work, it's probably int we want
	if (srv_format == DXGI_FORMAT_UNKNOWN)
	{
		srv_format = typeless_to_unorm(depth_tdesc.Format);
	}

	srv_desc.Format                    = srv_format;
	srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Texture2D.MostDetailedMip = 0;
	srv_desc.Texture2D.MipLevels       = 1;

	HRESULT hr = device->CreateShaderResourceView(depth_texture.Get(), &srv_desc, &depth_srv);

	if (FAILED(hr))
	{
		throw std::runtime_error("failed to create depth srv");
	}
}

void Direct3DDevice8::create_render_target()
{
	// get the address of the back buffer
	ID3D11Texture2D* pBackBuffer;
	swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<LPVOID*>(&pBackBuffer));

	D3D11_TEXTURE2D_DESC tex_desc {};
	pBackBuffer->GetDesc(&tex_desc);

	// use the back buffer address to create the render target
	device->CreateRenderTargetView(pBackBuffer, nullptr, &render_target);
	pBackBuffer->Release();

	tex_desc.Usage     = D3D11_USAGE_DEFAULT;
	tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	HRESULT hr = device->CreateTexture2D(&tex_desc, nullptr, &composite_texture);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create composite target texture");
	}

	D3D11_RENDER_TARGET_VIEW_DESC view_desc {};

	view_desc.Format             = tex_desc.Format;
	view_desc.ViewDimension      = D3D11_RTV_DIMENSION_TEXTURE2D;
	view_desc.Texture2D.MipSlice = 0;

	hr = device->CreateRenderTargetView(composite_texture.Get(), &view_desc, &composite_view);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create composite target view");
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc {};

	srv_desc.Format                    = tex_desc.Format;
	srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Texture2D.MostDetailedMip = 0;
	srv_desc.Texture2D.MipLevels       = 1;

	hr = device->CreateShaderResourceView(composite_texture.Get(), &srv_desc, &composite_srv);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create composite resource view");
	}

	// set the composite render target as the back buffer
	context->OMSetRenderTargets(1, composite_view.GetAddressOf(), depth_view.Get());
}

void Direct3DDevice8::create_native()
{
	if (!present_params.EnableAutoDepthStencil)
	{
		throw std::runtime_error("manual depth buffer not supported");
	}

	palette_flag = supports_palettes();

	DXGI_SWAP_CHAIN_DESC desc = {};

	desc.BufferCount       = 1;
	desc.BufferDesc.Format = to_dxgi(present_params.BackBufferFormat);
	desc.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferDesc.Width  = present_params.BackBufferWidth;
	desc.BufferDesc.Height = present_params.BackBufferHeight;
	desc.OutputWindow      = present_params.hDeviceWindow;
	desc.SampleDesc.Count  = 1;
	desc.Windowed          = present_params.Windowed;
	desc.Flags             = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	auto feature_level = static_cast<D3D_FEATURE_LEVEL>(0);

#ifdef _DEBUG
	constexpr auto flag = D3D11_CREATE_DEVICE_DEBUG;
#else
	constexpr auto flag = 0;
#endif

	auto error = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flag,
	                                           FEATURE_LEVELS, 2,
	                                           D3D11_SDK_VERSION, &desc, &swap_chain,
	                                           &device, &feature_level, &context);

	if (feature_level < D3D_FEATURE_LEVEL_11_0)
	{
		std::string msg = "Device does not meet the minimum required feature level (D3D_FEATURE_LEVEL_11_0).";
		msg.append("\r\nThis mod utilizes interlocked operations which are only available in DirectX 11.X and Shader Model 5.X.");

		throw std::runtime_error(msg.c_str());
	}

	if (error != S_OK)
	{
		throw std::runtime_error("Device creation failed with a known error that I'm too lazy to get the details of.");
	}

	device->QueryInterface(__uuidof(ID3D11InfoQueue), &info_queue);

	if (info_queue)
	{
		PrintDebug("D3D11 debug info queue enabled\n");
		info_queue->SetMuteDebugOutput(FALSE);
	}

	swap_chain->SetFullscreenState(!present_params.Windowed, nullptr);

	D3D11_RASTERIZER_DESC raster {};

	raster.FillMode        = D3D11_FILL_SOLID;
	raster.CullMode        = D3D11_CULL_NONE;
	raster.DepthClipEnable = TRUE;

	if (FAILED(device->CreateRasterizerState(&raster, &raster_state)))
	{
		throw std::runtime_error("failed to create rasterizer state");
	}

	context->RSSetState(raster_state.Get());

	create_depth_stencil();
	create_render_target();

	D3DVIEWPORT8 vp {};
	vp.Width  = present_params.BackBufferWidth;
	vp.Height = present_params.BackBufferHeight;
	vp.MaxZ   = 1.0f;
	SetViewport(&vp);

	D3D11_BUFFER_DESC cbuf_desc {};

	auto cbuffer_size = per_scene.cbuffer_size();

	cbuf_desc.ByteWidth           = int_multiple(cbuffer_size, 16);
	cbuf_desc.Usage               = D3D11_USAGE_DYNAMIC;
	cbuf_desc.BindFlags           = D3D11_BIND_CONSTANT_BUFFER;
	cbuf_desc.CPUAccessFlags      = D3D11_CPU_ACCESS_WRITE;
	cbuf_desc.StructureByteStride = cbuffer_size;

	HRESULT hr = device->CreateBuffer(&cbuf_desc, nullptr, &per_scene_cbuf);
	if (FAILED(hr))
	{
		throw std::runtime_error("per-scene CreateBuffer failed");
	}

	cbuffer_size = per_model.cbuffer_size();

	cbuf_desc.ByteWidth           = int_multiple(cbuffer_size, 16);
	cbuf_desc.StructureByteStride = cbuffer_size;

	hr = device->CreateBuffer(&cbuf_desc, nullptr, &per_model_cbuf);
	if (FAILED(hr))
	{
		throw std::runtime_error("per-model CreateBuffer failed");
	}

	cbuffer_size = per_pixel.cbuffer_size();

	cbuf_desc.ByteWidth           = int_multiple(cbuffer_size, 16);
	cbuf_desc.StructureByteStride = cbuffer_size;

	hr = device->CreateBuffer(&cbuf_desc, nullptr, &per_pixel_cbuf);
	if (FAILED(hr))
	{
		throw std::runtime_error("per-pixel CreateBuffer failed");
	}

	context->VSSetConstantBuffers(0, 1, per_scene_cbuf.GetAddressOf());
	context->PSSetConstantBuffers(0, 1, per_scene_cbuf.GetAddressOf());

	context->VSSetConstantBuffers(1, 1, per_model_cbuf.GetAddressOf());
	context->PSSetConstantBuffers(1, 1, per_model_cbuf.GetAddressOf());

	context->VSSetConstantBuffers(2, 1, per_pixel_cbuf.GetAddressOf());
	context->PSSetConstantBuffers(2, 1, per_pixel_cbuf.GetAddressOf());

#ifndef _DEBUG
	PrintDebug("precompiling shaders...\n");
	for (uint32_t i = 0; i <= ShaderFlags::count; ++i)
	{
		VertexShader vs;
		PixelShader ps;

		compile_shaders(i, vs, ps);
	}
	printf("done\n");
#endif

	oit_load_shaders();
	oit_init();
}

uint32_t ShaderFlags::sanitize(uint32_t flags)
{
	flags &= mask;

	if (flags & tci_envmap && (!(flags & fvf_tex1) || !(flags & fvf_normal)))
	{
		flags &= ~tci_envmap;
	}

	if (flags & rs_lighting && !(flags & fvf_normal))
	{
		flags &= ~rs_lighting;
	}

	if (flags & rs_specular && !(flags & rs_lighting))
	{
		flags &= ~rs_specular;
	}

	if (flags & oit && !(flags & rs_alpha))
	{
		flags &= ~oit;
	}

	return flags;
}

SamplerSettings::SamplerSettings()
{
	address_u      = D3DTADDRESS_WRAP;
	address_v      = D3DTADDRESS_WRAP;
	address_w      = D3DTADDRESS_WRAP;
	filter_mag     = D3DTEXF_POINT;
	filter_min     = D3DTEXF_POINT;
	filter_mip     = D3DTEXF_NONE;
	mip_lod_bias   = 0.0f;
	max_mip_level  = 0;
	max_anisotropy = 1;
}

bool SamplerSettings::operator==(const SamplerSettings& s) const
{
	return
		address_u.data()      == s.address_u.data() &&
		address_v.data()      == s.address_v.data() &&
		address_w.data()      == s.address_w.data() &&
		filter_mag.data()     == s.filter_mag.data() &&
		filter_min.data()     == s.filter_min.data() &&
		filter_mip.data()     == s.filter_mip.data() &&
		mip_lod_bias.data()   == s.mip_lod_bias.data() &&
		max_mip_level.data()  == s.max_mip_level.data() &&
		max_anisotropy.data() == s.max_anisotropy.data();
}

bool SamplerSettings::dirty() const
{
	return
		address_u.dirty() ||
		address_v.dirty() ||
		address_w.dirty() ||
		filter_mag.dirty() ||
		filter_min.dirty() ||
		filter_mip.dirty() ||
		mip_lod_bias.dirty() ||
		max_mip_level.dirty() ||
		max_anisotropy.dirty();
}

void SamplerSettings::clear()
{
	address_u.clear();
	address_v.clear();
	address_w.clear();
	filter_mag.clear();
	filter_min.clear();
	filter_mip.clear();
	mip_lod_bias.clear();
	max_mip_level.clear();
	max_anisotropy.clear();
}

void SamplerSettings::mark()
{
	address_u.mark();
	address_v.mark();
	address_w.mark();
	filter_mag.mark();
	filter_min.mark();
	filter_mip.mark();
	mip_lod_bias.mark();
	max_mip_level.mark();
	max_anisotropy.mark();
}

// IDirect3DDevice8
Direct3DDevice8::Direct3DDevice8(Direct3D8* d3d, const D3DPRESENT_PARAMETERS8& parameters)
	: present_params(parameters),
	  d3d(d3d)
{
	fragments_str = std::to_string(globals::max_fragments);

	blend_flags = BLEND_DEFAULT;

	render_state_values[D3DRS_ZENABLE]          = TRUE;
	render_state_values[D3DRS_ZWRITEENABLE]     = TRUE;
	render_state_values[D3DRS_FILLMODE]         = D3DFILL_SOLID;
	render_state_values[D3DRS_SRCBLEND]         = D3DBLEND_ONE;
	render_state_values[D3DRS_DESTBLEND]        = D3DBLEND_ZERO;
	render_state_values[D3DRS_CULLMODE]         = D3DCULL_CCW;
	render_state_values[D3DRS_ZFUNC]            = D3DCMP_LESS;
	render_state_values[D3DRS_ALPHAFUNC]        = D3DCMP_ALWAYS;
	render_state_values[D3DRS_ALPHABLENDENABLE] = FALSE;
	render_state_values[D3DRS_FOGENABLE]        = FALSE;
	render_state_values[D3DRS_SPECULARENABLE]   = TRUE;
	render_state_values[D3DRS_LIGHTING]         = TRUE;
	render_state_values[D3DRS_COLORVERTEX]      = TRUE;
	render_state_values[D3DRS_LOCALVIEWER]      = TRUE;
	render_state_values[D3DRS_BLENDOP]          = D3DBLENDOP_ADD;

	per_scene.mark();
	per_model.mark();
	per_pixel.mark();
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::QueryInterface(REFIID riid, void** ppvObj)
{
	if (ppvObj == nullptr)
	{
		return E_POINTER;
	}

	if (riid == __uuidof(this) ||
	    riid == __uuidof(IUnknown))
	{
		AddRef();

		*ppvObj = this;

		return S_OK;
	}

	return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Direct3DDevice8::AddRef()
{
	return Unknown::AddRef();
}

ULONG STDMETHODCALLTYPE Direct3DDevice8::Release()
{
	ULONG LastRefCount = Unknown::Release();

	if (LastRefCount == 0)
	{
		delete this;
	}

	return LastRefCount;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::TestCooperativeLevel()
{
	// TODO
	return D3DERR_INVALIDCALL;
	//return ProxyInterface->TestCooperativeLevel();
}

UINT STDMETHODCALLTYPE Direct3DDevice8::GetAvailableTextureMem()
{
#if 1
	return UINT_MAX;
#else
	return ProxyInterface->GetAvailableTextureMem();
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::ResourceManagerDiscardBytes(DWORD Bytes)
{
	UNREFERENCED_PARAMETER(Bytes);

#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->EvictManagedResources();
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDirect3D(Direct3D8** ppD3D8)
{
	if (ppD3D8 == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	d3d->AddRef();

	*ppD3D8 = d3d;

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDeviceCaps(D3DCAPS8* pCaps)
{
	if (!pCaps)
	{
		return D3DERR_INVALIDCALL;
	}

	// TODO
	*pCaps = {};

	pCaps->MaxTextureWidth  = UINT_MAX;
	pCaps->MaxTextureHeight = UINT_MAX;

	pCaps->Caps2                    = 0xFFFFFFFF;
	pCaps->Caps3                    = 0xFFFFFFFF;
	pCaps->PresentationIntervals    = 0xFFFFFFFF;
	pCaps->DevCaps                  = 0xFFFFFFFF;
	pCaps->PrimitiveMiscCaps        = 0xFFFFFFFF;
	pCaps->RasterCaps               = 0xFFFFFFFF;
	pCaps->ZCmpCaps                 = 0xFFFFFFFF;
	pCaps->SrcBlendCaps             = 0xFFFFFFFF;
	pCaps->DestBlendCaps            = 0xFFFFFFFF;
	pCaps->AlphaCmpCaps             = 0xFFFFFFFF;
	pCaps->ShadeCaps                = 0xFFFFFFFF;
	pCaps->TextureCaps              = D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_ALPHA;
	pCaps->TextureFilterCaps        = 0xFFFFFFFF;
	pCaps->CubeTextureFilterCaps    = 0xFFFFFFFF;
	pCaps->VolumeTextureFilterCaps  = 0xFFFFFFFF;
	pCaps->TextureAddressCaps       = 0xFFFFFFFF;
	pCaps->VolumeTextureAddressCaps = 0xFFFFFFFF;
	pCaps->LineCaps                 = 0xFFFFFFFF;
	pCaps->MaxTextureRepeat         = 0xFFFFFFFF;
	pCaps->MaxTextureAspectRatio    = 0xFFFFFFFF;
	pCaps->MaxAnisotropy            = 16;
	pCaps->StencilCaps              = 0xFFFFFFFF;
	pCaps->FVFCaps                  = 0xFFFFFFFF;
	pCaps->TextureOpCaps            = 0xFFFFFFFF;
	pCaps->MaxActiveLights          = LIGHT_COUNT;

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDisplayMode(D3DDISPLAYMODE* pMode)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->GetDisplayMode(0, pMode);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->GetCreationParameters(pParameters);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, Direct3DSurface8* pCursorBitmap)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (pCursorBitmap == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap->GetProxyInterface());
#endif
}

void STDMETHODCALLTYPE Direct3DDevice8::SetCursorPosition(UINT XScreenSpace, UINT YScreenSpace, DWORD Flags)
{
	// not required for SADX
#if 0
	ProxyInterface->SetCursorPosition(XScreenSpace, YScreenSpace, Flags);
#endif
}

BOOL STDMETHODCALLTYPE Direct3DDevice8::ShowCursor(BOOL bShow)
{
#if 1
	// not required for SADX
	return FALSE;
#else
	return ProxyInterface->ShowCursor(bShow);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS8* pPresentationParameters, Direct3DSwapChain8** ppSwapChain)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::CreateAdditionalSwapChain" << "(" << this << ", " << pPresentationParameters << ", " << ppSwapChain << ")' ..." << std::endl;
#endif

	if (pPresentationParameters == nullptr || ppSwapChain == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppSwapChain = nullptr;

	D3DPRESENT_PARAMETERS PresentParams;
	ConvertPresentParameters(*pPresentationParameters, PresentParams);

	// Get multisample quality level
	if (PresentParams.MultiSampleType != D3DMULTISAMPLE_NONE)
	{
		DWORD QualityLevels = 0;
		D3DDEVICE_CREATION_PARAMETERS CreationParams;
		ProxyInterface->GetCreationParameters(&CreationParams);

		if (d3d->GetProxyInterface()->CheckDeviceMultiSampleType(CreationParams.AdapterOrdinal,
			CreationParams.DeviceType, PresentParams.BackBufferFormat, PresentParams.Windowed,
			PresentParams.MultiSampleType, &QualityLevels) == S_OK &&
			d3d->GetProxyInterface()->CheckDeviceMultiSampleType(CreationParams.AdapterOrdinal,
				CreationParams.DeviceType, PresentParams.AutoDepthStencilFormat, PresentParams.Windowed,
				PresentParams.MultiSampleType, &QualityLevels) == S_OK)
		{
			PresentParams.MultiSampleQuality = (QualityLevels != 0) ? QualityLevels - 1 : 0;
		}
	}

	IDirect3DSwapChain9 *SwapChainInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateAdditionalSwapChain(&PresentParams, &SwapChainInterface);

	if (FAILED(hr))
	{
		return hr;
	}

	*ppSwapChain = new Direct3DSwapChain8(this, SwapChainInterface);

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::Reset(D3DPRESENT_PARAMETERS8* pPresentationParameters)
{
	if (!pPresentationParameters)
	{
		return D3DERR_INVALIDCALL;
	}

	// TODO: handle actual device lost state
	// TODO: handle/fix fullscreen toggle

	if (pPresentationParameters->BackBufferWidth != present_params.BackBufferWidth ||
	    pPresentationParameters->BackBufferHeight != present_params.BackBufferHeight ||
	    pPresentationParameters->Windowed != present_params.Windowed)
	{
		present_params = *pPresentationParameters;
		render_target = nullptr;

		swap_chain->ResizeBuffers(0, present_params.BackBufferWidth, present_params.BackBufferHeight,
		                          DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);

		create_depth_stencil();
		create_render_target();

		D3DVIEWPORT8 vp {};
		GetViewport(&vp);

		vp.Width  = present_params.BackBufferWidth;
		vp.Height = present_params.BackBufferHeight;

		SetViewport(&vp);

		oit_init();
	}

	return D3D_OK;
}

void Direct3DDevice8::oit_composite()
{
	if (!oit_actually_enabled)
	{
		return;
	}

	auto blend_ = blend_flags.data();
	blend_flags = BLEND_DEFAULT;
	update_blend();

	// Unbinds UAV read/write buffers and binds their read-only
	// shader resource views.
	oit_read();
	// Switches to the composite to begin the sorting process.
	context->PSSetShader(composite_ps.shader.Get(), nullptr, 0);
	context->VSSetShader(composite_vs.shader.Get(), nullptr, 0);

	// Unbind the last vertex & index buffers (one of the cubes)...
	context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// ...then draw 3 points. The composite shader uses SV_VertexID
	// to generate a full screen triangle, so we don't need a buffer!
	context->Draw(3, 0);

	blend_flags = blend_;
	update_blend();
}

void Direct3DDevice8::oit_start()
{
	if (!oit_enabled && oit_actually_enabled != oit_enabled)
	{
		oit_actually_enabled = oit_enabled;
		oit_write();
	}

	oit_actually_enabled = oit_enabled;

	if (oit_actually_enabled)
	{
		// Restore R/W access to the UAV buffers from the shader.
		oit_write();
	}
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::Present(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion)
{
	print_info_queue();
	UNREFERENCED_PARAMETER(pDirtyRegion);

	oit_start();

	auto interval = present_params.FullScreen_PresentationInterval;

	if (interval == D3DPRESENT_INTERVAL_IMMEDIATE)
	{
		interval = 0;
	}

	try
	{
		if (FAILED(swap_chain->Present(interval, 0)))
		{
			return D3DERR_INVALIDCALL;
		}
	}
	catch (std::exception&)
	{
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, Direct3DSurface8** ppBackBuffer)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (ppBackBuffer == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppBackBuffer = nullptr;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetBackBuffer(0, iBackBuffer, Type, &SurfaceInterface);

	if (FAILED(hr))
	{
		return hr;
	}

	*ppBackBuffer = address_table->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetRasterStatus(D3DRASTER_STATUS* pRasterStatus)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->GetRasterStatus(0, pRasterStatus);
#endif
}

void STDMETHODCALLTYPE Direct3DDevice8::SetGammaRamp(DWORD Flags, const D3DGAMMARAMP* pRamp)
{
#if 1
	// not required for SADX
	return;
#else
	ProxyInterface->SetGammaRamp(0, Flags, pRamp);
#endif
}

void STDMETHODCALLTYPE Direct3DDevice8::GetGammaRamp(D3DGAMMARAMP* pRamp)
{
#if 1
	// not required for SADX
	return;
#else
	ProxyInterface->GetGammaRamp(0, pRamp);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, Direct3DTexture8** ppTexture)
{
	if (ppTexture == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppTexture = nullptr;

	if (Pool == D3DPOOL_DEFAULT)
	{
	#if 0
		D3DDEVICE_CREATION_PARAMETERS CreationParams;
		ProxyInterface->GetCreationParameters(&CreationParams);

		if ((Usage & D3DUSAGE_DYNAMIC) == 0 &&
			SUCCEEDED(d3d->GetProxyInterface()->CheckDeviceFormat(CreationParams.AdapterOrdinal, CreationParams.DeviceType, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, Format)))
		{
			Usage |= D3DUSAGE_RENDERTARGET;
		}
		else
	#endif
		{
			Usage |= D3DUSAGE_DYNAMIC;
		}
	}

	auto result = new Direct3DTexture8(this, Width, Height, Levels, Usage, Format, Pool);

	try
	{
		result->create_native();
		*ppTexture = result;
	}
	catch (std::exception& ex)
	{
		delete result;
		PrintDebug(__FUNCTION__ " %s\n", ex.what());
		print_info_queue();
		return D3DERR_INVALIDCALL;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, Direct3DVolumeTexture8** ppVolumeTexture)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (ppVolumeTexture == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppVolumeTexture = nullptr;

	IDirect3DVolumeTexture9 *TextureInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, &TextureInterface, nullptr);

	if (FAILED(hr))
	{
		return hr;
	}

	*ppVolumeTexture = new Direct3DVolumeTexture8(this, TextureInterface);

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, Direct3DCubeTexture8** ppCubeTexture)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (ppCubeTexture == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppCubeTexture = nullptr;

	IDirect3DCubeTexture9 *TextureInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, &TextureInterface, nullptr);

	if (FAILED(hr))
	{
		return hr;
	}

	*ppCubeTexture = new Direct3DCubeTexture8(this, TextureInterface);

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, Direct3DVertexBuffer8** ppVertexBuffer)
{
	if (ppVertexBuffer == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppVertexBuffer = nullptr;
	auto result = new Direct3DVertexBuffer8(this, Length, Usage, FVF, Pool);

	try
	{
		result->create_native();
		*ppVertexBuffer = result;
	}
	catch (std::exception& ex)
	{
		delete result;
		PrintDebug(__FUNCTION__ " %s\n", ex.what());
		print_info_queue();
		return D3DERR_INVALIDCALL;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, Direct3DIndexBuffer8** ppIndexBuffer)
{
	if (ppIndexBuffer == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppIndexBuffer = nullptr;
	auto result = new Direct3DIndexBuffer8(this, Length, Usage, Format, Pool);

	try
	{
		result->create_native();
		*ppIndexBuffer = result;
	}
	catch (std::exception& ex)
	{
		delete result;
		PrintDebug(__FUNCTION__ " %s\n", ex.what());
		print_info_queue();
		return D3DERR_INVALIDCALL;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, Direct3DSurface8** ppSurface)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (ppSurface == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppSurface = nullptr;

	DWORD QualityLevels = 0;

	// Get multisample quality level
	if (MultiSample != D3DMULTISAMPLE_NONE)
	{
		D3DDEVICE_CREATION_PARAMETERS CreationParams;
		ProxyInterface->GetCreationParameters(&CreationParams);

		d3d->GetProxyInterface()->CheckDeviceMultiSampleType(CreationParams.AdapterOrdinal, CreationParams.DeviceType, Format, FALSE, MultiSample, &QualityLevels);
		QualityLevels = (QualityLevels != 0) ? QualityLevels - 1 : 0;
	}

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	HRESULT hr = ProxyInterface->CreateRenderTarget(Width, Height, Format, MultiSample, QualityLevels, Lockable, &SurfaceInterface, nullptr);

	if (FAILED(hr))
	{
		return hr;
	}

	*ppSurface = new Direct3DSurface8(this, SurfaceInterface);

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, Direct3DSurface8** ppSurface)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (ppSurface == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppSurface = nullptr;

	DWORD QualityLevels = 0;

	// Get multisample quality level
	if (MultiSample != D3DMULTISAMPLE_NONE)
	{
		D3DDEVICE_CREATION_PARAMETERS CreationParams;
		ProxyInterface->GetCreationParameters(&CreationParams);

		d3d->GetProxyInterface()->CheckDeviceMultiSampleType(CreationParams.AdapterOrdinal, CreationParams.DeviceType, Format, FALSE, MultiSample, &QualityLevels);
		QualityLevels = (QualityLevels != 0) ? QualityLevels - 1 : 0;
	}

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	HRESULT hr = ProxyInterface->CreateDepthStencilSurface(Width, Height, Format, MultiSample, QualityLevels, ZBufferDiscarding, &SurfaceInterface, nullptr);

	if (FAILED(hr))
	{
		return hr;
	}

	*ppSurface = new Direct3DSurface8(this, SurfaceInterface);

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateImageSurface(UINT Width, UINT Height, D3DFORMAT Format, Direct3DSurface8** ppSurface)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::CreateImageSurface" << "(" << this << ", " << Width << ", " << Height << ", " << Format << ", " << ppSurface << ")' ..." << std::endl;
#endif

	if (ppSurface == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppSurface = nullptr;

	if (Format == D3DFMT_R8G8B8)
	{
	#ifndef D3D8TO9NOLOG
		LOG << "> Replacing format 'D3DFMT_R8G8B8' with 'D3DFMT_X8R8G8B8' ..." << std::endl;
	#endif

		Format = D3DFMT_X8R8G8B8;
	}

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateOffscreenPlainSurface(Width, Height, Format, D3DPOOL_SYSTEMMEM, &SurfaceInterface, nullptr);

	if (FAILED(hr))
	{
	#ifndef D3D8TO9NOLOG
		LOG << "> 'IDirect3DDevice9::CreateOffscreenPlainSurface' failed with error code " << std::hex << hr << std::dec << "!" << std::endl;
	#endif

		return hr;
	}

	*ppSurface = new Direct3DSurface8(this, SurfaceInterface);

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CopyRects(Direct3DSurface8* pSourceSurface, const RECT* pSourceRectsArray, UINT cRects, Direct3DSurface8* pDestinationSurface, const POINT* pDestPointsArray)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (pSourceSurface == nullptr || pDestinationSurface == nullptr || pSourceSurface == pDestinationSurface)
	{
		return D3DERR_INVALIDCALL;
	}

	D3DSURFACE_DESC SourceDesc, DestinationDesc;
	pSourceSurface->GetProxyInterface()->GetDesc(&SourceDesc);
	pDestinationSurface->GetProxyInterface()->GetDesc(&DestinationDesc);

	if (SourceDesc.Format != DestinationDesc.Format)
	{
		return D3DERR_INVALIDCALL;
	}

	HRESULT hr = D3DERR_INVALIDCALL;

	if (cRects == 0)
	{
		cRects = 1;
	}

	for (UINT i = 0; i < cRects; i++)
	{
		RECT SourceRect, DestinationRect;

		if (pSourceRectsArray != nullptr)
		{
			SourceRect = pSourceRectsArray[i];
		}
		else
		{
			SourceRect.left = 0;
			SourceRect.right = SourceDesc.Width;
			SourceRect.top = 0;
			SourceRect.bottom = SourceDesc.Height;
		}

		if (pDestPointsArray != nullptr)
		{
			DestinationRect.left = pDestPointsArray[i].x;
			DestinationRect.right = DestinationRect.left + (SourceRect.right - SourceRect.left);
			DestinationRect.top = pDestPointsArray[i].y;
			DestinationRect.bottom = DestinationRect.top + (SourceRect.bottom - SourceRect.top);
		}
		else
		{
			DestinationRect = SourceRect;
		}

		if (SourceDesc.Pool == D3DPOOL_MANAGED || DestinationDesc.Pool != D3DPOOL_DEFAULT)
		{
			if (D3DXLoadSurfaceFromSurface != nullptr)
			{
				hr = D3DXLoadSurfaceFromSurface(pDestinationSurface->GetProxyInterface(), nullptr, &DestinationRect, pSourceSurface->GetProxyInterface(), nullptr, &SourceRect, D3DX_FILTER_NONE, 0);
			}
			else
			{
				hr = D3DERR_INVALIDCALL;
			}
		}
		else if (SourceDesc.Pool == D3DPOOL_DEFAULT)
		{
			hr = ProxyInterface->StretchRect(pSourceSurface->GetProxyInterface(), &SourceRect, pDestinationSurface->GetProxyInterface(), &DestinationRect, D3DTEXF_NONE);
		}
		else if (SourceDesc.Pool == D3DPOOL_SYSTEMMEM)
		{
			const POINT pt = { DestinationRect.left, DestinationRect.top };

			hr = ProxyInterface->UpdateSurface(pSourceSurface->GetProxyInterface(), &SourceRect, pDestinationSurface->GetProxyInterface(), &pt);
		}

		if (FAILED(hr))
		{
		#ifndef D3D8TO9NOLOG
			LOG << "Failed to translate 'IDirect3DDevice8::CopyRects' call from '[" << SourceDesc.Width << "x" << SourceDesc.Height << ", " << SourceDesc.Format << ", " << SourceDesc.MultiSampleType << ", " << SourceDesc.Usage << ", " << SourceDesc.Pool << "]' to '[" << DestinationDesc.Width << "x" << DestinationDesc.Height << ", " << DestinationDesc.Format << ", " << DestinationDesc.MultiSampleType << ", " << DestinationDesc.Usage << ", " << DestinationDesc.Pool << "]'!" << std::endl;
		#endif
			break;
		}
	}

	return hr;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::UpdateTexture(Direct3DBaseTexture8* pSourceTexture, Direct3DBaseTexture8* pDestinationTexture)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (pSourceTexture == nullptr || pDestinationTexture == nullptr || pSourceTexture->GetType() != pDestinationTexture->GetType())
	{
		return D3DERR_INVALIDCALL;
	}

	IDirect3DBaseTexture9 *SourceBaseTextureInterface, *DestinationBaseTextureInterface;

	switch (pSourceTexture->GetType())
	{
		case D3DRTYPE_TEXTURE:
			SourceBaseTextureInterface = static_cast<Direct3DTexture8 *>(pSourceTexture)->GetProxyInterface();
			DestinationBaseTextureInterface = static_cast<Direct3DTexture8 *>(pDestinationTexture)->GetProxyInterface();
			break;
		case D3DRTYPE_VOLUMETEXTURE:
			SourceBaseTextureInterface = static_cast<Direct3DVolumeTexture8 *>(pSourceTexture)->GetProxyInterface();
			DestinationBaseTextureInterface = static_cast<Direct3DVolumeTexture8 *>(pDestinationTexture)->GetProxyInterface();
			break;
		case D3DRTYPE_CUBETEXTURE:
			SourceBaseTextureInterface = static_cast<Direct3DCubeTexture8 *>(pSourceTexture)->GetProxyInterface();
			DestinationBaseTextureInterface = static_cast<Direct3DCubeTexture8 *>(pDestinationTexture)->GetProxyInterface();
			break;
		default:
			return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->UpdateTexture(SourceBaseTextureInterface, DestinationBaseTextureInterface);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetFrontBuffer(Direct3DSurface8* pDestSurface)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (pDestSurface == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->GetFrontBufferData(0, pDestSurface->GetProxyInterface());
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetRenderTarget(Direct3DSurface8* pRenderTarget, Direct3DSurface8* pNewZStencil)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	HRESULT hr;

	if (pRenderTarget != nullptr)
	{
		hr = ProxyInterface->SetRenderTarget(0, pRenderTarget->GetProxyInterface());

		if (FAILED(hr))
		{
			return hr;
		}
	}

	if (pNewZStencil != nullptr)
	{
		hr = ProxyInterface->SetDepthStencilSurface(pNewZStencil->GetProxyInterface());

		if (FAILED(hr))
		{
			return hr;
		}
	}
	else
	{
		ProxyInterface->SetDepthStencilSurface(nullptr);
	}

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetRenderTarget(Direct3DSurface8** ppRenderTarget)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (ppRenderTarget == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetRenderTarget(0, &SurfaceInterface);

	if (FAILED(hr))
	{
		return hr;
	}

	*ppRenderTarget = address_table->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDepthStencilSurface(Direct3DSurface8** ppZStencilSurface)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (ppZStencilSurface == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetDepthStencilSurface(&SurfaceInterface);

	if (FAILED(hr))
	{
		return hr;
	}

	*ppZStencilSurface = address_table->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::BeginScene()
{
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::EndScene()
{
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
	if (Flags & D3DCLEAR_TARGET)
	{
		float color[] = {
			((Color >> 16) & 0xFF) / 255.0f,
			((Color >> 8) & 0xFF) / 255.0f,
			(Color & 0xFF) / 255.0f,
			((Color >> 24) & 0xFF) / 255.0f,
		};

		context->ClearRenderTargetView(composite_view.Get(), color);
		context->ClearRenderTargetView(render_target.Get(), color);
	}

	if (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))
	{
		uint32_t flags = 0;

		if (Flags & D3DCLEAR_ZBUFFER)
		{
			flags |= D3D11_CLEAR_DEPTH;
		}

		if (Flags & D3DCLEAR_STENCIL)
		{
			flags |= D3D11_CLEAR_STENCIL;
		}

		context->ClearDepthStencilView(depth_view.Get(), flags, Z, static_cast<uint8_t>(Stencil));
	}

	return D3D_OK;
}

void Direct3DDevice8::update_wv_inv_t()
{
	matrix m = (per_model.worldMatrix.data() * per_scene.viewMatrix.data()).Invert();

	// don't need to transpose for reasons
	per_model.wvMatrixInvT = m;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetTransform(D3DTRANSFORMSTATETYPE State, const matrix* pMatrix)
{
	if (!pMatrix)
	{
		return D3DERR_INVALIDCALL;
	}

	switch (static_cast<uint32_t>(State))
	{
		case D3DTS_VIEW:
			per_scene.viewMatrix = *pMatrix;
			update_wv_inv_t();
			break;

		case D3DTS_PROJECTION:
			per_scene.projectionMatrix = *pMatrix;
			break;

		case D3DTS_TEXTURE0:
			per_model.textureMatrix = *pMatrix;
			break;

		case D3DTS_WORLD:
			per_model.worldMatrix = *pMatrix;
			update_wv_inv_t();
			break;

		default:
			return D3DERR_INVALIDCALL;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetTransform(D3DTRANSFORMSTATETYPE State, matrix* pMatrix)
{
	if (!pMatrix)
	{
		return D3DERR_INVALIDCALL;
	}

	switch (static_cast<uint32_t>(State))
	{
		case D3DTS_VIEW:
			*pMatrix = per_scene.viewMatrix;
			break;

		case D3DTS_PROJECTION:
			*pMatrix = per_scene.projectionMatrix;
			break;

		case D3DTS_TEXTURE0:
			*pMatrix = per_model.textureMatrix;
			break;

		case D3DTS_WORLD:
			*pMatrix = per_model.worldMatrix;
			break;

		default:
			return D3DERR_INVALIDCALL;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const matrix* pMatrix)
{
	if (!pMatrix)
	{
		return D3DERR_INVALIDCALL;
	}

	switch (static_cast<uint32_t>(State))
	{
		case D3DTS_VIEW:
			per_scene.viewMatrix = per_scene.viewMatrix * *pMatrix;
			break;

		case D3DTS_PROJECTION:
			per_scene.projectionMatrix = per_scene.projectionMatrix * *pMatrix;
			break;

		case D3DTS_TEXTURE0:
			per_model.textureMatrix = per_model.textureMatrix * *pMatrix;
			break;

		case D3DTS_WORLD:
			per_model.worldMatrix = per_model.worldMatrix * *pMatrix;
			break;

		default:
			return D3DERR_INVALIDCALL;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetViewport(const D3DVIEWPORT8* pViewport)
{
#if 1
	if (!pViewport)
	{
		return D3DERR_INVALIDCALL;
	}

	viewport.Width    = static_cast<float>(pViewport->Width);
	viewport.Height   = static_cast<float>(pViewport->Height);
	viewport.MaxDepth = pViewport->MaxZ;
	viewport.MinDepth = pViewport->MinZ;
	viewport.TopLeftX = static_cast<float>(pViewport->X);
	viewport.TopLeftY = static_cast<float>(pViewport->Y);

	context->RSSetViewports(1, &viewport);
	return D3D_OK;
#else
	return ProxyInterface->SetViewport(pViewport);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetViewport(D3DVIEWPORT8* pViewport)
{
	if (!pViewport)
	{
		return D3DERR_INVALIDCALL;
	}

	pViewport->Width  = static_cast<DWORD>(viewport.Width);
	pViewport->Height = static_cast<DWORD>(viewport.Height);
	pViewport->MaxZ   = viewport.MaxDepth;
	pViewport->MinZ   = viewport.MinDepth;
	pViewport->X      = static_cast<DWORD>(viewport.TopLeftX);
	pViewport->Y      = static_cast<DWORD>(viewport.TopLeftY);

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetMaterial(const D3DMATERIAL8* pMaterial)
{
	if (!pMaterial)
	{
		return D3DERR_INVALIDCALL;
	}

	material = *pMaterial;
	per_model.material = Material(material);
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetMaterial(D3DMATERIAL8* pMaterial)
{
	if (!pMaterial)
	{
		return D3DERR_INVALIDCALL;
	}

	*pMaterial = material;
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetLight(DWORD Index, const D3DLIGHT8* pLight)
{
	if (!pLight)
	{
		return D3DERR_INVALIDCALL;
	}

	if (Index >= per_model.lights.size())
	{
		return D3DERR_INVALIDCALL;
	}

	per_model.lights[Index] = Light(*pLight);
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetLight(DWORD Index, D3DLIGHT8* pLight)
{
	if (!pLight)
	{
		return D3DERR_INVALIDCALL;
	}

	if (Index >= per_model.lights.size())
	{
		return D3DERR_INVALIDCALL;
	}

	const auto& light = per_model.lights[Index].data();

	pLight->Type         = static_cast<D3DLIGHTTYPE>(light.Type);
	pLight->Diffuse      = { light.Diffuse.x, light.Diffuse.y, light.Diffuse.z, light.Diffuse.w };
	pLight->Specular     = { light.Diffuse.x, light.Diffuse.y, light.Diffuse.z, light.Diffuse.w };
	pLight->Ambient      = { light.Ambient.x, light.Ambient.y, light.Ambient.z, light.Ambient.w };
	pLight->Position     = { light.Position.x, light.Position.y, light.Position.z };
	pLight->Direction    = { light.Direction.x, light.Direction.y, light.Direction.z };
	pLight->Range        = light.Range;
	pLight->Falloff      = light.Falloff;
	pLight->Attenuation0 = light.Attenuation0;
	pLight->Attenuation1 = light.Attenuation1;
	pLight->Attenuation2 = light.Attenuation2;
	pLight->Theta        = light.Theta;
	pLight->Phi          = light.Phi;

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::LightEnable(DWORD Index, BOOL Enable)
{
	if (Index >= per_model.lights.size())
	{
		return D3DERR_INVALIDCALL;
	}

	Light light = per_model.lights[Index].data();
	light.Enabled = Enable == TRUE;
	per_model.lights[Index] = light;
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetLightEnable(DWORD Index, BOOL* pEnable)
{
	if (!pEnable)
	{
		return D3DERR_INVALIDCALL;
	}

	if (Index >= per_model.lights.size())
	{
		return D3DERR_INVALIDCALL;
	}

	*pEnable = per_model.lights[Index].data().Enabled;
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetClipPlane(DWORD Index, const float* pPlane)
{
	if (pPlane == nullptr || Index >= MAX_CLIP_PLANES)
	{
		return D3DERR_INVALIDCALL;
	}

	memcpy(StoredClipPlanes[Index], pPlane, sizeof(StoredClipPlanes[0]));
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetClipPlane(DWORD Index, float* pPlane)
{
	if (pPlane == nullptr || Index >= MAX_CLIP_PLANES)
	{
		return D3DERR_INVALIDCALL;
	}

	memcpy(pPlane, StoredClipPlanes[Index], sizeof(StoredClipPlanes[0]));
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
	switch (static_cast<DWORD>(State))
	{
		case D3DRS_LINEPATTERN:
		case D3DRS_ZVISIBLE:
		case D3DRS_EDGEANTIALIAS:
		case D3DRS_PATCHSEGMENTS:
		case D3DRS_CLIPPLANEENABLE:
		case D3DRS_ZBIAS:
			return D3DERR_INVALIDCALL;

		case D3DRS_SOFTWAREVERTEXPROCESSING:
			return D3D_OK;

		default:
			break;
	}

	if (State < D3DRS_ZENABLE || State > D3DRS_NORMALORDER)
	{
		return D3DERR_INVALIDCALL;
	}

	auto& ref = render_state_values[State];
	ref = Value;

	switch (State)
	{
		default:
			break;

		case D3DRS_ZWRITEENABLE:
			if (!ref.dirty())
			{
				break;
			}

			if (Value)
			{
				context->OMSetDepthStencilState(depth_state_rw.Get(), 0);
			}
			else
			{
				context->OMSetDepthStencilState(depth_state_ro.Get(), 0);
			}

			ref.clear();
			break;

		case D3DRS_DIFFUSEMATERIALSOURCE:
			per_model.diffuseSource = Value;
			break;

		case D3DRS_COLORVERTEX:
			per_model.colorVertex = !!Value;
			break;

		case D3DRS_SRCBLEND:
			blend_flags = (blend_flags.data() & ~0x0F) | Value;
			break;

		case D3DRS_DESTBLEND:
			blend_flags = (blend_flags.data() & ~0xF0) | (Value << 4);
			break;

		case D3DRS_ALPHABLENDENABLE:
			blend_flags = (blend_flags.data() & ~0x8000) | (Value ? 0x8000 : 0);
			break;

		case D3DRS_BLENDOP:
			if (ref.dirty())
			{
				PrintDebug("RS_BLENDOP: %u\n", Value);
			}

			ref.clear();

			blend_flags = (blend_flags.data() & ~0xF00) | (Value << 8);
			break;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue)
{
	if (pValue == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	switch (static_cast<DWORD>(State))
	{
		case D3DRS_LINEPATTERN:
		case D3DRS_ZVISIBLE:
		case D3DRS_EDGEANTIALIAS:
		case D3DRS_PATCHSEGMENTS:
		case D3DRS_CLIPPLANEENABLE:
		case D3DRS_ZBIAS:
			return D3DERR_INVALIDCALL;

		case D3DRS_SOFTWAREVERTEXPROCESSING:
			*pValue = 0;
			return D3D_OK;

		default:
			break;
	}

	if (State < D3DRS_ZENABLE || State > D3DRS_NORMALORDER)
	{
		return D3DERR_INVALIDCALL;
	}

	*pValue = render_state_values[State];
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::BeginStateBlock()
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->BeginStateBlock();
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::EndStateBlock(DWORD* pToken)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (pToken == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->EndStateBlock(reinterpret_cast<IDirect3DStateBlock9 **>(pToken));
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::ApplyStateBlock(DWORD Token)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (Token == 0)
	{
		return D3DERR_INVALIDCALL;
	}

	return reinterpret_cast<IDirect3DStateBlock9 *>(Token)->Apply();
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CaptureStateBlock(DWORD Token)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (Token == 0)
	{
		return D3DERR_INVALIDCALL;
	}

	return reinterpret_cast<IDirect3DStateBlock9 *>(Token)->Capture();
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeleteStateBlock(DWORD Token)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (Token == 0)
	{
		return D3DERR_INVALIDCALL;
	}

	reinterpret_cast<IDirect3DStateBlock9 *>(Token)->Release();

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateStateBlock(D3DSTATEBLOCKTYPE Type, DWORD* pToken)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::CreateStateBlock" << "(" << Type << ", " << pToken << ")' ..." << std::endl;
#endif

	if (pToken == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->CreateStateBlock(Type, reinterpret_cast<IDirect3DStateBlock9 **>(pToken));
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetClipStatus(const D3DCLIPSTATUS8* pClipStatus)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->SetClipStatus(pClipStatus);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetClipStatus(D3DCLIPSTATUS8* pClipStatus)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->GetClipStatus(pClipStatus);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetTexture(DWORD Stage, Direct3DBaseTexture8** ppTexture)
{
	if (ppTexture == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppTexture = nullptr;

	const auto it = texture_stages.find(Stage);

	if (it == texture_stages.end())
	{
		return D3DERR_INVALIDCALL;
	}

	auto texture = it->second;
	texture->AddRef();
	*ppTexture = it->second;
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetTexture(DWORD Stage, Direct3DBaseTexture8* pTexture)
{
	auto it = texture_stages.find(Stage);

	if (pTexture == nullptr)
	{
		context->PSSetShaderResources(Stage, 0, nullptr);

		if (it != texture_stages.end())
		{
			it->second->Release();
			texture_stages.erase(it);
		}

		return D3D_OK;
	}

	switch (pTexture->GetType())
	{
		case D3DRTYPE_TEXTURE:
			break;
		default:
			return D3DERR_INVALIDCALL;
	}

	auto texture = dynamic_cast<Direct3DTexture8*>(pTexture);

	if (it != texture_stages.end())
	{
		it->second->Release();
		it->second = texture;
		texture->AddRef();
	}
	else
	{
		texture_stages[Stage] = texture;
		texture->AddRef();
	}

	context->PSSetShaderResources(Stage, 1, texture->srv.GetAddressOf());
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue)
{
	if (!pValue)
	{
		return D3DERR_INVALIDCALL;
	}

	switch (Type)
	{
		case D3DTSS_ADDRESSU:
			*pValue = sampler_setting_values[Stage].address_u.data();
			break;
		case D3DTSS_ADDRESSV:
			*pValue = sampler_setting_values[Stage].address_v.data();
			break;
		case D3DTSS_MAGFILTER:
			*pValue = sampler_setting_values[Stage].filter_mag.data();
			break;
		case D3DTSS_MINFILTER:
			*pValue = sampler_setting_values[Stage].filter_min.data();
			break;
		case D3DTSS_MIPFILTER:
			*pValue = sampler_setting_values[Stage].filter_mip.data();
			break;
		case D3DTSS_MIPMAPLODBIAS:
			*reinterpret_cast<float*>(pValue) = sampler_setting_values[Stage].mip_lod_bias.data();
			break;
		case D3DTSS_MAXMIPLEVEL:
			*pValue = sampler_setting_values[Stage].max_mip_level.data();
			break;
		case D3DTSS_MAXANISOTROPY:
			*pValue = sampler_setting_values[Stage].max_anisotropy.data();
			break;
		case D3DTSS_ADDRESSW:
			*pValue = sampler_setting_values[Stage].address_u.data();
			break;
		default:
			*pValue = texture_state_values[Stage][Type];
			break;
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
	switch (Type)
	{
		case D3DTSS_ADDRESSU:
			sampler_setting_values[Stage].address_u = static_cast<D3DTEXTUREADDRESS>(Value);
			break;
		case D3DTSS_ADDRESSV:
			sampler_setting_values[Stage].address_v = static_cast<D3DTEXTUREADDRESS>(Value);
			break;
		case D3DTSS_MAGFILTER:
			sampler_setting_values[Stage].filter_mag = static_cast<D3DTEXTUREFILTERTYPE>(Value);
			break;
		case D3DTSS_MINFILTER:
			sampler_setting_values[Stage].filter_min = static_cast<D3DTEXTUREFILTERTYPE>(Value);
			break;
		case D3DTSS_MIPFILTER:
			sampler_setting_values[Stage].filter_mip = static_cast<D3DTEXTUREFILTERTYPE>(Value);
			break;
		case D3DTSS_MIPMAPLODBIAS:
			sampler_setting_values[Stage].mip_lod_bias = *reinterpret_cast<float*>(&Value);
			break;
		case D3DTSS_MAXMIPLEVEL:
			sampler_setting_values[Stage].max_mip_level = Value;
			break;
		case D3DTSS_MAXANISOTROPY:
			sampler_setting_values[Stage].max_anisotropy = Value;
			break;
		case D3DTSS_ADDRESSW:
			sampler_setting_values[Stage].address_w = static_cast<D3DTEXTUREADDRESS>(Value);
			break;
		default:
			texture_state_values[Stage][Type] = Value;
			break;
	}
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::ValidateDevice(DWORD* pNumPasses)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->ValidateDevice(pNumPasses);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetInfo(DWORD DevInfoID, void* pDevInfoStruct, DWORD DevInfoStructSize)
{
	UNREFERENCED_PARAMETER(DevInfoID);
	UNREFERENCED_PARAMETER(pDevInfoStruct);
	UNREFERENCED_PARAMETER(DevInfoStructSize);

#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::GetInfo" << "(" << this << ", " << DevInfoID << ", " << pDevInfoStruct << ", " << DevInfoStructSize << ")' ..." << std::endl;
#endif

	return S_FALSE;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (pEntries == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}
	return ProxyInterface->SetPaletteEntries(PaletteNumber, pEntries);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (pEntries == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}
	return ProxyInterface->GetPaletteEntries(PaletteNumber, pEntries);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetCurrentTexturePalette(UINT PaletteNumber)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (!PaletteFlag)
	{
		return D3DERR_INVALIDCALL;
	}
	return ProxyInterface->SetCurrentTexturePalette(PaletteNumber);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetCurrentTexturePalette(UINT* pPaletteNumber)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (!PaletteFlag)
	{
		return D3DERR_INVALIDCALL;
	}
	return ProxyInterface->GetCurrentTexturePalette(pPaletteNumber);
#endif
}

bool Direct3DDevice8::set_primitive_type(D3DPRIMITIVETYPE PrimitiveType) const
{
	auto t = to_d3d11(PrimitiveType);

	if (t != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)
	{
		context->IASetPrimitiveTopology(t);
		return true;
	}

	return false;
}

bool Direct3DDevice8::primitive_vertex_count(D3DPRIMITIVETYPE PrimitiveType, uint32_t& count)
{
	switch (PrimitiveType)
	{
		case D3DPT_TRIANGLELIST:
			count *= 3;
			break;

		case D3DPT_TRIANGLESTRIP:
			count += 2;
			break;

		case D3DPT_POINTLIST:
			break;

		case D3DPT_LINELIST:
			count *= 2;
			break;

		case D3DPT_LINESTRIP:
			++count;
			break;

		case D3DPT_TRIANGLEFAN:
			//PrintDebug(__FUNCTION__ ": D3DPT_TRIANGLEFAN not implemented\n");
			return false;

		default:
			return false;
	}

	return true;
}

// the other draw function (UP) gets routed through here
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
	if (PrimitiveType == D3DPT_TRIANGLEFAN)
	{
		auto stream = stream_sources[0]; // HACK
		auto& buffer = stream.buffer;

		auto point_count = PrimitiveCount + 2;
		auto stride = buffer->desc8.Size / point_count;
		auto offset = StartVertex * stride;

		BYTE* data;
		buffer->Lock(offset, buffer->desc8.Size - offset, &data, D3DLOCK_DISCARD);
		auto result = DrawPrimitiveUP(PrimitiveType, PrimitiveCount, data, stride);
		buffer->Unlock();
		return result;
	}

	if (!set_primitive_type(PrimitiveType))
	{
		return D3DERR_INVALIDCALL;
	}

	if (!update())
	{
		return D3DERR_INVALIDCALL;
	}

	uint32_t count = PrimitiveCount;

	if (!primitive_vertex_count(PrimitiveType, count))
	{
		return D3DERR_INVALIDCALL;
	}

	auto& alpha = render_state_values[D3DRS_ALPHABLENDENABLE];

	if (alpha.data() == TRUE && (oit_actually_enabled && oit_enabled))
	{
		DWORD ZWRITEENABLE;
		DWORD ZENABLE;

		GetRenderState(D3DRS_ZWRITEENABLE, &ZWRITEENABLE);
		GetRenderState(D3DRS_ZENABLE, &ZENABLE);

		// force zwrite on to enable writing 100% opaque
		// pixels to the real backbuffer and depth buffer.
		SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		SetRenderState(D3DRS_ZENABLE, TRUE); // this does nothing right now, but good practice!

		context->Draw(count, StartVertex);

		SetRenderState(D3DRS_ZWRITEENABLE, ZWRITEENABLE);
		SetRenderState(D3DRS_ZENABLE, ZENABLE);
	}
	else
	{
		context->Draw(count, StartVertex);
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT MinIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{
#if 1
	// TODO
	PrintDebug(__FUNCTION__ " not implemented\n");
	return D3DERR_INVALIDCALL;
#else
	ApplyClipPlanes();
	return ProxyInterface->DrawIndexedPrimitive(PrimitiveType, CurrentBaseVertexIndex, MinIndex, NumVertices, StartIndex, PrimitiveCount);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
	if (!update() || !pVertexStreamZeroData)
	{
		return D3DERR_INVALIDCALL;
	}

	if (PrimitiveType == D3DPT_TRIANGLEFAN)
	{
		const auto data = reinterpret_cast<const uint8_t*>(pVertexStreamZeroData);
		const auto stride = VertexStreamZeroStride;

		trifan_buffer.resize(3 * stride * PrimitiveCount);

		auto buffer = trifan_buffer.data();

		const auto v0 = &data[0];
		auto vx = &data[stride];

		for (size_t i = 0; i < PrimitiveCount; i++)
		{
			// 0
			memcpy(buffer, v0, stride);
			buffer += stride;

			// last (or second from 0) vertex
			memcpy(buffer, vx, stride);
			buffer += stride;

			// next vertex
			vx += stride;
			memcpy(buffer, vx, stride);
			buffer += stride;
		}

		return DrawPrimitiveUP(D3DPT_TRIANGLELIST, PrimitiveCount, trifan_buffer.data(), stride);
	}

	if (!set_primitive_type(PrimitiveType))
	{
		return D3DERR_INVALIDCALL;
	}

	uint32_t count = PrimitiveCount;

	if (!primitive_vertex_count(PrimitiveType, count))
	{
		return D3DERR_INVALIDCALL;
	}

	auto size = count * VertexStreamZeroStride;

	up_get(size);

	if (up_buffer == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	BYTE* ptr;
	up_buffer->Lock(0, size, &ptr, D3DLOCK_DISCARD);

	memcpy(ptr, pVertexStreamZeroData, size);

	up_buffer->Unlock();

	SetStreamSource(0, up_buffer.Get(), VertexStreamZeroStride);

	auto result = DrawPrimitive(PrimitiveType, 0, PrimitiveCount);

	SetStreamSource(0, nullptr, 0);
	up_buffers.push_back(up_buffer);
	up_buffer = nullptr;

	return result;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertexIndices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
#if 1
	// TODO
	return D3DERR_INVALIDCALL;
#else
	ApplyClipPlanes();
	return ProxyInterface->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, Direct3DVertexBuffer8* pDestBuffer, DWORD Flags)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (pDestBuffer == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer->GetProxyInterface(), nullptr, Flags);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateVertexShader(const DWORD* pDeclaration, const DWORD* pFunction, DWORD* pHandle, DWORD Usage)
{
	// not required for SADX
	return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetVertexShader(DWORD Handle)
{
	HRESULT hr;

	if ((Handle & 0x80000000) == 0)
	{
		FVF = Handle;
		//ProxyInterface->SetVertexShader(nullptr);
		//hr = ProxyInterface->SetFVF(Handle);

		auto fvf = Handle;

		shader_flags &= ~ShaderFlags::fvf_mask;

		if (fvf & D3DFVF_XYZRHW)
		{
			shader_flags |= ShaderFlags::fvf_rhw;
		}

		if (fvf & D3DFVF_NORMAL)
		{
			shader_flags |= ShaderFlags::fvf_normal;
		}

		if (fvf & D3DFVF_DIFFUSE)
		{
			shader_flags |= ShaderFlags::fvf_diffuse;
		}

		if (fvf & D3DFVF_TEX1)
		{
			shader_flags |= ShaderFlags::fvf_tex1;
		}

		CurrentVertexShaderHandle = 0;
		hr = D3D_OK;
	}
	else
	{
	#if 1
		// not required for SADX
		hr = D3DERR_INVALIDCALL;
	#else
		const DWORD handleMagic = Handle << 1;
		VertexShaderInfo *const ShaderInfo = reinterpret_cast<VertexShaderInfo *>(handleMagic);

		hr = ProxyInterface->SetVertexShader(ShaderInfo->Shader);
		ProxyInterface->SetVertexDeclaration(ShaderInfo->Declaration);

		CurrentVertexShaderHandle = Handle;
	#endif
	}

	return hr;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShader(DWORD* pHandle)
{
	if (pHandle == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	if (CurrentVertexShaderHandle == 0)
	{
		*pHandle = FVF;
		return D3D_OK;
	}
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	*pHandle = CurrentVertexShaderHandle;

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeleteVertexShader(DWORD Handle)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if ((Handle & 0x80000000) == 0)
	{
		return D3DERR_INVALIDCALL;
	}

	if (CurrentVertexShaderHandle == Handle)
	{
		SetVertexShader(0);
	}

	const DWORD HandleMagic = Handle << 1;
	VertexShaderInfo *const ShaderInfo = reinterpret_cast<VertexShaderInfo *>(HandleMagic);

	if (ShaderInfo->Shader != nullptr)
	{
		ShaderInfo->Shader->Release();
	}
	if (ShaderInfo->Declaration != nullptr)
	{
		ShaderInfo->Declaration->Release();
	}

	delete ShaderInfo;

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetVertexShaderConstant(DWORD Register, const void* pConstantData, DWORD ConstantCount)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->SetVertexShaderConstantF(Register, static_cast<const float *>(pConstantData), ConstantCount);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShaderConstant(DWORD Register, void* pConstantData, DWORD ConstantCount)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->GetVertexShaderConstantF(Register, static_cast<float *>(pConstantData), ConstantCount);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShaderDeclaration(DWORD Handle, void* pData, DWORD* pSizeOfData)
{
	UNREFERENCED_PARAMETER(Handle);
	UNREFERENCED_PARAMETER(pData);
	UNREFERENCED_PARAMETER(pSizeOfData);

#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::GetVertexShaderDeclaration" << "(" << this << ", " << Handle << ", " << pData << ", " << pSizeOfData << ")' ..." << std::endl;
	LOG << "> 'IDirect3DDevice8::GetVertexShaderDeclaration' is not implemented!" << std::endl;
#endif

	return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShaderFunction(DWORD Handle, void* pData, DWORD* pSizeOfData)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::GetVertexShaderFunction" << "(" << this << ", " << Handle << ", " << pData << ", " << pSizeOfData << ")' ..." << std::endl;
#endif

	if ((Handle & 0x80000000) == 0)
	{
		return D3DERR_INVALIDCALL;
	}

	const DWORD HandleMagic = Handle << 1;
	IDirect3DVertexShader9 *VertexShaderInterface = reinterpret_cast<VertexShaderInfo *>(HandleMagic)->Shader;

	if (VertexShaderInterface == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

#ifndef D3D8TO9NOLOG
	LOG << "> Returning translated shader byte code." << std::endl;
#endif

	return VertexShaderInterface->GetFunction(pData, reinterpret_cast<UINT *>(pSizeOfData));
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetStreamSource(UINT StreamNumber, Direct3DVertexBuffer8* pStreamData, UINT Stride)
{
	auto it = stream_sources.find(StreamNumber);

	if (it == stream_sources.end())
	{
		if (pStreamData != nullptr)
		{
			stream_sources[StreamNumber] = { pStreamData, Stride };
			pStreamData->AddRef();
		}
	}
	else
	{
		if (it->second.buffer)
		{
			it->second.buffer->Release();
		}

		if (pStreamData == nullptr)
		{
			stream_sources.erase(it);
		}
		else
		{
			it->second = { pStreamData, Stride };
			pStreamData->AddRef();
		}
	}

	if (pStreamData != nullptr)
	{
		UINT zero = 0;
		context->IASetVertexBuffers(StreamNumber, 1, pStreamData->buffer_resource.GetAddressOf(), &Stride, &zero);
	}
	else
	{
		context->IASetVertexBuffers(StreamNumber, 0, nullptr, nullptr, nullptr);
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetStreamSource(UINT StreamNumber, Direct3DVertexBuffer8** ppStreamData, UINT* pStride)
{
	if (ppStreamData == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppStreamData = nullptr;

	if (pStride)
	{
		*pStride = 0;
	}

	auto it = stream_sources.find(StreamNumber);

	if (it != stream_sources.end() && it->second.buffer)
	{
		*ppStreamData = it->second.buffer;
		it->second.buffer->AddRef();

		if (pStride)
		{
			*pStride = it->second.stride;
		}
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetIndices(Direct3DIndexBuffer8* pIndexData, UINT BaseVertexIndex)
{
	if (pIndexData == nullptr || BaseVertexIndex > 0x7FFFFFFF)
	{
		return D3DERR_INVALIDCALL;
	}

	if (index_buffer)
	{
		index_buffer->Release();
	}

	index_buffer = pIndexData;
	index_buffer->AddRef();

	CurrentBaseVertexIndex = static_cast<INT>(BaseVertexIndex);
	context->IASetIndexBuffer(index_buffer->buffer_resource.Get(), to_dxgi(index_buffer->desc8.Format), BaseVertexIndex);
	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetIndices(Direct3DIndexBuffer8** ppIndexData, UINT* pBaseVertexIndex)
{
	if (ppIndexData == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*ppIndexData = index_buffer;
	index_buffer->AddRef();

	if (pBaseVertexIndex != nullptr)
	{
		*pBaseVertexIndex = static_cast<UINT>(CurrentBaseVertexIndex);
	}

	return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreatePixelShader(const DWORD* pFunction, DWORD* pHandle)
{
	return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetPixelShader(DWORD Handle)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	CurrentPixelShaderHandle = Handle;

	return ProxyInterface->SetPixelShader(reinterpret_cast<IDirect3DPixelShader9 *>(Handle));
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPixelShader(DWORD* pHandle)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (pHandle == nullptr)
	{
		return D3DERR_INVALIDCALL;
	}

	*pHandle = CurrentPixelShaderHandle;

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeletePixelShader(DWORD Handle)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	if (Handle == 0)
	{
		return D3DERR_INVALIDCALL;
	}

	if (CurrentPixelShaderHandle == Handle)
	{
		SetPixelShader(0);
	}

	reinterpret_cast<IDirect3DPixelShader9 *>(Handle)->Release();

	return D3D_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetPixelShaderConstant(DWORD Register, const void* pConstantData, DWORD ConstantCount)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->SetPixelShaderConstantF(Register, static_cast<const float *>(pConstantData), ConstantCount);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPixelShaderConstant(DWORD Register, void* pConstantData, DWORD ConstantCount)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->GetPixelShaderConstantF(Register, static_cast<float *>(pConstantData), ConstantCount);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPixelShaderFunction(DWORD Handle, void* pData, DWORD* pSizeOfData)
{
	return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawRectPatch(UINT Handle, const float* pNumSegs, const D3DRECTPATCH_INFO* pRectPatchInfo)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawTriPatch(UINT Handle, const float* pNumSegs, const D3DTRIPATCH_INFO* pTriPatchInfo)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo);
#endif
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeletePatch(UINT Handle)
{
#if 1
	// not required for SADX
	return D3DERR_INVALIDCALL;
#else
	return ProxyInterface->DeletePatch(Handle);
#endif
}

void Direct3DDevice8::print_info_queue() const
{
	if (!info_queue)
	{
		return;
	}

	UINT64 i = 0;

	do
	{
		SIZE_T size = 0;
		HRESULT hr = info_queue->GetMessageW(i, nullptr, &size);

		if (hr != S_FALSE)
		{
			break;
		}

		if (!size)
		{
			break;
		}

		auto pMessage = reinterpret_cast<D3D11_MESSAGE*>(new uint8_t[size]);

		hr = info_queue->GetMessageW(i, pMessage, &size);

		if (hr == S_OK && pMessage->pDescription)
		{
			PrintDebug("%s\n", pMessage->pDescription);
		}

		delete[] pMessage;

		++i;
	} while (true);

	info_queue->ClearStoredMessages();
}

bool Direct3DDevice8::update_input_layout()
{
	if (!FVF.dirty() || !FVF.data())
	{
		return false;
	}

	//FVF.clear();

	auto it = fvf_layouts.find(FVF);

	if (it != fvf_layouts.end())
	{
		context->IASetInputLayout(it->second.Get());
		return true;
	}

	std::vector<D3D11_INPUT_ELEMENT_DESC> elements;

	D3D11_INPUT_ELEMENT_DESC pos_element {};
	pos_element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;

	DWORD fvf = FVF;

	switch (fvf & D3DFVF_POSITION_MASK)
	{
		case D3DFVF_XYZ:
			pos_element.SemanticName = "POSITION";
			pos_element.Format       = DXGI_FORMAT_R32G32B32_FLOAT;
			break;

		case D3DFVF_XYZRHW:
			pos_element.SemanticName = "POSITION";
			pos_element.Format       = DXGI_FORMAT_R32G32B32A32_FLOAT;
			break;

		default:
			throw std::runtime_error("unsupported D3DFVF_POSITION type");
	}

	fvf &= ~D3DFVF_POSITION_MASK;
	elements.push_back(pos_element);

	if (fvf & D3DFVF_NORMAL)
	{
		fvf ^= D3DFVF_NORMAL;
		D3D11_INPUT_ELEMENT_DESC e {};

		e.SemanticName      = "NORMAL";
		e.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
		e.Format            = DXGI_FORMAT_R32G32B32_FLOAT;
		e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

		elements.push_back(e);
	}

	if (fvf & D3DFVF_DIFFUSE)
	{
		fvf ^= D3DFVF_DIFFUSE;
		D3D11_INPUT_ELEMENT_DESC e {};

		e.SemanticName      = "COLOR";
		e.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
		e.Format            = DXGI_FORMAT_B8G8R8A8_UNORM;
		e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

		elements.push_back(e);
	}

	if (fvf & D3DFVF_TEX1)
	{
		fvf ^= D3DFVF_TEX1;
		D3D11_INPUT_ELEMENT_DESC e {};

		e.SemanticName      = "TEXCOORD";
		e.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
		e.Format            = DXGI_FORMAT_R32G32_FLOAT;
		e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

		elements.push_back(e);
	}

	if (fvf != 0)
	{
		PrintDebug("unsupported FVF\n");
		return false;
	}

	auto vs = get_vs(shader_flags);

	ComPtr<ID3D11InputLayout> layout;

	HRESULT hr = device->CreateInputLayout(elements.data(), elements.size(), vs.blob->GetBufferPointer(), vs.blob->GetBufferSize(), &layout);

	if (FAILED(hr))
	{
		//throw std::runtime_error("CreateInputLayout failed");
		PrintDebug("CreateInputLayout failed\n");
		return false;
	}

	fvf_layouts[FVF] = layout;
	context->IASetInputLayout(layout.Get());
	return true;
}

void Direct3DDevice8::commit_per_pixel()
{
	per_pixel.srcBlend   = render_state_values[D3DRS_SRCBLEND];
	per_pixel.destBlend  = render_state_values[D3DRS_DESTBLEND];
	per_pixel.fogMode    = render_state_values[D3DRS_FOGTABLEMODE];
	per_pixel.fogStart   = *reinterpret_cast<const float*>(&render_state_values[D3DRS_FOGSTART].data());
	per_pixel.fogEnd     = *reinterpret_cast<const float*>(&render_state_values[D3DRS_FOGEND].data());
	per_pixel.fogDensity = *reinterpret_cast<const float*>(&render_state_values[D3DRS_FOGDENSITY].data());

	if (!per_pixel.dirty() && !render_state_values[D3DRS_FOGCOLOR].dirty())
	{
		return;
	}

	per_pixel.set_color(render_state_values[D3DRS_FOGCOLOR]);

	D3D11_MAPPED_SUBRESOURCE mapped {};
	context->Map(per_pixel_cbuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);

	auto writer = CBufferWriter(reinterpret_cast<uint8_t*>(mapped.pData));
	per_pixel.write(writer);
	context->Unmap(per_pixel_cbuf.Get(), 0);
	per_pixel.clear();
}

void Direct3DDevice8::commit_per_model()
{
	if (!per_model.dirty())
	{
		return;
	}

	D3D11_MAPPED_SUBRESOURCE mapped {};
	context->Map(per_model_cbuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);

	auto writer = CBufferWriter(reinterpret_cast<uint8_t*>(mapped.pData));

	per_model.write(writer);
	per_model.clear();

	context->Unmap(per_model_cbuf.Get(), 0);
}

void Direct3DDevice8::commit_per_scene()
{
	per_scene.screenDimensions = { viewport.Width, viewport.Height };

	if (Camera_Data1)
	{
		const NJS_VECTOR& position = Camera_Data1->Position;
		per_scene.viewPosition = { position.x, position.y, position.z };
	}
	else
	{
		per_scene.viewPosition = per_scene.viewMatrix.data().Translation();
	}

	if (!per_scene.dirty())
	{
		return;
	}

	D3D11_MAPPED_SUBRESOURCE mapped {};
	context->Map(per_scene_cbuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);

	auto writer = CBufferWriter(reinterpret_cast<uint8_t*>(mapped.pData));
	per_scene.write(writer);
	per_scene.clear();
	context->Unmap(per_scene_cbuf.Get(), 0);
}

void Direct3DDevice8::update_sampler()
{
	for (auto& setting_it : sampler_setting_values)
	{
		auto& setting = setting_it.second;

		if (!setting.dirty())
		{
			continue;
		}

		setting.clear();

		const auto it = sampler_states.find(setting);

		if (it != sampler_states.end())
		{
			context->PSSetSamplers(setting_it.first, 1, it->second.GetAddressOf());
			return;
		}

		D3D11_SAMPLER_DESC sampler_desc {};

		sampler_desc.Filter         = to_d3d11(setting.filter_min, setting.filter_mag, setting.filter_mip);;
		sampler_desc.AddressU       = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(setting.address_u.data());
		sampler_desc.AddressV       = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(setting.address_v.data());
		sampler_desc.AddressW       = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(setting.address_w.data());
		sampler_desc.MinLOD         = -std::numeric_limits<float>::max();
		sampler_desc.MaxLOD         = std::numeric_limits<float>::max();
		sampler_desc.MaxAnisotropy  = setting.max_anisotropy;
		sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;

		ComPtr<ID3D11SamplerState> sampler_state;
		HRESULT hr = device->CreateSamplerState(&sampler_desc, &sampler_state);

		if (FAILED(hr))
		{
			throw std::runtime_error("CreateSamplerState failed");
		}

		context->PSSetSamplers(setting_it.first, 1, sampler_state.GetAddressOf());
		sampler_states[setting] = sampler_state;
	}
}

void Direct3DDevice8::compile_shaders(uint32_t flags, VertexShader& vs, PixelShader& ps)
{
	int result;

	do
	{
		try
		{
			vs = get_vs(flags);
			ps = get_ps(flags);
			break;
		}
		catch (std::exception& ex)
		{
			free_shaders();
			result = MessageBoxA(WindowHandle, ex.what(), "Shader compilation error", MB_RETRYCANCEL | MB_ICONERROR);
		}
	} while (result == IDRETRY);
}

void Direct3DDevice8::update_shaders()
{
	auto& fog_enable = render_state_values[D3DRS_FOGENABLE];

	if (fog_enable.data() != 0)
	{
		shader_flags |= ShaderFlags::rs_fog;
	}
	else
	{
		shader_flags &= ~ShaderFlags::rs_fog;
	}

	fog_enable.clear();

	auto& sampler_0 = texture_state_values[0];

	auto& tci = sampler_0[D3DTSS_TEXCOORDINDEX];

	if (tci.data() != 0 && shader_flags & ShaderFlags::fvf_tex1)
	{
		shader_flags |= ShaderFlags::tci_envmap;
	}
	else
	{
		shader_flags &= ~ShaderFlags::tci_envmap;
	}

	tci.clear();

	auto lighting = render_state_values[D3DRS_LIGHTING].data();

	if (lighting != 1 && shader_flags & ShaderFlags::fvf_normal)
	{
		shader_flags &= ~ShaderFlags::rs_lighting;
	}
	else
	{
		shader_flags |= ShaderFlags::rs_lighting;
	}

	auto& specular = render_state_values[D3DRS_SPECULARENABLE];

	if (specular.data() != 0 && shader_flags & ShaderFlags::rs_lighting)
	{
		shader_flags |= ShaderFlags::rs_specular;
	}
	else
	{
		shader_flags &= ~ShaderFlags::rs_specular;
	}

	specular.clear();

	auto& alpha = render_state_values[D3DRS_ALPHABLENDENABLE];

	if (alpha.data() != 1)
	{
		shader_flags &= ~ShaderFlags::rs_alpha;
	}
	else
	{
		shader_flags |= ShaderFlags::rs_alpha;
	}

	alpha.clear();

	if ((oit_actually_enabled && oit_enabled) && shader_flags & ShaderFlags::rs_alpha)
	{
		shader_flags |= ShaderFlags::oit;
	}
	else
	{
		shader_flags &= ~ShaderFlags::oit;
	}

	VertexShader vs;
	PixelShader ps;

	compile_shaders(shader_flags, vs, ps);

	context->VSSetShader(vs.shader.Get(), nullptr, 0);
	context->PSSetShader(ps.shader.Get(), nullptr, 0);
}

void Direct3DDevice8::update_blend()
{
	if (!blend_flags.dirty())
	{
		return;
	}

	blend_flags.clear();

	const auto it = blend_states.find(blend_flags.data());

	if (it != blend_states.end())
	{
		context->OMSetBlendState(it->second.Get(), nullptr, 0xFFFFFFFF);
		return;
	}

	D3D11_BLEND_DESC desc {};

	auto flags = blend_flags.data();

	desc.RenderTarget[0].BlendEnable           = flags >> 15 & 1;
	desc.RenderTarget[0].SrcBlend              = static_cast<D3D11_BLEND>(flags & 0xF);
	desc.RenderTarget[0].DestBlend             = static_cast<D3D11_BLEND>((flags >> 4) & 0xF);
	desc.RenderTarget[0].BlendOp               = static_cast<D3D11_BLEND_OP>((flags >> 8) & 0xF);
	desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	desc.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
	desc.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
	desc.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;

	ComPtr<ID3D11BlendState> blend_state;
	HRESULT hr = device->CreateBlendState(&desc, &blend_state);

	if (FAILED(hr))
	{
		throw std::runtime_error("CreateBlendState failed");
	}

	blend_states[flags] = blend_state;
	context->OMSetBlendState(blend_state.Get(), nullptr, 0xFFFFFFFF);
}

bool Direct3DDevice8::update()
{
	update_shaders();
	update_sampler();
	update_blend();
	commit_per_scene();
	commit_per_model();
	commit_per_pixel();
	return update_input_layout();
}

void Direct3DDevice8::free_shaders()
{
	shader_sources.clear();
	vertex_shaders.clear();
	pixel_shaders.clear();
}

void Direct3DDevice8::oit_load_shaders()
{
	D3D_SHADER_MACRO preproc[] = {
		{ "MAX_FRAGMENTS", fragments_str.c_str() },
		{}
	};

	int result;

	do
	{
		try
		{
			ComPtr<ID3DBlob> errors;
			ComPtr<ID3DBlob> blob;

			auto path = globals::get_system_path(L"composite.hlsl");

			HRESULT hr = D3DCompileFromFile(path.c_str(), &preproc[0], D3D_COMPILE_STANDARD_FILE_INCLUDE, "vs_main", "vs_5_0", 0, 0, &blob, &errors);

			if (FAILED(hr))
			{
				std::string str(static_cast<char*>(errors->GetBufferPointer()), 0, errors->GetBufferSize());
				throw std::runtime_error(str);
			}

			hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &composite_vs.shader);

			if (FAILED(hr))
			{
				throw std::runtime_error("composite vertex shader creation failed");
			}

			hr = D3DCompileFromFile(path.c_str(), &preproc[0], D3D_COMPILE_STANDARD_FILE_INCLUDE, "ps_main", "ps_5_0", 0, 0, &blob, &errors);

			if (FAILED(hr))
			{
				std::string str(static_cast<char*>(errors->GetBufferPointer()), 0, errors->GetBufferSize());
				throw std::runtime_error(str);
			}

			hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &composite_ps.shader);

			if (FAILED(hr))
			{
				throw std::runtime_error("composite pixel shader creation failed");
			}
			break;
		}
		catch (std::exception& ex)
		{
			print_info_queue();
			free_shaders();
			result = MessageBoxA(WindowHandle, ex.what(), "Shader compilation error", MB_RETRYCANCEL | MB_ICONERROR);
		}
	} while (result == IDRETRY);
}

void Direct3DDevice8::oit_release()
{
	static std::array<ID3D11UnorderedAccessView*, 5> null = {};

	context->OMSetRenderTargetsAndUnorderedAccessViews(1, render_target.GetAddressOf(), nullptr,
	                                                   1, null.size(), &null[0], nullptr);

	FragListHead     = nullptr;
	FragListHeadSRV  = nullptr;
	FragListHeadUAV  = nullptr;
	FragListCount    = nullptr;
	FragListCountSRV = nullptr;
	FragListCountUAV = nullptr;
	FragListNodes    = nullptr;
	FragListNodesSRV = nullptr;
	FragListNodesUAV = nullptr;
}

void Direct3DDevice8::oit_write()
{
	// Unbinds the shader resource views for our fragment list and list head.
	// UAVs cannot be bound as standard resource views and UAVs simultaneously.
	std::array<ID3D11ShaderResourceView*, 5> srvs = {};
	context->PSSetShaderResources(0, srvs.size(), &srvs[0]);

	std::array<ID3D11UnorderedAccessView*, 3> uavs = {
		FragListHeadUAV.Get(),
		FragListCountUAV.Get(),
		FragListNodesUAV.Get()
	};

	// This is used to set the hidden counter of FragListNodes to 0.
	// It only works on FragListNodes, but the number of elements here
	// must match the number of UAVs given.
	static const uint zero[3] = { 0, 0, 0 };

	// Binds our fragment list & list head UAVs for read/write operations.
	context->OMSetRenderTargetsAndUnorderedAccessViews(1, oit_actually_enabled ? composite_view.GetAddressOf() : render_target.GetAddressOf(),
	                                                   depth_view.Get(), 1, uavs.size(), &uavs[0], &zero[0]);

	// Resets the list head indices to FRAGMENT_LIST_NULL.
	// 4 elements are required as this can be used to clear a texture
	// with 4 color channels, even though our list head only has one.
	static const UINT clear_head[] = { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX };
	context->ClearUnorderedAccessViewUint(FragListHeadUAV.Get(), &clear_head[0]);

	static const UINT clear_count[] = { 0, 0, 0, 0 };
	context->ClearUnorderedAccessViewUint(FragListCountUAV.Get(), &clear_count[0]);
}

void Direct3DDevice8::oit_read()
{
	ID3D11UnorderedAccessView* uavs[3] = {};

	// Unbinds our UAVs.
	context->OMSetRenderTargetsAndUnorderedAccessViews(1, render_target.GetAddressOf(), nullptr, 1, 3, &uavs[0], nullptr);

	std::array<ID3D11ShaderResourceView*, 5> srvs = {
		FragListHeadSRV.Get(),
		FragListCountSRV.Get(),
		FragListNodesSRV.Get(),
		composite_srv.Get(),
		depth_srv.Get()
	};

	// Binds the shader resource views of our UAV buffers as read-only.
	context->PSSetShaderResources(0, srvs.size(), &srvs[0]);
}

void Direct3DDevice8::oit_init()
{
	oit_release();

	FragListHead_Init();
	FragListCount_Init();
	FragListNodes_Init();

	oit_write();
}

void Direct3DDevice8::FragListHead_Init()
{
	D3D11_TEXTURE2D_DESC desc2D = {};

	desc2D.ArraySize          = 1;
	desc2D.BindFlags          = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	desc2D.Usage              = D3D11_USAGE_DEFAULT;
	desc2D.Format             = DXGI_FORMAT_R32_UINT;
	desc2D.Width              = static_cast<UINT>(viewport.Width);
	desc2D.Height             = static_cast<UINT>(viewport.Height);
	desc2D.MipLevels          = 1;
	desc2D.SampleDesc.Count   = 1;
	desc2D.SampleDesc.Quality = 0;

	if (FAILED(device->CreateTexture2D(&desc2D, nullptr, &FragListHead)))
	{
		throw;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC descRV;

	descRV.Format                    = desc2D.Format;
	descRV.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
	descRV.Texture2D.MipLevels       = 1;
	descRV.Texture2D.MostDetailedMip = 0;

	if (FAILED(device->CreateShaderResourceView(FragListHead.Get(), &descRV, &FragListHeadSRV)))
	{
		throw;
	}

	D3D11_UNORDERED_ACCESS_VIEW_DESC descUAV;

	descUAV.Format              = desc2D.Format;
	descUAV.ViewDimension       = D3D11_UAV_DIMENSION_TEXTURE2D;
	descUAV.Buffer.FirstElement = 0;
	descUAV.Buffer.NumElements  = static_cast<UINT>(viewport.Width) * static_cast<UINT>(viewport.Height);
	descUAV.Buffer.Flags        = 0;

	if (FAILED(device->CreateUnorderedAccessView(FragListHead.Get(), &descUAV, &FragListHeadUAV)))
	{
		throw;
	}
}

void Direct3DDevice8::FragListCount_Init()
{
	D3D11_TEXTURE2D_DESC desc2D = {};

	desc2D.ArraySize          = 1;
	desc2D.BindFlags          = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	desc2D.Usage              = D3D11_USAGE_DEFAULT;
	desc2D.Format             = DXGI_FORMAT_R32_UINT;
	desc2D.Width              = static_cast<UINT>(viewport.Width);
	desc2D.Height             = static_cast<UINT>(viewport.Height);
	desc2D.MipLevels          = 1;
	desc2D.SampleDesc.Count   = 1;
	desc2D.SampleDesc.Quality = 0;

	if (FAILED(device->CreateTexture2D(&desc2D, nullptr, &FragListCount)))
	{
		throw;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC descRV;

	descRV.Format                    = desc2D.Format;
	descRV.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
	descRV.Texture2D.MipLevels       = 1;
	descRV.Texture2D.MostDetailedMip = 0;

	if (FAILED(device->CreateShaderResourceView(FragListCount.Get(), &descRV, &FragListCountSRV)))
	{
		throw;
	}

	D3D11_UNORDERED_ACCESS_VIEW_DESC descUAV;

	descUAV.Format              = desc2D.Format;
	descUAV.ViewDimension       = D3D11_UAV_DIMENSION_TEXTURE2D;
	descUAV.Buffer.FirstElement = 0;
	descUAV.Buffer.NumElements  = static_cast<UINT>(viewport.Width) * static_cast<UINT>(viewport.Height);
	descUAV.Buffer.Flags        = 0;

	if (FAILED(device->CreateUnorderedAccessView(FragListCount.Get(), &descUAV, &FragListCountUAV)))
	{
		throw;
	}
}

void Direct3DDevice8::FragListNodes_Init()
{
	D3D11_BUFFER_DESC descBuf = {};

	per_scene.bufferLength = static_cast<UINT>(viewport.Width) * static_cast<UINT>(viewport.Height) * globals::max_fragments;

	descBuf.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	descBuf.BindFlags           = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	descBuf.ByteWidth           = sizeof(OitNode) * static_cast<UINT>(viewport.Width) * static_cast<UINT>(viewport.Height) * globals::max_fragments;
	descBuf.StructureByteStride = sizeof(OitNode);

	if (FAILED(device->CreateBuffer(&descBuf, nullptr, &FragListNodes)))
	{
		throw;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC descRV = {};

	descRV.Format             = DXGI_FORMAT_UNKNOWN;
	descRV.ViewDimension      = D3D11_SRV_DIMENSION_BUFFER;
	descRV.Buffer.NumElements = static_cast<UINT>(viewport.Width) * static_cast<UINT>(viewport.Height) * globals::max_fragments;

	if (FAILED(device->CreateShaderResourceView(FragListNodes.Get(), &descRV, &FragListNodesSRV)))
	{
		throw;
	}

	D3D11_UNORDERED_ACCESS_VIEW_DESC descUAV;

	descUAV.Format              = DXGI_FORMAT_UNKNOWN;
	descUAV.ViewDimension       = D3D11_UAV_DIMENSION_BUFFER;
	descUAV.Buffer.FirstElement = 0;
	descUAV.Buffer.NumElements  = static_cast<UINT>(viewport.Width) * static_cast<UINT>(viewport.Height) * globals::max_fragments;
	descUAV.Buffer.Flags        = D3D11_BUFFER_UAV_FLAG_COUNTER;

	if (FAILED(device->CreateUnorderedAccessView(FragListNodes.Get(), &descUAV, &FragListNodesUAV)))
	{
		throw;
	}
}

void Direct3DDevice8::up_get(size_t target_size)
{
	auto rounded = round_pow2(target_size);

	for (auto it = up_buffers.begin(); it != up_buffers.end(); ++it)
	{
		if ((*it)->desc8.Size >= rounded && (*it)->desc8.Size < 2 * rounded)
		{
			up_buffer = *it;
			up_buffers.erase(it);
			return;
		}
	}

	PrintDebug(__FUNCTION__ " is allocating (%u rounded to %u bytes, %u total buffers)\n", target_size, rounded, up_buffers.size() + 1);
	CreateVertexBuffer(rounded, D3DUSAGE_DYNAMIC, FVF.data(), D3DPOOL_MANAGED, &up_buffer);
}
