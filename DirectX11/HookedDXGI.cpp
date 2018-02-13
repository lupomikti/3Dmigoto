// Object			OS				DXGI version	Feature level
// IDXGIFactory		Win7			1.0				11.0
// IDXGIFactory1	Win7			1.0				11.0
// IDXGIFactory2	Platform update	1.2				11.1
// IDXGIFactory3	Win8.1			1.3
// IDXGIFactory4					1.4
// IDXGIFactory5					1.5
//
// IDXGISwapChain	Win7			1.0				11.0
// IDXGISwapChain1	Platform update	1.2				11.1
// IDXGISwapChain2	Win8.1			1.3
// IDXGISwapChain3	Win10			1.4
// IDXGISwapChain4					1.5

#include <d3d11_1.h>

#include "HookedDXGI.h"
#include "HackerDXGI.h"

#include "DLLMainHook.h"
#include "log.h"
#include "util.h"
#include "D3D11Wrapper.h"


// This class is for a different approach than the wrapping of the system objects
// like we do with ID3D11Device for example.  When we wrap a COM object like that,
// it's not a real C++ object, and consequently cannot use the superclass normally,
// and requires boilerplate call-throughs for every interface to the object.  We
// may only care about a 5 calls, but we have to wrap all 150 calls. 
//
// Rather than do that with DXGI, this approach will be to singly hook the calls we
// are interested in, using the Nektra In-Proc hooking.  We'll still create
// objects for encapsulation where necessary, by returning HackerDXGIFactory1
// and HackerDXGIFactory2 when platform_update is set.  We won't ever return
// HackerDXGIFactory because the minimum on Win7 is IDXGIFactory1.
//
// For our hooks:
// It is worth noting, since it took me 3 days to figure it out, than even though
// they are defined C style, that we must use STDMETHODCALLTYPE (or__stdcall) 
// because otherwise the stack is broken by the different calling conventions.
//
// In normal method calls, the 'this' parameter is implicitly added.  Since we are
// using the C style dxgi interface though, we are declaring these routines differently.
//
// Since we want to allow reentrancy for the calls, we need to use the returned
// fnOrig* to call the original, instead of the alternate approach offered by
// Deviare.

#ifdef NTDDI_WIN10
// 3DMigoto was built with the Win 10 SDK (vs2015 branch) - we can use the
// 11On12 compatibility mode to enable some 3DMigoto functionality on DX12 to
// get the overlay working and display a warning. This won't be enough to
// enable hunting or replace shaders or anything, and the only noteworthy call
// from the game we will be intercepting is Present().
#include <d3d11on12.h>

static HackerDevice* prepare_devices_for_dx12_warning(IUnknown *unknown_device)
{
	ID3D12CommandQueue *d3d12_queue = NULL;
	ID3D12Device *d3d12_device = NULL;
	ID3D11Device *d3d11_device = NULL;
	ID3D11DeviceContext *d3d11_context = NULL;
	HackerDevice *dev_wrap = NULL;
	HackerContext *context_wrap = NULL;
	HRESULT hr;

	if (FAILED(unknown_device->QueryInterface(IID_ID3D12CommandQueue, (void**)&d3d12_queue)))
		goto out;

	LogInfo("Preparing to enable D3D11On12 compatibility mode for overlay...\n");

	if (FAILED(d3d12_queue->GetDevice(IID_ID3D12Device, (void**)&d3d12_device)))
		goto out;

	LogInfo(" ID3D12Device: %p\n", d3d12_device);

	// If you need the debug layer, force it in dxcpl.exe instead of here,
	// since we won't have enabled it on the D3D12 device, and doing so now
	// would reset it. If the game has used the flag to prevent the control
	// panel's registry key override we'd need to go to more heroics.
	hr = (*_D3D11On12CreateDevice)(d3d12_device,
			0, /* flags */
			NULL, 0, /* feature levels */
			(IUnknown**)&d3d12_queue,
			1, /* num queues */
			0, /* node mask */
			&d3d11_device, &d3d11_context, NULL);
	if (FAILED(hr)) {
		LogInfo("D3D11On12CreateDevice failed: 0x%x\n", hr);
		goto out;
	}

	LogInfo(" ID3D11Device: %p\n", d3d11_device);
	LogInfo(" ID3D11DeviceContext: %p\n", d3d11_context);

	dev_wrap = new HackerDevice((ID3D11Device1*)d3d11_device, (ID3D11DeviceContext1*)d3d11_context);
	context_wrap = HackerContextFactory((ID3D11Device1*)d3d11_device, (ID3D11DeviceContext1*)d3d11_context);

	LogInfo(" HackerDevice: %p\n", dev_wrap);
	LogInfo(" HackerContext: %p\n", context_wrap);

	dev_wrap->SetHackerContext(context_wrap);
	context_wrap->SetHackerDevice(dev_wrap);
	dev_wrap->Create3DMigotoResources();
	context_wrap->Bind3DMigotoResources();

	// We're going to intentionally leak the D3D11 objects, because we have
	// nothing to manage the reference to them and keep them alive - the
	// Hacker wrappers don't, because they expect the game to.

out:
	if (d3d12_device)
		d3d12_device->Release();
	if (d3d12_queue)
		d3d12_queue->Release();

	return dev_wrap;
}

