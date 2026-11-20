// WiiEarlyInit.cpp — runs before main(), and before the rest of static init.
//
// Why this file exists
// --------------------
// The game reads resources from *static initializers*, before main() is ever
// entered: SharedConstants::acceptableLetters and
// ChatAllowedCharacters::allowedCharacters are global constants whose
// initialisers call Resource::getResource("/font.txt").
//
// That is harmless on PS2, where assets are baked into the ELF and a resource
// lookup is a memory read. On the Wii the assets live on an SD card or a USB
// drive, so the lookup needs libfat mounted -- and mounting it in main() is far
// too late.
// The failure mode is nasty and gives no clue what happened:
//
//     static init -> getResource -> ifstream fails (nothing mounted)
//                 -> throw -> exception escapes a static initialiser
//                 -> std::terminate -> abort, all before any video init
//
// which on hardware and in Dolphin is a black window and a hang, with no output
// anywhere.
//
// SD or USB
// ----------
// Neither is a build-time choice and neither is preferred by construction. One
// call to fatInitDefault() mounts every device libfat knows about, and the
// install directory is then *resolved* rather than assumed -- see
// collectMountedDevices() and resolveAppDir() below. Everything downstream (assets, worlds, options,
// screenshots, this file's own boot log) hangs off wiiGetAppDir(), so nothing
// else in the port has to know which device it ended up on.
//
// Order here is deliberate: VIDEO AND CONSOLE FIRST, storage second. Storage is
// the thing most likely to fail or stall (no SD inserted, Dolphin without a
// card configured, a slow USB enumeration), and diagnosing that is impossible if
// the screen is not up yet to say so.
//
// Mechanisms, because no single one is sufficient:
//
//   1. A constructor with priority 101 (the lowest GCC allows; 0-100 are
//      reserved). Prioritised constructors run before unprioritised ones, so
//      video and the filesystem are ready before any of the game's own globals
//      are constructed.
//
//   2. wiiEnsureStorage() / wiiEnsureEarlyVideo() are idempotent and are also
//      called defensively from Resource_wii.cpp and File_wii.cpp, so nothing
//      depends on constructor ordering being what we expect across toolchain
//      versions.
//
//   3. A terminate handler. Anything thrown before the renderer exists would
//      otherwise vanish silently; routing it through CrashHandler::Crash puts
//      the message on the TV, which is the only place a user can read it.
#ifdef WII_PLATFORM

#include "wii/WiiEarlyInit.h"
#include "misc/SexyLog.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

#include <dirent.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fat.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/usbstorage.h>

// libogc 3.x routes libfat through DVM, which is what gives us partition-aware
// USB mounting (see tryMountUsb). Older libfat has no dvm.h and only the
// whole-disc fatMountSimple; both paths are kept so this file does not pin the
// toolchain version.
#if __has_include(<dvm.h>)
#	include <dvm.h>
#	define WII_HAVE_DVM 1
#endif

// Put the C heap in MEM2.
//
// libogc's _sbrk_r reads this weak global on entry and picks an arena from it,
// once and for all. It is NOT an overflow policy: disassembling sbrk.o
// (`powerpc-eabi-ar x libogc.a sbrk.o && powerpc-eabi-objdump -dr sbrk.o`) shows
// the MEM1 branch ending in
//
//     bl SYS_GetArena1Hi ; cmplw ; ble ok
//     li r9,12  /* ENOMEM */ ; li r31,-1 ; stw r9,0(r27) ; return -1
//
// with no path to Arena2 anywhere in it. An earlier note in README_wii.md
// claimed the heap "spills into MEM2 automatically once Arena1 is exhausted",
// and claimed to have verified it by disassembly. That is wrong, and it was the
// reason a failing malloc kept being dismissed as a weak hypothesis on this
// target: with MALLOC_MEM2 at its default of 0 the entire heap was the MEM1
// leftovers -- about 8 MB once the ~14.5 MB ELF, the two framebuffers and the
// GX FIFO are paid for. A world load spends that on a handful of chunk sections
// (the log shows single sections wanting ~190 KB of GX display list) and then
// every allocation fails, including texture loads, which is what put the game on
// its own out-of-memory screen seconds after entering a world.
//
// Arena2 is ~50 MB, so this is roughly a 6x heap. The cost is latency: MEM2 is
// GDDR3 and slower than MEM1's 1T-SRAM, and everything malloc'd moves there --
// including the Tessellator's 2 MB scratch buffer, which is the hottest CPU-side
// buffer in the game. If profiling later says that hurts, the answer is a small
// MEM1 allocator for chosen buffers, not flipping this back.
//
// The ELF's own text/data/bss still live in MEM1 and are unaffected; so are the
// framebuffers, which SYS_AllocateFramebuffer takes from the arena directly.
//
// This must be an initialised data symbol rather than something a constructor
// assigns: allocations happen during libc and static init, before any
// constructor of ours could run. The definition has to be extern "C" so it
// overrides libogc's weak C symbol instead of becoming a mangled C++ one --
// written as a linkage block rather than `extern "C" u32 MALLOC_MEM2 = 1;`
// because GCC cannot tell that linkage specifier from the `extern` storage
// class and warns about an initialised extern declaration.
extern "C" {
u32 MALLOC_MEM2 = 1;
}

