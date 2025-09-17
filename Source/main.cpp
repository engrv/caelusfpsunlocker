#include <Windows.h>
#include <iostream>
#include <vector>
#include <codecvt>
#include <unordered_map>
#include <chrono>
#include <TlHelp32.h>

#pragma comment(lib, "Shlwapi.lib")
#include <Shlwapi.h>

#include "ui.h"
#include "settings.h"
#include "rfu.h"
#include "procutil.h"
#include "sigscan.h"

HANDLE SingletonMutex;

std::vector<HANDLE> GetCaelusProcesses(bool include_client = true, bool include_studio = true)
{
	std::vector<HANDLE> result;
	if (include_client)
	{
		// this was pretty much all that needed patching here, studio was already correct
		for (HANDLE handle : ProcUtil::GetProcessesByImageName("KitsuKitsuPlayerBeta.exe")) result.emplace_back(handle);
	}
	if (include_studio) for (HANDLE handle : ProcUtil::GetProcessesByImageName("RobloxStudioBeta.exe")) result.emplace_back(handle);
	return result;
}

HANDLE GetCaelusProcess()
{
	auto processes = GetCaelusProcesses();

	if (processes.empty())
		return NULL;

	if (processes.size() == 1)
		return processes[0];

	printf("Multiple processes found! Select a process to inject into (%u - %zu):\n", 1, processes.size());
	for (int i = 0; i < processes.size(); i++)
	{
		try
		{
			ProcUtil::ProcessInfo info(processes[i], true);
			printf("[%d] [%s] %s\n", i + 1, info.name.c_str(), info.window_title.c_str());
		}
		catch (ProcUtil::WindowsException& e)
		{
			printf("[%d] Invalid process %p (%s, %X)\n", i + 1, processes[i], e.what(), e.GetLastError());
		}
	}

	int selection;

	while (true)
	{
		printf("\n>");
		std::cin >> selection;

		if (std::cin.fail())
		{
			std::cin.clear();
			std::cin.ignore(std::cin.rdbuf()->in_avail());
			printf("Invalid input, try again\n");
			continue;
		}

		if (selection < 1 || selection > processes.size())
		{
			printf("Please enter a number between %u and %zu\n", 1, processes.size());
			continue;
		}

		break;
	}

	return processes[selection - 1];
}

size_t FindTaskSchedulerFrameDelayOffset(HANDLE process, const void *scheduler)
{
	const size_t search_offset = 0x100;

	uint8_t buffer[0x100];
	if (!ProcUtil::Read(process, (const uint8_t *)scheduler + search_offset, buffer, sizeof(buffer)))
		return -1;

	for (int i = 0; i < sizeof(buffer) - sizeof(double); i += 4)
	{
		static const double frame_delay = 1.0 / 60.0;
		double difference = *(double *)(buffer + i) - frame_delay;
		difference = difference < 0 ? -difference : difference;
		if (difference < std::numeric_limits<double>::epsilon()) return search_offset + i;
	}

	return -1;
}