#else

static HackerDevice* prepare_devices_for_dx12_warning(IUnknown *unknown_device)
{
	return NULL;
}

#endif

// Takes an IUnknown device and finds the corresponding HackerDevice and
// DirectX device interfaces. The passed in IUnknown may be modified to point
// to the real DirectX device so ensure that it will be safe to pass to the
// original CreateSwapChain call.
static HackerDevice* sort_out_swap_chain_device_mess(IUnknown **device)
{
	HackerDevice *hackerDevice;

	// pDevice could be one of several different things:
	// - It could be a HackerDevice, if the game called CreateSwapChain()
	//   with the HackerDevice we returned from CreateDevice().
	// - It could be the original ID3D11Device if we have hooking enabled,
	//   as this has hooks to call into our code instead of being wrapped.
	// - It could be an IDXGIDevice if the game is being tricky (e.g. UE4
	//   finds this from QueryInterface on the ID3D11Device). This is
	//   legal, as an IDXGIDevice is just an interface to a D3D Device,
	//   same as ID3D11Device is an interface to a D3D Device.
	// - Since we have hooked this function, CreateDeviceAndSwapChain()
	//   could also call in here straight from the real d3d11.dll with an
	//   ID3D11Device that we haven't seen or wrapped yet. We avoid this
	//   case by re-implementing CreateDeviceAndSwapChain() ourselves, so
	//   now we will get a HackerDevice in that case.
	// - It could be an ID3D10Device
	// - It could be an ID3D12CommandQueue
	// - It could be some other thing we haven't heard of yet
	//
	// We call lookup_hacker_device to look it up from the IUnknown,
	// relying on COM's guarantee that IUnknown will match for different
	// interfaces to the same object, and noting that this call will bump
	// the refcount on hackerDevice:
	hackerDevice = lookup_hacker_device(*device);
	if (hackerDevice) {
		// Ensure that pDevice points to the real DX device before
		// passing it into DX for safety. We can probably get away
		// without this since it's an IUnknown and DX will have to
		// QueryInterface() it, but let's not tempt fate:
		*device = (hackerDevice)->GetPossiblyHookedOrigDevice1();
	} else {
		LogInfo("WARNING: Could not locate HackerDevice for %p\n", *device);
		analyse_iunknown(*device);

		if (check_interface_supported(*device, IID_ID3D11Device)) {
			// If we do end up in another situation where we are
			// seeing a device for the first time (like
			// CreateDeviceAndSwapChain calling back into us), we
			// could consider creating our HackerDevice here. But
			// for now we aren't expecting this to happen, so treat
			// it as fatal if it does.
			//
			// D3D11On12CreateDevice() could possibly lead us here,
			// depending on how that works.
			LogInfo("BUG: Unwrapped ID3D11Device!\n");
			DoubleBeepExit();
		}

		LogInfo("FATAL: Unsupported DirectX Version!\n");

		// Normally we flush the log file on the Present() call, but if
		// we didn't wrap the swap chain that will probably never
		// happen. Flush it now to ensure the above message shows up so
		// we know why:
		fflush(LogFile);

		// The swap chain is being created with a device that does NOT
		// support the DX11 API. 3DMigoto is probably doomed to fail at
		// this point, unless the game is about to retry with a
		// different device. Maybe we are better off just doing a
		// DoubleBeepExit(), but let's try to make sure we at least
		// don't crash things, which may be important if an application
		// uses mixed APIs for some reason - I've certainly seen the
		// Origin overlay try to init every DX version under the sun,
		// though I don't think it actually tried creating swap chains
		// for them. Still issue an audible warning as a hint at what
		// has happened:
		hackerDevice = prepare_devices_for_dx12_warning(*device);
		if (hackerDevice)
			LogOverlayW(LOG_DIRE, L"3DMigoto does not support DirectX 12\nPlease set the game to use DirectX 11\n");
		else
			BeepProfileFail();
	}

	return hackerDevice;
}

void ForceDisplayMode(DXGI_MODE_DESC *BufferDesc, BOOL Windowed)
{
	// Historically we have only forced the refresh rate when full-screen.
	// Not positive if it would hurt to do otherwise, but for now assuming
	// we might have had a good reason and keeping that behaviour. See also
	// the note in ResizeTarget().
	if (G->SCREEN_REFRESH >= 0 && !Windowed)
	{
		// FIXME: This may disable flipping (and use blitting instead)
		// if the forced numerator and denominator does not exactly
		// match a mode enumerated on the output. e.g. We would force
		// 60Hz as 60/1, but the display might actually use 60000/1001
		// for 60Hz and we would lose flipping and degrade performance.
		BufferDesc->RefreshRate.Numerator = G->SCREEN_REFRESH;
		BufferDesc->RefreshRate.Denominator = 1;
		LogInfo("->Forcing refresh rate to = %f\n",
			(float)BufferDesc->RefreshRate.Numerator / (float)BufferDesc->RefreshRate.Denominator);
	}
	if (G->SCREEN_WIDTH >= 0)
	{
		BufferDesc->Width = G->SCREEN_WIDTH;
		LogInfo("->Forcing Width to = %d\n", BufferDesc->Width);
	}
	if (G->SCREEN_HEIGHT >= 0)
	{
		BufferDesc->Height = G->SCREEN_HEIGHT;
		LogInfo("->Forcing Height to = %d\n", BufferDesc->Height);
	}

	// To support 3D Vision Direct Mode, we need to force the backbuffer from the
	// swapchain to be 2x its normal width.
	//
	// I don't particularly like that we've lumped this in with direct mode
	// - direct mode does *not* require a 2x width back buffer - it has two
	// completely separate back buffers, switched via nvapi. This is a hack
	// for one specific tool that has yet to see the light of day, and
	// ideally this would have been done in that tool, not here. For
	// quickly getting up and running, I don't see why the ordinary
	// resolution overrides would not have been sufficient, or adding a
	// multiplier to those if necessary.
	//   - DarkStarSword
	if (G->gForceStereo == 2)
	{
		BufferDesc->Width *= 2;
		LogInfo("->Direct Mode: Forcing Width to = %d\n", BufferDesc->Width);
	}
}


