#include "Library/CrashLogger.h"
#include "Library/OS.h"

#include "../CPUInfo.h"
#include "../LauncherCommon.h"
#include "../MemoryPatch.h"

#include "GameLauncher.h"
#include "LanguageHook.h"
#include "Library/StringFormat.h"

#define DEFAULT_LOG_FILE_NAME "Game.log"

static std::FILE* OpenLogFile()
{
	return LauncherCommon::OpenLogFile(DEFAULT_LOG_FILE_NAME);
}

GameLauncher::GameLauncher() : m_pGameStartup(NULL), m_params(), m_dlls()
{
}

GameLauncher::~GameLauncher()
{
	if (m_pGameStartup)
	{
		m_pGameStartup->Shutdown();
	}
}

int GameLauncher::Run()
{
	m_params.hInstance = OS::EXE::Get();
	m_params.logFileName = DEFAULT_LOG_FILE_NAME;

	char cmdLine[2048];
	snprintf(cmdLine, sizeof(cmdLine), "%s -dx10 -MOD VRMod", OS::CmdLine::Get());

	LauncherCommon::SetParamsCmdLine(m_params, cmdLine);

	CrashLogger::Enable(&OpenLogFile);

	this->LoadEngine();
	this->PatchEngine();

	m_pGameStartup = LauncherCommon::StartEngine(m_dlls.isWarhead ? m_dlls.pEXE : m_dlls.pCryGame, m_params);

	return m_pGameStartup->Run(NULL);
}

void GameLauncher::LoadEngine()
{
    LoadVRMod();
	m_dlls.pCrySystem = LauncherCommon::LoadDLL("CrySystem.dll");

	m_dlls.gameBuild = LauncherCommon::GetGameBuild(m_dlls.pCrySystem);
	m_dlls.isWarhead = LauncherCommon::IsCrysisWarhead(m_dlls.gameBuild);

	LauncherCommon::VerifyGameBuild(m_dlls.gameBuild);

	if (m_dlls.isWarhead)
	{
		m_dlls.pEXE = LauncherCommon::LoadCrysisWarheadEXE();
	}
	else
	{
		m_dlls.pCryGame = LauncherCommon::LoadDLL("CryGame.dll");
		m_dlls.pCryAction = LauncherCommon::LoadDLL("CryAction.dll");
	}

	m_dlls.pCryNetwork = LauncherCommon::LoadDLL("CryNetwork.dll");

	if (!m_params.isDedicatedServer && !OS::CmdLine::HasArg("-dedicated"))
	{
		if (LauncherCommon::IsDX10())
		{
			m_dlls.pCryRenderD3D10 = LauncherCommon::LoadDLL("CryRenderD3D10.dll");
		}
		else
		{
			m_dlls.pCryRenderD3D9 = LauncherCommon::LoadDLL("CryRenderD3D9.dll");
		}
	}
}