const void *FindTaskScheduler(HANDLE process, const char **error = nullptr)
{
	try
	{
		ProcUtil::ProcessInfo info;

		int tries = 5;
		int wait_time = 100;

		while (true)
		{
			info = ProcUtil::ProcessInfo(process);
			if (info.module.base != nullptr)
				break;

			if (tries--)
			{
				printf("[%p] Retrying in %dms...\n", process, wait_time);
				Sleep(wait_time);
				wait_time *= 2;
			}
			else
			{
				if (error) *error = "Failed to get process base! Please restart Caelus FPS Unlocker.";
				return nullptr;
			}
		}

		auto start = (const uint8_t *)info.module.base;
		auto end = start + info.module.size;

		printf("[%p] Process Base: %p\n", process, start);

		if (ProcUtil::IsProcess64Bit(process))
		{
			if (auto result = (const uint8_t *)ProcUtil::ScanProcess(process, "\x40\x53\x48\x83\xEC\x20\x0F\xB6\xD9\xE8\x00\x00\x00\x00\x86\x58\x04\x48\x83\xC4\x20\x5B\xC3", "xxxxxxxxxx????xxxxxxxxx", start, end))
			{
				auto gts_fn = result + 14 + ProcUtil::Read<int32_t>(process, result + 10);

				printf("[%p] GetTaskScheduler: %p\n", process, gts_fn);

				uint8_t buffer[0x100];
				if (ProcUtil::Read(process, gts_fn, buffer, sizeof(buffer)))
				{
					if (auto inst = sigscan::scan("\x48\x8B\x05\x00\x00\x00\x00\x48\x83\xC4\x38", "xxx????xxxx", (uintptr_t)buffer, (uintptr_t)buffer + 0x100))
					{
						const uint8_t *remote = gts_fn + (inst - buffer);
						return remote + 7 + *(int32_t *)(inst + 3);
					}
				}
			}
		}
		else
		{
			if (auto result = (const uint8_t *)ProcUtil::ScanProcess(process, "\x55\x8B\xEC\xE8\x00\x00\x00\x00\x8A\x4D\x08\x83\xC0\x04\x86\x08\x5D\xC3", "xxxx????xxxxxxxxxx", start, end))
			{
				auto gts_fn = result + 8 + ProcUtil::Read<int32_t>(process, result + 4);

				printf("[%p] GetTaskScheduler: %p\n", process, gts_fn);

				uint8_t buffer[0x100];
				if (ProcUtil::Read(process, gts_fn, buffer, sizeof(buffer)))
				{
					if (auto inst = sigscan::scan("\xA1\x00\x00\x00\x00\x8B\x4D\xF4", "x????xxx", (uintptr_t)buffer, (uintptr_t)buffer + 0x100))
					{
						return (const void *)(*(uint32_t *)(inst + 1));
					}
				}
			}
		}
	}
	catch (ProcUtil::WindowsException& e)
	{
	}

	return nullptr;
}

void NotifyError(const char* title, const char* error)
{
	if (Settings::SilentErrors || Settings::NonBlockingErrors)
	{
		HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
		CONSOLE_SCREEN_BUFFER_INFO info{};
		GetConsoleScreenBufferInfo(console, &info);

		WORD color = (info.wAttributes & 0xFF00) | FOREGROUND_RED | FOREGROUND_INTENSITY;
		SetConsoleTextAttribute(console, color);

		printf("[ERROR] %s\n", error);

		SetConsoleTextAttribute(console, info.wAttributes);

		if (!Settings::SilentErrors)
		{
			UI::SetConsoleVisible(true);
		}
	}
	else
	{
		MessageBoxA(UI::Window, error, title, MB_OK);
	}
}

struct CaelusProcess
{
	HANDLE handle = NULL;
	const void *ts_ptr = nullptr;
	const void *fd_ptr = nullptr;

	int retries_left = 0;

	bool Attach(HANDLE process, int retry_count)
	{
		handle = process;
		retries_left = retry_count;

		Tick();

		return ts_ptr != nullptr && fd_ptr != nullptr;
	}

	void Tick()
	{
		if (retries_left < 0) return;

		if (!ts_ptr)
		{
			const char* error = nullptr;
			ts_ptr = FindTaskScheduler(handle, &error);

			if (!ts_ptr)
			{
				if (error) retries_left = 0;
				if (retries_left-- <= 0)
					NotifyError("caelusfpsunlocker Error", error ? error : "Unable to find TaskScheduler! This is probably due to a Caelus update-- watch the github for any patches or a fix.");
				return;
			}
		}

		if (ts_ptr && !fd_ptr)
		{
			try
			{
				if (auto scheduler = (const uint8_t *)(ProcUtil::ReadPointer(handle, ts_ptr)))
				{
					printf("[%p] Scheduler: %p\n", handle, scheduler);

					size_t delay_offset = FindTaskSchedulerFrameDelayOffset(handle, scheduler);
					if (delay_offset == -1)
					{
						if (retries_left-- <= 0)
							NotifyError("caelusfpsunlocker Error", "Variable scan failed! This is probably due to a Caelus update-- watch the github for any patches or a fix.");
						return;
					}

					printf("[%p] Frame Delay Offset: %zu\n", handle, delay_offset);

					fd_ptr = scheduler + delay_offset;

					SetFPSCap(Settings::FPSCap);
				}
				else
				{
					printf("[%p] *ts_ptr == nullptr\n", handle);
				}
			}
			catch (ProcUtil::WindowsException& e)
			{
				printf("[%p] CaelusProcess::Tick failed: %s (%d)\n", handle, e.what(), e.GetLastError());
				if (retries_left-- <= 0)
					NotifyError("caelusfpsunlocker Error", "An exception occurred while performing the variable scan.");
			}
		}
	}

