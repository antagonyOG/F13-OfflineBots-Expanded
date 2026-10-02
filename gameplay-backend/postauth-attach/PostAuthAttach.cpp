#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <bcrypt.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Bcrypt.lib")

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kGameName[] = L"SummerCamp.exe";
constexpr wchar_t kShippingName[] = L"SummerCamp-Win64-Shipping.exe";
constexpr wchar_t kSupportedHash[] = L"941E249E4CAABF93A22FB57A6EB61D894D224C16A7464CD7451B41205436E62F";
constexpr wchar_t kBackendName[] = L"F13BaseGameOfflineBots.dll";
constexpr wchar_t kOnlineFixName[] = L"OnlineFix64.dll";
constexpr uintptr_t kGWorldRva = 0x03228578;
constexpr uintptr_t kGNamesRva = 0x02F88AC0;

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
};

std::wstring Sha256(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0, returned = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};
    const auto finish = [&](bool ok, const unsigned char* digest) {
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        std::wstring result;
        if (ok) for (int i = 0; i < 32; ++i) {
            result += L"0123456789ABCDEF"[digest[i] >> 4];
            result += L"0123456789ABCDEF"[digest[i] & 15];
        }
        return result;
    };
    unsigned char digest[32]{};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &returned, 0) < 0)
        return finish(false, digest);
    std::vector<unsigned char> object(objectBytes), buffer(65536);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0)
        return finish(false, digest);
    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = file.gcount();
        if (count && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0) < 0)
            return finish(false, digest);
    }
    return finish(file.eof() && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0, digest);
}

bool ValidGame(const fs::path& path) {
    if (Sha256(path) != kSupportedHash) return false;
    std::ifstream file(path, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    file.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!file || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return false;
    file.seekg(dos.e_lfanew);
    file.read(reinterpret_cast<char*>(&nt), sizeof(nt));
    return file && nt.Signature == IMAGE_NT_SIGNATURE &&
        nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
        nt.FileHeader.TimeDateStamp == 0x6777E461 &&
        nt.OptionalHeader.SizeOfImage == 0x035C9000;
}

fs::path BootstrapLog() {
    wchar_t temp[32768]{};
    DWORD count = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    return count && count < std::size(temp) ? fs::path(temp) / L"F13-OfflineBots-Bootstrap.log" : fs::path();
}

fs::path OwnExecutable() {
    std::vector<wchar_t> buffer(32768);
    const DWORD count = GetModuleFileNameW(nullptr, buffer.data(),
        static_cast<DWORD>(buffer.size()));
    return count && count < buffer.size()
        ? fs::path(std::wstring(buffer.data(), count))
        : fs::path();
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    Handle ownedToken(token);
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    return GetTokenInformation(token, TokenElevation, &elevation,
        sizeof(elevation), &returned) && elevation.TokenIsElevated != 0;
}

bool RelaunchElevated(const wchar_t* arguments = nullptr) {
    std::vector<wchar_t> executable(32768);
    const DWORD count = GetModuleFileNameW(nullptr, executable.data(),
        static_cast<DWORD>(executable.size()));
    if (!count || count >= executable.size()) return false;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"runas",
        executable.data(), arguments, nullptr,
        arguments ? SW_HIDE : SW_SHOWNORMAL)) > 32;
}

bool SamePath(const fs::path& left, const fs::path& right) {
    return _wcsicmp(left.lexically_normal().c_str(),
        right.lexically_normal().c_str()) == 0;
}

bool FindTarget(const fs::path& expected, DWORD& pid, Handle& process) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry)) return false;
    do {
        if (_wcsicmp(entry.szExeFile, kGameName) != 0 &&
            _wcsicmp(entry.szExeFile, kShippingName) != 0) continue;
        Handle candidate(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_READ |
            PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | SYNCHRONIZE, FALSE,
            entry.th32ProcessID));
        if (!candidate.value) continue;

        std::vector<wchar_t> path(32768);
        DWORD pathLength = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(candidate.value, 0,
                path.data(), &pathLength) ||
            !SamePath(fs::path(std::wstring(path.data(), pathLength)), expected))
            continue;

        if (pid != 0) {
            std::wcerr << L"More than one matching game process is running. "
                L"Close the extra copy first.\n";
            return false;
        }
        pid = entry.th32ProcessID;
        process.value = candidate.value;
        candidate.value = nullptr;
    } while (Process32NextW(snapshot.value, &entry));
    return pid != 0;
}

