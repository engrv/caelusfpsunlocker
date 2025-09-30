#pragma once

namespace Settings
{
	extern bool VSyncEnabled;
	extern unsigned char FPSCapSelection;
	extern double FPSCap;
	extern bool UnlockClient;
	//Disable studio setting until studio is complete
	//extern bool UnlockStudio;
	extern bool CheckForUpdates;
	extern bool NonBlockingErrors;
	extern bool SilentErrors;
	extern bool QuickStart;

	bool Init();
	bool Load();
	bool Save();

	void Update();
}