void GameLauncher::PatchEngine()
{
	const bool patchIntros = !OS::CmdLine::HasArg("-splash");

	if (m_dlls.isWarhead && m_dlls.pEXE)
	{
		if (patchIntros)
		{
			MemoryPatch::CryGame::DisableIntros(m_dlls.pEXE, m_dlls.gameBuild);
		}

		MemoryPatch::CryAction::AllowDX9ImmersiveMultiplayer(m_dlls.pEXE, m_dlls.gameBuild);

		MemoryPatch::WarheadEXE::FixHInstance(m_dlls.pEXE, m_dlls.gameBuild);
	}

	if (m_dlls.pCryGame)
	{
		MemoryPatch::CryGame::CanJoinDX10Servers(m_dlls.pCryGame, m_dlls.gameBuild);
		MemoryPatch::CryGame::EnableDX10Menu(m_dlls.pCryGame, m_dlls.gameBuild);

		if (patchIntros)
		{
			MemoryPatch::CryGame::DisableIntros(m_dlls.pCryGame, m_dlls.gameBuild);
		}
	}

	if (m_dlls.pCryAction)
	{
		MemoryPatch::CryAction::AllowDX9ImmersiveMultiplayer(m_dlls.pCryAction, m_dlls.gameBuild);
	}

	if (m_dlls.pCryNetwork)
	{
		MemoryPatch::CryNetwork::EnablePreordered(m_dlls.pCryNetwork, m_dlls.gameBuild);
		MemoryPatch::CryNetwork::AllowSameCDKeys(m_dlls.pCryNetwork, m_dlls.gameBuild);
		MemoryPatch::CryNetwork::FixInternetConnect(m_dlls.pCryNetwork, m_dlls.gameBuild);
		MemoryPatch::CryNetwork::FixFileCheckCrash(m_dlls.pCryNetwork, m_dlls.gameBuild);
	}

	if (m_dlls.pCrySystem)
	{
		MemoryPatch::CrySystem::RemoveSecuROM(m_dlls.pCrySystem, m_dlls.gameBuild);
		MemoryPatch::CrySystem::AllowDX9VeryHighSpec(m_dlls.pCrySystem, m_dlls.gameBuild);
		MemoryPatch::CrySystem::AllowMultipleInstances(m_dlls.pCrySystem, m_dlls.gameBuild);
		MemoryPatch::CrySystem::DisableCrashHandler(m_dlls.pCrySystem, m_dlls.gameBuild);
		MemoryPatch::CrySystem::FixCPUInfoOverflow(m_dlls.pCrySystem, m_dlls.gameBuild);
		MemoryPatch::CrySystem::HookCPUDetect(m_dlls.pCrySystem, m_dlls.gameBuild, &CPUInfo::Detect);
		MemoryPatch::CrySystem::HookError(m_dlls.pCrySystem, m_dlls.gameBuild, &CrashLogger::OnEngineError);
		MemoryPatch::CrySystem::HookLanguageInit(m_dlls.pCrySystem, m_dlls.gameBuild, &LanguageHook::OnInit);
		MemoryPatch::CrySystem::HookChangeUserPath(m_dlls.pCrySystem, m_dlls.gameBuild,
			&LauncherCommon::OnChangeUserPath);
	}

	if (m_dlls.pCryRenderD3D9)
	{
		MemoryPatch::CryRenderD3D9::HookAdapterInfo(m_dlls.pCryRenderD3D9, m_dlls.gameBuild,
			&LauncherCommon::OnD3D9Info);
	}

	if (m_dlls.pCryRenderD3D10)
	{
		MemoryPatch::CryRenderD3D10::FixLowRefreshRateBug(m_dlls.pCryRenderD3D10, m_dlls.gameBuild);
		MemoryPatch::CryRenderD3D10::HookAdapterInfo(m_dlls.pCryRenderD3D10, m_dlls.gameBuild,
			&LauncherCommon::OnD3D10Info);
		MemoryPatch::CryRenderD3D10::HookInitAPI(m_dlls.pCryRenderD3D10, m_dlls.gameBuild,
			&LauncherCommon::OnD3D10Init);
	}
}

void GameLauncher::LoadVRMod()
{
	std::string vrModPath = LauncherCommon::GetMainFolderPath();
	if (vrModPath.empty())
		vrModPath = ".";
#ifndef _WIN64
	vrModPath += "\\Mods\\VRMod\\Bin32\\VRMod.dll";
#else
	vrModPath += "\\Mods\\VRMod\\Bin64\\VRMod.dll";
#endif
	m_dlls.pVRMod = LauncherCommon::LoadDLL(vrModPath.c_str());

	InitVRModD3DHooks();
}

void GameLauncher::InitVRModD3DHooks()
{
	typedef bool (*InitD3DHooksFn)();
	auto fn = (InitD3DHooksFn) OS::DLL::FindSymbol(m_dlls.pVRMod, "CryVRInitD3DHooks");
	if (fn == nullptr || !fn())
	{
		throw StringFormat_Error("Could not initialize VR mod hooks!");
	}
}