// -----------------------------------------------------------------------------
// This tweaks the parameters passed to the real CreateSwapChain, to change behavior.
// These global parameters come originally from the d3dx.ini, so the user can
// change them.
//
// There is now also ForceDisplayParams1 which has some overlap.

void ForceDisplayParams(DXGI_SWAP_CHAIN_DESC *pDesc)
{
	if (pDesc == NULL)
		return;

	LogInfo("     Windowed = %d\n", pDesc->Windowed);
	LogInfo("     Width = %d\n", pDesc->BufferDesc.Width);
	LogInfo("     Height = %d\n", pDesc->BufferDesc.Height);
	LogInfo("     Refresh rate = %f\n",
		(float)pDesc->BufferDesc.RefreshRate.Numerator / (float)pDesc->BufferDesc.RefreshRate.Denominator);

	if (G->SCREEN_UPSCALING == 0 && G->SCREEN_FULLSCREEN > 0)
	{
		pDesc->Windowed = false;
		LogInfo("->Forcing Windowed to = %d\n", pDesc->Windowed);
	}

	if (G->SCREEN_FULLSCREEN == 2 || G->SCREEN_UPSCALING > 0)
	{
		// We install this hook on demand to avoid any possible
		// issues with hooking the call when we don't need it:
		// Unconfirmed, but possibly related to:
		// https://forums.geforce.com/default/topic/685657/3d-vision/3dmigoto-now-open-source-/post/4801159/#4801159
		//
		// This hook is also very important in case of Upscaling
		InstallSetWindowPosHook();
	}

	ForceDisplayMode(&pDesc->BufferDesc, pDesc->Windowed);
}

// Different variant for the CreateSwapChainForHwnd.
//
// We absolutely need the force full screen in order to enable 3D.  
// Batman Telltale needs this.
// The rest of the variants are less clear.

static void ForceDisplayParams1(DXGI_SWAP_CHAIN_DESC1 *pDesc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc)
{
	if (pFullscreenDesc) {
		LogInfo("     Windowed = %d\n", pFullscreenDesc->Windowed);
		LogInfo("     Refresh rate = %f\n",
			(float)pFullscreenDesc->RefreshRate.Numerator / (float)pFullscreenDesc->RefreshRate.Denominator);

		if (G->SCREEN_FULLSCREEN > 0)
		{
			pFullscreenDesc->Windowed = false;
			LogInfo("->Forcing Windowed to = %d\n", pFullscreenDesc->Windowed);
		}

		if (G->SCREEN_REFRESH >= 0 && !pFullscreenDesc->Windowed)
		{
			pFullscreenDesc->RefreshRate.Numerator = G->SCREEN_REFRESH;
			pFullscreenDesc->RefreshRate.Denominator = 1;
			LogInfo("->Forcing refresh rate to = %f\n",
				(float)pFullscreenDesc->RefreshRate.Numerator / (float)pFullscreenDesc->RefreshRate.Denominator);
		}
	}

	if (G->SCREEN_FULLSCREEN == 2)
	{
		// We install this hook on demand to avoid any possible
		// issues with hooking the call when we don't need it:
		// Unconfirmed, but possibly related to:
		// https://forums.geforce.com/default/topic/685657/3d-vision/3dmigoto-now-open-source-/post/4801159/#4801159

		InstallSetWindowPosHook();
	}

	if (pDesc)
	{
		LogInfo("     Width = %d\n", pDesc->Width);
		LogInfo("     Height = %d\n", pDesc->Height);

		if (G->SCREEN_WIDTH >= 0)
		{
			LogOverlay(LOG_DIRE, "*** Unimplemented feature to force screen width in CreateSwapChainForHwnd\n");
		}
		if (G->SCREEN_HEIGHT >= 0)
		{
			LogOverlay(LOG_DIRE, "*** Unimplemented feature to force screen height in CreateSwapChainForHwnd\n");
		}

		// To support 3D Vision Direct Mode, we need to force the backbuffer from the
		// swapchain to be 2x its normal width.
		if (G->gForceStereo == 2)
		{
			LogOverlay(LOG_DIRE, "*** Unimplemented feature for Direct Mode in CreateSwapChainForHwnd\n");
		}
	}
}


