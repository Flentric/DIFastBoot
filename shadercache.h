// ShaderCache: disk cache for the engine's D3DXCompileShader calls (Renderer::RCompileShader).
// The engine compiles every material shader from HLSL at each boot (~4 s of the preload). The engine only keeps the
// bytecode buffer and derives constants/semantics from it, so returning identical bytecode is equivalent.
// Key = 128-bit hash of source, defines, entry, profile, flags. Each entry also records the #includes the compile
// opened (type, name, content hash); a hit re-opens them through the engine's include handler and compares. Any
// mismatch or failure falls back to a real compile, so edited shaders or includes are never served stale.
// Included by fastboot.c (uses Log, cfgLog).

typedef struct { const char *Name, *Definition; } XMACRO;
typedef struct XBuffer XBuffer;
typedef struct {
    HRESULT(WINAPI *QueryInterface)(XBuffer *, const IID *, void **);
    ULONG(WINAPI *AddRef)(XBuffer *);
    ULONG(WINAPI *Release)(XBuffer *);
    void *(WINAPI *GetBufferPointer)(XBuffer *);
    DWORD(WINAPI *GetBufferSize)(XBuffer *);
} XBufferVtbl;
struct XBuffer { XBufferVtbl *v; };
typedef struct XInclude XInclude;
typedef struct {
    HRESULT(WINAPI *Open)(XInclude *, int type, const char *name, const void *parent, const void **data, UINT *bytes);
    HRESULT(WINAPI *Close)(XInclude *, const void *data);
} XIncludeVtbl;
struct XInclude { XIncludeVtbl *v; };

typedef HRESULT(WINAPI *CompileFn)(const char *src, UINT len, const XMACRO *defs, XInclude *inc, const char *entry,
                                   const char *profile, DWORD flags, XBuffer **shader, XBuffer **errors, void **ctab);
typedef HRESULT(WINAPI *CreateBufferFn)(DWORD size, XBuffer **buf);
typedef HRESULT(WINAPI *GetCTabFn)(const DWORD *code, void **ctab);

static CompileFn origCompile;
static CreateBufferFn xCreateBuffer;
static GetCTabFn xGetCTab;
static char cacheDir[MAX_PATH];
static volatile LONG scHits, scMisses, scStale, scVerified, scBad;
static volatile LONG64 scCompileUs, scHitUs;

#define SC_MAGIC 0x31435344u   // "DSC1"
#define SC_MAX_INC 64

typedef struct { unsigned long long a, b; } Hash128;

