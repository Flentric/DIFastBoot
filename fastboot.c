// DIFastBoot: faster Dead Island boot and loading screens.
//  - SkipIntro: sets the game's own -nologos flag (no logos, disclaimer or "press any key").
//  - Loading screens are drawn by the engine's background render thread (CGame::BackgroundRenderProc), which holds
//    the renderer lock through Present. With VSync on, every loading frame blocks the loader for up to a refresh.
//    LoadingScreenNoVSync presents those frames with D3DPRESENT_DONOTWAIT; LoadingScreenFPS replaces the thread's
//    fixed Sleep (>= 20 ms, 30 at boot) with a frame cap.
//  - TimingLog writes DIFastBoot.log: every engine/game log line with a timestamp, per-loading-screen stats, and a
//    sampling profile of the main thread for each phase.
#define _CRT_SECURE_NO_WARNINGS
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <tlhelp32.h>

static int cfgSkipIntro, cfgBootLockFix, cfgQuietLog, cfgSkipFacebook, cfgShaderCache, cfgTexCache, cfgCachedReads, cfgFastCrashLog, cfgFps, cfgNoVsync, cfgLog, cfgVerify;
static FILE *logf;
static CRITICAL_SECTION logCs;
static LARGE_INTEGER qpf, qpcBase;

static volatile BYTE *bgFlag;            // CGame BackgroundRender
static void **gRenderer;                 // &g_pRenderer
static volatile BYTE inBgPresent;
static volatile DWORD bgThreadId;
static volatile LONG bgFrames, bgDropped;
static volatile LONG64 bgPresentUs, loaderWaitUs;
static double nextFrame;

static double NowMs(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - qpcBase.QuadPart) * 1000.0 / (double)qpf.QuadPart;
}

static void Log(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    int n;
    if (!logf) return;
    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    if (n < 0) n = (int)strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    EnterCriticalSection(&logCs);
    fprintf(logf, "[%9.3f] %s\n", NowMs() / 1000.0, buf);
    LeaveCriticalSection(&logCs);
}

// ---- pattern scanning ----
static BYTE *Scan(HMODULE mod, const char *sig, int *count)
{
    BYTE pat[128], mask[128], *base = (BYTE *)mod, *hit = NULL;
    int len = 0;
    const char *s = sig;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    WORD i;
    *count = 0;
    while (*s) {
        if (*s == ' ') { s++; continue; }
        if (*s == '?') { pat[len] = 0; mask[len++] = 0; s += 2; continue; }
        pat[len] = (BYTE)strtoul(s, NULL, 16); mask[len++] = 1; s += 2;
    }
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        BYTE *p, *end;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        p = base + sec->VirtualAddress;
        end = p + sec->Misc.VirtualSize - len;
        for (; p <= end; p++) {
            int k;
            for (k = 0; k < len && (!mask[k] || p[k] == pat[k]); k++);
            if (k == len) { if (!hit) hit = p; (*count)++; }
        }
    }
    return hit;
}

static BYTE *FindBytes(HMODULE mod, const void *data, size_t len)
{
    BYTE *base = (BYTE *)mod, *p, *end;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    end = base + nt->OptionalHeader.SizeOfImage - len;
    for (p = base + nt->OptionalHeader.SizeOfHeaders; p <= end; p++)
        if (!memcmp(p, data, len)) return p;
    return NULL;
}

