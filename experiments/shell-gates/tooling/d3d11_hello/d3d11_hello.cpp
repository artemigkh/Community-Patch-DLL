// D3D11 hello world: create a device on the default adapter and print the adapter's name.
// Proves the x64 and x86 clang-cl + Windows SDK build path. Build with build_d3d11_hello.cmd.
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

int main()
{
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;
	D3D_FEATURE_LEVEL level = {};
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
		D3D11_SDK_VERSION, &device, &level, &context);
	if (FAILED(hr))
	{
		std::printf("D3D11CreateDevice failed: 0x%08lx\n", static_cast<unsigned long>(hr));
		return 1;
	}

	IDXGIDevice* dxgiDevice = nullptr;
	IDXGIAdapter* adapter = nullptr;
	DXGI_ADAPTER_DESC desc = {};
	if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) &&
		SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)))
	{
		std::printf("bitness: %d-bit\n", static_cast<int>(sizeof(void*) * 8));
		std::printf("adapter: %ls\n", desc.Description);
		std::printf("feature level: 0x%04x\n", static_cast<unsigned>(level));
		std::printf("dedicated video memory: %llu MB\n",
			static_cast<unsigned long long>(desc.DedicatedVideoMemory) / (1024ull * 1024ull));
	}

	if (adapter) adapter->Release();
	if (dxgiDevice) dxgiDevice->Release();
	context->Release();
	device->Release();
	return 0;
}