struct Modules {
    bool onlineFix = false;
    bool backend = false;
    bool antiCheat = false;
    uintptr_t gameModule = 0;
    uintptr_t loadLibraryModule = 0;
};

bool ReadModules(DWORD pid, const wchar_t* loadLibraryModuleName,
    Modules& modules) {
    Handle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (snapshot.value == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.value, &entry)) return false;
    do {
        if (_wcsicmp(entry.szModule, kOnlineFixName) == 0)
            modules.onlineFix = true;
        else if (_wcsicmp(entry.szModule, kBackendName) == 0)
            modules.backend = true;
        if (std::wstring(entry.szModule).find(L"EasyAntiCheat") != std::wstring::npos)
            modules.antiCheat = true;
        if (_wcsicmp(entry.szModule, kGameName) == 0 ||
            _wcsicmp(entry.szModule, kShippingName) == 0)
            modules.gameModule = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        if (_wcsicmp(entry.szModule, loadLibraryModuleName) == 0)
            modules.loadLibraryModule =
                reinterpret_cast<uintptr_t>(entry.modBaseAddr);
    } while (Module32NextW(snapshot.value, &entry));
    return modules.loadLibraryModule != 0;
}

template<typename T>
bool ReadRemote(HANDLE process, uintptr_t address, T& value) {
    SIZE_T read = 0;
    return address != 0 &&
        ReadProcessMemory(process, reinterpret_cast<const void*>(address),
            &value, sizeof(value), &read) && read == sizeof(value);
}

std::string ReadRemoteName(HANDLE process, uintptr_t names,
    int32_t index) {
    if (!names || index < 0 || index >= 2000000) return {};
    uintptr_t chunk = 0;
    uintptr_t entry = 0;
    if (!ReadRemote(process, names +
            static_cast<uintptr_t>(index / 16384) * sizeof(uintptr_t), chunk) ||
        !ReadRemote(process, chunk +
            static_cast<uintptr_t>(index % 16384) * sizeof(uintptr_t), entry))
        return {};
    char buffer[128]{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(process,
            reinterpret_cast<const void*>(entry + 0x10), buffer,
            sizeof(buffer) - 1, &read) || read == 0)
        return {};
    buffer[sizeof(buffer) - 1] = '\0';
    return std::string(buffer);
}

std::string ReadRemoteClassName(HANDLE process, uintptr_t names,
    uintptr_t object) {
    uintptr_t cls = 0;
    int32_t index = -1;
    if (!ReadRemote(process, object + 0x10, cls) ||
        !ReadRemote(process, cls + 0x18, index))
        return {};
    return ReadRemoteName(process, names, index);
}

