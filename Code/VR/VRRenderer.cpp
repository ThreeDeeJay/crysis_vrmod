#include "StdAfx.h"
#include "VRRenderer.h"

#include "Cry_Camera.h"
#include "Game.h"
#include "GameCVars.h"
#include "Hooks.h"
#include "IRenderAuxGeom.h"
#include "IronSight.h"
#include "IVehicleSystem.h"
#include "OpenXRRuntime.h"
#include "Player.h"
#include "RocketLauncher.h"
#include "VRManager.h"
#include "VRRenderUtils.h"
#include "Weapon.h"
#include "Menus/FlashMenuObject.h"
#include <imgui.h>
#include <backends/imgui_impl_dx10.h>

#include "BitFiddling.h"

namespace
{
	VRRenderer g_vrRendererImpl;
}

VRRenderer* gVRRenderer = &g_vrRendererImpl;

extern ID3D10Device1 *g_latestCreatedDevice;
extern IDXGISwapChain *g_latestCreatedSwapChain;

HRESULT STDMETHODCALLTYPE IDXGISwapChain_Present(IDXGISwapChain *pSelf, UINT SyncInterval, UINT Flags)
{
	HRESULT hr = 0;

	if (gVRRenderer->OnPrePresent(pSelf))
	{
		hr = hooks::CallOriginal(IDXGISwapChain_Present)(pSelf, SyncInterval, Flags);
		gVRRenderer->OnPostPresent();
	}

	return hr;
}

HRESULT STDMETHODCALLTYPE IDXGISwapChain_ResizeTarget(IDXGISwapChain *pSelf, const DXGI_MODE_DESC *pNewTargetParameters)
{
	if (!gVRRenderer->ShouldIgnoreWindowSizeChanges())
	{
		gVRRenderer->SetDesiredWindowSize(pNewTargetParameters->Width, pNewTargetParameters->Height);
		return hooks::CallOriginal(IDXGISwapChain_ResizeTarget)(pSelf, pNewTargetParameters);
	}

	return 0;
}

