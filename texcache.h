// TextureCache: disk cache for the engine's DXT mip generation.
// Textures shipped without mip maps get their chain built at every load: the engine calls D3DXLoadSurfaceFromMemory
// once per level with the full top level as source and a BOX filter, so D3DX decompresses, filters and re-compresses
// the whole image for every level (~4 s of the boot on the Overhaul mod). The result is deterministic, so the bytes
// D3DX wrote are stored under a hash of every input (source block rows, formats, sizes, filter, color key) and copied
// straight into the surface next time. Only DXT1/3/5 -> same format, scaled, no palettes; anything else, and any
// lock/read failure, goes to D3DX unchanged. Included by fastboot.c (uses Log, NowMs, Hash128 from shadercache.h).

typedef HRESULT(WINAPI *LoadSurfFn)(IDirect3DSurface9 *dst, const PALETTEENTRY *dstPal, const RECT *dstRect,
                                    const void *src, D3DFORMAT srcFmt, UINT srcPitch, const PALETTEENTRY *srcPal,
                                    const RECT *srcRect, DWORD filter, D3DCOLOR key);
static LoadSurfFn origLoadSurf;
static char texDir[MAX_PATH];
static volatile LONG tcHits, tcMisses, tcVerified, tcBad;
static volatile LONG64 tcHitUs, tcMissUs;

#define TC_MAGIC 0x31435444u   // "DTC1"

static DWORD DxtBlockBytes(D3DFORMAT f)
{
    if (f == D3DFMT_DXT1) return 8;
    if (f == D3DFMT_DXT3 || f == D3DFMT_DXT5) return 16;
    return 0;
}

// word-at-a-time version of the 128-bit hash; fine for the large source images
static void HashWords(Hash128 *h, const BYTE *p, size_t n)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        unsigned long long w;
        memcpy(&w, p + i, 8);
        h->a = (h->a ^ w) * 0x100000001B3ull;
        h->a ^= h->a >> 32;
        h->b = (h->b + w + 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
        h->b ^= h->b >> 29;
    }
    HashBytes(h, p + i, n - i);
}