static void WriteCall(BYTE *at, void *target, int total)
{
    DWORD old;
    int i;
    VirtualProtect(at, total, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE8;
    *(DWORD *)(at + 1) = (DWORD)target - (DWORD)(at + 5);
    for (i = 5; i < total; i++) at[i] = 0x90;
    VirtualProtect(at, total, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, total);
}

static DWORD Rva(void *p, const char *mod)
{
    return (DWORD)p - (DWORD)GetModuleHandleA(mod);
}

// ---- loading-screen thread ----
typedef void(__fastcall *AcquireFn)(void *self, void *edx, const char *name);
static AcquireFn origAcquire;

static void __fastcall HookAcquire(void *self, void *edx, const char *name)
{
    if (bgFlag && *bgFlag && GetCurrentThreadId() != bgThreadId) {
        double t = NowMs();
        origAcquire(self, edx, name);
        InterlockedExchangeAdd64(&loaderWaitUs, (LONG64)((NowMs() - t) * 1000.0));
        return;
    }
    origAcquire(self, edx, name);
}

static void InstallAcquireHook(void)
{
    // CRenderer vtable slot 42 = AcquireOwnership (push esi; mov esi,ecx; lea eax,[esi+..])
    DWORD **vt, old;
    BYTE *fn;
    if (origAcquire || !gRenderer || !*gRenderer) return;
    vt = *(DWORD ***)*gRenderer;
    fn = (BYTE *)vt[42];
    if (memcmp(fn, "\x56\x8B\xF1\x8D\x86", 5)) { Log(">> renderer lock slot not recognised, no lock-wait stats"); origAcquire = (AcquireFn)1; return; }
    origAcquire = (AcquireFn)fn;
    VirtualProtect(&vt[42], 4, PAGE_READWRITE, &old);
    vt[42] = (DWORD *)HookAcquire;
    VirtualProtect(&vt[42], 4, old, &old);
}

// replaces the per-frame Sleep(BackgroundRenderSleep) in the background render loop
static void WINAPI BgSleep(DWORD ms)
{
    bgThreadId = GetCurrentThreadId();
    InterlockedIncrement(&bgFrames);
    if (cfgLog && !origAcquire) InstallAcquireHook();
    if (cfgFps > 0) {
        double t = NowMs(), period = 1000.0 / cfgFps, wait;
        if (nextFrame == 0 || t - nextFrame > 250) nextFrame = t;
        nextFrame += period;
        wait = nextFrame - t;
        Sleep(wait >= 1.0 ? (DWORD)wait : 1);   // always yield: the loader needs the lock
    } else {
        Sleep(ms);
    }
}

// replaces Sleep(200) in CRenderer::BeginFrame's "device not ready" exit, which runs with the renderer lock held. Until
// the fullscreen switch at the menu every boot loading-screen frame lands there, locking the loader out ~95% of the time.
static void WINAPI NotReadySleep(DWORD ms)
{
    if (GetCurrentThreadId() == bgThreadId) return;   // the loading-screen thread sleeps in BgSleep, lock released
    Sleep(ms);
}

// replaces "mov eax,[edx+28h]; push 0 x3; call eax" (g_pRenderer->Present(0,0,0)) in the background render loop
__declspec(naked) static void BgPresentThunk(void)
{
    __asm {
        mov inBgPresent, 1
        push 0
        push 0
        push 0
        call dword ptr [edx + 28h]
        mov inBgPresent, 0
        ret
    }
}

// replaces "mov ecx,[edx+44h]; call ecx" (IDirect3DDevice9::Present) in CRenderer::Present
static HRESULT WINAPI DevicePresent(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
{
    HRESULT hr;
    double t;
    if (!inBgPresent) return IDirect3DDevice9_Present(dev, src, dst, wnd, dirty);
    t = NowMs();
    hr = D3DERR_INVALIDCALL;
    if (cfgNoVsync) {
        IDirect3DSwapChain9 *sc = NULL;
        if (SUCCEEDED(IDirect3DDevice9_GetSwapChain(dev, 0, &sc))) {
            hr = IDirect3DSwapChain9_Present(sc, src, dst, wnd, dirty, D3DPRESENT_DONOTWAIT);
            IDirect3DSwapChain9_Release(sc);
            if (hr == D3DERR_WASSTILLDRAWING) { InterlockedIncrement(&bgDropped); hr = D3D_OK; }
        }
    }
    if (hr == D3DERR_INVALIDCALL) hr = IDirect3DDevice9_Present(dev, src, dst, wnd, dirty);
    InterlockedExchangeAdd64(&bgPresentUs, (LONG64)((NowMs() - t) * 1000.0));
    return hr;
}

#include "shadercache.h"
#include "texcache.h"
#include "cachedreads.h"

// ITools::FacebookInitialize (ITools vtable slot 1) builds CFacebookPc and runs its Create: ~5 s of heap churn and
// waiting on every boot for a service that no longer exists. Without it g_pIFacebook stays null; Destroy, SendPost and
// the lobby's Facebook task null-check it. FacebookAuthorizeAccount (slot 4) does not, so it returns an empty string.
static void PatchFacebook(HMODULE eng)
{
    static const BYTE initRet[] = {0xC2, 0x0C, 0x00};                          // ret 0Ch
    static const BYTE authEmpty[] = {0x8B, 0x44, 0x24, 0x04, 0x33, 0xC9,        // mov eax,[esp+4]; xor ecx,ecx
                                     0x89, 0x08, 0x89, 0x48, 0x04, 0x89, 0x48, 0x08, // zero the result string
                                     0xC2, 0x0C, 0x00};                         // ret 0Ch
    int n1, n2;
    DWORD old;
    BYTE *init = Scan(eng, "56 6A 30 FF 15 ?? ?? ?? ?? 8B F0 83 C4 04 85 F6 74 ?? 8B 44 24 10 8B 4C 24 0C", &n1);
    BYTE *auth = Scan(eng, "A1 ?? ?? ?? ?? 8B 4C 24 08 56 8B 74 24 08 56 50 8B 44 24 18 E8 ?? ?? ?? ?? 8B C6 5E C2 0C 00", &n2);
    if (n1 != 1 || n2 != 1) { Log(">> SkipFacebook: %d/%d matches, not patched", n1, n2); return; }
    VirtualProtect(auth, sizeof(authEmpty), PAGE_EXECUTE_READWRITE, &old);
    memcpy(auth, authEmpty, sizeof(authEmpty));
    VirtualProtect(auth, sizeof(authEmpty), old, &old);
    VirtualProtect(init, sizeof(initRet), PAGE_EXECUTE_READWRITE, &old);
    memcpy(init, initRet, sizeof(initRet));
    VirtualProtect(init, sizeof(initRet), old, &old);
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    Log(">> SkipFacebook: FacebookInitialize off (engine+%X), AuthorizeAccount stubbed (engine+%X)",
        Rva(init, "engine_x86_rwdi.dll"), Rva(auth, "engine_x86_rwdi.dll"));
}

static void PatchEngine(void)
{
    HMODULE eng = GetModuleHandleA("engine_x86_rwdi.dll");
    BYTE *p;
    int n;
    if (!eng) { Log(">> engine_x86_rwdi.dll not loaded, loading-screen patches skipped"); return; }

    p = Scan(eng, "6A 00 E8 ?? ?? ?? ?? 8D 44 24 10 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 50 FF 15 ?? ?? ?? ?? A0 ?? ?? ?? ?? 84 C0 0F 85", &n);
    if (n != 1) { Log(">> loading-screen sleep: %d matches, not patched", n); return; }
    bgFlag = *(BYTE **)(p + 29);
    if (cfgFps > 0 || cfgLog || cfgBootLockFix) {
        WriteCall(p + 22, BgSleep, 6);
        Log(">> loading-screen sleep patched at engine+%X", Rva(p + 22, "engine_x86_rwdi.dll"));
    }

    if (cfgBootLockFix) {
        BYTE *q = Scan(eng, "F7 87 7C 01 00 00 00 08 00 00 74 24 68 C8 00 00 00 FF 15 ?? ?? ?? ?? 84 DB", &n);
        if (n == 1) {
            WriteCall(q + 17, NotReadySleep, 6);
            Log(">> BootLockFix: BeginFrame not-ready sleep patched at engine+%X", Rva(q + 17, "engine_x86_rwdi.dll"));
        } else Log(">> BootLockFix: %d matches, not patched", n);
    }

    if (cfgSkipFacebook) PatchFacebook(eng);

    p = Scan(eng, "8B 0D ?? ?? ?? ?? 8B 11 8B 42 28 6A 00 6A 00 6A 00 FF D0 8B 35", &n);
    if (n != 1) { Log(">> loading-screen present: %d matches, not patched", n); return; }
    gRenderer = *(void ***)(p + 2);

    if (cfgNoVsync || cfgLog) {
        BYTE *d = Scan(eng, "8B 0D ?? ?? ?? ?? 8B 11 53 57 50 8B 44 24 48 50 51 8B 4A 44 FF D1", &n);
        if (n != 1) { Log(">> device present: %d matches, not patched", n); return; }
        WriteCall(d + 17, DevicePresent, 5);
        WriteCall(p + 8, BgPresentThunk, 11);
        Log(">> loading-screen present patched at engine+%X / +%X", Rva(p + 8, "engine_x86_rwdi.dll"), Rva(d + 17, "engine_x86_rwdi.dll"));
    }
}

// ---- -nologos ----
static BOOL PatchGame(HMODULE game)
{
    static const char opt[] = "-drawmovieframe";   // the option parsed right after -nologos sets its flag
    BYTE *str = FindBytes(game, opt, sizeof(opt)), *ref;
    BYTE movecx[5];
    if (!str) { Log(">> SkipIntro: option string not found"); return FALSE; }
    movecx[0] = 0xB9;
    *(DWORD *)(movecx + 1) = (DWORD)str;
    ref = FindBytes(game, movecx, 5);
    if (!ref || ref[-7] != 0xC6 || ref[-6] != 0x05 || ref[-1] != 0x01) { Log(">> SkipIntro: -nologos flag not found"); return FALSE; }
    **(BYTE **)(ref - 5) = 1;
    Log(">> SkipIntro: -nologos flag set (game+%X)", Rva(*(BYTE **)(ref - 5), "game_x86_rwdi.dll"));
    return TRUE;
}

// ---- sampling profiler (TimingLog): EIP + return addresses on the stack of the main and loading-screen threads ----
#define PROF_SLOTS 65536
static HANDLE mainThread;
static DWORD watcherId;
static CRITICAL_SECTION profCs;
// 0 main stack, 1 loading thread stack, 2 main leaf, 3 main nearest engine/game/fs caller, 4 main leaf module
// 5 busiest other thread stack, 6 its leaf, 7 its nearest own caller, 8 its leaf module
#define PROF_TABLES 9
static DWORD profAddr[PROF_TABLES][PROF_SLOTS], profCount[PROF_TABLES][PROF_SLOTS];
static DWORD profSamples[PROF_TABLES];
static DWORD stackCopy[16384];
static struct { BYTE *lo, *hi; const char *name; } profMods[3];

static void ProfAdd(int w, DWORD a)
{
    DWORD h = (a * 2654435761u) >> 16;
    while (profAddr[w][h] && profAddr[w][h] != a) h = (h + 1) & (PROF_SLOTS - 1);
    if (!profAddr[w][h]) profAddr[w][h] = a;
    profCount[w][h]++;
}

static int ProfMod(DWORD a)
{
    int i;
    for (i = 0; i < 3; i++)
        if (profMods[i].lo && (BYTE *)a >= profMods[i].lo && (BYTE *)a < profMods[i].hi) return i;
    return -1;
}

static BOOL AfterCall(BYTE *r)
{
    return r[-5] == 0xE8 || (r[-6] == 0xFF && (r[-5] & 0x38) == 0x10) || (r[-2] == 0xFF && (r[-1] & 0x38) == 0x10) ||
           (r[-3] == 0xFF && (r[-2] & 0x38) == 0x10);
}

static void ProfSetModule(int i, const char *name)
{
    HMODULE m = GetModuleHandleA(name);
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    if (!m || profMods[i].lo) return;
    nt = (IMAGE_NT_HEADERS *)((BYTE *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    profMods[i].name = name;
    profMods[i].lo = (BYTE *)m + sec->VirtualAddress;   // .text is the first section in all three
    profMods[i].hi = profMods[i].lo + sec->Misc.VirtualSize;
}

static void ProfSample(int w, HANDLE th)
{
    CONTEXT c;
    MEMORY_BASIC_INFORMATION mbi;
    DWORD n, i, seen[64], nseen = 0;
    c.ContextFlags = CONTEXT_CONTROL;
    if (SuspendThread(th) == (DWORD)-1) return;
    if (!GetThreadContext(th, &c)) { ResumeThread(th); return; }
    // copy the stack while suspended; no CRT/heap use here
    n = 0;
    if (VirtualQuery((void *)c.Esp, &mbi, sizeof(mbi))) {
        DWORD top = (DWORD)mbi.BaseAddress + mbi.RegionSize;
        n = (top - c.Esp) / 4;
        if (n > 16384) n = 16384;
        memcpy(stackCopy, (void *)c.Esp, n * 4);
    }
    ResumeThread(th);
    EnterCriticalSection(&profCs);
    profSamples[w]++;
    if (w == 0 || w == 5) {
        int x = w == 0 ? 2 : 6;
        HMODULE hm = NULL;
        DWORD own = ProfMod(c.Eip) >= 0 ? c.Eip : 0;
        for (i = 0; !own && i < n; i++)
            if (ProfMod(stackCopy[i]) >= 0 && AfterCall((BYTE *)stackCopy[i])) own = stackCopy[i];
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)c.Eip, &hm);
        profSamples[x]++; ProfAdd(x, c.Eip);
        if (own) { profSamples[x + 1]++; ProfAdd(x + 1, own); }
        profSamples[x + 2]++; ProfAdd(x + 2, hm ? (DWORD)hm : 1);
    }
    for (i = 0; i < n; i++) {
        DWORD a = stackCopy[i], k;
        if (ProfMod(a) < 0 || !AfterCall((BYTE *)a)) continue;
        for (k = 0; k < nseen && seen[k] != a; k++);
        if (k < nseen) continue;
        if (nseen < 64) seen[nseen++] = a;
        ProfAdd(w, a);
    }
    LeaveCriticalSection(&profCs);
}

static void ProfDump(const char *phase)
{
    static const char *who[PROF_TABLES] = {"main", "loading-screen thread", "main leaf EIP", "main nearest own caller", "main leaf module", "busiest other thread", "busy leaf EIP", "busy nearest own caller", "busy leaf module"};
    int shown, i, w;
    if (!mainThread) return;
    EnterCriticalSection(&profCs);
    for (w = 0; w < PROF_TABLES; w++) {
        if (!profSamples[w]) continue;
        Log(">> profile [%s, %s]: %lu samples; top sites (%% of samples)", phase, who[w], profSamples[w]);
        for (shown = 0; shown < (w == 0 || w == 5 ? 60 : w == 4 || w == 8 ? 12 : 25); shown++) {
            DWORD best = 0, bi = 0, a;
            int m;
            char where[64];
            for (i = 0; i < PROF_SLOTS; i++) if (profCount[w][i] > best) { best = profCount[w][i]; bi = i; }
            if (!best) break;
            a = profAddr[w][bi];
            m = ProfMod(a);
            if (w == 4 || w == 8) {
                char mn[MAX_PATH] = "?";
                if (a > 1) GetModuleFileNameA((HMODULE)a, mn, MAX_PATH);
                Log(">>   %5.1f%% %s", 100.0 * best / profSamples[w], strrchr(mn, '\\') ? strrchr(mn, '\\') + 1 : mn);
                profCount[w][bi] = 0;
                continue;
            }
            if (m >= 0) sprintf(where, "%s+%X", profMods[m].name, a - (DWORD)GetModuleHandleA(profMods[m].name));
            else {
                HMODULE h = NULL;
                char mn[MAX_PATH] = "?";
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &h);
                if (h) { GetModuleFileNameA(h, mn, MAX_PATH); }
                sprintf(where, "%s+%X", strrchr(mn, '\\') ? strrchr(mn, '\\') + 1 : mn, h ? a - (DWORD)h : a);
            }
            Log(">>   %5.1f%% %s", 100.0 * best / profSamples[w], where);
            profCount[w][bi] = 0;
        }
        memset(profAddr[w], 0, sizeof(profAddr[w]));
        memset(profCount[w], 0, sizeof(profCount[w]));
        profSamples[w] = 0;
    }
    LeaveCriticalSection(&profCs);
}

// TimingLog: each thread's CPU use over the last second, top 4, with its entry point
typedef LONG(WINAPI *NtQITFn)(HANDLE, int, void *, ULONG, ULONG *);
static void LogThreadCpu(void)
{
    static struct { DWORD id; ULONGLONG cpu; } last[512];
    static int nlast;
    static double lastT;
    struct { DWORD id; ULONGLONG d; DWORD start; } top[4] = {0};
    static NtQITFn qit;
    THREADENTRY32 te;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    DWORD pid = GetCurrentProcessId();
    double t = NowMs(), dt = t - lastT;
    char line[512];
    int i, j, len;
    if (!qit) qit = (NtQITFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        HANDLE h;
        FILETIME a, b, k, u;
        ULONGLONG cpu, prev = 0;
        DWORD start = 0;
        if (te.th32OwnerProcessID != pid) continue;
        h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        GetThreadTimes(h, &a, &b, &k, &u);
        if (qit) qit(h, 9, &start, 4, NULL);   // ThreadQuerySetWin32StartAddress
        CloseHandle(h);
        cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
        for (i = 0; i < nlast && last[i].id != te.th32ThreadID; i++);
        if (i < nlast) prev = last[i].cpu;
        else if (nlast < 512) { last[nlast].id = te.th32ThreadID; i = nlast++; }
        else continue;
        last[i].cpu = cpu;
        if (!prev) continue;
        for (j = 0; j < 4 && top[j].d >= cpu - prev; j++);
        if (j < 4) {
            memmove(&top[j + 1], &top[j], (3 - j) * sizeof(top[0]));
            top[j].id = te.th32ThreadID; top[j].d = cpu - prev; top[j].start = start;
        }
    }
    CloseHandle(snap);
    lastT = t;
    if (dt <= 0 || dt > 5000) return;
    len = sprintf(line, ">> threads:");
    for (j = 0; j < 4 && top[j].d; j++) {
        HMODULE hm = NULL;
        char mn[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)top[j].start, &hm);
        if (hm) GetModuleFileNameA(hm, mn, MAX_PATH);
        len += sprintf(line + len, "  %lu%s %.0f%% [%s+%lX]", top[j].id, top[j].id == GetThreadId(mainThread) ? "(main)" : "",
                       top[j].d / (dt * 100.0), strrchr(mn, '\\') ? strrchr(mn, '\\') + 1 : mn, hm ? top[j].start - (DWORD)hm : top[j].start);
    }
    Log("%s", line);
}