// -----------------------------------------------------------------------------
// Actual hook for any IDXGICreateSwapChainForHwnd calls the game makes.
// This can only be called with Win7+platform_update or greater, using
// the IDXGIFactory2.
// 
// This type of SwapChain cannot be made through the CreateDeviceAndSwapChain,
// so there is only one logical path to create this, which is 
// IDXGIFactory2->CreateSwapChainForHwnd.  That means that the Device has
// already been created with CreateDevice, and dereferenced through the 
// chain of QueryInterface calls to get the IDXGIFactory2.

HRESULT(__stdcall *fnOrigCreateSwapChainForHwnd)(
	IDXGIFactory2 * This,
	/* [annotation][in] */
	_In_  IUnknown *pDevice,
	/* [annotation][in] */
	_In_  HWND hWnd,
	/* [annotation][in] */
	_In_  const DXGI_SWAP_CHAIN_DESC1 *pDesc,
	/* [annotation][in] */
	_In_opt_  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
	/* [annotation][in] */
	_In_opt_  IDXGIOutput *pRestrictToOutput,
	/* [annotation][out] */
	_Out_  IDXGISwapChain1 **ppSwapChain) = nullptr;


HRESULT __stdcall Hooked_CreateSwapChainForHwnd(
	IDXGIFactory2 * This,
	/* [annotation][in] */
	_In_  IUnknown *pDevice,
	/* [annotation][in] */
	_In_  HWND hWnd,
	/* [annotation][in] */
	_In_  const DXGI_SWAP_CHAIN_DESC1 *pDesc,
	/* [annotation][in] */
	_In_opt_  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
	/* [annotation][in] */
	_In_opt_  IDXGIOutput *pRestrictToOutput,
	/* [annotation][out] */
	_Out_  IDXGISwapChain1 **ppSwapChain)
{
	HackerDevice *hackerDevice = NULL;
	HackerContext *hackerContext = NULL;
	HackerSwapChain *hackerSwapChain = NULL;
	IDXGISwapChain1 *origSwapChain = NULL;

	LogInfo("Hooked IDXGIFactory2::CreateSwapChainForHwnd(%p) called\n", This);
	LogInfo("  Device = %p\n", pDevice);
	LogInfo("  SwapChain = %p\n", ppSwapChain);
	LogInfo("  Description1 = %p\n", pDesc);
	LogInfo("  FullScreenDescription = %p\n", pFullscreenDesc);

	// Save window handle so we can translate mouse coordinates to the window:
	G->hWnd = hWnd;

	if (pDesc != nullptr)
	{
		// Required in case the software mouse and upscaling are on at the same time
		// TODO: Use a helper class to track *all* different resolutions
		G->GAME_INTERNAL_WIDTH = pDesc->Width;
		G->GAME_INTERNAL_HEIGHT = pDesc->Height;

		if (G->mResolutionInfo.from == GetResolutionFrom::SWAP_CHAIN)
		{
			// TODO: Use a helper class to track *all* different resolutions
			G->mResolutionInfo.width = pDesc->Width;
			G->mResolutionInfo.height = pDesc->Height;
			LogInfo("Got resolution from swap chain: %ix%i\n",
				G->mResolutionInfo.width, G->mResolutionInfo.height);
		}
	}

	// Inputs structures are const, so copy them to allow modification:
	DXGI_SWAP_CHAIN_DESC1 desc1 = { 0 };
	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullScreenDesc = { 0 };
	if (pDesc) {
		memcpy(&desc1, pDesc, sizeof(DXGI_SWAP_CHAIN_DESC1));
		pDesc = &desc1;
	}
	if (pFullscreenDesc) {
		memcpy(&fullScreenDesc, pFullscreenDesc, sizeof(DXGI_SWAP_CHAIN_FULLSCREEN_DESC));
		pFullscreenDesc = &fullScreenDesc;
	}
	ForceDisplayParams1(&desc1, &fullScreenDesc);

	// FIXME: Implement upscaling

	hackerDevice = sort_out_swap_chain_device_mess(&pDevice);

	HRESULT hr = fnOrigCreateSwapChainForHwnd(This, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
	if (FAILED(hr))
	{
		LogInfo("->Failed result %#x\n\n", hr);
		goto out_release;
	}

	if (hackerDevice) {
		origSwapChain = *ppSwapChain;
		hackerContext = hackerDevice->GetHackerContext();

		hackerSwapChain = new HackerSwapChain(origSwapChain, hackerDevice, hackerContext);

		// When creating a new swapchain, we can assume this is the game creating
		// the most important object, and return the wrapped swapchain to the game
		// so it will call our Present.
		*ppSwapChain = hackerSwapChain;
	}

	LogInfo("->return result %#x, HackerSwapChain = %p wrapper of ppSwapChain = %p\n\n", hr, hackerSwapChain, origSwapChain);
out_release:
	if (hackerDevice)
		hackerDevice->Release();
	return hr;
}

// -----------------------------------------------------------------------------
// This hook should work in all variants, including the CreateSwapChain1
// and CreateSwapChainForHwnd

void HookCreateSwapChainForHwnd(void* factory2)
{
	LogInfo("*** IDXGIFactory2 creating hook for CreateSwapChainForHwnd. \n");

	IDXGIFactory2* dxgiFactory = reinterpret_cast<IDXGIFactory2*>(factory2);

	SIZE_T hook_id;
	DWORD dwOsErr = cHookMgr.Hook(&hook_id, (void**)&fnOrigCreateSwapChainForHwnd,
		lpvtbl_CreateSwapChainForHwnd(dxgiFactory), Hooked_CreateSwapChainForHwnd, 0);

	if (dwOsErr == ERROR_SUCCESS)
		LogInfo("  Successfully installed IDXGIFactory2->CreateSwapChainForHwnd hook.\n");
	else
		LogInfo("  *** Failed install IDXGIFactory2->CreateSwapChainForHwnd hook.\n");
}

// -----------------------------------------------------------------------------
// Actual hook for any IDXGICreateSwapChain calls the game makes.
//
// There are two primary paths that can arrive here.
// ---1. d3d11->CreateDeviceAndSwapChain
//	This path arrives here with a normal ID3D11Device1 device, not a HackerDevice.
//	This is called implictly from the middle of CreateDeviceAndSwapChain.---
//	No longer necessary, with CreateDeviceAndSwapChain broken into two direct calls.
// 2. IDXGIFactory->CreateSwapChain after CreateDevice
//	This path requires a pDevice passed in, which is a HackerDevice.  This is the
//	secret path, where they take the Device and QueryInterface to get IDXGIDevice
//	up to getting Factory, where they call CreateSwapChain. In this path, we can
//	expect the input pDevice to have already been setup as a HackerDevice.
//
//
// In prior code, we were looking for possible IDXGIDevice's as the pDevice input.
// That should not be a problem now, because we are specifically trying to cast
// that input into an ID3D11Device1 using QueryInterface.  Leaving the original
// code commented out at the bottom of the file, for reference.

HRESULT(__stdcall *fnOrigCreateSwapChain)(
	IDXGIFactory * This,
	/* [annotation][in] */
	_In_  IUnknown *pDevice,
	/* [annotation][in] */
	_In_  DXGI_SWAP_CHAIN_DESC *pDesc,
	/* [annotation][out] */
	_Out_  IDXGISwapChain **ppSwapChain) = nullptr;


HRESULT __stdcall Hooked_CreateSwapChain(
	IDXGIFactory * This,
	/* [annotation][in] */
	_In_  IUnknown *pDevice,
	/* [annotation][in] */
	_In_  DXGI_SWAP_CHAIN_DESC *pDesc,
	/* [annotation][out] */
	_Out_  IDXGISwapChain **ppSwapChain)
{
	HackerDevice *hackerDevice = NULL;
	HackerContext *hackerContext = NULL;
	HackerSwapChain *swapchainWrap = NULL;
	IDXGISwapChain1 *origSwapChain = NULL;

	LogInfo("\nHooked IDXGIFactory::CreateSwapChain(%p) called\n", This);
	LogInfo("  Device = %p\n", pDevice);
	LogInfo("  SwapChain = %p\n", ppSwapChain);
	LogInfo("  Description = %p\n", pDesc);

	hackerDevice = sort_out_swap_chain_device_mess(&pDevice);

	DXGI_SWAP_CHAIN_DESC origSwapChainDesc;
	if (pDesc != nullptr)
	{
		// Save window handle so we can translate mouse coordinates to the window:
		G->hWnd = pDesc->OutputWindow;

		if (G->SCREEN_UPSCALING > 0)
		{
			// Copy input swap chain desc in case it's modified
			memcpy(&origSwapChainDesc, pDesc, sizeof(DXGI_SWAP_CHAIN_DESC));

			// For the upscaling case, fullscreen has to be set after swap chain is created
			pDesc->Windowed = true;
		}

		// Required in case the software mouse and upscaling are on at the same time
		// TODO: Use a helper class to track *all* different resolutions
		G->GAME_INTERNAL_WIDTH = pDesc->BufferDesc.Width;
		G->GAME_INTERNAL_HEIGHT = pDesc->BufferDesc.Height;

		if (G->mResolutionInfo.from == GetResolutionFrom::SWAP_CHAIN)
		{
			// TODO: Use a helper class to track *all* different resolutions
			G->mResolutionInfo.width = pDesc->BufferDesc.Width;
			G->mResolutionInfo.height = pDesc->BufferDesc.Height;
			LogInfo("Got resolution from swap chain: %ix%i\n",
				G->mResolutionInfo.width, G->mResolutionInfo.height);
		}
	}

	ForceDisplayParams(pDesc);

	HRESULT hr = fnOrigCreateSwapChain(This, pDevice, pDesc, ppSwapChain);
	if (FAILED(hr))
	{
		LogInfo("->Failed result %#x\n\n", hr);
		goto out_release;
	}

	if (hackerDevice) {
		// Always upcast to IDXGISwapChain1 whenever possible.
		// If the upcast fails, that means we have a normal IDXGISwapChain,
		// but we'll still store it as an IDXGISwapChain1.  It's a little
		// weird to reinterpret this way, but should cause no problems in
		// the Win7 no platform_udpate case.
		if (SUCCEEDED((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&origSwapChain))))
			(*ppSwapChain)->Release();
		else
			origSwapChain = reinterpret_cast<IDXGISwapChain1*>(*ppSwapChain);

		hackerContext = hackerDevice->GetHackerContext();

		// Original swapchain has been successfully created. Now we want to
		// wrap the returned swapchain as either HackerSwapChain or HackerUpscalingSwapChain.

		if (G->SCREEN_UPSCALING == 0)		// Normal case
		{
			swapchainWrap = new HackerSwapChain(origSwapChain, hackerDevice, hackerContext);
			LogInfo("->HackerSwapChain %p created to wrap %p\n", swapchainWrap, origSwapChain);
		}
		else								// Upscaling case
		{
			swapchainWrap = new HackerUpscalingSwapChain(origSwapChain, hackerDevice, hackerContext,
				&origSwapChainDesc, pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, This);
			LogInfo("  HackerUpscalingSwapChain %p created to wrap %p.\n", swapchainWrap, origSwapChain);

			if (G->SCREEN_UPSCALING == 2 || !origSwapChainDesc.Windowed)
			{
				// Some games react very strange (like render nothing) if set full screen state is called here)
				// Other games like The Witcher 3 need the call to ensure entering the full screen on start
				// (seems to be game internal stuff)  ToDo: retest if this is still necessary, lots of changes.
				origSwapChain->SetFullscreenState(TRUE, nullptr);
			}
		}

		// When creating a new swapchain, we can assume this is the game creating
		// the most important object. Return the wrapped swapchain to the game so it
		// will call our Present.
		*ppSwapChain = swapchainWrap;
	}

	LogInfo("->return result %#x, HackerSwapChain = %p wrapper of ppSwapChain = %p\n\n", hr, swapchainWrap, origSwapChain);
out_release:
	if (hackerDevice)
		hackerDevice->Release();
	return hr;
}