static HRESULT WINAPI CachedLoadSurf(IDirect3DSurface9 *dst, const PALETTEENTRY *dstPal, const RECT *dstRect,
                                     const void *src, D3DFORMAT srcFmt, UINT srcPitch, const PALETTEENTRY *srcPal,
                                     const RECT *srcRect, DWORD filter, D3DCOLOR key)
{
    D3DSURFACE_DESC d;
    DWORD bb = DxtBlockBytes(srcFmt), sw, sh, dw, dh, rowBytes, rows, y, got;
    Hash128 k;
    char path[MAX_PATH], tmp[MAX_PATH];
    D3DLOCKED_RECT lr;
    HANDLE f;
    HRESULT hr;
    double t = NowMs();
    BYTE *buf;

    if (!bb || !src || !srcRect || dstPal || srcPal || FAILED(IDirect3DSurface9_GetDesc(dst, &d)) || d.Format != srcFmt)
        return origLoadSurf(dst, dstPal, dstRect, src, srcFmt, srcPitch, srcPal, srcRect, filter, key);
    dw = dstRect ? dstRect->right - dstRect->left : d.Width;
    dh = dstRect ? dstRect->bottom - dstRect->top : d.Height;
    sw = srcRect->right - srcRect->left;
    sh = srcRect->bottom - srcRect->top;
    if ((sw == dw && sh == dh) || (dstRect && (dw != d.Width || dh != d.Height)) || (srcRect->left | srcRect->top) & 3)
        return origLoadSurf(dst, dstPal, dstRect, src, srcFmt, srcPitch, srcPal, srcRect, filter, key);

    k = HashInit();
    {
        DWORD hdr[8] = {TC_MAGIC, (DWORD)srcFmt, sw, sh, dw, dh, filter, key};
        const BYTE *s = (const BYTE *)src + (srcRect->top / 4) * srcPitch + (srcRect->left / 4) * bb;
        DWORD srows = (sh + 3) / 4, sbytes = ((sw + 3) / 4) * bb;
        HashBytes(&k, hdr, sizeof(hdr));
        for (y = 0; y < srows; y++) HashWords(&k, s + y * srcPitch, sbytes);
    }
    rows = (dh + 3) / 4;
    rowBytes = ((dw + 3) / 4) * bb;
    sprintf(path, "%s\\%016llX%016llX.bin", texDir, k.a, k.b);

    // hit: copy the stored D3DX output into the surface
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        BOOL ok = FALSE;
        buf = (BYTE *)malloc(rows * rowBytes);
        if (buf && GetFileSize(f, NULL) == rows * rowBytes && ReadFile(f, buf, rows * rowBytes, &got, NULL) &&
            got == rows * rowBytes) {
            if (cfgVerify) {
                // debug: let D3DX do the real work and compare with the cached bytes
                if (SUCCEEDED(origLoadSurf(dst, dstPal, dstRect, src, srcFmt, srcPitch, srcPal, srcRect, filter, key)) &&
                    SUCCEEDED(IDirect3DSurface9_LockRect(dst, &lr, dstRect, D3DLOCK_READONLY))) {
                    BOOL same = TRUE;
                    for (y = 0; y < rows && same; y++) same = !memcmp((BYTE *)lr.pBits + y * lr.Pitch, buf + y * rowBytes, rowBytes);
                    IDirect3DSurface9_UnlockRect(dst);
                    InterlockedIncrement(same ? &tcVerified : &tcBad);
                    ok = TRUE;
                }
            } else if (SUCCEEDED(IDirect3DSurface9_LockRect(dst, &lr, dstRect, 0))) {
                for (y = 0; y < rows; y++) memcpy((BYTE *)lr.pBits + y * lr.Pitch, buf + y * rowBytes, rowBytes);
                IDirect3DSurface9_UnlockRect(dst);
                ok = TRUE;
            }
        }
        free(buf);
        CloseHandle(f);
        if (ok) {
            InterlockedIncrement(&tcHits);
            InterlockedExchangeAdd64(&tcHitUs, (LONG64)((NowMs() - t) * 1000.0));
            return D3D_OK;
        }
    }

    // miss: let D3DX do it, then store what it wrote
    hr = origLoadSurf(dst, dstPal, dstRect, src, srcFmt, srcPitch, srcPal, srcRect, filter, key);
    InterlockedIncrement(&tcMisses);
    if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DSurface9_LockRect(dst, &lr, dstRect, D3DLOCK_READONLY))) {
        sprintf(tmp, "%s.%lu.tmp", path, GetCurrentThreadId());
        f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            BOOL ok = TRUE;
            DWORD w;
            for (y = 0; ok && y < rows; y++) ok = WriteFile(f, (BYTE *)lr.pBits + y * lr.Pitch, rowBytes, &w, NULL);
            CloseHandle(f);
            if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
        }
        IDirect3DSurface9_UnlockRect(dst);
    }
    InterlockedExchangeAdd64(&tcMissUs, (LONG64)((NowMs() - t) * 1000.0));
    return hr;
}

static void InstallTextureCache(HMODULE eng, const char *asiPath)
{
    HMODULE dx = GetModuleHandleA("d3dx9_43.dll");
    FARPROC fn = dx ? GetProcAddress(dx, "D3DXLoadSurfaceFromMemory") : NULL;
    BYTE *base = (BYTE *)eng;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    char *slash;
    int patched = 0;
    if (!eng || !fn) { Log(">> TextureCache: d3dx9_43 not loaded, off"); return; }
    strcpy(texDir, asiPath);
    slash = strrchr(texDir, '.');
    if (slash) strcpy(slash, ".texcache");
    CreateDirectoryA(texDir, NULL);
    origLoadSurf = (LoadSurfFn)fn;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; t->u1.Function; t++) {
            DWORD old;
            if (t->u1.Function != (DWORD)fn) continue;
            VirtualProtect(&t->u1.Function, 4, PAGE_READWRITE, &old);
            t->u1.Function = (DWORD)CachedLoadSurf;
            VirtualProtect(&t->u1.Function, 4, old, &old);
            patched++;
        }
    }
    Log(">> TextureCache: %s (%s)", patched ? "on" : "import not found, off", texDir);
}

static void TextureCacheStats(void)
{
    if (tcHits || tcMisses)
        Log(">> TextureCache so far: %ld hits (%.2f s), %ld generated by D3DX (%.2f s)%s", tcHits, tcHitUs / 1e6, tcMisses,
            tcMissUs / 1e6, cfgVerify ? "" : "");
    if (cfgVerify) Log(">> TextureCache verify: %ld identical, %ld DIFFERENT", tcVerified, tcBad);
}
