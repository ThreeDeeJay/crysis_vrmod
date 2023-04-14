#include "Library/CrashLogger.h"
#include "Library/OS.h"

#include "../CPUInfo.h"
#include "../LauncherCommon.h"
#include "../MemoryPatch.h"

#include "GameLauncher.h"

#include "Library/StringTools.h"

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
	m_params.hInstance = OS::Module::GetEXE();
	m_params.logFileName = DEFAULT_LOG_FILE_NAME;

	char cmdLine[2048];
	snprintf(cmdLine, sizeof(cmdLine), "%s -dx10 -MOD VRMod", OS::CmdLine::Get());

	LauncherCommon::SetParamsCmdLine(m_params, cmdLine);

	CrashLogger::Enable(&OpenLogFile);

	this->LoadEngine();
	this->PatchEngine();

	m_pGameStartup = LauncherCommon::StartEngine(m_dlls.pCryGame, m_params);

	return m_pGameStartup->Run(NULL);
}

void GameLauncher::LoadEngine()
{
	LoadVRMod();
	m_dlls.pCrySystem = LauncherCommon::LoadModule("CrySystem.dll");

	m_dlls.gameBuild = LauncherCommon::GetGameBuild(m_dlls.pCrySystem);

	LauncherCommon::VerifyGameBuild(m_dlls.gameBuild);

	m_dlls.pCryGame = LauncherCommon::LoadModule("CryGame.dll");
	m_dlls.pCryAction = LauncherCommon::LoadModule("CryAction.dll");
	m_dlls.pCryNetwork = LauncherCommon::LoadModule("CryNetwork.dll");

	const bool isDX10 = !OS::CmdLine::HasArg("-dx9") && (OS::CmdLine::HasArg("-dx10") || OS::IsVistaOrLater());

	if (isDX10)
	{
		m_dlls.pCryRenderD3D10 = LauncherCommon::LoadModule("CryRenderD3D10.dll");
	}
}

void GameLauncher::PatchEngine()
{
	if (m_dlls.pCryGame)
	{
		MemoryPatch::CryGame::CanJoinDX10Servers(m_dlls.pCryGame, m_dlls.gameBuild);
		MemoryPatch::CryGame::EnableDX10Menu(m_dlls.pCryGame, m_dlls.gameBuild);

		if (!OS::CmdLine::HasArg("-splash"))
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
		MemoryPatch::CrySystem::UnhandledExceptions(m_dlls.pCrySystem, m_dlls.gameBuild);
		MemoryPatch::CrySystem::HookCPUDetect(m_dlls.pCrySystem, m_dlls.gameBuild, &CPUInfo::Detect);
		MemoryPatch::CrySystem::HookError(m_dlls.pCrySystem, m_dlls.gameBuild, &CrashLogger::OnEngineError);
	}

	if (m_dlls.pCryRenderD3D10)
	{
		MemoryPatch::CryRenderD3D10::FixLowRefreshRateBug(m_dlls.pCryRenderD3D10, m_dlls.gameBuild);
	}
}

void GameLauncher::LoadVRMod()
{
	std::string vrModPath = LauncherCommon::GetMainFolderPath();
	if (vrModPath.empty())
		vrModPath = ".";
	vrModPath += "\\Mods\\VRMod\\Bin64\\VRMod.dll";
	m_dlls.pVRMod = LauncherCommon::LoadModule(vrModPath.c_str());

	InitVRModD3DHooks();
}

void GameLauncher::InitVRModD3DHooks()
{
	typedef bool (*InitD3DHooksFn)();
	auto fn = (InitD3DHooksFn) OS::Module::FindSymbol(m_dlls.pVRMod, "CryVRInitD3DHooks");
	if (fn == nullptr || !fn())
	{
		throw StringTools::Error("Could not initialize VR mod hooks!");
	}
}