HRESULT STDMETHODCALLTYPE IDXGISwapChain_ResizeBuffers(IDXGISwapChain *pSelf, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	return hooks::CallOriginal(IDXGISwapChain_ResizeBuffers)(pSelf, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

BOOL WINAPI Hook_SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int  X, int  Y, int  cx, int  cy, UINT uFlags)
{
	if (!gVRRenderer->ShouldIgnoreWindowSizeChanges())
	{
		return hooks::CallOriginal(Hook_SetWindowPos)(hWnd, hWndInsertAfter, 0, 0, cx, cy, uFlags);
	}

	return TRUE;
}

void FASTCALL VR_ISystem_Render(ISystem *pSelf)
{
	gVRRenderer->Render(hooks::CallOriginal(VR_ISystem_Render), pSelf);
}

void FASTCALL VR_ISystem_Quit(ISystem *pSelf)
{
	gVRRenderer->Shutdown();
	gVR->Shutdown();
	gXR->Shutdown();
	hooks::CallOriginal(VR_ISystem_Quit)(pSelf);
}

void FASTCALL VR_CryRenderD3D10_Shadows(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4)
{
	// only execute this function for the first eye, as shadow maps do not need to be regenerated for the second eye
	// should save a small bit of performance
	if (gVRRenderer->ShouldRenderShadowMaps() || !g_pGameCVars->vr_shadow_optimization)
		hooks::CallOriginal(VR_CryRenderD3D10_Shadows)(arg1, arg2, arg3, arg4);
}

void *ModuleAddress(void *base, std::size_t offset)
{
	return static_cast<unsigned char*>(base) + offset;
}

void VRRenderer::Init()
{
	hooks::InstallVirtualFunctionHook("ISystem::Render", gEnv->pSystem, &ISystem::Render, &VR_ISystem_Render);
	hooks::InstallHook("SetWindowPos", &SetWindowPos, &Hook_SetWindowPos);
	hooks::InstallVirtualFunctionHook("ISystem::Quit", gEnv->pSystem, &ISystem::Quit, &VR_ISystem_Quit);

	m_lastPresentCallTime = gEnv->pTimer->GetAsyncTime().GetMilliSecondsAsInt64();

	HMODULE renderDll = GetModuleHandleW(L"CryRenderD3D10.dll");
	if (renderDll)
	{
		CryLogAlways("Found handle for D3D10 renderer, attempting to hook shadow rendering function");
		hooks::InstallHook("CryD3D10Render::Shadows", ModuleAddress(renderDll, 0x0bee30), &VR_CryRenderD3D10_Shadows);
	}

	IDXGISwapChain *swapChain = g_latestCreatedSwapChain;
	CryLogAlways("Retrieved swap chain: %ul", (uintptr_t)swapChain);

	if (swapChain != nullptr)
	{
		hooks::InstallVirtualFunctionHook("IDXGISwapChain::Present", swapChain, 8, &IDXGISwapChain_Present);
		hooks::InstallVirtualFunctionHook("IDXGISwapChain::ResizeBuffers", swapChain, 13, &IDXGISwapChain_ResizeBuffers);
		hooks::InstallVirtualFunctionHook("IDXGISwapChain::ResizeTarget", swapChain, 14, &IDXGISwapChain_ResizeTarget);
		gVR->SetSwapChain(swapChain);
	}

	ComPtr<ID3D10Device1> device;
	swapChain->GetDevice(__uuidof(ID3D10Device1), (void**)device.ReleaseAndGetAddressOf());
	m_gui.Init(device.Get());
}

void VRRenderer::Shutdown()
{
	m_gui.Shutdown();
}

void VRRenderer::Render(SystemRenderFunc renderFunc, ISystem* pSystem)
{
	UpdateShaderParamsForReflexSight();

	IDXGISwapChain *currentSwapChain = g_latestCreatedSwapChain;
	int64 milliSecsSinceLastPresentCall = gEnv->pTimer->GetAsyncTime().GetMilliSecondsAsInt64() - m_lastPresentCallTime;
	if (currentSwapChain != gVR->GetSwapChain() || milliSecsSinceLastPresentCall > 1000)
	{
		CryLogAlways("May have lost our Present hook, recreating!");
		hooks::RemoveHook(&IDXGISwapChain_Present);
		hooks::RemoveHook(&IDXGISwapChain_ResizeTarget);
		hooks::RemoveHook(&IDXGISwapChain_ResizeBuffers);
		hooks::InstallVirtualFunctionHook("IDXGISwapChain::Present", currentSwapChain, 8, &IDXGISwapChain_Present);
		hooks::InstallVirtualFunctionHook("IDXGISwapChain::ResizeBuffers", currentSwapChain, 13, &IDXGISwapChain_ResizeBuffers);
		hooks::InstallVirtualFunctionHook("IDXGISwapChain::ResizeTarget", currentSwapChain, 14, &IDXGISwapChain_ResizeTarget);
		gVR->SetSwapChain(currentSwapChain);
	}
	m_originalViewCamera = pSystem->GetViewCamera();

	gVR->AwaitFrame();

	RenderSingleEye(0, renderFunc, pSystem);
	// need to call RenderBegin to reset state, otherwise we get messed up object culling and other issues
	pSystem->RenderBegin();
	RenderSingleEye(1, renderFunc, pSystem);

	Vec2i renderSize = gVR->GetRenderSize();
	gEnv->pRenderer->SetScissor(0, 0, renderSize.x, renderSize.y);
	// clear render target to fully transparent for HUD render
	ColorF transparent(0, 0, 0, 0);
	gEnv->pRenderer->ClearBuffer(FRT_CLEAR_COLOR | FRT_CLEAR_IMMEDIATE, &transparent);

	if (GetRenderMode() == RM_2D)
	{
		CCamera cam = m_originalViewCamera;
		gVR->ModifyCameraFor2D(cam);
		pSystem->SetViewCamera(cam);
		// for things like the binoculars, we skip the stereo rendering and instead render to the 2D screen
		renderFunc(pSystem);
		pSystem->SetViewCamera(m_originalViewCamera);
	}

	m_didRenderThisFrame = true;
}

bool VRRenderer::OnPrePresent(IDXGISwapChain *swapChain)
{
	m_lastPresentCallTime = gEnv->pTimer->GetAsyncTime().GetMilliSecondsAsInt64();

	if (SAFE_MENU_FUNC_RET(IsMenuActive()) && !SAFE_MENU_FUNC_RET(IsLoadingScreenActive()))
	{
		m_gui.Render();
	}

	gVR->SetSwapChain(swapChain);
	gVR->CaptureHUD();

	return true;
}

void VRRenderer::OnPostPresent()
{
	gVR->FinishFrame(m_didRenderThisFrame);
	m_didRenderThisFrame = false;
}

const CCamera& VRRenderer::GetCurrentViewCamera() const
{
	if (m_viewCamOverridden)
		return m_originalViewCamera;

	return gEnv->pSystem->GetViewCamera();
}

void VRRenderer::ProjectToScreenPlayerCam(float ptx, float pty, float ptz, float* sx, float* sy, float* sz)
{
	const CCamera &currentCam = gEnv->pRenderer->GetCamera();
	gEnv->pRenderer->SetCamera(GetCurrentViewCamera());
	gEnv->pRenderer->ProjectToScreen(ptx, pty, ptz, sx, sy, sz);
	gEnv->pRenderer->SetCamera(currentCam);
}

void VRRenderer::SetDesiredWindowSize(int width, int height)
{
	Vec2i renderSize = gVR->GetRenderSize();
	//ImGui::GetStyle() = ImGuiStyle();
	//ImGui::StyleColorsDark();
	//ImGui::GetStyle().ScaleAllSizes(float(renderSize.y) / height);
	//ImGui::GetIO().FontGlobalScale = float(renderSize.y) / height;
	m_windowWidth = width;
	m_windowHeight = height;
}

Vec2i VRRenderer::GetWindowSize() const
{
	return Vec2i(m_windowWidth, m_windowHeight);
}

void VRRenderer::ChangeRenderResolution(int width, int height)
{
	m_ignoreWindowSizeChanges = true;
	gEnv->pRenderer->ChangeResolution(width, height, 8, 0, false);
	gEnv->pRenderer->EnableVSync(false);
	m_ignoreWindowSizeChanges = false;
}

bool VRRenderer::ShouldRenderVR() const
{
	if (g_pGameCVars->vr_cutscenes_2d && g_pGame->GetIGameFramework()->GetIViewSystem()->IsPlayingCutScene())
		return false;

	if (SAFE_MENU_FUNC_RET(IsLoadingScreenActive()))
		return false;

	CPlayer *pPlayer = static_cast<CPlayer *>(gEnv->pGame->GetIGameFramework()->GetClientActor());
	if (pPlayer)
	{
		if (CWeapon* weapon = pPlayer->GetWeapon(pPlayer->GetCurrentItemId(false)))
		{
			if (weapon->IsZoomed() || weapon->IsZooming())
				return false;
		}

		if (pPlayer->GetLinkedVehicle() && g_pGameCVars->vr_vehicle_2d)
		{
			return false;
		}
	}

	return !m_binocularsActive;
}

VRRenderMode VRRenderer::GetRenderMode() const
{
	if (ShouldRenderVR())
		return RM_VR;

	if (m_binocularsActive)
		return RM_2D;

	if (CPlayer* player = gVR->GetLocalPlayer())
	{
		if (CWeapon* weapon = player->GetWeapon(player->GetCurrentItemId()))
		{
			if (weapon->IsZoomed() || weapon->IsZooming())
				return RM_2D;
		}
	}

	return g_pGameCVars->vr_cinema_3d ? RM_3D : RM_2D;
}

extern void DrawHUDFaders();

void VRRenderer::RenderSingleEye(int eye, SystemRenderFunc renderFunc, ISystem* pSystem)
{
	VRRenderMode renderMode = GetRenderMode();

	m_currentEye = eye;

	ICVar* particlesDebug = gEnv->pConsole->GetCVar("e_particles_debug");
	int origParticlesDebug = particlesDebug ? particlesDebug->GetIVal() : 0;
	if (particlesDebug && eye == 1)
	{
		// this disables updating of the particles system, to avoid doing extra work for the second eye
		particlesDebug->SetFlags(particlesDebug->GetFlags() & (~VF_CHEAT));
		particlesDebug->Set((int)(origParticlesDebug | AlphaBit('z')));
	}

	CCamera eyeCam = m_originalViewCamera;
	if (renderMode == RM_VR)
	{
		gVR->ModifyViewCamera(eye, eyeCam);
		float fov = eyeCam.GetFov();
		gEnv->pRenderer->EF_Query(EFQ_DrawNearFov, (INT_PTR)&fov);
	}
	else
	{
		gVR->ModifyViewCameraFor3DCinema(eye, eyeCam);
	}
	pSystem->SetViewCamera(eyeCam);
	m_viewCamOverridden = true;

	ColorF black(0, 0, 0, 1);
	gEnv->pRenderer->ClearBuffer(FRT_CLEAR_COLOR | FRT_CLEAR_IMMEDIATE, &black);

	CFlashMenuObject* menu = static_cast<CGame*>(gEnv->pGame)->GetMenu();
	// do not render while in menu, as it shows a rotating game world that is disorienting
	if (!menu->IsMenuActive() && renderMode != RM_2D)
	{
		renderFunc(pSystem);
		if (renderMode == RM_VR)
			DrawCrosshair();
	}

	pSystem->SetViewCamera(m_originalViewCamera);
	m_viewCamOverridden = false;

	DrawHUDFaders();

	gVR->CaptureEye(eye);

	if (particlesDebug && eye == 1)
	{
		particlesDebug->Set(origParticlesDebug);
	}

	m_currentEye = -1;
}

void VRRenderer::DrawCrosshair()
{
	// don't show crosshair during cutscenes
	if (gEnv->pGame->GetIGameFramework()->GetIViewSystem()->IsPlayingCutScene())
		return;

	CPlayer *pPlayer = static_cast<CPlayer *>(gEnv->pGame->GetIGameFramework()->GetClientActor());
	if (!pPlayer)
		return;
	if (pPlayer->GetLinkedVehicle() || pPlayer->GetActorStats()->mountedWeaponID)
		return;

	if (CWeapon *weapon = pPlayer->GetWeapon(pPlayer->GetCurrentItemId(true)))
	{
		// don't draw a crosshair if the weapon laser is active
		if (weapon->IsLamLaserActivated() || weapon->GetEntity()->GetClass() == CItem::sRocketLauncherClass)
			return;
	}

	const CCamera& cam = m_originalViewCamera;
	Vec3 crosshairPos = cam.GetPosition();
	Vec3 dir = cam.GetViewdir();

	SMovementState moveState;
	pPlayer->GetMovementController()->GetMovementState(moveState);
	crosshairPos = moveState.weaponPosition;
	dir = moveState.fireDirection;

	dir.Normalize();
	float maxDistance = 10.f;

	std::vector<IPhysicalEntity*> skipEntities;
	skipEntities.push_back(pPlayer->GetEntity()->GetPhysics());
	if (pPlayer->GetLinkedVehicle())
	{
		if (!g_pGameCVars->vr_vehicle_crosshair)
			return;

		skipEntities.push_back(pPlayer->GetLinkedVehicle()->GetEntity()->GetPhysics());
		IPhysicalEntity* vecSkipEnts[8];
		int numSkips = pPlayer->GetLinkedVehicle()->GetSkipEntities(vecSkipEnts, 8);
		for (int i = 0; i < numSkips; ++i)
			skipEntities.push_back(vecSkipEnts[i]);
		maxDistance = 16.f;
	}
	else if (pPlayer->GetActorStats()->mountedWeaponID)
	{
		if (!g_pGameCVars->vr_vehicle_crosshair)
			return;
	}
	else
	{
		if (!g_pGameCVars->vr_weapon_crosshair)
			return;
	}

	const int objects = ent_all;
	const int flags = (geom_colltype_ray << rwi_colltype_bit) | rwi_colltype_any | (10 & rwi_pierceability_mask) | (geom_colltype14 << rwi_colltype_bit);
	ray_hit hit;
	if (gEnv->pPhysicalWorld->RayWorldIntersection(crosshairPos, dir*maxDistance, objects, flags, &hit, 1, skipEntities.data(), skipEntities.size()))
	{
		crosshairPos = hit.pt;
	}
	else
	{
		crosshairPos += dir * maxDistance;
	}

	// for the moment, draw something primitive with the debug tools. Maybe later we can find something more elegant...
	SAuxGeomRenderFlags geomMode;
	geomMode.SetDepthTestFlag(e_DepthTestOff);
	geomMode.SetMode2D3DFlag(e_Mode3D);
	geomMode.SetDrawInFrontMode(e_DrawInFrontOn);
	gEnv->pRenderer->GetIRenderAuxGeom()->SetRenderFlags(geomMode);
	gEnv->pRenderer->GetIRenderAuxGeom()->DrawSphere(crosshairPos, 0.03f, ColorB(240, 240, 240));
	gEnv->pRenderer->GetIRenderAuxGeom()->Flush();
}

void VRRenderer::UpdateShaderParamsForReflexSight()
{
	// hack to get the reflex sights working
	// added a new public param to the shader that we need to feed with a world position for where the dot should be

	CPlayer* player = gVR->GetLocalPlayer();
	CWeapon* weapon = player ? player->GetWeapon(player->GetCurrentItemId()) : nullptr;

	if (!weapon)
		return;

	auto* matMgr = gEnv->p3DEngine->GetMaterialManager();
	IMaterial* mat = matMgr->FindMaterial("objects/weapons/attachments/reflex_rifle/reflex_rifle");
	if (!mat)
		return;
	mat = mat->GetSubMtl(1);
	if (!mat)
		return;

	SInputShaderResources updatedRes(mat->GetShaderItem().m_pShaderResources);
	auto& params = updatedRes.m_ShaderParams;
	// find or insert the ScopePos parameter
	SShaderParam* parm = nullptr;
	for (auto& param : params)
	{
		if (stricmp(param.m_Name, "ScopePos") == 0)
		{
			parm = &param;
			break;
		}
	}
	if (!parm)
	{
		parm = &(*params.push_back());
		strcpy(parm->m_Name, "ScopePos");
		parm->m_Type = eType_VECTOR;
	}

	// calc position for the reflex sight dot
	SMovementState moveState;
	player->GetMovementController()->GetMovementState(moveState);
	Vec3 dotPos = moveState.weaponPosition + 10 * moveState.fireDirection;

	parm->m_Value.m_Vector[0] = dotPos.x;
	parm->m_Value.m_Vector[1] = dotPos.y;
	parm->m_Value.m_Vector[2] = dotPos.z;

	mat->GetShaderItem().m_pShaderResources->SetShaderParams(&updatedRes, mat->GetShaderItem().m_pShader);
}

void VRRenderer::RenderImGui()
{
}

void VRRenderer::DrawImGui()
{
}