// -----------------------------------------------------------------------------
// This hook should work in all variants, including the CreateSwapChain1
// and CreateSwapChainForHwnd

void HookCreateSwapChain(void* factory)
{
	LogInfo("*** IDXGIFactory creating hook for CreateSwapChain. \n");

	IDXGIFactory* dxgiFactory = reinterpret_cast<IDXGIFactory*>(factory);

	SIZE_T hook_id;
	DWORD dwOsErr = cHookMgr.Hook(&hook_id, (void**)&fnOrigCreateSwapChain,
		lpvtbl_CreateSwapChain(dxgiFactory), Hooked_CreateSwapChain, 0);

	if (dwOsErr == ERROR_SUCCESS)
		LogInfo("  Successfully installed IDXGIFactory->CreateSwapChain hook.\n");
	else
		LogInfo("  *** Failed install IDXGIFactory->CreateSwapChain hook.\n");
}


// -----------------------------------------------------------------------------
// Actual function called by the game for every CreateDXGIFactory they make.
// This is only called for the in-process game, not system wide.
//
// We are going to always upcast to an IDXGIFactory2 for any calls here.
// The only time we'll not use Factory2 is on Win7 without the evil update.

HRESULT(__stdcall *fnOrigCreateDXGIFactory)(
	REFIID riid,
	_Out_ void   **ppFactory
	) = nullptr;

