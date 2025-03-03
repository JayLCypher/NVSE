#include "IdentifyEXE.h"
#include "Error.h"
#include "nvse/nvse_version.h"

static bool GetFileVersion(const char * path, VS_FIXEDFILEINFO * info) {
	bool result = false;

	const UInt32 versionSize = GetFileVersionInfoSize(path, nullptr);
	if (!versionSize) {
		_ERROR("GetFileVersionInfoSize failed (%08X)", GetLastError());
		return false;
	}

	
	if (auto *versionBuf = new UInt8[versionSize]; versionBuf) {
		if (GetFileVersionInfo(path, NULL, versionSize, versionBuf)) {
			VS_FIXEDFILEINFO *retrievedInfo = nullptr;
			UInt32 realVersionSize = sizeof(VS_FIXEDFILEINFO);

			if (VerQueryValue(versionBuf, "\\", reinterpret_cast<void **>(&retrievedInfo), reinterpret_cast<PUINT>(&realVersionSize)) && retrievedInfo) {
				*info = *retrievedInfo;
				result = true;
			}
			else { _ERROR("VerQueryValue failed (%08X)", GetLastError()); }
		}
		else { _ERROR("GetFileVersionInfo failed (%08X)", GetLastError()); }
		delete [] versionBuf;
	}
	return result;
}

static bool GetFileVersionNumber(const char * path, UInt64 * out) {
	VS_FIXEDFILEINFO	versionInfo;
	if (!GetFileVersion(path, &versionInfo)) { return false; }

	_MESSAGE("dwSignature = %08X", versionInfo.dwSignature);
	_MESSAGE("dwStrucVersion = %08X", versionInfo.dwStrucVersion);
	_MESSAGE("dwFileVersionMS = %08X", versionInfo.dwFileVersionMS);
	_MESSAGE("dwFileVersionLS = %08X", versionInfo.dwFileVersionLS);
	_MESSAGE("dwProductVersionMS = %08X", versionInfo.dwProductVersionMS);
	_MESSAGE("dwProductVersionLS = %08X", versionInfo.dwProductVersionLS);
	_MESSAGE("dwFileFlagsMask = %08X", versionInfo.dwFileFlagsMask);
	_MESSAGE("dwFileFlags = %08X", versionInfo.dwFileFlags);
	_MESSAGE("dwFileOS = %08X", versionInfo.dwFileOS);
	_MESSAGE("dwFileType = %08X", versionInfo.dwFileType);
	_MESSAGE("dwFileSubtype = %08X", versionInfo.dwFileSubtype);
	_MESSAGE("dwFileDateMS = %08X", versionInfo.dwFileDateMS);
	_MESSAGE("dwFileDateLS = %08X", versionInfo.dwFileDateLS);

	const UInt64 version = static_cast<UInt64>(versionInfo.dwFileVersionMS) << 32 | versionInfo.dwFileVersionLS;

	*out = version;
	return true;
}

const IMAGE_SECTION_HEADER *GetImageSection(const UInt8 *base, const char *name) {
	const auto *dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	const auto *ntHeader = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dosHeader->e_lfanew); // Fix potential cast from const to non-const issues
	const IMAGE_SECTION_HEADER *sectionHeader = IMAGE_FIRST_SECTION(ntHeader);

	for(UInt32 i = 0; i < ntHeader->FileHeader.NumberOfSections; i++) {
		if(const IMAGE_SECTION_HEADER	*section = &sectionHeader[i]; !strcmp(reinterpret_cast<const char *>(section->Name), name)) { return section; }
	}
	return nullptr;
}

// steam EXE will have the .bind section
bool IsSteamImage(const UInt8 *base) { return GetImageSection(base, ".bind") != nullptr; }
bool IsUPXImage(const UInt8 *base) { return GetImageSection(base, "UPX0") != nullptr;}

struct DebugHeader {
	char	sig[4];		// "RSDS"
	UInt8	guid[0x10];
	UInt32	age;
	// path follows

	[[nodiscard]] bool SignatureValid() const { return memcmp(sig, "RSDS", 4) == 0; }

