#include <Windows.h>
#include "Game/Engine/Engine.hpp"
#include "Game/Features/Features.hpp"
#include "Game/Setup/OfflineSetup.hpp"
#include "Game/Setup/FrozenJasonBridge.hpp"
#include "Utils/Logger/Logger.hpp"
#include "Utils/Memory.hpp"
#include "../vendor/minhook/include/MinHook.h"
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <intrin.h>
#include <string>
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "User32.lib")

static volatile LONG g_Running = 1;
static HMODULE g_BackendModule = nullptr;

using OfflineLifecycleFn = void(__fastcall*)(void*);
using FindSaveGameFn = void*(__fastcall*)(void*, void*);
using RequestExitFn = void(__fastcall*)(bool);

static OfflineLifecycleFn g_OriginalOfflineBots818 = nullptr;
static OfflineLifecycleFn g_OriginalOfflineBots958 = nullptr;
static OfflineLifecycleFn g_OfflineParent818 = nullptr;
static OfflineLifecycleFn g_OfflineParent958 = nullptr;
static FindSaveGameFn g_OriginalFindSaveGame = nullptr;
static thread_local bool g_StockJasonIntroLookup = false;
static RequestExitFn g_OriginalRequestExit = nullptr;
static std::atomic<bool> g_LifecycleTraceHooksInstalled{ false };
static std::atomic<uint32_t> g_818TraceCount{ 0 };
static std::atomic<uint32_t> g_958TraceCount{ 0 };
static volatile LONG g_RequestExitLogging = 0;
static std::atomic<ULONGLONG> g_LastF1HotkeyRequestAt{ 0 };
static std::atomic<ULONGLONG> g_LastF3HotkeyRequestAt{ 0 };

// Donor-style, click-through status window. UI work runs on the existing
// backend worker at 4 Hz; there is no renderer hook or game-object scan.
static HWND g_HostedStatusWindow = nullptr;
static HFONT g_HostedStatusFont = nullptr;
static int g_HostedStatusPaintState = 0;
static constexpr wchar_t HostedStatusClass[] = L"F13BaseHostedJasonStatus";

static LRESULT CALLBACK HostedStatusProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT)
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT rect{}; GetClientRect(window, &rect);
        FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_HostedStatusPaintState == 2 ? RGB(80,255,120) : RGB(255,220,80));
        HGDIOBJ previous = g_HostedStatusFont ? SelectObject(dc, g_HostedStatusFont) : nullptr;
        DrawTextW(dc, g_HostedStatusPaintState == 2 ? L"AI JASON: ACTIVE" : L"AI JASON: ARMED",
            -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (previous) SelectObject(dc, previous);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

static void UpdateHostedStatusWindow()
{
    static ULONGLONG nextAt = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < nextAt) return;
    nextAt = now + 250;
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    const int state = OfflineSetup::IsHostedCounselorArmed() ? 2 :
        (OfflineSetup::IsHostedJasonArmed() ? 1 : 0);
    if (!state)
    {
        if (g_HostedStatusWindow && IsWindowVisible(g_HostedStatusWindow))
            ShowWindow(g_HostedStatusWindow, SW_HIDE);
        return;
    }
    if (!g_HostedStatusWindow)
    {
        WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HostedStatusProc; wc.hInstance = g_BackendModule;
        wc.lpszClassName = HostedStatusClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        g_HostedStatusFont = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        g_HostedStatusWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT |
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, HostedStatusClass,
            L"AI Jason Status", WS_POPUP, 0, 0, 330, 32, nullptr, nullptr, g_BackendModule, nullptr);
        if (!g_HostedStatusWindow)
        {
            if (g_HostedStatusFont) DeleteObject(g_HostedStatusFont);
            g_HostedStatusFont = nullptr;
            Logger::Error("HOSTED AI status window unavailable; arming remains logged");
            return;
        }
        SetLayeredWindowAttributes(g_HostedStatusWindow, RGB(0,0,0), 255, LWA_COLORKEY);
    }
    HWND gameWindow = GetForegroundWindow();
    DWORD pid = 0; GetWindowThreadProcessId(gameWindow, &pid);
    RECT client{};
    if (pid != GetCurrentProcessId() || gameWindow == g_HostedStatusWindow ||
        IsIconic(gameWindow) || !GetClientRect(gameWindow, &client) ||
        client.right < 640 || client.bottom < 360)
    {
        if (IsWindowVisible(g_HostedStatusWindow)) ShowWindow(g_HostedStatusWindow, SW_HIDE);
        return;
    }
    POINT origin{}; ClientToScreen(gameWindow, &origin);
    static HWND positionedGameWindow = nullptr;
    static POINT positionedOrigin{};
    const bool hidden = !IsWindowVisible(g_HostedStatusWindow);
    if (g_HostedStatusPaintState != state || hidden)
    {
        g_HostedStatusPaintState = state;
        InvalidateRect(g_HostedStatusWindow, nullptr, TRUE);
    }
    if (hidden || positionedGameWindow != gameWindow ||
        positionedOrigin.x != origin.x || positionedOrigin.y != origin.y)
    {
        if (SetWindowPos(g_HostedStatusWindow, HWND_TOPMOST, origin.x + 18, origin.y + 18,
            330, 32, SWP_NOACTIVATE | SWP_SHOWWINDOW))
        {
            positionedGameWindow = gameWindow;
            positionedOrigin = origin;
        }
    }
}

