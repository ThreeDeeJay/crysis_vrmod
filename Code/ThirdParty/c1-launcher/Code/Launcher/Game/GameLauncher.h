#pragma once

#include "CryCommon/CryGame/IGameStartup.h"
#include "CryCommon/CrySystem/ISystem.h"

class GameLauncher
{
	IGameStartup* m_pGameStartup;
	SSystemInitParams m_params;

	struct DLLs
	{
		void* pEXE;
		void* pCryGame;
		void* pCryAction;
		void* pCryNetwork;
		void* pCrySystem;
		void* pCryRenderD3D9;
		void* pCryRenderD3D10;
		void* pVRMod;

		int gameBuild;
		bool isWarhead;
	};

	DLLs m_dlls;

public:
	GameLauncher();
	~GameLauncher();

	int Run();

private:
	void LoadEngine();
	void PatchEngine();
	void LoadVRMod();
	void InitVRModD3DHooks();
};
