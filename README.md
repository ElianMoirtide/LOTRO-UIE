# LOTRO-UIE (UI Enhanced) — v0.6

A DirectX 11 proxy, UI super-resolution, and 3D post-processing project for *The Lord of the Rings Online* (64-bit client), designed to fix blurry fonts, tiny UI badges, low-resolution world/radar maps, and dated 2007 post-processing visuals directly inside the DX11 render pipeline on 1440p, 4K, and HiDPI displays.

---

## Features

- **2× Mini-CNN Compute Super-Resolution (`cs_5_0`) + VRAM Cache (`Ctrl+F4`)**:
  - Upscales UI textures, icons, font atlases, and **offscreen UI render targets** (`UpscaleUiRenderTargets = true`, e.g. text windows at *Global Zoom 3.00*) to `2×` resolution in real time.
  - Detail-preserving architecture for small glyphs (`<= 96 px` edges): protects thin strokes, serifs, and umlauts/accents from erosion.
  - Flicker-free dynamic throttling and automatic dirty tracking across `Map`, `UpdateSubresource`, `CopyResource`, `OMSetRenderTargets`, and `ClearRenderTargetView`.
- **Subpixel UI & Font Reconstruction Shader (`ps_4_0`, `F8` / `F7`)**:
  - Edge-directed 12-tap reconstruction with adaptive unsharp masking and 4 sharpness presets (`Low 0.35`, `Med 0.70`, `High 1.05`, `Ultra 1.40` via `F7`).
  - **Map Mode (`MapMode = true`)**: Calms coarse parchment grain on upscaled maps (`896×576`/`1024×768`) while preserving crisp text strokes and semi-transparent overlays.
  - **Nameplate Sharpening (`EnhanceNameplates = true`)**: Sharpens floating 3D nameplates (`DepthEnable=TRUE, DepthWriteMask=ZERO`) without touching 3D world geometry.
- **Interactive In-Game Control Center (`Ctrl+F9`)**:
  - Rendered directly in DirectX 11 and controlled by mouse click: toggle and cycle every feature, sharpness preset, UI theme, glassmorphism, icon polish, Middle-earth color grade, 3D rim-lighting, vignette/horizon AA, eye-care mode, live hovered scaling (`-5%` / `+5%`), clipboard export, and `Save All Settings to INI` without touching config files — completely independent of the LOTRO Lua sandbox and available right from the character select screen.
- **Universal UI Modernization & Font Legibility (`[UiTheme]`)**:
  - **Font Outline / Halo Boost (`FontOutlineBoost = 0.35`)**: Generates a subtle dark micro-contrast halo around translucent bright glyphs so quest tracker text, chat, and tooltips remain crisp over bright snow or sky.
  - **Adaptive Font Weight / Stem Boost (`FontWeightBoost = 0.25`)**: Intelligently reinforces hairline horizontal/vertical font stems (`Off` / `Soft 25%` / `Med 50%` / `Bold 80%`) so thin 1px legacy glyphs read like modern Regular/Medium weights at 1440p/4K.
  - **Multi-Profile Modern UI Themes (`ThemeDarkSlate`, `ThemeProfile`)**: Transforms washed-out grey-brown panel backgrounds into modern **Obsidian Slate**, **Elven Bronze**, or **Dwarven Iron** palettes while keeping golden borders and bright text vibrant.
  - **UI Frosted Glassmorphism (`Glassmorphism = 0.55`)**: Builds a smooth multi-pass quarter/eighth-resolution blur of the 3D scene behind semi-transparent HUD, chat, and tooltip panels for a crisp frosted-glass backdrop without blurring foreground text.
  - **UI Icon & Gold Border Polish (`IconPolish = 0.40`)**: Applies selective local micro-contrast, vibrance, and subtle beveled metallic luster to colorful skill/item icons (`>= 24 px` textures) and golden frames while keeping text glyphs clean.
  - **Eye-Care & Anti-Glare Modes (`EyeComfortMode`)**: Cycles `Off` -> `Anti-Glare` -> `Warm 5000K` -> `Night 4300K` across UI and 3D world.
  - **Hover Focus & Contrast Boost (`HoverFocusBoost = true`)**: Gently boosts contrast and backdrop separation on the hovered UI panel/tooltip.
  - **OLED Auto-Dim (`OledAutoDim`)**: Softly dims static top/bottom peripheral HUD bars (default `72%` opacity) while the cursor rests in the central 3D viewport, restoring `100%` opacity when hovering near them.