	bool IsNoGore(bool * probablyValid) const {
		*probablyValid = false;

		if (SignatureValid()) {
			const auto *path = reinterpret_cast<const char *>(this + 1);

			// path will start with <drive letter> colon backslash
			if(path[1] != ':') return false;
			if(path[2] != '\\') return false;

			// make sure the string isn't stupidly long and only contains printable characters
			for(UInt32 i = 0; i < 0x80; i++) {
				const char data = path[i];
				if(!data) {
					*probablyValid = true;

					if(strstr(path, "FalloutNVng.pdb")) {
						_MESSAGE("pdb path = %s", path);
						return true;
					}
					break;
				}
				if(data < 0 || !isprint(data)) { break; }
			}
		}
		return false;
	}
};

const UInt8 *GetVirtualAddress(const UInt8 * base, UInt32 addr) {
	const auto *dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	const auto *ntHeader = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dosHeader->e_lfanew);
	const IMAGE_SECTION_HEADER	* sectionHeader = IMAGE_FIRST_SECTION(ntHeader);

	for(UInt32 i = 0; i < ntHeader->FileHeader.NumberOfSections; i++) {
		if(const IMAGE_SECTION_HEADER *section = &sectionHeader[i]; addr >= section->VirtualAddress && addr <= (section->VirtualAddress + section->SizeOfRawData)) {
			const UInt32 offset = addr - section->VirtualAddress;
			return base + section->PointerToRawData + offset;
		}
	}
	return nullptr;
}

bool IsNoGore_BasicScan(const UInt8 *base, bool *probable) {
	*probable = false;

	const auto *dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	const auto *ntHeader = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dosHeader->e_lfanew);
	const auto *debugDir = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY *>(GetVirtualAddress(base, ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress));

	if(!debugDir) { return false; }
	if(debugDir->Characteristics) { return false; }
	if(debugDir->Type != IMAGE_DEBUG_TYPE_CODEVIEW) { return false; }
	if(debugDir->SizeOfData >= 0x100) { return false; }
	if(debugDir->AddressOfRawData >= 0x10000000) { return false; }
	if(debugDir->PointerToRawData >= 0x10000000) { return false; }

	const auto *debugHeader = reinterpret_cast<const DebugHeader *>(base + debugDir->PointerToRawData);

	if(!debugHeader->SignatureValid()) { return false; }

	*probable = true;
	bool headerProbable;	// thrown away
	return debugHeader->IsNoGore(&headerProbable);
}

// nogore EXE has debug info pointing to FalloutNVng.pdb
// however sometimes the debug info is pointing to the wrong place?
bool IsNoGore(const UInt8 * base) {
	bool	result = false;
	bool	probable = false;

	// first check the header where it says it should be
	result = IsNoGore_BasicScan(base, &probable);
	if (!probable) {
		_MESSAGE("using slow nogore check");

		// keep scanning, now do the slow and manual way
		// look for RSDS header in .rdata

		const IMAGE_SECTION_HEADER	*rdataSection = GetImageSection(base, ".rdata");
		if(rdataSection) {
			const UInt8	* sectionBase = base + rdataSection->PointerToRawData;
			UInt32		sectionLen = rdataSection->SizeOfRawData;

			__try {
				for(UInt32 i = 0; (i + sizeof(DebugHeader)) <= sectionLen; i += 4) {
					if (const auto *header = reinterpret_cast<const DebugHeader *>(sectionBase + i); header->IsNoGore(&probable)) {
						result = true;
						break;
					}
					if(probable) { break; }
				}
			}
			__except(EXCEPTION_EXECUTE_HANDLER) { _WARNING("exception while scanning for nogore"); }
		}
	}

	return result;
}

bool ScanEXE(const char * path, bool * isSteam, bool * isNoGore, bool * isUPX) {
	// open and map the file in to memory
	const HANDLE file = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if(file == INVALID_HANDLE_VALUE) {
		_ERROR("ScanEXE: couldn't open file (%d)", GetLastError());
		return false;
	}

	bool result = false;

	if (const HANDLE mapping = CreateFileMapping(file, nullptr, PAGE_READONLY, 0, 0, nullptr); mapping) {
		if(const auto *fileBase = static_cast<const UInt8 *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)); fileBase) {
			*isSteam = IsSteamImage(fileBase);
			*isNoGore = IsNoGore(fileBase);
			*isUPX = IsUPXImage(fileBase);

			result = true;
			UnmapViewOfFile(fileBase);
		}
		else { _ERROR("ScanEXE: couldn't map file (%d)", GetLastError()); }
		CloseHandle(mapping);
	}
	else { _ERROR("ScanEXE: couldn't create file mapping (%d)", GetLastError()); 	}

	CloseHandle(file);
	return result;
}