HRESULT __stdcall Hooked_CreateDXGIFactory(REFIID riid, void **ppFactory)
{
	LogInfo("*** Hooked_CreateDXGIFactory called with riid: %s\n", NameFromIID(riid).c_str());

	// If this happens to be first call from the game, let's make sure to load
	// up our d3d11.dll and the .ini file.
	InitD311();

	// If we are being requested to create a DXGIFactory2, lie and say it's not possible.
	if (riid == __uuidof(IDXGIFactory2) && !G->enable_platform_update)
	{
		LogInfo("  returns E_NOINTERFACE as error for IDXGIFactory2.\n");
		*ppFactory = NULL;
		return E_NOINTERFACE;
	}

	HRESULT hr = fnOrigCreateDXGIFactory(riid, ppFactory);
	if (FAILED(hr))
	{
		LogInfo("->failed with HRESULT=%x\n", hr);
		return hr;
	}

	if (!fnOrigCreateSwapChain)
		HookCreateSwapChain(*ppFactory);

	// With the addition of the platform_update, we need to allow for specifically
	// creating a DXGIFactory2 instead of DXGIFactory1.  We want to always upcast
	// the highest supported object for each scenario, to properly suppport
	// QueryInterface and GetParent upcasts.

	IUnknown* factoryUnknown = reinterpret_cast<IUnknown*>(*ppFactory);
	IDXGIFactory2* dxgiFactory = reinterpret_cast<IDXGIFactory2*>(*ppFactory);
	HRESULT res = factoryUnknown->QueryInterface(IID_PPV_ARGS(&dxgiFactory));
	if (SUCCEEDED(res))
	{
		factoryUnknown->Release();
		*ppFactory = (void*)dxgiFactory;
		LogInfo("  Upcast QueryInterface(IDXGIFactory2) returned result = %x, factory = %p\n", res, dxgiFactory);

		if (!fnOrigCreateSwapChainForHwnd)
			HookCreateSwapChainForHwnd(*ppFactory);
	}

	LogInfo("  CreateDXGIFactory returned factory = %p, result = %x\n", *ppFactory, hr);
	return hr;
}