static void DestroyHostedStatusWindow()
{
    if (g_HostedStatusWindow) DestroyWindow(g_HostedStatusWindow);
    if (g_HostedStatusFont) DeleteObject(g_HostedStatusFont);
    g_HostedStatusWindow = nullptr; g_HostedStatusFont = nullptr;
    UnregisterClassW(HostedStatusClass, g_BackendModule);
}

static bool IsVerifiedOfflineSandboxWorld()
{
    UWorld* world = Engine::GetWorld();
    if (!world || !Memory::IsReadable(world, sizeof(UWorld)))
        return false;
    auto* modeField = reinterpret_cast<UObject**>(
        reinterpret_cast<uintptr_t>(world) + 0xF0);
    auto* stateField = reinterpret_cast<UObject**>(
        reinterpret_cast<uintptr_t>(world) + 0xF8);
    if (!Memory::IsReadable(modeField, sizeof(UObject*)) ||
        !Memory::IsReadable(stateField, sizeof(UObject*)))
        return false;
    UObject* mode = *modeField;
    UObject* state = *stateField;
    if (!mode || !state ||
        !Memory::IsReadable(mode, sizeof(UObject)) ||
        !Memory::IsReadable(state, sizeof(UObject)) ||
        !mode->Class || !state->Class ||
        !Memory::IsReadable(mode->Class, sizeof(UClass)) ||
        !Memory::IsReadable(state->Class, sizeof(UClass)))
        return false;
    return mode->Class->GetName().find("SCGameMode_Sandbox") !=
               std::string::npos &&
           state->Class->GetName().find("SCGameState_Sandbox") !=
               std::string::npos;
}

static bool QueueF1JasonHotkeyRequest(const char* source)
{
    const bool offline = OfflineSetup::IsCounselorMatchInProgress();
    if ((offline && !func) || (!offline && !OfflineSetup::IsHostedCounselorArmed()))
    {
        Logger::Debug(std::string("F1 ") + source +
            " ignored: requires Offline Bots Counselor or active F8-armed hosted match");
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    ULONGLONG previous = g_LastF1HotkeyRequestAt.load();
    do
    {
        if (now - previous < 700)
            return false;
    }
    while (!g_LastF1HotkeyRequestAt.compare_exchange_weak(
        previous, now));

    const bool queued = offline ? func->QueueFirstJasonSandbox() : OfflineSetup::QueueHostedAdditionalJason();
    Logger::Debug(std::string("F1 ") + source +
        (queued ? " additional Jason request queued" :
                  " additional Jason request rejected/pending"));
    return queued;
}

static bool QueueF3CounselorHotkeyRequest(const char* source)
{
    const bool offline = OfflineSetup::IsCounselorMatchInProgress();
    if (!offline && !OfflineSetup::IsHostedCounselorArmed())
    {
        Logger::Debug(std::string("F3 ") + source +
            " ignored: requires Offline Bots Counselor or F8-armed hosted match");
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    ULONGLONG previous = g_LastF3HotkeyRequestAt.load();
    do
    {
        // The worker and the game-window hook can observe the same key press.
        // Allow only one request even if the game thread consumes it quickly.
        if (now - previous < 700)
            return false;
    }
    while (!g_LastF3HotkeyRequestAt.compare_exchange_weak(
        previous, now));

    const bool queued = offline ? QueueCounselorBotRequest() : OfflineSetup::QueueHostedCounselor();
    Logger::Debug(std::string("F3 ") + source +
        (queued ? " counselor request queued" :
                  " counselor request rejected/pending"));
    return queued;
}

// Exact stock-base-game-2025 lifecycle mappings. Each target was mapped from
// the finalized Resurrected function body and checked against a unique or
// structurally identical stock implementation before this port was enabled.
static constexpr uintptr_t RVA_OfflineBots818 = 0x381570;
static constexpr uintptr_t RVA_OfflineBots958 = 0x3831E0;
static constexpr uintptr_t RVA_OfflineParent818 = 0x3811F0;
static constexpr uintptr_t RVA_OfflineParent958 = 0x382090;
static constexpr uintptr_t RVA_FindSaveGame = 0x299F30;
static constexpr uintptr_t RVA_RequestExit = 0x600030;
static constexpr uintptr_t RVA_GErrorHist = 0x2FE0600;
static constexpr uintptr_t RVA_GIsCriticalError = 0x2FEC603;
static constexpr uintptr_t RVA_LooseFileAlignmentGate = 0x5ECF93;

static bool IsExactSupportedRuntime()
{
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module)
        return false;

    const auto* image = reinterpret_cast<const uint8_t*>(module);
    if (!Memory::IsReadable(image, sizeof(IMAGE_DOS_HEADER)))
        return false;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return false;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        image + dos->e_lfanew);
    if (!Memory::IsReadable(nt, sizeof(*nt)) ||
        nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return false;
    }

    wchar_t path[32768]{};
    const DWORD pathLength = GetModuleFileNameW(
        module,
        path,
        static_cast<DWORD>(std::size(path)));
    if (pathLength == 0 || pathLength >= std::size(path))
        return false;

    const wchar_t* filename = wcsrchr(path, L'\\');
    filename = filename ? filename + 1 : path;

    return (_wcsicmp(filename, L"SummerCamp.exe") == 0 ||
            _wcsicmp(filename, L"SummerCamp-Win64-Shipping.exe") == 0) &&
        nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        nt->FileHeader.TimeDateStamp == 0x6777E461 &&
        nt->OptionalHeader.SizeOfImage == 0x035C9000;
}