- **Per-Element UI Scaling via Geometry Shader (`[ElementScale]`), Clipboard Export (`Ctrl+F2`) & Live Scaling (`Ctrl+PgUp`/`Ctrl+PgDn`)**:
  - Selectively scale individual UI elements (e.g., tiny level badges, quest indicators, or icons).
  - **1-Click Clipboard Export (`Ctrl+F2`)**: Hover any UI element and press `Ctrl+F2` to copy a ready-to-paste `[ElementScale]` rule (`Element_<WxH>_<Hash> = 0x<Hash>, +15%`) directly to the Windows clipboard.
  - **Live In-Game Scaling (`Ctrl+PgUp` / `Ctrl+PgDn`)**: Scales the hovered UI element live by `+5%` / `-5%` and automatically saves the rule to `lotro_uie.ini`.
- **Smart Hover Magnifier (`HoverMagnifier = true`, hold `ALT`)**:
  - Holding `ALT` dynamically zooms the compact UI element under your mouse cursor to `1.25×` (`+25%`) via Geometry Shader bounding-box expansion.
- **Custom HD Texture Replacement (`lotro_uie_overrides/`)**:
  - Drop custom `.png` or `.dds` textures named `0x<16HexHash>.png` or `.dds` into `x64/lotro_uie_overrides/` to replace legacy UI textures on the GPU at runtime (live-reloadable with `Ctrl+F6`).
- **Split-Screen A/B Live Comparison (`Ctrl+F3`)**:
  - Renders unmodified vanilla LOTRO on the left half of the screen and LOTRO-UIE on the right half (`< ORIG | UIE >`).
- **3D World-Only PostFX (`Ctrl+F5`, `[WorldPostFX]`) & 16× Anisotropic World Samplers (`[Samplers]`)**:
  - Executes **before** the first UI draw call (independent of `F8` UI toggling) on the 3D scene alone: Contrast Adaptive Sharpening (`CasStrength`), sky/fog debanding (`Deband`), midtone `Clarity`, `Vibrance`, atmospheric `Dehaze`, and soft-knee highlight `AntiGlare`.
  - **Middle-earth Region Color Grading (`ColorProfile`)**: Cycles `Natural Modern` (balanced neutrality), `Rivendell Gold` (warm golden highlights & emerald greens), `Lothlorien Silver` (cool moonlight blues & ethereal highlights), and `Mordor Grim` (desaturated ash tones & deep contrast).
  - **3D Dynamic Rim-Lighting & Metallic Specular Pop (`RimSpecular = 0.28`)**: Adds subtle screen-space silhouette rim light and metallic specular pop to armor, weapons, stone arches, and trim.
  - **Cinematic Vignette & Upper-Horizon Anti-Shimmer (`Vignette = 0.16`, `HorizonSoften = 0.25`)**: Soft radial framing plus upper-sky silhouette smoothing to calm shimmering distant mountain edges without softening close-up foreground foliage.
  - Upgrades 3D world mipmapped samplers to `16×` Anisotropic filtering with `-0.35` LOD bias.
- **Compressed 32-Bit RGBA `.png` Asset Dumper (`Ctrl+F11`)**:
  - Exports captured UI textures cleanly as lossless RGBA8 `.png` files (approx. 85–92% smaller than `.bmp`) alongside texture hash mappings in `summary.txt` and `capture.json`.

---

## Default Hotkeys (Modifier-Safe for LOTRO)

To avoid conflicting with LOTRO's Fellowship selection keys (`F1`–`F6`) and System Menu (`F10`), diagnostic and secondary bindings use `CTRL+` modifiers:

| Keybind | Action |
| :--- | :--- |
| **`Ctrl+F9`** | Open/close the **Interactive In-Game Control Center** (mouse clickable) |
| **`F8`** | Toggle UI & Font Enhancer on/off |
| **`F7`** | Cycle Sharpness Presets (`Low` → `Med` → `High` → `Ultra`) |
| **`F9`** | Toggle top-left diagnostic HUD (displays live `Hover:0x...` hash & size) |
| **`Ctrl+F2`** | Copy **hovered UI texture hash** as a ready `[ElementScale]` rule to the clipboard |
| **`Ctrl+PgUp` / `Ctrl+PgDn`** | Live-scale hovered UI element **`+5%` / `-5%`** & save to `lotro_uie.ini` |
| **Hold `ALT`** | **Smart Hover Magnifier**: zoom the compact UI element under the cursor to `1.25×` |
| **`Ctrl+F3`** | Toggle Split-Screen A/B comparison (Left = Vanilla, Right = LOTRO-UIE) |
| **`Ctrl+F4`** | Toggle 2× Mini-CNN Super-Resolution on/off |
| **`Ctrl+F5`** | Toggle 3D World PostFX on/off |
| **`Ctrl+F6`** | Live-reload `lotro_uie.ini` and `lotro_uie_overrides/` in-game |
| **`Ctrl+F10`** | Capture frame diagnostics (`summary.txt`, `draws.csv`, `capture.json`) |
| **`Ctrl+F11`** | Full frame capture + compressed `.png` texture & shader dump |