// every 250 ms: the thread (other than main, loading-screen and ours) that used the most CPU since the last check
static DWORD PickBusyThread(DWORD skip1, DWORD skip2)
{
    static struct { DWORD id; ULONGLONG cpu; } last[512];
    static int nlast;
    THREADENTRY32 te;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    DWORD best = 0, pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    ULONGLONG bestD = 0;
    int i;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        HANDLE h;
        FILETIME a, b, k, u;
        ULONGLONG cpu, prev = 0;
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || te.th32ThreadID == skip1 || te.th32ThreadID == skip2 ||
            te.th32ThreadID == watcherId) continue;
        h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        GetThreadTimes(h, &a, &b, &k, &u);
        CloseHandle(h);
        cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
        for (i = 0; i < nlast && last[i].id != te.th32ThreadID; i++);
        if (i < nlast) prev = last[i].cpu;
        else if (nlast < 512) { last[nlast].id = te.th32ThreadID; i = nlast++; }
        else continue;
        if (prev && cpu - prev > bestD) { bestD = cpu - prev; best = te.th32ThreadID; }
        last[i].cpu = cpu;
    }
    CloseHandle(snap);
    return bestD > 250000 ? best : 0;   // > 25 ms of CPU in the last 250 ms
}

static DWORD WINAPI Sampler(LPVOID unused)
{
    HANDLE bgThread = NULL, busyThread = NULL;
    DWORD bgId = 0, busyId = 0;
    double lastPick = 0;
    for (;;) {
        if (NowMs() - lastPick >= 250) {
            DWORD id = PickBusyThread(GetThreadId(mainThread), bgThreadId);
            lastPick = NowMs();
            if (id != busyId) {
                if (busyThread) CloseHandle(busyThread);
                busyThread = id ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id) : NULL;
                busyId = id;
            }
        }
        if (busyThread) ProfSample(5, busyThread);
        ProfSetModule(0, "engine_x86_rwdi.dll");
        ProfSetModule(1, "game_x86_rwdi.dll");
        ProfSetModule(2, "filesystem_x86_rwdi.dll");
        ProfSample(0, mainThread);
        if (bgFlag && *bgFlag && bgThreadId) {
            if (bgThreadId != bgId) {
                if (bgThread) CloseHandle(bgThread);
                bgId = bgThreadId;
                bgThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, bgId);
            }
            if (bgThread) ProfSample(1, bgThread);
        }
        Sleep(2);
        if (NowMs() > 180000) return 0;   // boot + first load only; sampling stalls the threads briefly
    }
}