// Stock EXE GetCurrentMenu (RVA 0x427DA0) reads the menu stack's last
// 32-byte record at +8. Its owner comes from UWorld +0x3F8, then +0x450.
// This companion reads that state without loading code into the game during
// sign-in. A loaded/preconstructed OfflinePlay widget alone is not enough.
std::string CurrentMenuClass(HANDLE process, uintptr_t gameModule,
    std::wstring& diagnostic) {
    uintptr_t world = 0;
    uintptr_t gameInstance = 0;
    uintptr_t localPlayers = 0;
    uintptr_t localPlayer = 0;
    uintptr_t playerController = 0;
    uintptr_t menuOwner = 0;
    uintptr_t menuStack = 0;
    uintptr_t records = 0;
    uintptr_t widget = 0;
    uintptr_t names = 0;
    int32_t count = 0;
    if (!ReadRemote(process, gameModule + kGWorldRva, world)) {
        diagnostic = L"GWorld global unreadable";
        return {};
    }
    diagnostic = L"world=" + std::to_wstring(world);
    // GetCurrentMenu calls UUserWidget::GetOwningPlayer, then reads that
    // PlayerController's MyHUD at +0x3F8 and the HUD stack at +0x450.
    // Reach the same controller read-only through UWorld -> GameInstance ->
    // LocalPlayers[0] -> PlayerController, matching the in-game Engine route.
    if (!ReadRemote(process, world + 0x140, gameInstance) ||
        !ReadRemote(process, gameInstance + 0x38, localPlayers) ||
        !ReadRemote(process, localPlayers, localPlayer) ||
        !ReadRemote(process, localPlayer + 0x30, playerController) ||
        !ReadRemote(process, playerController + 0x3F8, menuOwner) ||
        !ReadRemote(process, menuOwner + 0x450, menuStack) ||
        !ReadRemote(process, menuStack + 0xF0, records) ||
        !ReadRemote(process, menuStack + 0xF8, count) ||
        count <= 0 || count > 64 ||
        !ReadRemote(process, records +
            static_cast<uintptr_t>(count - 1) * 0x20 + 0x8, widget) ||
        !ReadRemote(process, gameModule + kGNamesRva, names)) {
        diagnostic += L" instance=" + std::to_wstring(gameInstance) +
            L" players=" + std::to_wstring(localPlayers) +
            L" player=" + std::to_wstring(localPlayer) +
            L" controller=" + std::to_wstring(playerController) +
            L" owner=" + std::to_wstring(menuOwner) +
            L" stack=" + std::to_wstring(menuStack) +
            L" count=" + std::to_wstring(count) +
            L" records=" + std::to_wstring(records) +
            L" widget=" + std::to_wstring(widget);
        return {};
    }
    diagnostic += L" instance=" + std::to_wstring(gameInstance) +
        L" players=" + std::to_wstring(localPlayers) +
        L" player=" + std::to_wstring(localPlayer) +
        L" controller=" + std::to_wstring(playerController) +
        L" owner=" + std::to_wstring(menuOwner) +
        L" stack=" + std::to_wstring(menuStack) +
        L" count=" + std::to_wstring(count) +
        L" widget=" + std::to_wstring(widget);
    return ReadRemoteClassName(process, names, widget);
}

void AutoLog(const fs::path& directory, const std::wstring& message) {
    (void)directory;
    std::wofstream output(BootstrapLog(), std::ios::app);
    if (output) output << GetTickCount64() << L" " << message << L"\n";
}

bool Attach(HANDLE process, DWORD pid, const fs::path& dll);

