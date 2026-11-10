/* vtable_index.c - vtable slot numbers taken from the SDK's C interface
 * declarations, so shell_composite.cpp never hard-codes a slot number. */
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <stddef.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#define SLOT(vtbl, member) ((unsigned)(offsetof(vtbl, member) / sizeof(void*)))

const unsigned VT_SwapChain_Present = SLOT(IDXGISwapChainVtbl, Present);
const unsigned VT_SwapChain_GetBuffer = SLOT(IDXGISwapChainVtbl, GetBuffer);
const unsigned VT_SwapChain_ResizeBuffers = SLOT(IDXGISwapChainVtbl, ResizeBuffers);
const unsigned VT_SwapChain1_Present1 = SLOT(IDXGISwapChain1Vtbl, Present1);
const unsigned VT_Context_ClearRenderTargetView = SLOT(ID3D11DeviceContextVtbl, ClearRenderTargetView);
const unsigned VT_Context_OMSetRenderTargets = SLOT(ID3D11DeviceContextVtbl, OMSetRenderTargets);
const unsigned VT_Context_ExecuteCommandList = SLOT(ID3D11DeviceContextVtbl, ExecuteCommandList);
const unsigned VT_Context_OMSetRenderTargetsAndUAVs = SLOT(ID3D11DeviceContextVtbl, OMSetRenderTargetsAndUnorderedAccessViews);