// ---- log timeline: engine/game _CLog through the import tables ----
typedef void(__cdecl *CLogFn)(unsigned, unsigned, const char *, ...);
typedef void(__cdecl *CLogStrFn)(unsigned, unsigned, const void *);
typedef unsigned(__cdecl *CLFilterFn)(unsigned);
static CLogFn origCLog;
static CLogStrFn origCLogStr;
static CLFilterFn origCLFilter;
#define TAG_WARN 0x5741524Eu   // 'WARN'

// QuietLog: callers ask _CLFilter before formatting a line; saying no to WARN skips the format and the file write
static unsigned __cdecl HookCLFilter(unsigned type)
{
    return type == TAG_WARN ? 0 : origCLFilter(type);
}

static const char *TypeTag(unsigned t, char *tag)
{
    int i;
    for (i = 0; i < 4; i++) { char c = (char)(t >> (24 - 8 * i)); tag[i] = c ? c : ' '; }
    tag[4] = 0;
    return tag;
}

static void __cdecl HookCLog(unsigned type, unsigned filter, const char *fmt, ...)
{
    char buf[4096], tag[5];
    va_list ap;
    if (cfgQuietLog && type == TAG_WARN) return;
    if (!cfgLog) {
        va_start(ap, fmt);
        _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
        va_end(ap);
        buf[sizeof(buf) - 1] = 0;
        origCLog(type, filter, "%s", buf);
        return;
    }
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    Log("%s: %s", TypeTag(type, tag), buf);
    origCLog(type, filter, "%s", buf);
}