static bool InstallDonorLooseFileReadFallback()
{
#if defined(F13_BASE_GAME_PORT)
    // The release target is PAK-based for both supported installation layouts.
    // Never apply the Resurrected-only async-read patch to the stock executable.
    Logger::Debug(
        "base-port: packed resource path selected; Resurrected loose-file patch disabled");
    return true;
#else
    const wchar_t* commandLine = GetCommandLineW();
    if (!commandLine ||
        (wcsstr(commandLine, L"-NoPak") == nullptr &&
         wcsstr(commandLine, L"-nopak") == nullptr))
    {
        Logger::Debug(
            "18L-AF: packed-file launch detected; donor loose-file fallback not required");
        return true;
    }

    const uintptr_t base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base)
        return false;

    uint8_t* gate = reinterpret_cast<uint8_t*>(
        base + RVA_LooseFileAlignmentGate);
    const uint8_t* fallback = reinterpret_cast<const uint8_t*>(
        base + 0x5ECFD8);

    // Resurrected's FWindowsReadRequest constructor force-exits on the
    // unaligned texture-mip reads produced by -NoPak loose files. The working
    // community build routes this exact case through a native aligned bounce
    // buffer. Resurrected still contains that same fallback at +0x5ECFD8; only
    // its gate differs. Branch directly to the preserved fallback.
    const uint8_t expected[] =
    {
        0x8B,0x05,0xFB,0x0F,0xB0,0x02
    };
    const uint8_t patched[] =
    {
        0xE9,0x40,0x00,0x00,0x00,0x90
    };
    const uint8_t fallbackExpected[] =
    {
        0x33,0xD2,0xE8,0x01,0x0E,0xF5,0xFF,0x48,
        0x89,0x83,0xB0,0x00,0x00,0x00,0xEB,0x10
    };

    if (!Memory::IsReadable(gate, sizeof(expected)) ||
        !Memory::IsReadable(fallback, sizeof(fallbackExpected)) ||
        std::memcmp(
            fallback,
            fallbackExpected,
            sizeof(fallbackExpected)) != 0)
    {
        Logger::Error(
            "18L-AF: native bounce-buffer fallback signature mismatch; patch refused");
        return false;
    }

    if (std::memcmp(gate, patched, sizeof(patched)) == 0)
        return true;

    if (std::memcmp(gate, expected, sizeof(expected)) != 0)
    {
        Logger::Error(
            "18L-AF: loose-file alignment gate signature mismatch; donor fallback NOT enabled");
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(
            gate,
            sizeof(patched),
            PAGE_EXECUTE_READWRITE,
            &oldProtection))
    {
        Logger::Error(
            "18L-AF: VirtualProtect failed for donor loose-file fallback");
        return false;
    }

    std::memcpy(gate, patched, sizeof(patched));
    FlushInstructionCache(
        GetCurrentProcess(),
        gate,
        sizeof(patched));

    DWORD ignored = 0;
    VirtualProtect(gate, sizeof(patched), oldProtection, &ignored);

    Logger::Success(
        "18L-AF: donor loose-file async-read fallback enabled | unaligned texture mips now use Resurrected's native aligned bounce buffer");
    return true;
#endif
}

