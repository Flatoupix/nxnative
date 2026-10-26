# Good Job! (`0100B0500FE4E000`)

Base game 1.0.0, SDK 9.3.1, Unity 2018.4.14f1 IL2CPP with FMOD, on NVN. The standard rewriter and host run this build with the profile's memory layout. A build-gated rewriter cut keeps Unity's frame-rate and v-sync setters at the Mac profile's 60 fps values.

On the Mac: the French title screen and first level render correctly. The bring-up test reached the objective "ALLEZ CHERCHER VOTRE UNIFORME DE TRAVAIL", verified WASD movement, a carryable-box interaction, the pause menu and resume. After the cold-start shader/pipeline warm-up, the Mac profile presents at a stable 60 fps (16.66 ms frame time) instead of the game's 30-fps present interval: the verification run measured 60.0 fps, zero dropped frames and p95 frame time below 17 ms through frame 2,700. The signed Mac app is about 722 MB. A first-level screenshot is included in the repository gallery.

The Mac app now opens a native pre-game settings window. It remembers the selected window/render resolution (1280×720, 1600×900 or 1920×1080), 30/60 fps, fullscreen and the Metal performance HUD. `DM_LAUNCHER=0` skips it for automated runs, and values explicitly supplied through `env.txt` remain locked as overrides.

## Portability verdict

**Good Job! is natively playable on an Apple-silicon Mac with this profile.** This is a real native execution port, not a video stream or a bundled emulator: the original ARM64 Switch modules run through the rewriter, the host supplies the Horizon/NVN services, and the NVN graphics calls are translated to Metal. The game needs no source-code conversion. Its 1.0.0 main, `subsdk0` and `sdk` modules, RomFS and Metal shader libraries are packed into a self-contained `.app`.

The result is reproducible from a legally dumped NSP and the profile in this directory. A successful bring-up proves the important path—allocator start-up, French title, first playable level, input, pause/resume, audio callbacks, shader translation and 60/30-fps pacing—but it is not a claim that every campaign scene or every Switch feature has been certified. The remaining validation items are listed under [Open](#open).

## Rebuild the Mac app

From the repository root, with Xcode's Metal Toolchain installed:

```sh
python3 scripts/build_ipa_from_nsp.py /path/to/0100B0500FE4E000-nsp-folder \
  --keys /path/to/prod.keys --platform mac-app --language fr \
  --out "/path/to/Good Job!.app"
```

The input folder must contain the base game (and optionally its update/DLC). The script selects this profile by title ID, extracts the modules outside the repository, rewrites them, compiles or reuses the 8,701 Metal shader libraries, and packs the app. Xcode is needed for rebuilding; the resulting `.app` does not need Xcode or the NSP at runtime.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x240000000`, `mem_alias 0x218000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | SDK 9.3.1's `StandardAllocator` needs more virtual-address headroom than the generated 5 GiB layout. The reservation is address space, not committed RAM. |
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | Matches the 16 MiB system-resource declaration in the NPDM. |
| `shader_groups /Data/=data` | [`game.tsv`](game.tsv) | Unity's NVN shader assets are in the RomFS `Data` tree. |
| `fps_target 60`, `present_interval display` | [`game.tsv`](game.tsv) | Removes the Switch interval-2 presentation cap; verified in the first playable scene. |

## Frame rate

Good Job! calls Unity's `Application.set_targetFrameRate(30)` and `QualitySettings.set_vSyncCount(2)` after boot. `patches.py` gates two dynamic argument cuts on main build `951d09ee…`: they read the launcher's target, using 60/one v-sync or 30/two v-syncs. Without them, the host's 60 Hz display pacing still leaves Unity alternating missed ticks (33 ms frames). The first cold run can still compile Metal pipelines on demand; the shipped pipeline archive and the per-user archive remove those stalls after the initial warm-up.

## Shaders

The survey found 8,701 shaders: 4,533 vertex, 4,118 pixel and 50 compute. All 8,701 compiled successfully into the Mac Metal cache with Xcode's Metal Toolchain. The first playable scene rendered without a missing-shader or fatal-log marker.

## Open

- Full campaign progression and long-session stability are not yet certified.
- Save/reload and two-controller local co-op remain unverified.
- Audio callbacks ran without underruns during the test; every music and effect scene still needs listening validation.