namespace
{

bool s_storageTried = false;
bool s_storageReady = false;
bool s_videoReady   = false;

// True until GX takes the framebuffer over. See wiiLog's channel 1.
bool s_bootPhase    = true;

// The resolved install directory and the engine's log path inside it.
//
// Plain char arrays, not std::string: these are written from a
// constructor(101), and a namespace-scope std::string is constructed by an
// *unprioritised* constructor, which would therefore run afterwards and
// overwrite the resolved path with an empty one. Zero-initialised statically,
// so they read as empty before resolveAppDir() runs rather than as garbage.
char s_appDir[128];
char s_logPath[192];
bool s_appDirHasData = false;
FILE *s_logFile = nullptr;

// Prefix every logical line with monotonic uptime and a sequence number. The
// sequence is especially useful when a damaged FAT write loses the tail of a
// session: it makes the gap explicit instead of looking like inactivity.
bool s_logAtLineStart = true;
u32  s_logSequence = 0;

// The devices newlib actually has mounted, right now -- asked rather than
// assumed. No device name appears anywhere in this port as a literal.
//
// fatInitDefault() mounts everything libfat knows about and registers each
// mounted volume in newlib's devoptab table. Disassembling dvmInit() in libfat
// 2.x (`powerpc-eabi-ar x libfat.a dvm_libogc.o && powerpc-eabi-objdump -dr`)
// shows dvmProbeMountDiscIface() called for __io_wiisd as "sd", __io_usbstorage
// as "usb" (gated on the weak g_dvmOgcUsbMount, which defaults to 1), and the
// two GameCube SD adapters as "carda"/"cardb" -- plus "usb2", "usb3"... for the
// extra partitions of a drive that also carries a WBFS or NTFS volume, since
// dvmProbeMountDisc names partition 0 "usb" and partition N "usb<N+1>".
//
// Reading the table means this port supports a device by virtue of libfat
// supporting it. The hardcoded array that used to be here had to be edited every
// time that list changed, and silently ignored anything missing from it: a drive
// whose FAT32 volume landed on usb5: was invisible with the game sitting right
// there on it. It also made SD look privileged when it never was.
//
// Order is mount order, which is libfat's own preference and therefore the
// historical one. It only ever decides ties -- two complete installs on two
// devices.
//
// Table entries are sparse, and the low ones are the std streams. A null name is
// skipped; everything else is simply probed, because a non-storage device (a
// console tty, stdnull) fails the main.pak probe exactly like a storage device
// with no install on it. No name filtering, therefore no name knowledge.
size_t collectMountedDevices(const char *out[], size_t maxDevices)
{
	size_t aCount = 0;
	for (int i = 0; i < STD_MAX && aCount < maxDevices; i++)
	{
		const devoptab_t *aDevice = devoptab_list[i];
		if (aDevice == nullptr || aDevice->name == nullptr || aDevice->name[0] == '\0')
			continue;
		out[aCount++] = aDevice->name;
	}
	return aCount;
}

// Where an install lives on a device, and the one file that proves it is
// complete. font.txt is the right probe because it is the very first resource
// the game reads -- from a static initialiser, before main() -- so anything
// that fails this check was going to fail the boot anyway.
const char *const kAppSubDir   = "/apps/PlantsVsZombies";
const char *const kProbeFile   = "/main.pak";

// Used only when nothing mounted at all -- see the end of resolveAppDir().
const char *const kPlaceholderDevice = "sd";

// A USB hard disk can still be spinning up this early in the boot, so the first
// mount attempt losing is normal rather than exceptional. Only ever spent when
// the alternative is booting with no game data at all.
const int kUsbMountAttempts   = 4;
const int kUsbRetryDelayUs    = 500 * 1000;

bool fileExists(const char *path)
{
	FILE *f = std::fopen(path, "rb");
	if (!f)
		return false;
	std::fclose(f);
	return true;
}

bool dirExists(const char *path)
{
	DIR *d = opendir(path);
	if (!d)
		return false;
	closedir(d);
	return true;
}

// Whether <dir> holds a staged data/ tree, i.e. is a real install and not just
// an empty folder of the right name.
bool hasGameData(const char *dir)
{
	char path[256];
	std::snprintf(path, sizeof(path), "%s%s", dir, kProbeFile);
	return fileExists(path);
}

// The directory the loader started us from, which is the strongest possible
// answer to "SD or USB?" because it is not a guess at all.
//
// libfat's dvmInit() ends by chdir()ing to the directory part of argv[0]
// whenever the loader passed one -- the Homebrew Channel and wiiload both do --
// so immediately after fatInitDefault() the current directory *is* the install
// directory, on whichever device the DOL was actually launched from. Booting
// the .elf straight from Dolphin passes no argv, and then this fails and the
// device list below takes over.
bool getLaunchDir(char *out, size_t outSize)
{
	if (!getcwd(out, outSize))
		return false;

	// A path with no device prefix is not something libfat can resolve later;
	// treat it as "the loader told us nothing".
	if (!std::strchr(out, ':'))
		return false;

	// getcwd hands back a trailing slash ("sd:/apps/BetaPlusPlus/"); every
	// caller appends "/...", so drop it. Stopping at ':' keeps a bare device
	// root ("sd:/") from collapsing into "sd:".
	size_t len = std::strlen(out);
	while (len > 1 && out[len - 1] == '/' && out[len - 2] != ':')
		out[--len] = '\0';

	// A cwd of the device root means the loader launched us from the top of the
	// card, which is not a layout this port stages -- and accepting it would
	// produce "sd://data/assets/...". Let the probe below answer instead.
	return len > 0 && out[len - 1] != '/';
}

// Picks the install directory. Called again after a late USB mount, so it must
// stay idempotent.
void resolveAppDir()
{
	char candidate[sizeof(s_appDir)];

	// 1. The launch directory, when it holds a complete install. This is the
	//    case that makes USB work with no configuration: install to
	//    usb:/apps/BetaPlusPlus, launch it, and every path follows.
	if (getLaunchDir(candidate, sizeof(candidate)) && hasGameData(candidate))
	{
		std::snprintf(s_appDir, sizeof(s_appDir), "%s", candidate);
		s_appDirHasData = true;
		return;
	}

	// 2. Otherwise look for a staged install on each mounted device in turn.
	const char *aDevices[STD_MAX];
	const size_t aDeviceCount = collectMountedDevices(aDevices, STD_MAX);

	for (size_t i = 0; i < aDeviceCount; i++)
	{
		std::snprintf(candidate, sizeof(candidate), "%s:%s", aDevices[i], kAppSubDir);
		if (hasGameData(candidate))
		{
			std::snprintf(s_appDir, sizeof(s_appDir), "%s", candidate);
			s_appDirHasData = true;
			return;
		}
	}

	// 3. Nothing has game data. The path still has to be *something* usable, so
	//    that the error messages name a real location and a later `build wii.bat
	//    data` lands where the game will look.
	s_appDirHasData = false;

	//    The launch directory first: incomplete or not, that is where the user
	//    put this build, so it is where the missing data/ belongs.
	if (getLaunchDir(candidate, sizeof(candidate)))
	{
		std::snprintf(s_appDir, sizeof(s_appDir), "%s", candidate);
		return;
	}

	//    Then the first device that mounted at all.
	for (size_t i = 0; i < aDeviceCount; i++)
	{
		char root[32];
		std::snprintf(root, sizeof(root), "%s:/", aDevices[i]);
		if (dirExists(root))
		{
			std::snprintf(s_appDir, sizeof(s_appDir), "%s:%s", aDevices[i], kAppSubDir);
			return;
		}
	}

	//    And with nothing mounted at all, the placeholder. This is the one device
	//    name left in the file, and it is not a choice: there is nothing mounted
	//    to resolve, and wiiGetAppDir()'s contract is that it always hands back a
	//    device-prefixed path rather than something the callers have to check. It
	//    exists so the "no storage" message can name a plausible location, and
	//    every open through it is going to fail either way.
	std::snprintf(s_appDir, sizeof(s_appDir), "%s:%s", kPlaceholderDevice, kAppSubDir);
}

// Fills in s_logPath, and makes sure the directories leading to it exist.
//
// This has to be composed rather than hardcoded, for the same reason the install
// directory is resolved rather than assumed: the log belongs next to the build
// that wrote it, on whichever device that turned out to be.
//
// The location is NOT a boot log of this file's own invention. It is
// <install>/savedata/userdata/log.txt -- the engine's log, the exact path
// TodAssertInitForApp computes on this target (GetAppDataFolder() is
// <cwd>/savedata/ and it appends userdata/log.txt). The two agree by
// construction because SexyAppBase::Init derives the app data folder from
// wiiGetAppDir() here, not from getcwd. One file, one convention shared with PS2
// and desktop, and no second wii_boot.log to wonder about.
//
// Called after resolveAppDir(), including after a late USB mount re-resolves it,
// so the path always names the device the game is actually reading from.
void buildLogPath()
{
	// mkdir per level: newlib has no mkdir -p, and libfat fails the create rather
	// than making intermediate directories. EEXIST is the normal case from the
	// second session onwards and is not worth checking for.
	char aDir[sizeof(s_logPath)];

	std::snprintf(aDir, sizeof(aDir), "%s/savedata", s_appDir);
	mkdir(aDir, 0777);

	std::snprintf(aDir, sizeof(aDir), "%s/savedata/userdata", s_appDir);
	mkdir(aDir, 0777);

	std::snprintf(s_logPath, sizeof(s_logPath), "%s/savedata/userdata/log.txt",
	              s_appDir);
}

// Mounts USB by hand, for when the automatic mount inside fatInitDefault() came
// up empty.
//
// Deliberately not fatMountSimple("usb", &__io_usbstorage): that mounts the disc
// as one unpartitioned volume starting at sector 0, which is not what a USB
// stick or hard disk looks like -- they carry a partition table and the FAT
// volume is rarely the whole device. dvmProbeMountDiscIface() is what
// fatInitDefault itself uses, and the 4 x 64 sectors matches libfat's own
// g_dvmDefaultCachePages / g_dvmDefaultSectorsPerPage, so a retried mount
// behaves exactly like the automatic one.
bool tryMountUsb()
{
#ifdef WII_HAVE_DVM
	return dvmProbeMountDiscIface("usb", &__io_usbstorage, 4, 64) > 0;
#else
	return fatMountSimple("usb", &__io_usbstorage);
#endif
}

bool mountUsbWithRetries()
{
	wiiLog("  no game data yet; waiting for USB...\n");
	for (int attempt = 1; attempt <= kUsbMountAttempts; ++attempt)
	{
		if (tryMountUsb())
		{
			wiiLog("  usb mounted on attempt %d\n", attempt);
			return true;
		}
		if (attempt < kUsbMountAttempts)
			usleep(kUsbRetryDelayUs);
	}
	wiiLog("  usb did not answer after %d attempts\n", kUsbMountAttempts);
	return false;
}

GXRModeObj *s_rmode = nullptr;
void       *s_xfb   = nullptr;

// Everything sbrk will ever be able to hand out, sampled as early as this port
// can sample it. See wiiGetHeapCeiling().
u32 s_heapCeiling = 0;

void wiiTerminateHandler()
{
	std::string message = "Unhandled exception before the game started";

	// Recover the in-flight exception's message. Without this the crash screen
	// would say nothing more useful than "it stopped".
	if (std::current_exception())
	{
		try
		{
			std::rethrow_exception(std::current_exception());
		}
		catch (const std::exception &e)
		{
			message = std::string("Unhandled exception: ") + e.what();
		}
		catch (...)
		{
			message = "Unhandled non-standard exception";
		}
	}

	// Reset to the default first: if the crash screen itself throws, this must
	// abort rather than re-enter and recurse forever.
	std::set_terminate(std::abort);
	wiiLog("[WII][FATAL] %s\n", message.c_str());
	for (int i = 0; i < 60 * 8; ++i)
		VIDEO_WaitVSync();
	std::abort();
}

// fatInitDefault() returning true only means *a* device mounted, not that it is
// the one holding the game. Under Dolphin in particular there is a default
// sd.raw image that mounts perfectly well while containing none of our files,
// which produces a "file not found" that looks like a staging bug and is
// actually a config one.
//
// Listing what really mounted, and what the chosen device really holds, settles
// that question at boot instead of after a round of guessing.
void reportStorageLayout()
{
	// Which devices came up. "(none)" is a real fault; a short list is not --
	// most Wiis have no GameCube SD adapter, and plenty have no USB drive
	// plugged in.
	//
	// Now that this is the mount table rather than a probe of names we expected,
	// it also answers the opposite question: a device here that the port would
	// never have guessed (usb5:, a future libfat name) is one the install can
	// legitimately live on.
	const char *aDevices[STD_MAX];
	const size_t aDeviceCount = collectMountedDevices(aDevices, STD_MAX);

	wiiLog("  mounted:");
	bool any = false;
	for (size_t i = 0; i < aDeviceCount; i++)
	{
		char root[32];
		std::snprintf(root, sizeof(root), "%s:/", aDevices[i]);
		if (dirExists(root))
		{
			wiiLog(" %s", root);
			any = true;
		}
	}
	if (!any)
		wiiLog(" (none)");
	wiiLog("\n");

	wiiLog("  app dir: %s%s\n", s_appDir, s_appDirHasData ? "" : "  *** NO GAME DATA ***");

	// The top of the chosen device, which is what tells a "wrong card" apart
	// from a "right card, data never staged".
	char deviceRoot[sizeof(s_appDir)];
	std::snprintf(deviceRoot, sizeof(deviceRoot), "%s", s_appDir);
	if (char *slash = std::strchr(deviceRoot, '/'))
		slash[1] = '\0';

	DIR *root = opendir(deviceRoot);
	if (!root)
	{
		wiiLog("  %s will not open\n", deviceRoot);
	}
	else
	{
		wiiLog("  %s contains:", deviceRoot);
		int shown = 0;
		struct dirent *entry;
		while ((entry = readdir(root)) != nullptr && shown < 8)
		{
			if (entry->d_name[0] == '.')
				continue;
			wiiLog(" %s", entry->d_name);
			++shown;
		}
		if (shown == 0)
			wiiLog(" (empty)");
		wiiLog("\n");
		closedir(root);
	}

	if (!s_appDirHasData)
	{
		wiiLog("  -> nothing staged on any mounted device.\n");
		wiiLog("     Copy main.pak and properties to apps/PlantsVsZombies\n");
		wiiLog("     or to a USB drive (either works), and make sure\n");
		wiiLog("     you ran: build wii.bat data\n");
		wiiLog("     Dolphin: Config > Wii > SD Card,\n");
		wiiLog("     tick 'Automatically Sync with Folder',\n");
		wiiLog("     set it to bin\\wii\\sd, then RESTART emulation.\n");
	}
}

__attribute__((constructor(101)))
void wiiEarlyInit()
{
	std::set_terminate(wiiTerminateHandler);
	wiiEnsureEarlyVideo();

	// MALLOC_MEM2=1 selects Arena2 once and for all; Arena1 is not fallback heap
	// space. Sample MEM2 here, before libfat and game globals allocate, so Java's
	// maxMemory and the RAM log use the memory malloc can actually reach.
	s_heapCeiling = SYS_GetArena2Size();
	wiiLog("heap ceiling %u KB (MEM2); MEM1 non-heap %u KB\n",
	       s_heapCeiling / 1024u,
	       (u32)SYS_GetArena1Size() / 1024u);

	wiiEnsureStorage();
}

} // namespace