int AutoLaunchAndWatch(const fs::path& directory, const fs::path& game,
    const fs::path& backend) {
    ShowWindow(GetConsoleWindow(), SW_HIDE);
    DWORD pid = 0;
    Handle process;
    if (!FindTarget(game, pid, process)) {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION launched{};
        const fs::path root = directory.parent_path().parent_path().parent_path();
        const fs::path wrapper = root / kGameName;
        const fs::path launch = fs::is_regular_file(wrapper) ? wrapper : game;
        std::wstring command = L"\"" + launch.wstring() + L"\"";
        std::wstring working = launch.parent_path().wstring();
        if (!CreateProcessW(launch.c_str(), command.data(), nullptr, nullptr,
                FALSE, 0, nullptr, working.c_str(), &startup, &launched)) {
            AutoLog(directory, L"Game launch failed: " +
                std::to_wstring(GetLastError()));
            return 3;
        }
        CloseHandle(launched.hThread);
        CloseHandle(launched.hProcess);
        AutoLog(directory, L"Launch target=" + launch.wstring());
        for (int i = 0; i < 120 && !FindTarget(game, pid, process); ++i)
            Sleep(500);
    }
    if (!process.value) {
        AutoLog(directory, L"Matching game process was not found");
        return 4;
    }
    AutoLog(directory, L"Watching game PID " + std::to_wstring(pid));
    AutoLog(directory, L"PID=" + std::to_wstring(pid));

    bool attached = false;
    bool attachAttempted = false;
    int stableOfflineMenu = 0;
    std::string lastMenu;
    std::wstring lastDiagnostic;
    while (WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) {
        Modules modules{};
        if (ReadModules(pid, L"kernel32.dll", modules) &&
            modules.gameModule) {
            if (modules.antiCheat) {
                AutoLog(directory, L"INITIALIZATION: FAILED | Anti-cheat module present; attachment refused");
                MessageBoxW(nullptr, L"Anti-cheat is active. Offline Bots will not attach. Close the game and use a supported offline setup. No security setting was changed.", L"F13 Offline Bots", MB_ICONERROR);
                return 6;
            }
            if (modules.backend) {
                if (!attached) AutoLog(directory, L"BACKEND MODULE: LOADED | already present; initialization not inferred");
                attached = true;
            } else if (!attachAttempted) {
                std::wstring diagnostic;
                const std::string menu = CurrentMenuClass(
                    process.value, modules.gameModule, diagnostic);
                if (diagnostic != lastDiagnostic) {
                    AutoLog(directory, L"Menu probe: " + diagnostic);
                    lastDiagnostic = diagnostic;
                }
                if (menu != lastMenu) {
                    AutoLog(directory, L"Current menu: " +
                        std::wstring(menu.begin(), menu.end()));
                    lastMenu = menu;
                }
                stableOfflineMenu = menu == "OfflinePlayMenuWidget_C"
                    ? stableOfflineMenu + 1 : 0;
                if (stableOfflineMenu >= 2) {
                    attachAttempted = true;
                    attached = Attach(process.value, pid, backend);
                    AutoLog(directory, attached
                        ? L"BACKEND MODULE: LOADED | Offline Play ready; backend initialization remains pending"
                        : L"INITIALIZATION: FAILED | Attach failed; will not retry in this game process");
                    if (!attached) {
                        MessageBoxW(nullptr, L"Offline Bots attachment failed. Run VERIFY-F13-OFFLINE-BOTS.bat and send the diagnostic. No second attach will be attempted in this process.", L"F13 Offline Bots", MB_ICONERROR);
                        return 5;
                    }
                }
            }
        }
        if (attached) {
            // Once loaded, the launcher does no more module/menu polling.
            WaitForSingleObject(process.value, INFINITE);
            break;
        }
        Sleep(1000);
    }
    AutoLog(directory, L"Game closed; watcher exiting");
    return attached ? 0 : 5;
}