	void SetFPSCap(double cap)
	{
		if (fd_ptr)
		{
			try
			{
				static const double min_frame_delay = 1.0 / 10000.0;
				double frame_delay = cap <= 0.0 ? min_frame_delay : 1.0 / cap;

				ProcUtil::Write(handle, fd_ptr, frame_delay);
			}
			catch (ProcUtil::WindowsException &e)
			{
				printf("[%p] CaelusProcess::SetFPSCap failed: %s (%d)\n", handle, e.what(), e.GetLastError());
			}
		}
	}
};

std::unordered_map<DWORD, CaelusProcess> AttachedProcesses;

void SetFPSCapExternal(double value)
{
	for (auto& it : AttachedProcesses)
	{
		it.second.SetFPSCap(value);
	}
}

void pause()
{
	printf("Press enter to continue . . .");
	getchar();
}

DWORD WINAPI WatchThread(LPVOID)
{
	printf("Watch thread started\n");

	while (1)
	{
		auto processes = GetCaelusProcesses(Settings::UnlockClient, Settings::UnlockStudio);

		for (auto& process : processes)
		{
			DWORD id = GetProcessId(process);

			if (AttachedProcesses.find(id) == AttachedProcesses.end())
			{
				printf("Injecting into new process %p (pid %d)\n", process, id);
				CaelusProcess caelus_process;

				caelus_process.Attach(process, 3);

				AttachedProcesses[id] = caelus_process;

				printf("New size: %zu\n", AttachedProcesses.size());
			}
			else
			{
				CloseHandle(process);
			}
		}

		for (auto it = AttachedProcesses.begin(); it != AttachedProcesses.end();)
		{
			HANDLE process = it->second.handle;

			DWORD code;
			BOOL result = GetExitCodeProcess(process, &code);

			if (code != STILL_ACTIVE)
			{
				printf("Purging dead process %p (pid %d) (code %X)\n", process, GetProcessId(process), code);
				it = AttachedProcesses.erase(it);
				CloseHandle(process);
				printf("New size: %zu\n", AttachedProcesses.size());
			}
			else
			{
				it->second.Tick();
				it++;
			}
		}

		UI::AttachedProcessesCount = AttachedProcesses.size();

		Sleep(2000);
	}

	return 0;
}

bool CheckRunning()
{
	SingletonMutex = CreateMutexA(NULL, FALSE, "RFUMutex");

	if (!SingletonMutex)
	{
		MessageBoxA(NULL, "Unable to create mutex", "Error", MB_OK);
		return false;
	}

	return GetLastError() == ERROR_ALREADY_EXISTS;
}

int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	if (!Settings::Init())
	{
		char buffer[64];
		sprintf_s(buffer, "Unable to initiate settings\nGetLastError() = %X", GetLastError());
		MessageBoxA(NULL, buffer, "Error", MB_OK);
		return 0;
	}

	UI::IsConsoleOnly = strstr(lpCmdLine, "--console") != nullptr;

	if (UI::IsConsoleOnly)
	{
		UI::ToggleConsole();

		printf("Waiting for Caelus...\n");

		HANDLE process;

		do
		{
			Sleep(100);
			process = GetCaelusProcess();
		}
		while (!process);

		printf("Found Caelus...\n");
		printf("Attaching...\n");

		if (!CaelusProcess().Attach(process, 0))
		{
			printf("\nERROR: unable to attach to process\n");
			pause();
			return 0;
		}

		CloseHandle(process);

		printf("\nSuccess! The injector will close in 3 seconds...\n");

		Sleep(3000);

		return 0;
	}
	else
	{
		if (CheckRunning())
		{
			MessageBoxA(NULL, "Caelus FPS Unlocker is already running", "Error", MB_OK);
		}
		else
		{
			if (!Settings::QuickStart)
				UI::ToggleConsole();
			else
				UI::CreateHiddenConsole();

			if (Settings::CheckForUpdates)
			{
				printf("Checking for updates...\n");
				if (CheckForUpdates()) return 0;
			}

			if (!Settings::QuickStart)
			{
				printf("Minimizing to system tray in 2 seconds...\n");
				Sleep(2000);
				UI::ToggleConsole();
			}

			return UI::Start(hInstance, WatchThread);
		}
	}
} 