static void __cdecl HookCLogStr(unsigned type, unsigned filter, const void *str)
{
    char tag[5];
    const char *s = str ? *(const char *const *)str : NULL;
    if (cfgQuietLog && type == TAG_WARN) return;
    if (s && cfgLog) Log("%s: %s", TypeTag(type, tag), s);
    origCLogStr(type, filter, str);
}

static void HookImports(HMODULE mod)
{
    HMODULE fs = GetModuleHandleA("filesystem_x86_rwdi.dll");
    FARPROC clog = GetProcAddress(fs, "?_CLog@@YAXIIPBDZZ");
    FARPROC clogStr = GetProcAddress(fs, "?_CLog@@YAXIIABV?$string_base@D@ttl@@@Z");
    FARPROC clFilter = GetProcAddress(fs, "?_CLFilter@@YAII@Z");
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    if (!mod || !clog || !clogStr || !clFilter || !(cfgLog || cfgQuietLog)) return;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    origCLog = (CLogFn)clog;
    origCLogStr = (CLogStrFn)clogStr;
    origCLFilter = (CLFilterFn)clFilter;
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; t->u1.Function; t++) {
            void *to = NULL;
            DWORD old;
            if (t->u1.Function == (DWORD)clog) to = HookCLog;
            else if (t->u1.Function == (DWORD)clogStr) to = HookCLogStr;
            else if (t->u1.Function == (DWORD)clFilter && cfgQuietLog) to = HookCLFilter;
            if (!to) continue;
            VirtualProtect(&t->u1.Function, 4, PAGE_READWRITE, &old);
            t->u1.Function = (DWORD)to;
            VirtualProtect(&t->u1.Function, 4, old, &old);
        }
    }
}

