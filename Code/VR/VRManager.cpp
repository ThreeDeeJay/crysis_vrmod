#include "StdAfx.h"
#include "VRManager.h"
#include <openvr.h>

VRManager s_VRManager;
VRManager* gVR = &s_VRManager;

bool VRManager::Init()
{
	if (m_initialized)
		return true;

	vr::EVRInitError error;
	vr::VR_Init(&error, vr::VRApplication_Scene);
	if (error != vr::VRInitError_None)
	{
		CryError("Failed to initialize OpenVR: %s", vr::VR_GetVRInitErrorAsEnglishDescription(error));
		return false;
	}

	vr::VRCompositor()->SetTrackingSpace(vr::TrackingUniverseSeated);

	m_initialized = true;
	return true;
}

void VRManager::Shutdown()
{
	m_eyeTextures[0].Reset();
	m_eyeTextures[1].Reset();
	m_device.Reset();

	if (!m_initialized)
		return;

	vr::VR_Shutdown();
	m_initialized = false;
}

void VRManager::AwaitFrame()
{
	if (!m_initialized)
		return;

	vr::VRCompositor()->WaitGetPoses(nullptr, 0, nullptr, 0);
}

void VRManager::CaptureEye(IDXGISwapChain *swapchain)
{
	if (!m_device)
		InitDevice(swapchain);

	if (!m_eyeTextures[m_currentEye])
	{
		CreateEyeTexture(m_currentEye);
		if (!m_eyeTextures[m_currentEye])
			return;
	}

	D3D10_TEXTURE2D_DESC desc;
	m_eyeTextures[m_currentEye]->GetDesc(&desc);
	Vec2i expectedSize = GetRenderSize();
	if (desc.Width != expectedSize.x || desc.Height != expectedSize.y)
	{
		// recreate with new resolution
		CreateEyeTexture(m_currentEye);
		if (!m_eyeTextures[m_currentEye])
			return;
	}

	// acquire and copy the current swap chain buffer to the eye texture
	ComPtr<ID3D10Texture2D> texture;
	swapchain->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)texture.GetAddressOf());
	if (!texture)
	{
		CryLogAlways("Error: failed to acquire current swapchain buffer");
		return;
	}

	D3D10_TEXTURE2D_DESC rtDesc;
	texture->GetDesc(&rtDesc);
	if (rtDesc.SampleDesc.Count > 1)
	{
		CryLogAlways("Resolving back buffer...");
		m_device->ResolveSubresource(m_eyeTextures[m_currentEye].Get(), 0, texture.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
	}
	else
	{
		CryLogAlways("Copying back buffer...");
		m_device->CopySubresourceRegion(m_eyeTextures[m_currentEye].Get(), 0, 0, 0, 0, texture.Get(), 0, nullptr);
	}
	m_device->Flush();

	CryLogAlways("Submitting VR eye texture...");
	vr::Texture_t vrTexData;
	vrTexData.eType = vr::TextureType_DirectX;
	vrTexData.eColorSpace = vr::ColorSpace_Auto;
	vrTexData.handle = m_eyeTextures11[m_currentEye].Get();
	auto error = vr::VRCompositor()->Submit(m_currentEye == 0 ? vr::Eye_Left : vr::Eye_Right, &vrTexData);
	if (error != vr::VRCompositorError_None)
	{
		CryLogAlways("Submitting eye texture failed: %i", error);
	}
}

void VRManager::FinishFrame()
{
	if (!m_initialized)
		return;

	vr::VRCompositor()->PostPresentHandoff();
}

Vec2i VRManager::GetRenderSize() const
{
	if (!m_initialized)
		return Vec2i(1280, 800);

	uint32_t width, height;
	vr::VRSystem()->GetRecommendedRenderTargetSize(&width, &height);
	return Vec2i(width, height);
}

void VRManager::SetCurrentEyeTarget(int eye)
{
	m_currentEye = eye;
}