bool IdentifyEXE(const char * procName, const bool isEditor, std::string * dllSuffix, ProcHookInfo * hookInfo) {
	UInt64	version;
	bool	isSteam, isNoGore, isUPX;

	// check file version
	if(!GetFileVersionNumber(procName, &version)) {
		PrintLoaderError("Couldn't retrieve EXE version information.");
		return false;
	}

	_MESSAGE("version = %016I64X", version);

	// check for Steam EXE
	if(!ScanEXE(procName, &isSteam, &isNoGore, &isUPX)) {
		PrintLoaderError("Failed to identify Steam EXE.");
		return false;
	}

	if (isNoGore) {
		PrintLoaderError("You have the german NoGore version of FalloutNV.exe which is not supported by NVSE. To fix this search online for a \"German Uncut Fallout New Vegas Patch\" or request a different EXE from Steam customer support.");
		return false;
	}

	// ### how to tell the difference between nogore versions and standard without a global checksum?
	// ### since we're mapping the exe to check for steam anyway, checksum the .text segment maybe?

	_MESSAGE(isSteam ? "steam exe" : "normal exe");
	if(isNoGore) { _MESSAGE("nogore"); }
	if(isUPX) { _MESSAGE("upx"); }

	hookInfo->version = version;
	hookInfo->noGore = isNoGore;

	if(isUPX) { hookInfo->procType = kProcType_Packed; }
	else if(isSteam) { hookInfo->procType = kProcType_Steam; }
	else { hookInfo->procType = kProcType_Normal; }

	bool result = false;

	if (isEditor) {
		switch(version) {
			case 0x0001000100000106:	// 1.1.0.262 (original release)
				hookInfo->hookCallAddr = 0x00C61BA1;
				hookInfo->loadLibAddr = 0x00D2218C;
				*dllSuffix = "1_1";
				PrintLoaderError("Please update to the latest version of the GECK.");
				break;

			case 0x00010003000001C4:	// 1.3.0.452
				hookInfo->hookCallAddr = 0x00C62AF1;
				hookInfo->loadLibAddr = 0x00D2318C;
				*dllSuffix = "1_3";
				PrintLoaderError("Please update to the latest version of the GECK.");
				break;

			case 0x0001000400000206:	// 1.4.0.518
				hookInfo->hookCallAddr = 0x00C62BC1;
				hookInfo->loadLibAddr = 0x00D2318C;
				*dllSuffix = "1_4";
				result = true;
				break;

			default:
				PrintLoaderError("You have an unknown version of the CS. Please check http://nvse.silverlock.org to make sure you're using the latest version of NVSE. (version = %016I64X)", version);
				break;
		}
	}
	else {
		if(isUPX) { PrintLoaderError("Packed versions of Fallout are not supported."); }
		else {
			constexpr UInt64 kCurVersion = 0x000100040000020D;	// 1.4.0.525

			if(version < kCurVersion) { PrintLoaderError("Please update to the latest version of Fallout."); }
			else if(version == kCurVersion) {
				if (isNoGore) {
					hookInfo->hookCallAddr = 0x00ECC3FB;
					hookInfo->loadLibAddr = 0x00FDF0B0;
					*dllSuffix = "1_4ng";
					result = true;
				}
				else {
					hookInfo->hookCallAddr = 0x00ECC46B;
					hookInfo->loadLibAddr = 0x00FDF0B0;
					*dllSuffix = "1_4";
					result = true;
				}
			}
			else {
				PrintLoaderError("You are using a newer version of Fallout than this version of NVSE supports. If the patch to this version just came out, please be patient while we update our code. In the meantime, please check http://nvse.silverlock.org to make sure you're using the latest version of NVSE. (version = %016I64X %08X)", version, PACKED_NVSE_VERSION);
			}
		}
	}
	return result;
}