---

## Download & Installation (Standalone Launcher & Steam)

Prebuilt binaries (compiled automatically from source on GitHub Actions and verified by both DX11 smoke tests) are available without needing to build anything locally:

- **Latest Release:** Download from **[GitHub Releases](https://github.com/ElianMoirtide/LOTRO-UIE/releases/latest)** (`LOTRO-UIE-*-dxgi.zip` or `LOTRO-UIE-*-d3d11.zip`, including `SHA256SUMS.txt`).

### Install Steps
1. Make sure the LOTRO Launcher is set to the **64-bit client** and in-game graphics are set to **DirectX 11**.
2. **Recommended installation (especially for Steam + `GameOverlayRenderer64.dll`):**
   - Extract **`dxgi.dll`** and **`lotro_uie.ini`** from `LOTRO-UIE-*-dxgi.zip` (or copy them from `build/dist/dxgi/` if building locally) into the **`x64`** subfolder of your LOTRO installation (e.g. `...\Steam\steamapps\common\Lord of the Rings Online\x64\`, alongside `lotroclient64.exe`).
   - *(Important: Place the DLL **only** inside the `x64` subfolder — **not** in the root directory next to `LotroLauncher.exe`, otherwise the Qt launcher will also try to load the proxy DLL. Remove any leftover `d3d11.dll` from `x64` when using `dxgi.dll`.)*
3. **Alternative (without Steam Overlay):**
   - Extract `d3d11.dll` and `lotro_uie.ini` from `LOTRO-UIE-*-d3d11.zip` (or copy them from `build/dist/d3d11/`) into the `x64` subfolder.

---

## Per-Element UI Scaling (`[ElementScale]`)

If a specific UI element (e.g., the tiny *Level 50* badge on the character panel) is too small:

### Quick In-Game Method
1. Hover your mouse cursor over the target UI element.
2. Press **`Ctrl+PgUp`** / **`Ctrl+PgDn`** to scale it `+5%` / `-5%` live, or open **`Ctrl+F9`** and click **Adjust Hovered Element Scale**. Rules are saved automatically to `lotro_uie.ini`.

### Manual / Custom Offset Method
1. Press **`F9`** in-game to display the top-left HUD, hover the target UI element, and press **`Ctrl+F2`** to copy its `[ElementScale]` rule to the clipboard (or read `Hover:0x...` directly from the HUD).
2. Open `lotro_uie.ini` in the `x64` folder and paste/edit the rule under `[ElementScale]`:
   ```ini
   [ElementScale]
   CharLevelBadge = 0x8F3A12C49011B2E1, +15%
   ```
   *(Optional: specify pixel offsets and maximum quad size filter: `CharLevelBadge = 0x8F3A12C49011B2E1, +15%, 0, 0, 160, 60`)*
3. Save `lotro_uie.ini` and press **`Ctrl+F6`** in-game to reload immediately.

---

## Building & Testing (Visual Studio / CMake, x64)

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Run the automated smoke tests (verifying DX11 proxy stability, zero-warning shader compilation, sharpness presets, 2× Mini-CNN compute upscaling, offscreen UI render targets, `[ElementScale]`, MapMode, 3D World PostFX + frosted backdrop blur, and `.png`/`.dds`/`.bmp` capture exports) for both DLL modes:

```powershell
.\build\Release\lotro_uie_smoke_test.exe .\build\dist\d3d11\d3d11.dll
.\build\Release\lotro_uie_smoke_test.exe --dxgi .\build\dist\dxgi\dxgi.dll
```

Pushing a `v*` tag (or creating a new `v*` release tag in GitHub Releases) triggers [build-and-release.yml](.github/workflows/build-and-release.yml) via GitHub Actions to run both smoke tests and automatically attach versioned `.zip` packages + `SHA256SUMS.txt` to the GitHub Release.