static void HashBytes(Hash128 *h, const void *data, size_t n)
{
    const BYTE *p = (const BYTE *)data;
    size_t i;
    for (i = 0; i < n; i++) {
        h->a = (h->a ^ p[i]) * 0x100000001B3ull;                 // FNV-1a
        h->b = (h->b + p[i] + 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
        h->b ^= h->b >> 29;
    }
}

static void HashStr(Hash128 *h, const char *s)
{
    static const char zero = 0;
    if (s) HashBytes(h, s, strlen(s));
    HashBytes(h, &zero, 1);   // separator, so ("ab","c") != ("a","bc")
}

static Hash128 HashInit(void)
{
    Hash128 h = {0xCBF29CE484222325ull, 0x84222325CBF29CE4ull};
    return h;
}

// ---- include recorder (wraps the engine's include handler during a real compile) ----
typedef struct {
    int type;
    char name[MAX_PATH];
    Hash128 hash;
} IncRec;

typedef struct {
    XInclude base;
    XInclude *real;
    int count;
    BOOL overflow;
    IncRec rec[SC_MAX_INC];
} RecInclude;

static HRESULT WINAPI RecOpen(XInclude *self, int type, const char *name, const void *parent, const void **data, UINT *bytes)
{
    RecInclude *r = (RecInclude *)self;
    HRESULT hr = r->real->v->Open(r->real, type, name, parent, data, bytes);
    if (SUCCEEDED(hr)) {
        if (r->count < SC_MAX_INC && name && strlen(name) < MAX_PATH) {
            IncRec *e = &r->rec[r->count++];
            e->type = type;
            strcpy(e->name, name);
            e->hash = HashInit();
            HashBytes(&e->hash, *data, *bytes);
        } else {
            r->overflow = TRUE;
        }
    }
    return hr;
}

static HRESULT WINAPI RecClose(XInclude *self, const void *data)
{
    RecInclude *r = (RecInclude *)self;
    return r->real->v->Close(r->real, data);
}

static XIncludeVtbl recVtbl = {RecOpen, RecClose};

// ---- cache file: magic, bytecode size, include count, includes {type, name len, name, hash}, bytecode ----
static void CachePath(char *out, const Hash128 *k)
{
    sprintf(out, "%s\\%016llX%016llX.bin", cacheDir, k->a, k->b);
}

static BOOL CacheLoad(const Hash128 *k, XInclude *inc, XBuffer **shader)
{
    char path[MAX_PATH];
    HANDLE f;
    DWORD size, got, pos = 0;
    BYTE *buf;
    BOOL ok = FALSE;
    CachePath(path, k);
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    size = GetFileSize(f, NULL);
    buf = (BYTE *)malloc(size ? size : 1);
    if (buf && size >= 12 && ReadFile(f, buf, size, &got, NULL) && got == size) {
        DWORD codeSize = *(DWORD *)(buf + 4), incs = *(DWORD *)(buf + 8), i;
        pos = 12;
        ok = *(DWORD *)buf == SC_MAGIC && incs <= SC_MAX_INC;
        for (i = 0; ok && i < incs; i++) {
            int type;
            DWORD nlen;
            char name[MAX_PATH];
            Hash128 want, have;
            const void *data = NULL;
            UINT bytes = 0;
            if (pos + 8 > size) { ok = FALSE; break; }
            type = *(int *)(buf + pos);
            nlen = *(DWORD *)(buf + pos + 4);
            pos += 8;
            if (nlen >= MAX_PATH || pos + nlen + 16 > size) { ok = FALSE; break; }
            memcpy(name, buf + pos, nlen);
            name[nlen] = 0;
            pos += nlen;
            memcpy(&want, buf + pos, 16);
            pos += 16;
            // the include must still exist with identical content
            if (!inc || FAILED(inc->v->Open(inc, type, name, NULL, &data, &bytes))) { ok = FALSE; break; }
            have = HashInit();
            HashBytes(&have, data, bytes);
            inc->v->Close(inc, data);
            if (have.a != want.a || have.b != want.b) ok = FALSE;
        }
        if (!ok) InterlockedIncrement(&scStale);
        if (ok && pos + codeSize == size && codeSize >= 4 && SUCCEEDED(xCreateBuffer(codeSize, shader))) {
            memcpy((*shader)->v->GetBufferPointer(*shader), buf + pos, codeSize);
        } else {
            ok = FALSE;
        }
    }
    free(buf);
    CloseHandle(f);
    return ok;
}

static void CacheStore(const Hash128 *k, const RecInclude *rec, XBuffer *shader)
{
    char path[MAX_PATH], tmp[MAX_PATH];
    HANDLE f;
    DWORD hdr[3], w, i;
    BOOL ok;
    CachePath(path, k);
    sprintf(tmp, "%s.%lu.tmp", path, GetCurrentThreadId());
    f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    hdr[0] = SC_MAGIC;
    hdr[1] = shader->v->GetBufferSize(shader);
    hdr[2] = rec->count;
    ok = WriteFile(f, hdr, 12, &w, NULL);
    for (i = 0; ok && i < (DWORD)rec->count; i++) {
        DWORD h2[2];
        h2[0] = rec->rec[i].type;
        h2[1] = (DWORD)strlen(rec->rec[i].name);
        ok = WriteFile(f, h2, 8, &w, NULL) && WriteFile(f, rec->rec[i].name, h2[1], &w, NULL) &&
             WriteFile(f, &rec->rec[i].hash, 16, &w, NULL);
    }
    ok = ok && WriteFile(f, shader->v->GetBufferPointer(shader), hdr[1], &w, NULL);
    CloseHandle(f);
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
}

static HRESULT WINAPI CachedCompile(const char *src, UINT len, const XMACRO *defs, XInclude *inc, const char *entry,
                                    const char *profile, DWORD flags, XBuffer **shader, XBuffer **errors, void **ctab)
{
    Hash128 k = HashInit();
    RecInclude rec;
    HRESULT hr;
    double t = NowMs();
    const XMACRO *d;
    if (!src || !shader) return origCompile(src, len, defs, inc, entry, profile, flags, shader, errors, ctab);

    HashBytes(&k, &len, 4);
    HashBytes(&k, src, len);
    for (d = defs; d && d->Name; d++) { HashStr(&k, d->Name); HashStr(&k, d->Definition); }
    HashStr(&k, "|");
    HashStr(&k, entry);
    HashStr(&k, profile);
    HashBytes(&k, &flags, 4);

    if (cfgVerify && CacheLoad(&k, inc, shader)) {
        // debug: compile for real and compare with the cached bytecode
        XBuffer *cached = *shader;
        hr = origCompile(src, len, defs, inc, entry, profile, flags, shader, errors, ctab);
        if (SUCCEEDED(hr) && *shader) {
            DWORD n1 = cached->v->GetBufferSize(cached), n2 = (*shader)->v->GetBufferSize(*shader);
            BOOL same = n1 == n2 && !memcmp(cached->v->GetBufferPointer(cached), (*shader)->v->GetBufferPointer(*shader), n1);
            InterlockedIncrement(same ? &scVerified : &scBad);
        }
        cached->v->Release(cached);
        return hr;
    }
    if (!cfgVerify && CacheLoad(&k, inc, shader)) {
        if (errors) *errors = NULL;
        if (ctab) {
            *ctab = NULL;
            if (FAILED(xGetCTab((const DWORD *)(*shader)->v->GetBufferPointer(*shader), ctab))) {
                (*shader)->v->Release(*shader);
                *shader = NULL;
                goto compile;
            }
        }
        InterlockedIncrement(&scHits);
        InterlockedExchangeAdd64(&scHitUs, (LONG64)((NowMs() - t) * 1000.0));
        return S_OK;
    }

compile:
    rec.base.v = &recVtbl;
    rec.real = inc;
    rec.count = 0;
    rec.overflow = FALSE;
    hr = origCompile(src, len, defs, inc ? &rec.base : NULL, entry, profile, flags, shader, errors, ctab);
    InterlockedIncrement(&scMisses);
    InterlockedExchangeAdd64(&scCompileUs, (LONG64)((NowMs() - t) * 1000.0));
    if (SUCCEEDED(hr) && *shader && !rec.overflow) CacheStore(&k, &rec, *shader);
    return hr;
}

static void InstallShaderCache(HMODULE eng, const char *asiPath)
{
    HMODULE dx = GetModuleHandleA("d3dx9_43.dll");
    FARPROC compile = dx ? GetProcAddress(dx, "D3DXCompileShader") : NULL;
    BYTE *base = (BYTE *)eng;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    char *slash;
    int patched = 0;
    if (!eng || !compile) { Log(">> ShaderCache: d3dx9_43 not loaded, off"); return; }
    xCreateBuffer = (CreateBufferFn)GetProcAddress(dx, "D3DXCreateBuffer");
    xGetCTab = (GetCTabFn)GetProcAddress(dx, "D3DXGetShaderConstantTable");
    if (!xCreateBuffer || !xGetCTab) { Log(">> ShaderCache: d3dx9_43 exports missing, off"); return; }

    strcpy(cacheDir, asiPath);
    slash = strrchr(cacheDir, '.');
    if (slash) strcpy(slash, ".shadercache");
    CreateDirectoryA(cacheDir, NULL);

    origCompile = (CompileFn)compile;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; t->u1.Function; t++) {
            DWORD old;
            if (t->u1.Function != (DWORD)compile) continue;
            VirtualProtect(&t->u1.Function, 4, PAGE_READWRITE, &old);
            t->u1.Function = (DWORD)CachedCompile;
            VirtualProtect(&t->u1.Function, 4, old, &old);
            patched++;
        }
    }
    Log(">> ShaderCache: %s (%s)", patched ? "on" : "import not found, off", cacheDir);
}

static void ShaderCacheStats(const char *when)
{
    if (scHits || scMisses)
        Log(">> ShaderCache %s: %ld hits (%.2f s), %ld compiled (%.2f s), %ld stale entries recompiled", when,
            scHits, scHitUs / 1e6, scMisses, scCompileUs / 1e6, scStale);
    if (cfgVerify) Log(">> ShaderCache verify: %ld identical, %ld DIFFERENT", scVerified, scBad);
}