// -----------------------------------------------------------------------------
//
// We are going to always upcast to an IDXGIFactory2 for any calls here.
// The only time we'll not use Factory2 is on Win7 without the evil update.
//
// ToDo: It is probably possible for a game to fetch a Factory2 via QueryInterface,
//  and we might need to hook that as well.  However, in order to Query, they
//  need a Factory or Factory1 to do so, which will call us here anyway.  At least
//  until Win10, where the d3d11.dll also then includes CreateDXGIFactory2. We only 
//  really care about installing a hook for CreateSwapChain which will still get done.

HRESULT(__stdcall *fnOrigCreateDXGIFactory1)(
	REFIID riid,
	_Out_ void   **ppFactory
	) = nullptr;

HRESULT __stdcall Hooked_CreateDXGIFactory1(REFIID riid, void **ppFactory1)
{
	LogInfo("*** Hooked_CreateDXGIFactory1 called with riid: %s\n", NameFromIID(riid).c_str());

	// If this happens to be first call from the game, let's make sure to load
	// up our d3d11.dll and the .ini file.
	InitD311();

	// If we are being requested to create a DXGIFactory2, lie and say it's not possible.
	if (riid == __uuidof(IDXGIFactory2) && !G->enable_platform_update)
	{
		LogInfo("  returns E_NOINTERFACE as error for IDXGIFactory2.\n");
		*ppFactory1 = NULL;
		return E_NOINTERFACE;
	}

	// Call original factory, regardless of what they requested, to keep the
	// same expected sequence from their perspective.  (Which includes refcounts)
	HRESULT hr = fnOrigCreateDXGIFactory1(riid, ppFactory1);
	if (FAILED(hr))
	{
		LogInfo("->failed with HRESULT=%x\n", hr);
		return hr;
	}

	if (!fnOrigCreateSwapChain)
		HookCreateSwapChain(*ppFactory1);

	// With the addition of the platform_update, we need to allow for specifically
	// creating a DXGIFactory2 instead of DXGIFactory1.  We want to always upcast
	// the highest supported object for each scenario, to properly suppport
	// QueryInterface and GetParent upcasts.

	IUnknown* factoryUnknown = reinterpret_cast<IUnknown*>(*ppFactory1);
	IDXGIFactory2* dxgiFactory = reinterpret_cast<IDXGIFactory2*>(*ppFactory1);
	HRESULT res = factoryUnknown->QueryInterface(IID_PPV_ARGS(&dxgiFactory));
	if (SUCCEEDED(res))
	{
		factoryUnknown->Release();
		*ppFactory1 = (void*)dxgiFactory;
		LogInfo("  Upcast QueryInterface(IDXGIFactory2) returned result = %x, factory = %p\n", res, dxgiFactory);

		if (!fnOrigCreateSwapChainForHwnd)
			HookCreateSwapChainForHwnd(*ppFactory1);
	}

	LogInfo("  CreateDXGIFactory1 returned factory = %p, result = %x\n", *ppFactory1, hr);
	return hr;
}


// -----------------------------------------------------------------------------

// Some partly obsolete comments, but still maybe worthwhile as thoughts on DXGI.

// The hooks need to be installed late, and cannot be installed during DLLMain, because
// they need to be installed in the COM object vtable itself, and the order cannot be
// defined that early.  Because the documentation says it's not viable at DLLMain time,
// we'll install these hooks at InitD311() time, essentially the first call of D3D11.
// 
// The piece we care about in DXGI is the swap chain, and we don't otherwise have a
// good way to access it.  It can be created directly via DXGI, and not through 
// CreateDeviceAndSwapChain.
//
// Not certain, but it seems likely that we only need to hook a given instance of the
// calls we want, because they are not true objects with attached vtables, they have
// a non-standard vtable/indexing system, and the main differentiator is the object
// passed in as 'this'.  
//
// After much experimentation and study, it seems clear that we should use the in-proc
// version of Deviare. I tried to see if Deviare2 would be a match, but they have a 
// funny event callback mechanism that requires an ATL object connection, and is not
// really suited for same-process operations.  It's really built with separate
// processes in mind.

// For this object, we want to use the CINTERFACE, not the C++ interface.
// The reason is that it allows us easy access to the COM object vtable, which
// we need to hook in order to override the functions.  Once we include dxgi.h
// it will be defined with C headers instead.
//
// This is a little odd, but it's the way that Detours hooks COM objects, and
// thus it seems superior to the Nektra approach of groping the vtable directly
// using constants and void* pointers.
// 
// This is only used for .cpp file here, not the .h file, because otherwise other
// units get compiled with this CINTERFACE, which wrecks their calling out.


// -----------------------------------------------------------------------------
// Functionality removed during refactoring.
// 
// These are here, because our HackerDXGI is only HackerSwapChain now.
// If we want these calls, we'll need to add further hooks here.


