// CachedReads: the engine's resource streamer opens the .pak archives with FILE_FLAG_NO_BUFFERING, which bypasses the
// Windows file cache, so every level load re-reads the drive in small queue-depth-1 reads even when the data was read
// moments ago. Buffered handles accept any offset/size/alignment, so dropping the flag is transparent to the reader;
// the OS then serves repeat reads from RAM and reads ahead. Hooked through filesystem_x86_rwdi.dll's import table.
// FastCrashLog: crash.log is opened with FILE_FLAG_WRITE_THROUGH, so every log line waits for the disk (~0.4 ms each).
// The Overhaul mod's player anim scripts log ~6,000 parse-error lines per save load, ~2.5 s of the load. Without the
// flag each line still goes to the OS with its own WriteFile, so a game crash loses nothing.
// Included by fastboot.c (uses Log, cfgLog).

typedef HANDLE(WINAPI *CreateFileAFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileAFn origCreateFileA;
static volatile LONG crUnbuffered, crLogged;

static HANDLE WINAPI BufferedCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                         DWORD flags, HANDLE tmpl)
{
    if (name && (flags & FILE_FLAG_WRITE_THROUGH) && cfgFastCrashLog) {
        size_t n = strlen(name);
        if (n >= 9 && !_stricmp(name + n - 9, "crash.log")) {
            flags &= ~(DWORD)FILE_FLAG_WRITE_THROUGH;
            Log(">> FastCrashLog: %s opened without write-through", name);
        }
    }
    if (flags & FILE_FLAG_NO_BUFFERING) {
        InterlockedIncrement(&crUnbuffered);
        if (cfgLog && InterlockedIncrement(&crLogged) <= 40)
            Log(">> CachedReads: %s opened unbuffered (flags %08lX)%s", name ? name : "?", flags,
                cfgCachedReads ? ", buffering restored" : "");
        if (cfgCachedReads) flags &= ~(DWORD)FILE_FLAG_NO_BUFFERING;
    }
    return origCreateFileA(name, access, share, sa, disp, flags, tmpl);
}

static void InstallCachedReads(void)
{
    HMODULE fs = GetModuleHandleA("filesystem_x86_rwdi.dll");
    FARPROC fn = GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateFileA");
    BYTE *base = (BYTE *)fs;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int patched = 0;
    if (!fs || !fn) return;
    origCreateFileA = (CreateFileAFn)fn;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; t->u1.Function; t++) {
            DWORD old;
            if (t->u1.Function != (DWORD)fn) continue;
            VirtualProtect(&t->u1.Function, 4, PAGE_READWRITE, &old);
            t->u1.Function = (DWORD)BufferedCreateFileA;
            VirtualProtect(&t->u1.Function, 4, old, &old);
            patched++;
        }
    }
    Log(">> CachedReads: %s", patched ? (cfgCachedReads ? "on" : "watching only") : "import not found");
}