void wiiLogEndBootPhase()
{
	s_bootPhase = false;
}

extern "C" void wiiLog(const char *fmt, ...)
{
#if SEXY_LOG_LEVEL == 0
	// Built with logging off. Nothing is formatted and no channel is touched.
	(void)fmt;
#else
	char message[512];

	va_list args;
	va_start(args, fmt);
	const int formatted = std::vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	if (formatted < 0)
		return;

	// Keep a malformed or unexpectedly long diagnostic from merging with the
	// next line. No allocation is allowed here: this is also called during OOM.
	if ((size_t)formatted >= sizeof(message))
	{
		static const char suffix[] = " [truncated]\n";
		const size_t suffixLen = sizeof(suffix) - 1;
		std::memcpy(message + sizeof(message) - suffixLen - 1, suffix, suffixLen + 1);
	}

	// One façade among several now; the channels below moved into the shared
	// sink so that TodTrace and a wrapped printf reach them too.
	SexyLogWrite(message);
#endif
}

// The Wii's platform sink (see misc/SexyLog.h). Everything the engine logs on
// this target arrives here: wiiLog, TodTrace and friends, and every printf,
// which the linker wraps for exactly the reason channel 1 documents below.
extern "C" void SexyLogSinkWrite(const char *theLine)
{
	if (theLine == nullptr)
		return;

	// Stamped here rather than in wiiLog, so a TodTrace line and a printf carry
	// the same [elapsed][sequence] prefix a wiiLog line does and the three can
	// be read as a single ordered transcript. Having two unstamped timelines
	// that could not be interleaved was half the reason for unifying this.
	char line[576];
	if (s_logAtLineStart)
	{
		const u32 elapsedMs = (u32)ticks_to_millisecs(gettime());
		const u32 sequence = ++s_logSequence;
		std::snprintf(line, sizeof(line), "[%06u.%03u][%05u] %s",
			elapsedMs / 1000u, elapsedMs % 1000u, sequence, theLine);
	}
	else
	{
		std::snprintf(line, sizeof(line), "%s", theLine);
	}

	const size_t lineLen = std::strlen(line);
	s_logAtLineStart = lineLen > 0 && line[lineLen - 1] == '\n';

	// 1. Screen console, DURING BOOT ONLY.
	//
	//    Not a preference -- writing here after GX takes over is corruption. The
	//    console libogc set up in wiiEnsureEarlyVideo draws its text straight
	//    into the framebuffer, and gx_wii.cpp adopts that same framebuffer as
	//    buffer 0 rather than allocating a third one. So every line printed once
	//    the game is running paints characters over the rendered image, on the
	//    frames that land on buffer 0, and a line that reaches the bottom of the
	//    console scrolls it -- a full-screen memmove of ~600 KB, mid-frame.
	//
	//    That combination is what a periodic diagnostic looked like in practice:
	//    text flashing over the world and the game stopping for about a second.
	if (s_bootPhase)
		std::fputs(line, stdout);

	// 2. Dolphin's log window / a USB Gecko. Survives GX taking the screen, but
	//    depends on the emulator's log configuration (verbosity, "write to
	//    window", and the OSReport channel all enabled) or on real hardware
	//    plugged into an EXI adapter -- so it cannot be relied on alone.
	SYS_Report("%s", line);

	// 3. savedata/userdata/log.txt on the card or drive -- the engine's own log,
	//    the same file PS2 and desktop write. The only channel that needs nothing
	//    configured and works identically in Dolphin and on hardware, and the
	//    only one that exists at all on a console with no Gecko attached.
	// The handle stays null until storage is mounted and buildLogPath has run,
	// which is exactly the window where there is nowhere to write to anyway.
	//
	// Keep the stream open and flush each message. The old open/write/close path
	// repeated the FAT directory lookup and metadata update for every level-2
	// sample, causing a visible hitch. fflush preserves the important property:
	// a crash still leaves every completed diagnostic on disk.
	if (s_logFile != nullptr)
	{
		std::fputs(line, s_logFile);
		std::fflush(s_logFile);
	}
}