// ---- watcher: game DLL arrival, loading-screen sessions ----
static DWORD WINAPI Watcher(LPVOID unused)
{
    HMODULE game = NULL;
    BOOL wasBg = FALSE;
    double bgStart = 0, lastFlush = 0, lastRate = 0, lastSlice = 0;
    ULONGLONG lastMainCpu = 0, lastProcCpu = 0, lastRead = 0;
    int session = 0;
    for (;;) {
        double t;
        if (!game && (game = GetModuleHandleA("game_x86_rwdi.dll")) != NULL) {
            Log(">> game_x86_rwdi.dll loaded");
            if (cfgSkipIntro) PatchGame(game);
            HookImports(game);
        }
        t = NowMs();
        if (bgFlag) {
            BOOL bg = *bgFlag != 0;
            if (bg && !wasBg) {
                bgStart = t;
                InterlockedExchange(&bgFrames, 0);
                InterlockedExchange(&bgDropped, 0);
                InterlockedExchange64(&bgPresentUs, 0);
                InterlockedExchange64(&loaderWaitUs, 0);
                Log(">> loading screen %d start", ++session);
            } else if (!bg && wasBg) {
                bgThreadId = 0;
                double dur = (t - bgStart) / 1000.0;
                ShaderCacheStats("so far");
                TextureCacheStats();
                Log(">> loading screen %d end: %.2f s, %ld frames (%.1f fps), %ld dropped, present %.2f s, loader lock wait %.2f s",
                    session, dur, bgFrames, dur > 0 ? bgFrames / dur : 0.0, bgDropped,
                    bgPresentUs / 1e6, loaderWaitUs / 1e6);
            }
            wasBg = bg;
        }
        if (mainThread && t - lastSlice >= 2000 && t < 60000) {
            char ph[48];
            sprintf(ph, "%.0f-%.0f s", lastSlice / 1000.0, t / 1000.0);
            ProfDump(ph);
            lastSlice = t;
        }
        if (logf && mainThread && t - lastRate >= 1000 && t < 180000) {
            // once a second: main-thread CPU use, all-threads CPU use, bytes read by the process
            IO_COUNTERS io;
            FILETIME a, b, k, u, pk, pu;
            ULONGLONG mainCpu, procCpu, rd;
            GetThreadTimes(mainThread, &a, &b, &k, &u);
            GetProcessTimes(GetCurrentProcess(), &a, &b, &pk, &pu);
            GetProcessIoCounters(GetCurrentProcess(), &io);
            mainCpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
            procCpu = (((ULONGLONG)pk.dwHighDateTime << 32) | pk.dwLowDateTime) + (((ULONGLONG)pu.dwHighDateTime << 32) | pu.dwLowDateTime);
            rd = io.ReadTransferCount;
            if (lastRate > 0) {
                double dt = (t - lastRate) * 10000.0;   // 100 ns units
                Log(">> rate: main thread %3.0f%% cpu, process %4.0f%% cpu (100%% = 1 core), read %6.1f MB/s",
                    100.0 * (mainCpu - lastMainCpu) / dt, 100.0 * (procCpu - lastProcCpu) / dt,
                    (rd - lastRead) / 1048576.0 / ((t - lastRate) / 1000.0));
            }
            lastMainCpu = mainCpu; lastProcCpu = procCpu; lastRead = rd; lastRate = t;
            LogThreadCpu();
        }
        if (logf && t - lastFlush > 500) {
            EnterCriticalSection(&logCs);
            fflush(logf);
            LeaveCriticalSection(&logCs);
            lastFlush = t;
        }
        Sleep(game ? 5 : 1);
    }
}