void VRManager::InitDevice(IDXGISwapChain* swapchain)
{
	// fixme: for some reason, we cannot submit textures created by the game's D3D10 to OpenVR directly.
	// It will complain about shared resources not being supported.
	// If we create our own device (D3D10 or D3D11, doesn't matter, might as well make it D3D11 for future OpenXR support),
	// then we can share textures with that devices and submitting those shared textures to OpenVR works :shrug:
	// But only if we don't use the IDXGIAdapter that the game uses directly, but rather look for it ourselves.
	// Something about that particular adapter instance throws OpenVR.
	// Maybe at some point I can figure it out and we can remove this entire second device and just submit directly.
	// Note: the game does use DXGI 1.1, everything looks fine, not sure what's going on.

	CryLogAlways("Acquiring device...");
	swapchain->GetDevice(__uuidof(ID3D10Device1), (void**)m_device.ReleaseAndGetAddressOf());
	if (!m_device)
	{
		CryLogAlways("Failed to get game's D3D10.1 device!");
		return;
	}

	if (!m_initialized)
		return;

	ComPtr<IDXGIDevice> dxgiDevice;
	m_device->QueryInterface(_uuidof(IDXGIDevice), (void**)dxgiDevice.GetAddressOf());
	ComPtr<IDXGIAdapter> dxgiAdapter;
	dxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf());

	ComPtr<IDXGIAdapter> adapterToUse;
	if (dxgiAdapter)
	{
		// if we use this adapter directly for our copy device, OpenVR submission fails. Instead, look up a matching adapter ourselves

		DXGI_ADAPTER_DESC refDesc;
		dxgiAdapter->GetDesc(&refDesc);

		ComPtr<IDXGIFactory1> factory;
		CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf());
		UINT idx = 0;
		while (factory->EnumAdapters(idx, adapterToUse.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND)
		{
			++idx;
			DXGI_ADAPTER_DESC desc;
			adapterToUse->GetDesc(&desc);
			if (desc.AdapterLuid.HighPart == refDesc.AdapterLuid.HighPart && desc.AdapterLuid.LowPart == refDesc.AdapterLuid.LowPart)
			{
				CryLogAlways("Found matching DXGI adapter: %ls", desc.Description);
				break;
			}
		}
	}

	if (!adapterToUse)
	{
		CryLogAlways("Warning: failed to find game's DXGI Adapter, falling back to default adapter");
	}

	HRESULT hr = D3D11CreateDevice(adapterToUse.Get(), adapterToUse ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, m_device11.ReleaseAndGetAddressOf(), nullptr, m_context.ReleaseAndGetAddressOf());
	CryLogAlways("D3D11 creation result: %i", hr);
}

void VRManager::CreateEyeTexture(int eye)
{
	if (!m_device)
		return;

	Vec2i size = GetRenderSize();
	CryLogAlways("Creating eye texture %i: %i x %i", eye, size.x, size.y);

	D3D10_TEXTURE2D_DESC desc = {};
	desc.Width = size.x;
	desc.Height = size.y;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.ArraySize = 1;
	desc.MipLevels = 1;
	desc.Usage = D3D10_USAGE_DEFAULT;
	desc.BindFlags = D3D10_BIND_SHADER_RESOURCE | D3D10_BIND_RENDER_TARGET;
	desc.MiscFlags = D3D10_RESOURCE_MISC_SHARED;
	HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, m_eyeTextures[eye].ReleaseAndGetAddressOf());
	CryLogAlways("CreateTexture2D return code: %i", hr);

	if (!m_initialized || !m_device11)
		return;

	ComPtr<IDXGIResource> dxgiRes;
	m_eyeTextures[eye]->QueryInterface(__uuidof(IDXGIResource), (void**)dxgiRes.GetAddressOf());

	HANDLE handle;
	dxgiRes->GetSharedHandle(&handle);
	hr = m_device11->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void**)m_eyeTextures11[eye].ReleaseAndGetAddressOf());
	CryLogAlways("OpenSharedResource return code: %i", hr);
}
