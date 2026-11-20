#ifdef WII_PLATFORM

#include <exception>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <gccore.h>
#include <wiiuse/wpad.h>

#include "LawnApp.h"
#include "Resources.h"
#include "Sexy.TodLib/TodStringFile.h"
#include "WiiEarlyInit.h"
#include "WiiHeap.h"

using namespace Sexy;

bool (*gAppCloseRequest)() = nullptr;
bool (*gAppHasUsedCheatKeys)() = nullptr;
SexyString (*gGetCurrentLevelName)() = nullptr;

extern "C" {
unsigned int __stacksize__ = 1024 * 1024;
}

static void PvZWiiWaitBeforeExit()
{
	wiiLog("\nPress HOME (Wiimote) or START (GC) to exit...\n");
	for (int i = 0; i < 60 * 10; ++i)
	{
		WPAD_ScanPads();
		PAD_ScanPads();
		if ((WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) || (PAD_ButtonsDown(0) & PAD_BUTTON_START))
			break;
		VIDEO_WaitVSync();
	}
}

static void PvZWiiTerminate()
{
	wiiLog("[WII][FATAL] std::terminate\n");
	PvZWiiWaitBeforeExit();
	std::abort();
}

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;

	wiiEnsureEarlyVideo();
	// Build stamp: tells at a glance whether the .dol on the card is the one
	// just built, so a missing diagnostic is never confused with a stale copy.
	wiiLog("[WII] pvz_wii main entered (built " __DATE__ " " __TIME__ ")\n");
	std::set_terminate(PvZWiiTerminate);

	WPAD_Init();
	PAD_Init();

	if (!wiiEnsureStorage())
	{
		wiiLog("[WII][FATAL] SD/USB storage is unavailable\n");
		PvZWiiWaitBeforeExit();
		return 1;
	}

	// libfat normally adopts argv[0]'s directory, but that is not guaranteed
	// when an ELF is launched directly by Dolphin or wiiload.  All legacy PvZ
	// asset paths (including "main.pak") are relative to the process cwd.
	const char* appDir = wiiGetAppDir();
	if (chdir(appDir) != 0)
	{
		wiiLog("[WII][FATAL] cannot chdir to app directory: %s\n", appDir);
		PvZWiiWaitBeforeExit();
		return 1;
	}
	wiiLog("[WII] cwd set to %s\n", appDir);

	TodStringListSetColors(gLawnStringFormats, gLawnStringFormatCount);
	gGetCurrentLevelName = LawnGetCurrentLevelName;
	gAppCloseRequest = LawnGetCloseRequest;
	gAppHasUsedCheatKeys = LawnHasUsedCheatKeys;
	gExtractResourcesByName = Sexy::ExtractResourcesByName;

	try
	{
		wiiHeapReport("before LawnApp new");
		gLawnApp = new LawnApp();
		wiiHeapReport("before LawnApp Init");
		gLawnApp->Init();
		wiiHeapReport("after LawnApp Init");
		gLawnApp->Start();
		gLawnApp->Shutdown();
		delete gLawnApp;
		gLawnApp = nullptr;
	}
	catch (const std::bad_alloc&)
	{
		wiiHeapCaptureBadAlloc();
		wiiLog("[WII][FATAL] std::bad_alloc\n");
		PvZWiiWaitBeforeExit();
		return 1;
	}
	catch (const std::exception& e)
	{
		wiiLog("[WII][FATAL] %s\n", e.what());
		PvZWiiWaitBeforeExit();
		return 1;
	}
	catch (...)
	{
		wiiLog("[WII][FATAL] unknown exception\n");
		PvZWiiWaitBeforeExit();
		return 1;
	}

	wiiLog("[WII] clean shutdown\n");
	return 0;
}

#endif