static void Init(HMODULE self)
{
    char ini[MAX_PATH], path[MAX_PATH], *slash;
    FILETIME created, x, now;
    ULONGLONG age;
    LARGE_INTEGER q;

    // timestamps count from process creation
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&q);
    GetProcessTimes(GetCurrentProcess(), &created, &x, &x, &x);
    GetSystemTimeAsFileTime(&now);
    age = (((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) - (((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime);
    qpcBase.QuadPart = q.QuadPart - (LONGLONG)(age * (ULONGLONG)qpf.QuadPart / 10000000ULL);

    GetModuleFileNameA(self, path, MAX_PATH);
    strcpy(ini, path);
    slash = strrchr(ini, '.');
    if (slash) strcpy(slash, ".ini");
    cfgSkipIntro = GetPrivateProfileIntA("MAIN", "SkipIntro", 1, ini);
    cfgCachedReads = GetPrivateProfileIntA("MAIN", "CachedReads", 0, ini);
    cfgFastCrashLog = GetPrivateProfileIntA("MAIN", "FastCrashLog", 1, ini);
    cfgTexCache = GetPrivateProfileIntA("MAIN", "TextureCache", 1, ini);
    cfgShaderCache = GetPrivateProfileIntA("MAIN", "ShaderCache", 1, ini);
    cfgSkipFacebook = GetPrivateProfileIntA("MAIN", "SkipFacebook", 1, ini);
    cfgQuietLog = GetPrivateProfileIntA("MAIN", "QuietLog", 1, ini);
    cfgBootLockFix = GetPrivateProfileIntA("MAIN", "BootLockFix", 1, ini);
    cfgFps = GetPrivateProfileIntA("MAIN", "LoadingScreenFPS", 60, ini);
    cfgNoVsync = GetPrivateProfileIntA("MAIN", "LoadingScreenNoVSync", 1, ini);
    cfgLog = GetPrivateProfileIntA("DEBUG", "TimingLog", 0, ini);
    cfgVerify = GetPrivateProfileIntA("DEBUG", "VerifyCaches", 0, ini);

    InitializeCriticalSection(&logCs);
    if (cfgLog) {
        strcpy(path, ini);
        slash = strrchr(path, '.');
        strcpy(slash, ".log");
        logf = fopen(path, "w");
    }
    Log(">> DIFastBoot: SkipIntro=%d BootLockFix=%d QuietLog=%d SkipFacebook=%d ShaderCache=%d TextureCache=%d CachedReads=%d FastCrashLog=%d LoadingScreenFPS=%d LoadingScreenNoVSync=%d (thread %lu)",
        cfgSkipIntro, cfgBootLockFix, cfgQuietLog, cfgSkipFacebook, cfgShaderCache, cfgTexCache, cfgCachedReads, cfgFastCrashLog, cfgFps, cfgNoVsync, GetCurrentThreadId());

    PatchEngine();
    if (cfgShaderCache) {
        GetModuleFileNameA(self, path, MAX_PATH);
        InstallShaderCache(GetModuleHandleA("engine_x86_rwdi.dll"), path);
    }
    if (cfgCachedReads || cfgFastCrashLog || cfgLog) InstallCachedReads();
    if (cfgTexCache) {
        GetModuleFileNameA(self, path, MAX_PATH);
        InstallTextureCache(GetModuleHandleA("engine_x86_rwdi.dll"), path);
    }
    HookImports(GetModuleHandleA("engine_x86_rwdi.dll"));
    if (cfgLog) {
        InitializeCriticalSection(&profCs);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &mainThread,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        CloseHandle(CreateThread(NULL, 0, Sampler, NULL, 0, NULL));
    }
    CloseHandle(CreateThread(NULL, 0, Watcher, NULL, 0, &watcherId));
}

BOOL WINAPI DllMain(HMODULE mod, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        Init(mod);
    } else if (reason == DLL_PROCESS_DETACH && logf) {
        fflush(logf);
    }
    return TRUE;
}
