/* Every other export of the system d3d11.dll, forwarded unchanged: a jump
 * through a pointer resolved from C:\Windows\System32\d3d11.dll at load. Other
 * modules in the process bind to "d3d11.dll" by name, so the proxy must offer
 * all of them. D3D11CreateDevice and D3D11CreateDeviceAndSwapChain are in
 * proxy.c. */
#include <windows.h>

#define EXPORTS(X) \
   X(D3D11CreateDeviceForD3D12) X(D3DKMTCloseAdapter) X(D3DKMTDestroyAllocation) X(D3DKMTDestroyContext) \
   X(D3DKMTDestroyDevice) X(D3DKMTDestroySynchronizationObject) X(D3DKMTPresent) X(D3DKMTQueryAdapterInfo) \
   X(D3DKMTSetDisplayPrivateDriverFormat) X(D3DKMTSignalSynchronizationObject) X(D3DKMTUnlock) \
   X(D3DKMTWaitForSynchronizationObject) X(EnableFeatureLevelUpgrade) X(OpenAdapter10) X(OpenAdapter10_2) \
   X(CreateDirect3D11DeviceFromDXGIDevice) X(CreateDirect3D11SurfaceFromDXGISurface) X(D3D11CoreCreateDevice) \
   X(D3D11CoreCreateLayeredDevice) X(D3D11CoreGetLayeredDeviceSize) X(D3D11CoreRegisterLayers) \
   X(D3D11On12CreateDevice) X(D3DKMTCreateAllocation) X(D3DKMTCreateContext) X(D3DKMTCreateDevice) \
   X(D3DKMTCreateSynchronizationObject) X(D3DKMTEscape) X(D3DKMTGetContextSchedulingPriority) \
   X(D3DKMTGetDeviceState) X(D3DKMTGetDisplayModeList) X(D3DKMTGetMultisampleMethodList) X(D3DKMTGetRuntimeData) \
   X(D3DKMTGetSharedPrimaryHandle) X(D3DKMTLock) X(D3DKMTOpenAdapterFromHdc) X(D3DKMTOpenResource) \
   X(D3DKMTQueryAllocationResidency) X(D3DKMTQueryResourceInfo) X(D3DKMTRender) X(D3DKMTSetAllocationPriority) \
   X(D3DKMTSetContextSchedulingPriority) X(D3DKMTSetDisplayMode) X(D3DKMTSetGammaRamp) X(D3DKMTSetVidPnSourceOwner) \
   X(D3DKMTWaitForVerticalBlankEvent) X(D3DPerformance_BeginEvent) X(D3DPerformance_EndEvent) \
   X(D3DPerformance_GetStatus) X(D3DPerformance_SetMarker)

#define STUB(name) \
   __attribute__((used)) void *proxy_real_##name; \
   __declspec(dllexport) __attribute__((naked)) void name(void) { __asm__("jmp *proxy_real_" #name "(%rip)"); }
EXPORTS(STUB)

void proxy_resolve_stubs(HMODULE real)
{
#define RESOLVE(name) proxy_real_##name = (void *)GetProcAddress(real, #name);
   EXPORTS(RESOLVE)
}