bool Attach(HANDLE process, DWORD pid, const fs::path& dll) {
    HMODULE localKernel = GetModuleHandleW(L"kernel32.dll");
    FARPROC localLoad = localKernel
        ? GetProcAddress(localKernel, "LoadLibraryW") : nullptr;
    HMODULE localOwner = nullptr;
    if (!localLoad || !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(localLoad), &localOwner))
        return false;
    std::vector<wchar_t> ownerPath(32768);
    const DWORD ownerLength = GetModuleFileNameW(localOwner,
        ownerPath.data(), static_cast<DWORD>(ownerPath.size()));
    if (!ownerLength || ownerLength >= ownerPath.size()) return false;
    const std::wstring ownerName =
        fs::path(std::wstring(ownerPath.data(), ownerLength))
            .filename().wstring();

    Modules before{};
    if (!ReadModules(pid, ownerName.c_str(), before)) {
        std::wcerr << L"Cannot inspect loaded modules (Win32 "
            << GetLastError() << L"). No attachment attempted.\n";
        return false;
    }
    if (before.antiCheat) {
        std::wcerr << L"Anti-cheat is active. No attachment attempted.\n";
        return false;
    }
    if (before.backend) {
        std::wcout << L"Offline Bots is already loaded.\n";
        return true;
    }

    const uintptr_t offset = reinterpret_cast<uintptr_t>(localLoad) -
        reinterpret_cast<uintptr_t>(localOwner);
    const auto remoteLoad = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        before.loadLibraryModule + offset);

    const std::wstring path = dll.wstring();
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remotePath) return false;

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, path.c_str(), bytes,
            &written) || written != bytes) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    Handle thread(CreateRemoteThread(process, nullptr, 0, remoteLoad,
        remotePath, 0, nullptr));
    if (!thread.value) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread.value, 20000);
    if (wait != WAIT_OBJECT_0) {
        // The remote thread may still be reading the path. Intentionally do
        // not free its memory or start a second attach attempt in this run.
        std::wcerr << L"Attachment did not complete within 20 seconds. "
            L"Do not retry until the game is closed.\n";
        return false;
    }
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

    Modules after{};
    if (!ReadModules(pid, ownerName.c_str(), after) || !after.backend) {
        std::wcerr << L"Remote loader returned, but Offline Bots is not "
            L"listed as a loaded module.\n";
        return false;
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool autoLaunch = argc == 2 &&
        _wcsicmp(argv[1], L"--auto-launch") == 0;
    const bool checkOnly = argc == 2 && _wcsicmp(argv[1], L"--check-files") == 0;
    if (!checkOnly && !IsElevated()) {
        if (!RelaunchElevated(autoLaunch ? L"--auto-launch" : nullptr)) {
            std::wcerr << L"Administrator approval is required to attach "
                L"to the elevated game.\n";
            return 1;
        }
        return 0;
    }

    const fs::path ownExecutable = OwnExecutable();
    const fs::path directory = ownExecutable.parent_path();
    const fs::path shipping = directory / kShippingName;
    const fs::path game = fs::is_regular_file(shipping) ? shipping : directory / kGameName;
    const fs::path backend = directory / kBackendName;
    std::wofstream(BootstrapLog(), std::ios::trunc) << L"F13 Offline Bots 1.0.0-rc1\n";
    SYSTEMTIME utc{}; GetSystemTime(&utc);
    wchar_t utcText[40]{};
    swprintf_s(utcText, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
    AutoLog(directory, L"SessionStartUTC=" + std::wstring(utcText));
    AutoLog(directory, L"BOOTSTRAP ENTRY: EXECUTED | Method=dedicated delayed launcher");
    AutoLog(directory, L"BootstrapModulePath=" + ownExecutable.wstring());
    AutoLog(directory, L"ExecutablePath=" + game.wstring());
    AutoLog(directory, L"ExecutableProcessName=" + game.filename().wstring());
    AutoLog(directory, L"ExecutableSHA256=" + Sha256(game));
    AutoLog(directory, L"BackendPath=" + backend.wstring());
    AutoLog(directory, L"BackendSHA256=" + Sha256(backend));
    AutoLog(directory, L"BACKEND MODULE: PENDING | INITIALIZATION: PENDING GAME START");
    const auto finish = [](int result) {
        std::wcout << L"Press Enter to close this window.\n";
        std::wstring ignored;
        std::getline(std::wcin, ignored);
        return result;
    };
    if (directory.empty() || !ValidGame(game) || !fs::is_regular_file(backend)) {
        AutoLog(directory, L"INITIALIZATION: FAILED | Unsupported executable or missing required backend DLL");
        if (!checkOnly) MessageBoxW(nullptr, L"Unsupported game executable or missing F13BaseGameOfflineBots.dll. Run VERIFY-F13-OFFLINE-BOTS.bat and send the diagnostic file.", L"F13 Offline Bots", MB_ICONERROR);
        return 2;
    }
    if (checkOnly) { std::wcout << L"GAME BUILD: SUPPORTED | BACKEND DLL: PRESENT\n"; return 0; }

    if (autoLaunch)
        return AutoLaunchAndWatch(directory, game, backend);

    std::wcout << L"Sign in normally first, then return to the game's main "
        L"menu.\nType YES only after sign-in has completed: ";
    std::wstring confirmation;
    std::getline(std::wcin, confirmation);
    if (confirmation != L"YES") {
        std::wcout << L"Cancelled; no DLL was loaded.\n";
        return finish(0);
    }

    DWORD pid = 0;
    Handle process;
    if (!FindTarget(game, pid, process)) {
        std::wcerr << L"The matching SummerCamp.exe is not running, or "
            L"access was denied. Run this tool as administrator.\n";
        return finish(3);
    }
    if (!Attach(process.value, pid, backend)) {
        std::wcerr << L"Attach failed (last Win32 error "
            << GetLastError() << L").\n";
        return finish(4);
    }

    std::wcout << L"Offline Bots DLL loaded into PID " << pid
        << L". Return to the game and open Offline Play.\n";
    return finish(0);
}
