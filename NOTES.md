# DIFastBoot

ASI (`scripts\DIFastBoot.asi` + `.ini`, loaded by UAL as winmm.dll). Build: `build.bat` (cl, x86).

## What it patches (installed Jan-2012 build, found by signature)

| Site | Engine RVA | Change |
|---|---|---|
| `CGame::BackgroundRenderProc` per-frame `Sleep(BackgroundRenderSleep)` | +0x17567C | call `BgSleep` (frame cap, `LoadingScreenFPS`) |
| same loop, `g_pRenderer->Present(0,0,0)` | +0x1755CD | call `BgPresentThunk` (marks the call as a loading-screen present) |
| `CRenderer::Present`, `IDirect3DDevice9::Present` | +0x3C72DD | call `DevicePresent`: loading-screen frames use swap chain `Present(..., D3DPRESENT_DONOTWAIT)` |
| game `-nologos` flag (set by `Game::ReadExecuteParams`) | game +0xB409D8 | set to 1 (`SkipIntro`) |
| `MainMenuModuleDI::LoadBackgroundLevel` (loads menuback.map) | game +0x291B60 | `ret` (`SkipMenuBackground`); the level pointer (+0x7F4) is null-checked by Unload/Is3dBackgroundUsed/SetBackgroundAnimState |

| `CRenderer::BeginFrame` "device not ready" `Sleep(200)` (runs with the renderer lock held) | engine +0x3C6B5D | call `NotReadySleep`: returns at once on the loading-screen thread (`BootLockFix`) |
| engine/game imports of filesystem `_CLFilter` / `_CLog` | IAT | WARN lines dropped (`QuietLog`) |

BootLockFix is the big one: until the fullscreen switch at the menu, every boot loading-screen frame took the lock,
entered BeginFrame and slept 200 ms, so the main thread spent 53% of init waiting in AcquireOwnership (9.5 s).
Init 17.9 s -> 8.7 s. QuietLog: the game's own crash.log writes were ~19% of the preload phase (~2 s saved).
Result (4K, VSync, run 2026-09-27): menu at ~28 s after process start vs ~47 s stock; a save loads fine.
Left: ~15 s common preload (real data, disk + parsing), ~6 s ApplyLanguages (voice-over pack).

| Site (round 3) | Where | Change |
|---|---|---|
| `ITools::FacebookInitialize` (ITools vtable slot 1) | engine +0x18DC60 | `ret 0Ch` (`SkipFacebook`); slot 4 `FacebookAuthorizeAccount` (+0x18DD00, the one user of g_pIFacebook without a null check) returns an empty string |
| engine import `D3DXCompileShader` | IAT | `shadercache.h`: disk cache keyed by source/defines/entry/profile/flags, includes re-validated by content hash |
| engine import `D3DXLoadSurfaceFromMemory` | IAT | `texcache.h`: caches D3DX's output for scaled DXT uploads (runtime mip generation for textures without mips) |

Round 3 (2026-09-27, self-tested): main thread was 75-95% CPU at 5-20 MB/s during the preload (CPU-bound, not disk).
Profile: D3DXCompileShader 214 shaders = 5.7 s; scaled DXT D3DXLoadSurfaceFromMemory 1966 calls = 4-5.8 s (each mip
level built from the full top level: decompress, box filter, recompress); Facebook init ~5 s (heap churn + waiting).
With both caches warm: menu at ~9.3 s after process start (47 s stock). VerifyCaches run: 240 shaders and 1966
texture levels byte-identical to fresh D3DX output. Cache sizes: ~5.7 MB textures, shaders small.

Boot timeline, run 1 (4K, VSync): 0-3 s device; 3-16 s common character/weapon preload (no loading screen exists yet);
17-33 s GameDI::OnInitialize (5 s level, 5.8 s ApplyLanguages -> voice-over pack); 33-47 s menuback.map. Boot-time
loading-screen frames never reach Present (BeginFrame fails until the fullscreen switch), so the boot is black.
TimingLog also samples the main thread every 2 ms for the first 180 s (">> profile" blocks per phase).

`TimingLog=1` also swaps `CRenderer` vtable slot 42 (AcquireOwnership) to time loader waits, and hooks the
engine/game imports of filesystem's `_CLog` to write every log line with a timestamp to `DIFastBoot.log`.

## Why

Loading screens (boot intro and level loads) are drawn by the engine's background render thread
(`ITools::EnableBackgroundRendering`, sleep clamped to >= 20 ms; the intro asks for 30). Each frame it takes the
renderer lock (`CRenderer::AcquireOwnership`, a critical section) at BeginFrame and keeps it through Present, so with
VSync on the loader thread waits up to a full refresh per loading frame whenever it needs the renderer.

`-nologos`: `IntroModule::StartIntro` jumps to BeginInitialSequence (no intro movies), BeginInitialSequence jumps the
logo timelines to "EndShow", and `IntroModule::OnGameInitialized` calls `GoToMenu(0,1)` instead of the
"press any key" screen.

2011 PDB names used: CGame::BackgroundRenderProc 101487C0, EnableBackgroundRendering 10148B70,
CRenderer::Present 10401D20, AcquireOwnership 10402B70.

## Round 4: save load (2026-09-27)

Save load ~9.7 s. The main thread only pumps messages and Sleeps while a worker (engine+177900 via the sp_thread
trampoline +384EB0) loads; the last ~2.5 s were the main thread creating the player (`SessionDI::LoadLevel` ->
`LocalClientDI::CreateRepresentation`) blocked in WriteFile: the Overhaul mod's `anims_player.scr` uses constants DI
doesn't define (VIS_EVENT_IK_DISABLE, EVENT_PISTOL_RUN, ...), ~1,224 CTCParse errors = ~6,000 ERRR lines per load, and
crash.log is opened with FILE_FLAG_WRITE_THROUGH (flags 80000080). `FastCrashLog` drops that flag for crash.log only:
save load 9.7 -> 7.65 s (A/B x2). `CachedReads` (drop NO_BUFFERING on the .rpack opens, flags 60000000) measured no
gain (A/B x2), so it defaults to 0 and is not in the ini. TimingLog now also logs per-thread CPU once a second and
profiles the busiest non-main thread.

Left: ~5-6 s of the worker reading/decoding .rpack data through the engine's queue-depth-1 pipeline (filesystem
async thread +1BD00); boot: CreateDevice ~1.4 s at 4K, an initial Present ~0.4 s, CGame::OnActivate's Sleep(200) per
window activation (+17479C, left alone: alt-tab device handling), ~3.5 s common asset parsing.

## v2: SkipMenuBackground removed (2026-09-27)

A tester found the New Game character select blank: MenuSelectCharacterDi poses the characters in menuback.map. A
deferred load at the screen's ShowScreen loaded the level (1.8 s) but the view stayed black (the menu wires the 3D
scene up when it activates). With the shader/texture caches and BootLockFix the scene only costs ~1.3 s at boot (menu
~10.8 s of game time vs ~9.5 s), so the option was removed and the level loads as stock.
