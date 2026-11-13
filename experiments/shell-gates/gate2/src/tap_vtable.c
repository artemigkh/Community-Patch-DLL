/* tap_vtable.c - vtable slot numbers from the SDK's C interface declarations (no hard-coded slots). */
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <stddef.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#define SLOT(vtbl, member) ((unsigned)(offsetof(vtbl, member) / sizeof(void*)))

const unsigned VT_SwapChain_Present = SLOT(IDXGISwapChainVtbl, Present);
const unsigned VT_SwapChain1_Present1 = SLOT(IDXGISwapChain1Vtbl, Present1);
const unsigned VT_Factory_CreateSwapChain = SLOT(IDXGIFactoryVtbl, CreateSwapChain);
const unsigned VT_Factory2_CreateSwapChainForHwnd = SLOT(IDXGIFactory2Vtbl, CreateSwapChainForHwnd);
const unsigned VT_Device_CreateBuffer = SLOT(ID3D11DeviceVtbl, CreateBuffer);
const unsigned VT_Device_CreateTexture2D = SLOT(ID3D11DeviceVtbl, CreateTexture2D);
const unsigned VT_Device_CreateInputLayout = SLOT(ID3D11DeviceVtbl, CreateInputLayout);
const unsigned VT_Device_CreateVertexShader = SLOT(ID3D11DeviceVtbl, CreateVertexShader);
const unsigned VT_Device_CreateGeometryShader = SLOT(ID3D11DeviceVtbl, CreateGeometryShader);
const unsigned VT_Device_CreatePixelShader = SLOT(ID3D11DeviceVtbl, CreatePixelShader);
const unsigned VT_Device_CreateHullShader = SLOT(ID3D11DeviceVtbl, CreateHullShader);
const unsigned VT_Device_CreateDomainShader = SLOT(ID3D11DeviceVtbl, CreateDomainShader);