void wiiEnsureEarlyVideo()
{
	if (s_videoReady)
		return;
	s_videoReady = true;

	VIDEO_Init();
	s_rmode = VIDEO_GetPreferredMode(nullptr);

	// Uncached (K1) mapping: the VI DMAs straight out of main memory and never
	// snoops the CPU's data cache, so console text written through a cached
	// mapping would sit in L1 and never appear.
	s_xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(s_rmode));
	console_init(s_xfb, 20, 20, s_rmode->fbWidth, s_rmode->xfbHeight,
	             s_rmode->fbWidth * VI_DISPLAY_PIX_SZ);

	VIDEO_Configure(s_rmode);
	VIDEO_SetNextFramebuffer(s_xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (s_rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();

	wiiLog("Plants vs. Zombies - Wii\n");
	wiiLog("video %dx%d ok\n", s_rmode->fbWidth, s_rmode->xfbHeight);
}

bool wiiEnsureStorage()
{
	if (s_storageTried)
		return s_storageReady;
	s_storageTried = true;

	// If this is the first thing to run (a File built from some other static
	// initialiser), make sure the message below is visible.
	wiiEnsureEarlyVideo();

	wiiLog("mounting storage...\n");

	// Mounts SD, USB and the GameCube SD adapters in one call, and chdir()s to
	// the directory the DOL was launched from. See collectMountedDevices.
	s_storageReady = fatInitDefault();

	// SD or USB is a runtime answer, not a build-time one.
	resolveAppDir();

	// Only pay for the USB retry when it can still change the outcome: an SD
	// install has already been found by this point and boots at full speed.
	if (!s_appDirHasData && !dirExists("usb:/") && mountUsbWithRetries())
	{
		s_storageReady = true;
		resolveAppDir();
	}

	if (s_storageReady)
	{
		// Must precede the fopen: s_logPath is empty until this runs, and an
		// fopen("") fails silently into "the log channel simply never wrote
		// anything", which is exactly how this went unnoticed.
		buildLogPath();

		// Keep one stream for the whole session. "w" truncates the previous
		// session before the first line is written.
		s_logFile = std::fopen(s_logPath, "w");
		if (s_logFile != nullptr)
		{
			std::setvbuf(s_logFile, nullptr, _IOLBF, 0);
			wiiLog("[WII][LOG] file=%s\n", s_logPath);
		}
		else
		{
			wiiLog("[WII][LOG][ERROR] cannot open %s errno=%d\n",
			       s_logPath, errno);
		}

		wiiLog("storage mounted\n");
		reportStorageLayout();
	}
	else
	{
		wiiLog("*** NO SD/USB STORAGE ***\n");
		wiiLog("Insert an SD card or plug in a USB drive holding\n");
		wiiLog("  apps/PlantsVsZombies/ , or in Dolphin:\n");
		wiiLog("  Config > Wii > SD Card, enable Insert SD Card\n");
		wiiLog("  and Sync with Folder, point it at bin\\wii\\sd\n");
	}
	return s_storageReady;
}

const char *wiiGetAppDir()
{
	wiiEnsureStorage();

	// Resolution happens inside wiiEnsureStorage, so this is only ever empty if
	// a future edit reorders the two. Keep the contract (never empty) anyway --
	// the callers concatenate onto it without checking.
	if (s_appDir[0] == '\0')
		std::snprintf(s_appDir, sizeof(s_appDir), "sd:%s", kAppSubDir);
	return s_appDir;
}

bool wiiHasGameData()
{
	wiiEnsureStorage();
	return s_appDirHasData;
}

GXRModeObj *wiiGetRenderMode()
{
	wiiEnsureEarlyVideo();
	return s_rmode;
}

void *wiiGetEarlyFramebuffer()
{
	wiiEnsureEarlyVideo();
	return s_xfb;
}

u32 wiiGetHeapCeiling()
{
	if (s_heapCeiling == 0)
	{
		// Called before the constructor ran (or the constructor was elided by a
		// future toolchain). Sampling now is worse -- allocation has already
		// happened -- but it is still a real number, and it can only
		// under-report, which is the safe direction for a "how much is left"
		// display.
		s_heapCeiling = SYS_GetArena1Size() + SYS_GetArena2Size();
	}
	return s_heapCeiling;
}

#endif // WII_PLATFORM