static void AppendEmergencyLog(const char* text)
{
    wchar_t tempPath[MAX_PATH]{};
    wchar_t logPath[MAX_PATH]{};

    DWORD pathLength = GetTempPathW(MAX_PATH, tempPath);
    if (pathLength == 0 || pathLength >= MAX_PATH)
        return;

    if (swprintf_s(
            logPath,
            L"%sResurrectedOfflineBots-18L-AC.log",
            tempPath) < 0)
    {
        return;
    }

    HANDLE file = CreateFileW(
        logPath,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(
        file,
        text,
        static_cast<DWORD>(strlen(text)),
        &written,
        nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

static void __fastcall RequestExitTraceHook(bool force)
{
    // Stop the polling worker before UE starts destroying the world. The
    // previous build kept walking GWorld/level actor arrays for several
    // seconds after RequestExit, overlapping object destruction.
    InterlockedExchange(&g_Running, 0);

    // This hook is diagnostic only. The donor-style lifecycle currently gets
    // through ClientPlayIntro, after which Resurrected calls force-exit without
    // producing a crash dump. Record the exact caller and hidden fatal state,
    // then preserve the original behavior unchanged.
    if (InterlockedCompareExchange(&g_RequestExitLogging, 1, 0) == 0)
    {
        const uintptr_t base =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const uintptr_t caller =
            reinterpret_cast<uintptr_t>(_ReturnAddress());

        void* frames[32]{};
        const USHORT frameCount =
            CaptureStackBackTrace(0, 32, frames, nullptr);

        uint8_t critical = 0xFF;
        if (base && Memory::IsReadable(
                reinterpret_cast<void*>(base + RVA_GIsCriticalError),
                sizeof(critical)))
        {
            critical = *reinterpret_cast<volatile uint8_t*>(
                base + RVA_GIsCriticalError);
        }

        char errorUtf8[4096]{};
        const wchar_t* errorHist = base
            ? reinterpret_cast<const wchar_t*>(base + RVA_GErrorHist)
            : nullptr;

        if (errorHist &&
            Memory::IsReadable(errorHist, 1024 * sizeof(wchar_t)))
        {
            int wideLength = 0;
            while (wideLength < 1023 && errorHist[wideLength] != L'\0')
                ++wideLength;

            if (wideLength > 0)
            {
                WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    errorHist,
                    wideLength,
                    errorUtf8,
                    static_cast<int>(sizeof(errorUtf8) - 1),
                    nullptr,
                    nullptr);

                for (char* p = errorUtf8; *p; ++p)
                {
                    if (*p == '\r' || *p == '\n')
                        *p = ' ';
                }
            }
        }

        char line[8192]{};
        int used = sprintf_s(
            line,
            "[REQUEST_EXIT_TRACE] force=%u thread=%lu caller=%p callerRva=0x%llX critical=%u errorHist=\"%s\" frames=",
            force ? 1u : 0u,
            GetCurrentThreadId(),
            reinterpret_cast<void*>(caller),
            static_cast<unsigned long long>(base && caller >= base
                ? caller - base
                : 0),
            static_cast<unsigned>(critical),
            errorUtf8[0] ? errorUtf8 : "<empty>");

        if (used < 0)
            used = 0;

        for (USHORT i = 0;
             i < frameCount && used < static_cast<int>(sizeof(line) - 64);
             ++i)
        {
            const uintptr_t address =
                reinterpret_cast<uintptr_t>(frames[i]);
            const int appended = sprintf_s(
                line + used,
                sizeof(line) - static_cast<size_t>(used),
                "%s%p(rva=0x%llX)",
                i == 0 ? "" : ",",
                frames[i],
                static_cast<unsigned long long>(base && address >= base
                    ? address - base
                    : 0));

            if (appended <= 0)
                break;

            used += appended;
        }

        if (used < static_cast<int>(sizeof(line) - 3))
        {
            line[used++] = '\r';
            line[used++] = '\n';
            line[used] = '\0';
        }

        AppendEmergencyLog(line);
    }

    if (g_OriginalRequestExit)
        g_OriginalRequestExit(force);
}

static std::string DescribeLocalState()
{
    APlayerController* controller =
        Engine::GetLocalPlayerController();

    if (!controller)
        return "controller=<none> | pawn=<none>";

    std::string controllerClass =
        controller->Class
        ? controller->Class->GetName()
        : std::string("<unknown>");

    std::string pawnClass = "<none>";

    if (controller->AcknowledgedPawn &&
        controller->AcknowledgedPawn->Class)
    {
        pawnClass =
            controller->AcknowledgedPawn->Class->GetName();
    }

    return "controller=" + controllerClass +
        " | pawn=" + pawnClass;
}

static void __fastcall OfflineBots818TraceHook(void* gameMode)
{
    const bool counsel =
        OfflineSetup::IsCounselorMenuRouteLatched();
    OfflineSetup::ApplyCommittedMatchDurationBeforeStart(gameMode);
    OfflineSetup::ApplyCommittedOfflineDifficultyBeforeStart(gameMode);

    if (counsel)
    {
        uint32_t n = g_818TraceCount.fetch_add(1) + 1;
        if (n <= 4)
        {
            Logger::Success(
                "18L-AD COUNSEL HandleMatchHasStarted ENTER | " +
                DescribeLocalState());
        }
    }

    if (counsel)
    {
        // Donor OfflineBotsC is a sibling of OfflineBots. Its InProgress
        // override calls the common Offline parent, then creates AI Jason.
        // Never call the stock OfflineBots override on this route because it
        // creates the human-Jason lifecycle we are replacing.
        if (g_OfflineParent818)
            g_OfflineParent818(gameMode);

        OfflineSetup::MarkCounselorMatchInProgress();

        if (!OfflineSetup::SpawnCounselorModeJasonAfterMatch(gameMode))
        {
            Logger::Error(
                "18L-AD COUNSEL HandleMatchHasStarted: native AI Jason creation/adoption failed");
        }
    }
    else if (g_OriginalOfflineBots818)
    {
        g_OriginalOfflineBots818(gameMode);
    }

    OfflineSetup::BeginCommittedOfflineSettingsAfterStart(gameMode);

    if (counsel)
    {
        uint32_t n = g_818TraceCount.load();
        if (n <= 4)
        {
            Logger::Success(
                "18L-AD COUNSEL HandleMatchHasStarted RETURN: Offline parent preserved; donor-style AI Jason handed to frozen behavior | " +
                DescribeLocalState());
        }
    }
}

static void* __fastcall FindSaveGameStockIntroHook(
    void* owner,
    void* requestedClass)
{
    void* selected = g_OriginalFindSaveGame
        ? g_OriginalFindSaveGame(owner, requestedClass)
        : nullptr;
    if (selected || !g_StockJasonIntroLookup)
        return selected;

    // Do not change the global save registry or any other lookup.  The
    // disconnected-login path sometimes leaves only this stock intro's
    // selection-save lookup empty; its own class default is a valid UObject
    // carrying the native default selections.
    return OfflineSetup::StockJasonSelectionSaveFallback(requestedClass);
}

static void __fastcall OfflineBots958TraceHook(void* gameMode)
{
    const bool counsel =
        OfflineSetup::IsCounselorMenuRouteLatched();

    if (counsel)
    {
        uint32_t n = g_958TraceCount.fetch_add(1) + 1;
        if (n <= 4)
        {
            Logger::Success(
                "18L-AD COUNSEL HandlePreMatchIntro ENTER | " +
                DescribeLocalState());
        }
    }

    if (counsel)
    {
        // Donor ordering is counselor population/human RestartPlayer first,
        // followed by the shared Offline parent PreMatchIntro handler.
        const bool born =
            OfflineSetup::BirthSelectedCounselorForPreMatch(gameMode);

        if (!born)
        {
            Logger::Error(
                "18L-AD COUNSEL HandlePreMatchIntro: counselor birth failed; refusing the stock human-Jason override");
        }

        if (g_OfflineParent958)
            g_OfflineParent958(gameMode);
    }
    else if (g_OriginalOfflineBots958)
    {
        g_StockJasonIntroLookup = true;
        g_OriginalOfflineBots958(gameMode);
        g_StockJasonIntroLookup = false;
    }

    if (counsel)
    {
        uint32_t n = g_958TraceCount.load();
        if (n <= 4)
        {
            Logger::Success(
                "18L-AD COUNSEL HandlePreMatchIntro RETURN: local counselor birth + Offline parent path complete | " +
                DescribeLocalState());
        }
    }
}

static bool InstallLifecycleTraceHooks()
{
    if (g_LifecycleTraceHooksInstalled.load())
        return true;

    uintptr_t base =
        (uintptr_t)GetModuleHandleW(nullptr);

    if (!base)
        return false;

    uint8_t* fn818 =
        (uint8_t*)(base + RVA_OfflineBots818);

    uint8_t* fn958 =
        (uint8_t*)(base + RVA_OfflineBots958);

    uint8_t* parent818 =
        (uint8_t*)(base + RVA_OfflineParent818);

    uint8_t* parent958 =
        (uint8_t*)(base + RVA_OfflineParent958);

    const uint8_t sig818[] =
    {
        0x48,0x89,0x4C,0x24,0x08,0x55,0x53,0x56
    };

    const uint8_t sig958[] =
    {
        0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74
    };

    const uint8_t sigParent818[] =
    {
        0x48,0x8B,0xC4,0x48,0x81,0xEC,0x88,0x00
    };

    const uint8_t sigParent958[] =
    {
        0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83
    };

    const uint8_t sigFindSaveGame[] =
    {
        0x48,0x89,0x54,0x24,0x10,0x53,0x48,0x83
    };
    uint8_t* findSaveGame =
        reinterpret_cast<uint8_t*>(base + RVA_FindSaveGame);

    if (!Memory::IsReadable(fn818, sizeof(sig818)) ||
        !Memory::IsReadable(fn958, sizeof(sig958)) ||
        !Memory::IsReadable(parent818, sizeof(sigParent818)) ||
        !Memory::IsReadable(parent958, sizeof(sigParent958)) ||
        !Memory::IsReadable(findSaveGame, sizeof(sigFindSaveGame)) ||
        std::memcmp(fn818, sig818, sizeof(sig818)) != 0 ||
        std::memcmp(fn958, sig958, sizeof(sig958)) != 0 ||
        std::memcmp(parent818, sigParent818, sizeof(sigParent818)) != 0 ||
        std::memcmp(parent958, sigParent958, sizeof(sigParent958)) != 0 ||
        std::memcmp(findSaveGame, sigFindSaveGame,
            sizeof(sigFindSaveGame)) != 0)
    {
        Logger::Error(
            "18L-AD: OfflineBots/Offline-parent lifecycle signatures mismatch; counselor hooks NOT installed");
        return false;
    }

    g_OfflineParent818 =
        reinterpret_cast<OfflineLifecycleFn>(parent818);
    g_OfflineParent958 =
        reinterpret_cast<OfflineLifecycleFn>(parent958);

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK &&
        initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        Logger::Error(
            "18L-AC: MH_Initialize failed for lifecycle trace hooks");
        return false;
    }

    if (MH_CreateHook(
            reinterpret_cast<LPVOID>(fn818),
            reinterpret_cast<LPVOID>(&OfflineBots818TraceHook),
            reinterpret_cast<LPVOID*>(&g_OriginalOfflineBots818)) != MH_OK)
    {
        Logger::Error(
            "18L-AC: failed to create +0x818 pass-through trace hook");
        return false;
    }

    if (MH_CreateHook(
            reinterpret_cast<LPVOID>(fn958),
            reinterpret_cast<LPVOID>(&OfflineBots958TraceHook),
            reinterpret_cast<LPVOID*>(&g_OriginalOfflineBots958)) != MH_OK)
    {
        Logger::Error(
            "18L-AC: failed to create +0x958 pass-through trace hook");
        return false;
    }

    if (MH_CreateHook(
            reinterpret_cast<LPVOID>(findSaveGame),
            reinterpret_cast<LPVOID>(&FindSaveGameStockIntroHook),
            reinterpret_cast<LPVOID*>(&g_OriginalFindSaveGame)) != MH_OK)
    {
        Logger::Error(
            "STOCK JASON INTRO: failed to install selection-save lookup fallback");
        return false;
    }

    if (MH_EnableHook(fn818) != MH_OK ||
        MH_EnableHook(fn958) != MH_OK ||
        MH_EnableHook(findSaveGame) != MH_OK)
    {
        Logger::Error(
            "18L-AC: failed to enable lifecycle trace hooks");
        return false;
    }

    g_LifecycleTraceHooksInstalled.store(true);

    Logger::Success(
        "base-port: counselor lifecycle hooks installed | COUNSEL +0x958=birth before intro | COUNSEL +0x818=parent then AI Jason | stock Jason route untouched");

    return true;
}

static DWORD WINAPI BackendWorker(LPVOID)
{
    if (!IsExactSupportedRuntime())
    {
        Logger::Error(
            "base-port: unsupported executable identity; failing closed before hooks or gameplay work");
        return 0;
    }

    Logger::Success("F13 Base Game Offline Bots backend loaded");
    SYSTEMTIME releaseUtc{}; GetSystemTime(&releaseUtc);
    char releaseUtcText[40]{};
    sprintf_s(releaseUtcText, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        releaseUtc.wYear, releaseUtc.wMonth, releaseUtc.wDay,
        releaseUtc.wHour, releaseUtc.wMinute, releaseUtc.wSecond, releaseUtc.wMilliseconds);
    Logger::Success("RELEASE SESSION: PID=" + std::to_string(GetCurrentProcessId()) +
        " | version=1.0.0-rc1 | utc=" + releaseUtcText + " | BACKEND: LOADED");
    Logger::Debug(
        "18L-AC: CORRECT NATIVE COUNSEL ROUTE foundation | ILLBackendBlueprintLibrary::RequestOfflineMode redirect | OfflineBots lifecycle trace only | no Jason->counselor possession swap | frozen AI auto-start disabled");

    bool engineReady = false;
    bool f1WasDown = false;
    bool f3WasDown = false;
    bool f6WasDown = false;
    bool f7WasDown = false;
    bool f8WasDown = false;
#if defined(F13_ENABLE_OPTIONAL_DEBUG_HOOK)
    ULONGLONG hookMenuReadyAt = 0;
    bool hookLoadAttempted = false;
#else
    Logger::Success("STANDALONE BUILD: optional Epstein overlay auto-load disabled; gameplay uses the integrated backend only");
#endif

    while (InterlockedCompareExchange(&g_Running, 1, 1) != 0)
    {
        if (!engineReady)
        {
            engineReady = Engine::Initialize();
            if (!engineReady)
            {
                Logger::Debug("Engine not ready; retrying");
                Sleep(1000);
                continue;
            }

            Logger::Success("Resurrected engine ready");
            InstallDonorLooseFileReadFallback();
            InstallLifecycleTraceHooks();
            Logger::Success(
                "18L-AF SAFETY: donor loose-file fallback uses Resurrected's intact aligned buffer/copy completion path; lifecycle hooks remain route-scoped");
        }

        LARGE_INTEGER workerTickStarted{};
        QueryPerformanceCounter(&workerTickStarted);

        // Frontend counselor selection + synchronous COUNSEL route coordinator.
        // Deliberately do NOT call frozen AI TickAIOnly in this foundation test.
        // IsInGame also installs the existing game-thread setup-request bridge.
        // Calling it only after the route latch left the selected counselor
        // queued throughout the frontend and native map/settings picker.
        const bool inGame = Engine::IsInGame();
        UpdateHostedStatusWindow();
        // F7 is a bounded diagnostic for a human Jason in ordinary Offline
        // Bots. It is deliberately outside the Counselor-mode hotkey gate.
        const SHORT f7State = GetAsyncKeyState(VK_F7);
        const bool f7Down = (f7State & 0x8000) != 0;
        if (inGame && f7Down && !f7WasDown)
            Engine::QueueStockKnifeTrace();
        f7WasDown = f7Down;
        const bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8Down && !f8WasDown)
        {
            DWORD foregroundPid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
            if (foregroundPid == GetCurrentProcessId())
            {
                OfflineSetup::QueueHostedCounselorArm();
            }
        }
        f8WasDown = f8Down;
        const bool counselorMatchActive =
            OfflineSetup::IsCounselorMatchInProgress();
        // The packed counselor route can keep an empty PersistentLevel actor
        // snapshot after its native match has started. Engine::IsInGame()
        // then reads false even while the adopted Jason is actively ticking.
        // The lifecycle latch is the authoritative hotkey gate for this mode.
        if ((counselorMatchActive && func) || OfflineSetup::IsHostedCounselorArmed())
        {
            // Cache spawn assets only after the counselor match lifecycle has
            // started.  The menu world also satisfies Engine::IsInGame(), so
            // running this during OnlineFix sign-in needlessly scans assets and
            // was the principal stock-port startup regression.
            if (counselorMatchActive)
                func->TickAIOnly();

            const SHORT f1State = GetAsyncKeyState(VK_F1);
            const SHORT f3State = GetAsyncKeyState(VK_F3);
            const SHORT f6State = GetAsyncKeyState(VK_F6);
            const bool f1Down = (f1State & 0x8000) != 0;
            const bool f3Down = (f3State & 0x8000) != 0;
            const bool f6Down = (f6State & 0x8000) != 0;
            const bool f1Pressed = (f1State & 0x0001) != 0;
            const bool f3Pressed = (f3State & 0x0001) != 0;
            const bool f6Pressed = (f6State & 0x0001) != 0;
            // The optional Private Lobby AI module owns F1/F3 while armed.
            // This keeps the definitive Offline Bots backend fully active in
            // its own modes without allowing both backends to consume the
            // same key press in a stock private Hunt match.
            bool privateLobbyOwnsHotkeys = false;
            if (HMODULE privateLobby = GetModuleHandleW(L"ResurrectedPrivateLobbyAI.dll"))
            {
                using IsArmedFn = DWORD(WINAPI*)(LPVOID);
                const auto isArmed = reinterpret_cast<IsArmedFn>(
                    GetProcAddress(privateLobby, "RPLAI_IsArmed"));
                privateLobbyOwnsHotkeys = isArmed && isArmed(nullptr) != 0;
            }
            if (!privateLobbyOwnsHotkeys && (f1Pressed || f1Down) && !f1WasDown)
            {
                QueueF1JasonHotkeyRequest("worker");
            }
            if (!privateLobbyOwnsHotkeys && (f3Pressed || f3Down) && !f3WasDown)
            {
                QueueF3CounselorHotkeyRequest("worker");
            }
            if (counselorMatchActive && (f6Pressed || f6Down) && !f6WasDown)
            {
                FrozenJasonBridge::QueueMaskPickupDiagnostic();
                Logger::Debug("F6 mask pickup diagnostic queued");
            }
            f1WasDown = f1Down;
            f3WasDown = f3Down;
            f6WasDown = f6Down;
        }
        else
        {
            f1WasDown = false;
            f3WasDown = false;
            f6WasDown = false;
        }
        OfflineSetup::TickWorker();

#if defined(F13_ENABLE_OPTIONAL_DEBUG_HOOK)
        // Developer opt-in only; excluded from the standalone release build.
        // The hook's DirectX bootstrap must not run inside the proxy loader's
        // startup DLL list. Wait for the confirmed visible stock main menu,
        // then load this optional local DLL once from our own directory.
        if (!hookLoadAttempted && OfflineSetup::IsStockMainMenuReadyForHook())
        {
            const ULONGLONG now = GetTickCount64();
            if (!hookMenuReadyAt)
                hookMenuReadyAt = now;
            else if (now - hookMenuReadyAt >= 2000)
            {
                hookLoadAttempted = true;
                if (GetModuleHandleW(L"EpsteinHook.dll"))
                {
                    Logger::Debug("Stock Epstein Hook already loaded; delayed load skipped");
                }
                else if (!GetModuleHandleW(L"OnlineFix64.dll"))
                {
                    Logger::Error("Stock Epstein Hook not loaded: OnlineFix64.dll is absent");
                }
                else
                {
                    wchar_t modulePath[32768]{};
                    const DWORD count = GetModuleFileNameW(
                        g_BackendModule, modulePath,
                        static_cast<DWORD>(std::size(modulePath)));
                    if (!count || count >= std::size(modulePath))
                    {
                        Logger::Error("Stock Epstein Hook not loaded: backend path unavailable");
                    }
                    else
                    {
                        std::wstring hookPath(modulePath, count);
                        const size_t separator = hookPath.find_last_of(L"\\/");
                        if (separator == std::wstring::npos)
                        {
                            Logger::Error("Stock Epstein Hook not loaded: backend directory unavailable");
                        }
                        else
                        {
                            hookPath.resize(separator + 1);
                            hookPath += L"EpsteinHook.dll";
                            const DWORD attributes = GetFileAttributesW(hookPath.c_str());
                            if (attributes == INVALID_FILE_ATTRIBUTES ||
                                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                            {
                                Logger::Debug("Stock Epstein Hook absent; optional delayed load skipped");
                            }
                            else if (LoadLibraryW(hookPath.c_str()))
                            {
                                Logger::Success("Stock Epstein Hook loaded after main menu confirmation");
                            }
                            else
                            {
                                Logger::Error("Stock Epstein Hook delayed load failed: Win32 " +
                                    std::to_string(GetLastError()));
                            }
                        }
                    }
                }
            }
        }

#endif
        // This worker is separate from the game/render threads. A ten-second
        // aggregate shows whether its frontend/resource work competes with
        // gameplay without turning the profiler itself into log spam.
        LARGE_INTEGER workerTickFinished{};
        QueryPerformanceCounter(&workerTickFinished);
        static LARGE_INTEGER workerFrequency = [] {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            return frequency;
        }();
        static uint64_t workerTotalUs = 0;
        static uint64_t workerMaximumUs = 0;
        static uint32_t workerSamples = 0;
        static ULONGLONG workerNextReportAt = 0;
        if (workerFrequency.QuadPart > 0)
        {
            const uint64_t elapsedUs = static_cast<uint64_t>(
                (workerTickFinished.QuadPart - workerTickStarted.QuadPart) *
                1000000LL / workerFrequency.QuadPart);
            workerTotalUs += elapsedUs;
            workerMaximumUs = (std::max)(workerMaximumUs, elapsedUs);
            ++workerSamples;
            const ULONGLONG now = GetTickCount64();
            if (!workerNextReportAt)
                workerNextReportAt = now + 10000;
            if (now >= workerNextReportAt)
            {
                Logger::Debug(
                    "18L-PERF worker tick | samples=" +
                    std::to_string(workerSamples) + " | meanUs=" +
                    std::to_string(workerSamples
                        ? workerTotalUs / workerSamples : 0) +
                    " | maxUs=" + std::to_string(workerMaximumUs) +
                    " | counselorMatch=" +
                    (OfflineSetup::IsCounselorMatchInProgress()
                        ? "true" : "false"));
                workerTotalUs = workerMaximumUs = 0;
                workerSamples = 0;
                workerNextReportAt = now + 10000;
            }
        }
        Sleep(50);
    }

    DestroyHostedStatusWindow();
    return 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_QueueJason(LPVOID)
{
    return (func && func->QueueFirstJasonSandbox()) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_QueueCounselor(LPVOID)
{
    return QueueCounselorBotRequest() ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_QueueCounselorFromHotkey(LPVOID)
{
    return QueueF3CounselorHotkeyRequest("window") ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_QueueJasonFromHotkey(LPVOID)
{
    return QueueF1JasonHotkeyRequest("window") ? 1u : 0u;
}

// Private-lobby integration seam.  The private module owns replication and
// role handoff; this backend contributes only the already-proven frozen Jason
// behavior driver and its route-scoped safety hooks.
extern "C" __declspec(dllexport) DWORD WINAPI ROB_AdoptPrivateLobbyJason(
    LPVOID jason,
    LPVOID killerController,
    LPVOID counselor)
{
    return FrozenJasonBridge::AdoptCounselorModeJason(
        reinterpret_cast<AActor*>(jason),
        reinterpret_cast<UObject*>(killerController),
        reinterpret_cast<AActor*>(counselor)) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_ResetPrivateLobbyJason(
    LPVOID)
{
    FrozenJasonBridge::ResetCounselorModeJason();
    return 1u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_Ping(LPVOID)
{
    return 0x524F424Fu; // "ROBO"
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_SetMapIndex(LPVOID value)
{
    return OfflineSetup::SetMapIndex((int32_t)(uintptr_t)value) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_SetJasonIndex(LPVOID value)
{
    return OfflineSetup::SetJasonIndex((int32_t)(uintptr_t)value) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_SetPlayerCounselorIndex(LPVOID value)
{
    return OfflineSetup::SetPlayerCounselorIndex((int32_t)(uintptr_t)value) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_SetDifficulty(LPVOID value)
{
    return OfflineSetup::SetDifficulty((int32_t)(uintptr_t)value) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_SetCounselorCount(LPVOID value)
{
    return OfflineSetup::SetCounselorCount((int32_t)(uintptr_t)value) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_SetWeather(LPVOID value)
{
    return OfflineSetup::SetWeather((int32_t)(uintptr_t)value) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_ArmSelectedSetup(LPVOID)
{
    return OfflineSetup::QueueArmSelectedSetup() ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_QueueSelectedJason(LPVOID)
{
    return OfflineSetup::QueueSelectedJason() ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_QueueSelectedCounselor(LPVOID)
{
    return OfflineSetup::QueueSelectedCounselor() ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_ApplyGameSetupPreset(LPVOID)
{
    return OfflineSetup::QueueApplyGameSetupPreset() ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_DumpCounselorRoster(LPVOID)
{
    return OfflineSetup::QueueDumpCounselorRoster() ? 1u : 0u;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_BackendModule = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, BackendWorker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        InterlockedExchange(&g_Running, 0);
    }

    return TRUE;
}
