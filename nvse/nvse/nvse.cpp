#include <filesystem>

#include "CommandTable.h"
#include "Hooks_DirectInput8Create.h"
#include "Hooks_SaveLoad.h"
#include "Hooks_Editor.h"
#include "Hooks_Logging.h"
#include "Hooks_Gameplay.h"
#include "Hooks_Script.h"
#include "Hooks_Animation.h"
#include "Hooks_Dialog.h"
#include "Hooks_Other.h"
#include "ThreadLocal.h"
#include "SafeWrite.h"
#include "Utilities.h"
#include "Commands_Input.h"
#include "GameAPI.h"
#include "EventManager.h"

#if RUNTIME
IDebugLog	gLog("nvse.log");

#else
IDebugLog	gLog("nvse_editor.log");

#endif
UInt32 logLevel = IDebugLog::kLevel_Message;

#if RUNTIME

// fix dinput code so it doesn't acquire the keyboard/mouse in exclusive mode
// bBackground Mouse works on startup, but when gaining focus that setting is ignored
// there's probably a better way to fix the bug but this is good enough
void PatchCoopLevel() {
	SafeWrite8(0x00A227A1 + 1, 0x16);
	SafeWrite8(0x00A229CB + 1, 0x06);
	SafeWrite8(0x00A23CAD + 1, 0x06);
}

#endif

UInt32 waitForDebugger;
UInt32 createHookWindow;
UInt32 et;
UInt32 au3D;
bool g_warnScriptErrors = false;
bool g_noSaveWarnings = false;
std::filesystem::path g_pluginLogPath;

void WaitForDebugger(void)
{
	_MESSAGE("Waiting for debugger");
	while(!IsDebuggerPresent())
	{
		Sleep(10);
	}
	Sleep(1000 * 2);
}

// Moved to a separate function, since strings can throw, and NVSE_Initialize has a __try block which doesn't allow that.
void ReadNVSEPluginLogPath()
{
	g_pluginLogPath = GetNVSEConfigOption("LOGGING", "sPluginLogPath");
	if (g_pluginLogPath.has_extension() || g_pluginLogPath.has_filename()) [[unlikely]]
		g_pluginLogPath = "";
}

void NVSE_Initialize(void)
{
#ifndef _DEBUG
	__try {
#endif
		FILETIME	now;
		GetSystemTimeAsFileTime(&now);

#if RUNTIME
		UInt32 bMousePatch = 0;
		if (GetNVSEConfigOption_UInt32("DEBUG", "EscapeMouse", &bMousePatch) && bMousePatch)
			PatchCoopLevel();
		
		UInt32 noFileWarning = 0;
		if (GetNVSEConfigOption_UInt32("RELEASE", "bNoSaveWarnings", &noFileWarning) && noFileWarning)
			g_noSaveWarnings = true;

		UInt32 bLogToFolder = false;
		if (GetNVSEConfigOption_UInt32("LOGGING", "bLogToFolder", &bLogToFolder) && bLogToFolder)
			gLog.SetLogFolderOption(true);

		_MESSAGE("NVSE runtime: initialize (version = %d.%d.%d %08X %08X%08X)",
			NVSE_VERSION_INTEGER, NVSE_VERSION_INTEGER_MINOR, NVSE_VERSION_INTEGER_BETA, RUNTIME_VERSION,
			now.dwHighDateTime, now.dwLowDateTime);

		GetNVSEConfigOption_UInt32("TESTS", "EnableRuntimeTests", &s_AreRuntimeTestsEnabled);
#else
		_MESSAGE("NVSE editor: initialize (version = %d.%d.%d %08X %08X%08X)",
			NVSE_VERSION_INTEGER, NVSE_VERSION_INTEGER_MINOR, NVSE_VERSION_INTEGER_BETA, EDITOR_VERSION,
			now.dwHighDateTime, now.dwLowDateTime);
#endif
		_MESSAGE("imagebase = %08X", GetModuleHandle(nullptr));

#ifdef _DEBUG
		logLevel = IDebugLog::kLevel_DebugMessage;
		if (GetNVSEConfigOption_UInt32("DEBUG", "LogLevel", &logLevel) && logLevel)
			if (logLevel>IDebugLog::kLevel_DebugMessage)
				logLevel = IDebugLog::kLevel_DebugMessage;
		
		if (GetNVSEConfigOption_UInt32("DEBUG", "AlternateUpdate3D", &au3D) && au3D)
			alternateUpdate3D = true;
		// SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);

#if RUNTIME
		if (GetNVSEConfigOption_UInt32("RUNTIME DEBUG", "WaitForDebugger", &waitForDebugger) && waitForDebugger)
			WaitForDebugger();
		GetNVSEConfigOption_UInt32("FIXES", "EnablePrintDuringOnEquip", &s_CheckInsideOnActorEquipHook);
#else
		if (GetNVSEConfigOption_UInt32("EDITOR DEBUG", "WaitForDebugger", &waitForDebugger) && waitForDebugger)
			WaitForDebugger();
#endif

#else
		if (GetNVSEConfigOption_UInt32("RELEASE", "AlternateUpdate3D", &au3D) && au3D)
			alternateUpdate3D = true;
		if (GetNVSEConfigOption_UInt32("RELEASE", "LogLevel", &logLevel) && logLevel)
			if (logLevel>IDebugLog::kLevel_DebugMessage)
				logLevel = IDebugLog::kLevel_DebugMessage;
#endif
		_memcpy = memcpy;
		_memmove = memmove;

		ReadNVSEPluginLogPath();

		gLog.SetLogLevel(static_cast<IDebugLog::LogLevel>(logLevel));

		MersenneTwister::init_genrand(static_cast<unsigned long>(GetTickCount64())); // GetTickCount overflows every 49 days according to C28159

#if RUNTIME
		// Runs before CommandTable::Init to prevent plugins from being able to register events before ours (breaks assert).
		EventManager::Init();	
#endif
		CommandTable::Init();

#if RUNTIME
		Commands_Input_Init();
		Hook_DirectInput8Create_Init();
		Hook_Gameplay_Init();
		Hook_SaveLoad_Init();
		Hook_Logging_Init();
		ThreadLocalData::Init();
		Hook_Script_Init();
		Hook_Animation_Init();
		OtherHooks::Hooks_Other_Init();

		Hook_Dialog_Init();
		PatchGameCommandParser();
#endif

#if EDITOR
		Hook_Editor_Init();
		Hook_Compiler_Init();
		FixEditorFont();
		PatchDefaultCommandParser();
#if 0
		FixErrorReportBug();

		// Disable check on vanilla opcode range for use of commands as conditionals
		PatchConditionalCommands();
#endif

		// Add "string_var" as alias for "long"
		CreateTokenTypedefs();

		// Allow use of special characters '$', '[', and ']' in string params to script commands
		PatchIsAlpha();

#ifdef _DEBUG
		if (GetNVSEConfigOption_UInt32("EDITOR DEBUG", "CreateHookWindow", &createHookWindow) && createHookWindow)
			CreateHookWindow();
#endif
#endif
		FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

#ifndef _DEBUG
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		_ERROR("exception");
	}
#endif

	_MESSAGE("init complete");
}

extern "C" 
{
	// entrypoint
	void StartNVSE(void)
	{
		NVSE_Initialize();
	}

	BOOL WINAPI DllMain(HANDLE hDllHandle, DWORD dwReason, LPVOID lpreserved)
	{
		if (dwReason == DLL_PROCESS_ATTACH)
		{
			NVSE_Initialize();
		}
		return TRUE;
	}
};