//STDMETHODIMP HackerDXGIFactory::MakeWindowAssociation(THIS_
//	HWND WindowHandle,
//	UINT Flags)
//{
//	if (LogFile)
//	{
//		LogInfo("HackerDXGIFactory::MakeWindowAssociation(%s@%p) called with WindowHandle = %p, Flags = %x\n", type_name(this), this, WindowHandle, Flags);
//		if (Flags) LogInfoNoNL("  Flags =");
//		if (Flags & DXGI_MWA_NO_WINDOW_CHANGES) LogInfoNoNL(" DXGI_MWA_NO_WINDOW_CHANGES(no monitoring)");
//		if (Flags & DXGI_MWA_NO_ALT_ENTER) LogInfoNoNL(" DXGI_MWA_NO_ALT_ENTER");
//		if (Flags & DXGI_MWA_NO_PRINT_SCREEN) LogInfoNoNL(" DXGI_MWA_NO_PRINT_SCREEN");
//		if (Flags) LogInfo("\n");
//	}
//
//	if (G->SCREEN_ALLOW_COMMANDS && Flags)
//	{
//		LogInfo("  overriding Flags to allow all window commands\n");
//
//		Flags = 0;
//	}
//	HRESULT hr = mOrigFactory->MakeWindowAssociation(WindowHandle, Flags);
//	LogInfo("  returns result = %x\n", hr);
//
//	return hr;
//}
//
//

//
//static bool FilterRate(int rate)
//{
//	if (!G->FILTER_REFRESH[0]) return false;
//	int i = 0;
//	while (G->FILTER_REFRESH[i] && G->FILTER_REFRESH[i] != rate)
//		++i;
//	return G->FILTER_REFRESH[i] == 0;
//}
//
//STDMETHODIMP HackerDXGIOutput::GetDisplayModeList(THIS_
//	/* [in] */ DXGI_FORMAT EnumFormat,
//	/* [in] */ UINT Flags,
//	/* [annotation][out][in] */
//	__inout  UINT *pNumModes,
//	/* [annotation][out] */
//	__out_ecount_part_opt(*pNumModes, *pNumModes)  DXGI_MODE_DESC *pDesc)
//{
//	LogInfo("HackerDXGIOutput::GetDisplayModeList(%s@%p) called\n", type_name(this), this);
//
//	HRESULT ret = mOrigOutput->GetDisplayModeList(EnumFormat, Flags, pNumModes, pDesc);
//	if (ret == S_OK && pDesc)
//	{
//		for (UINT j = 0; j < *pNumModes; ++j)
//		{
//			int rate = pDesc[j].RefreshRate.Numerator / pDesc[j].RefreshRate.Denominator;
//			if (FilterRate(rate))
//			{
//				LogInfo("  Skipping mode: width=%d, height=%d, refresh rate=%f\n", pDesc[j].Width, pDesc[j].Height,
//					(float)pDesc[j].RefreshRate.Numerator / (float)pDesc[j].RefreshRate.Denominator);
//				// ToDo: Does this work?  I have no idea why setting width and height to 8 would matter.
//				pDesc[j].Width = 8; pDesc[j].Height = 8;
//			}
//			else
//			{
//				LogInfo("  Mode detected: width=%d, height=%d, refresh rate=%f\n", pDesc[j].Width, pDesc[j].Height,
//					(float)pDesc[j].RefreshRate.Numerator / (float)pDesc[j].RefreshRate.Denominator);
//			}
//		}
//	}
//
//	return ret;
//}
//
//STDMETHODIMP HackerDXGIOutput::FindClosestMatchingMode(THIS_
//	/* [annotation][in] */
//	__in  const DXGI_MODE_DESC *pModeToMatch,
//	/* [annotation][out] */
//	__out  DXGI_MODE_DESC *pClosestMatch,
//	/* [annotation][in] */
//	__in_opt  IUnknown *pConcernedDevice)
//{
//	if (pModeToMatch) LogInfo("HackerDXGIOutput::FindClosestMatchingMode(%s@%p) called: width=%d, height=%d, refresh rate=%f\n", type_name(this), this,
//		pModeToMatch->Width, pModeToMatch->Height, (float)pModeToMatch->RefreshRate.Numerator / (float)pModeToMatch->RefreshRate.Denominator);
//
//	HRESULT hr = mOrigOutput->FindClosestMatchingMode(pModeToMatch, pClosestMatch, pConcernedDevice);
//
//	if (pClosestMatch && G->SCREEN_REFRESH >= 0)
//	{
//		pClosestMatch->RefreshRate.Numerator = G->SCREEN_REFRESH;
//		pClosestMatch->RefreshRate.Denominator = 1;
//	}
//	if (pClosestMatch && G->SCREEN_WIDTH >= 0) pClosestMatch->Width = G->SCREEN_WIDTH;
//	if (pClosestMatch && G->SCREEN_HEIGHT >= 0) pClosestMatch->Height = G->SCREEN_HEIGHT;
//	if (pClosestMatch) LogInfo("  returning width=%d, height=%d, refresh rate=%f\n",
//		pClosestMatch->Width, pClosestMatch->Height, (float)pClosestMatch->RefreshRate.Numerator / (float)pClosestMatch->RefreshRate.Denominator);
//
//	LogInfo("  returns hr=%x\n", hr);
//	return hr;
//}
//
//
