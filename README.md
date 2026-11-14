# LOTRO-UIE (UI Enhanced) — v0.5

DirectX 11 Proxy-, UI-Super-Resolution- & 3D-PostFX-Projekt für *The Lord of the Rings Online* (64-Bit Client), um verschwommene Schriften, UI-Panels, niedrig aufgelöste Karten und das 3D-Weltbild auf 1440p/4K-/HiDPI-Monitoren gezielt in der Render-Pipeline zu verbessern.

---

## Features in v0.5

- **2× Mini-CNN Compute Super-Resolution (`cs_5_0`) + VRAM-Cache (`Ctrl+F4`)**:
  - Skaliert UI-Texturen, Icons, Schrift-Atlanten und **Offscreen-UI-RenderTargets** (`UpscaleUiRenderTargets = true`, z. B. Textfenster bei `Global Zoom 3.00`) in Echtzeit auf `2×` Auflösung hoch.
  - Detail-erhaltende Architektur für kleine Glyphen (`<= 96 px` Kantenlänge): schützt dünne Buchstabenstriche, Serifen und `i`/`ä`/`ö`/`ü`-Punkte vor Ausdünnung.
  - Flackerfreies Dynamic-Throttling und automatische Dirty-Erkennung bei `Map`, `UpdateSubresource`, `CopyResource`, `OMSetRenderTargets` und `ClearRenderTargetView`.
- **Subpixel-UI- & Font-Reconstruction-Shader (`ps_4_0`, `F8` / `F7`)**:
  - Kantengerichtete 12-Tap-Rekonstruktion mit adaptivem Unsharp-Masking und 4 Schärfe-Presets (`Low 0.35`, `Med 0.70`, `High 1.05`, `Ultra 1.40` via `F7`).
  - **Map-Modus (`MapMode = true`)**: Beruhigt grobes Pergament-Korn auf hochskalierten Karten (`896×576`/`1024×768`), schützt aber gleichzeitig dünne Textstriche und halbtransparente Text-Overlays (z. B. *„Zum Wegzoomen mit …“*).
  - **Nameplate-Schärfung (`EnhanceNameplates = true`)**: Schärft schwebende 3D-Namen/Titel (`DepthEnable=TRUE, DepthWriteMask=ZERO`), ohne 3D-Geometrie anzufassen.
- **Interaktives In-Game Control Center (`Ctrl+F9`)**:
  - Ein direkt in DirectX 11 gerendertes, per Maus klickbares Steuerungsmenü im Spiel: Alle Features, Schärfe-Stufen, UI-Themen, Hover-Skalierung (`-5%` / `+5%`), Clipboard-Export und das Speichern in `lotro_uie.ini` lassen sich live per Mausklick bedienen — völlig unabhängig von der restriktiven LOTRO-Lua-Sandbox und charakterübergreifend ab dem Charakterauswahl-Bildschirm verfügbar.
- **Per-Element UI-Skalierung via Geometry Shader (`[ElementScale]`), Clipboard-Export (`Ctrl+F2`) & Live-Skalierung (`Ctrl+Bild↑`/`Ctrl+Bild↓`)**:
  - Einzelne UI-Elemente (z. B. das kleine *„Level 50“*-Feld im Charakterfenster oder kleine Icons/Plaketten) können gezielt vergrößert werden.
  - **1-Klick Clipboard-Export (`Ctrl+F2`)**: Bewege die Maus über ein UI-Element und drücke `Ctrl+F2` — LOTRO-UIE kopiert sofort eine fertige Konfigurationszeile (`Element_64x24_8F3A12 = 0x8F3A12C49011B2E1, +15%`) in die Windows-Zwischenablage!
  - **Live In-Game Skalierung (`Ctrl+Bild↑` / `Ctrl+Bild↓`)**: Vergrößert oder verkleinert das UI-Element unter dem Mauszeiger sofort live im Spiel um `+5%` / `-5%` und speichert die Regel automatisch in `lotro_uie.ini`.
- **Smart Hover-Lupe (`HoverMagnifier = true`, `ALT` gedrückt halten)**:
  - Hältst du im Spiel `ALT` gedrückt, wird genau das kompakte UI-Quad unter dem Mauszeiger (z. B. ein kleines Buff-Icon, Tooltip-Badge oder Münz-/Stufen-Feld) per Geometry-Shader-Bounding-Box dynamisch auf `1.25×` (`+25%`) vergrößert.
- **UI-Modernisierung (`[UiTheme]`)**:
  - **Font Backdrop & Outline Boost (`FontOutlineBoost = 0.35`)**: Erzeugt auf halbtransparenten Buchstabenrändern einen feinen dunklen Mikro-Kontrasthof, sodass helle Schriften (Questtracker, Chat, Namen) auch vor hellem Schnee oder Himmel gestochen scharf lesbar bleiben.
  - **Dark-Slate UI-Theme (`ThemeDarkSlate`)**: Verwandelt ausgewaschene grau-braune LOTRO-Fensterhintergründe über eine selektive Tonwertkurve in ein modernes, dunkles Schiefer-Design, während goldene Rahmen und helle Schriften leuchtend erhalten bleiben.
  - **OLED Auto-Dim (`OledAutoDim`)**: Dimmt statische obere/untere HUD-Leisten sanft auf `72%` Deckkraft ab, solange sich der Mauszeiger im mittleren 3D-Spielfeld befindet, und blendet sie bei Annäherung der Maus sofort wieder auf `100%` ein.
- **Custom HD Texture Replacement (`lotro_uie_overrides/`)**:
  - Lege eigene hochauflösende `.png`- oder `.dds`-Texturen unter ihrem Hash-Namen (`0x<16HexHash>.png` oder `.dds`) in den Ordner `x64/lotro_uie_overrides/` — LOTRO-UIE ersetzt die alte Spieltextur beim Rendern automatisch auf der GPU (live neu ladbar via `Ctrl+F6`).
- **Split-Screen A/B Live-Vergleich (`Ctrl+F3`)**:
  - Zeigt auf der linken Bildschirmhälfte das unveränderte Original-LOTRO und auf der rechten Hälfte LOTRO-UIE (inkl. Trennlinie & `< ORIG | UIE >`-Anzeige).
- **3D World-Only PostFX (`Ctrl+F5`) & 16× Anisotrope Welt-Sampler**:
  - Führt direkt **vor** dem ersten UI-Draw-Call Contrast Adaptive Sharpening (CAS), Midtone-Clarity, Vibrance und Sky-Debanding ausschließlich auf der 3D-Spielwelt aus (unabhängig davon, ob `F8` ein- oder ausgeschaltet ist).
  - Hebt 3D-Terrain-/Welt-Sampler automatisch auf `16× Anisotrop` und `MipLodBias = -0.35` an.
- **Komprimierter 32-Bit RGBA `.png` Asset-Dumper (`Ctrl+F11`)**:
  - Exportiert UI-Texturen ohne externe Bibliotheken direkt als verlustfreie RGBA8-`.png`-Dateien (ca. 85–92 % kleiner als `.bmp`) inkl. Textur-Hash-Zuordnung in `summary.txt` und `capture.json`.

---

## Standard-Hotkeys (LOTRO-kompatibel mit Modifiern)

Damit die LOTRO-eigenen Tasten für Gefährten-Auswahl (`F1`–`F6`) und das Systemmenü (`F10`) frei bleiben, nutzen die Diagnose- und Nebentasten standardmäßig `CTRL+`:

| Tastenkombination | Funktion |
| :--- | :--- |
| **`Ctrl+F9`** | **Interaktives In-Game Control Center** öffnen/schließen (per Maus klickbar) |
| **`F8`** | UI- & Schrift-Verbesserung ein-/ausschalten |
| **`F7`** | Schärfe-Preset durchschalten (`Low` → `Med` → `High` → `Ultra`) |
| **`F9`** | Diagnose-HUD oben links ein-/ausblenden (zeigt auch `Hover:0x...`) |
| **`Ctrl+F2`** | **Hover-Textur-Hash** als fertige `[ElementScale]`-Regel in die Zwischenablage kopieren |
| **`Ctrl+Bild↑` / `Ctrl+Bild↓`** | Gehovertes UI-Element live im Spiel **`+5%` / `-5%` skalieren** & in `lotro_uie.ini` speichern |
| **`ALT` (halten)** | **Smart Hover-Lupe**: Vergrößert das kompakte UI-Element unter der Maus live auf `1.25×` |
| **`Ctrl+F3`** | Split-Screen A/B-Vergleich (Links = Original, Rechts = LOTRO-UIE) |
| **`Ctrl+F4`** | 2× Mini-CNN Super-Resolution ein-/ausschalten |
| **`Ctrl+F5`** | 3D-Welt PostFX (CAS + Clarity + Vibrance + Deband) ein-/ausschalten |
| **`Ctrl+F6`** | `lotro_uie.ini` & `lotro_uie_overrides/` live im Spiel neu laden |
| **`Ctrl+F10`** | Frame-Diagnose-Capture (`summary.txt`, `draws.csv`, `capture.json`) |
| **`Ctrl+F11`** | Vollständiger Capture + `.png`/Shader-Dump |

---

## Installation (Standalone-Launcher & Steam)

1. Stelle sicher, dass im LOTRO-Launcher der **64-Bit-Client** und im Spiel unter Grafik **DirectX 11** aktiviert sind.
2. **Empfohlene Installation (insbesondere für Steam + `GameOverlayRenderer64.dll`):**
   - Kopiere **`dxgi.dll`** und **`lotro_uie.ini`** aus `build/dist/dxgi/` in den **`x64`**-Unterordner deiner LOTRO-Installation (z. B. `...\Steam\steamapps\common\Lord of the Rings Online\x64\`, dort wo `lotroclient64.exe` liegt).
   - *(Wichtig: Lege die DLL **nur** in den `x64`-Unterordner, **nicht** in das Hauptverzeichnis neben `LotroLauncher.exe`, da der Qt-Launcher im Hauptverzeichnis sonst ebenfalls versucht, die Proxy-DLL zu laden. Lösche eine eventuell noch im `x64`-Ordner liegende alte `d3d11.dll`, wenn du `dxgi.dll` verwendest.)*
3. **Alternative ohne Steam-Overlay:**
   - Kopiere `d3d11.dll` und `lotro_uie.ini` aus `build/dist/d3d11/` in den `x64`-Unterordner.

---

## Einzelne UI-Elemente vergrößern (`[ElementScale]`)

Wenn dir ein bestimmtes Element (z. B. *„Level 50“* im Charakterfenster) zu klein ist:
1. Drücke im Spiel **`F9`**, damit das HUD oben links sichtbar ist.
2. Bewege die Maus genau auf das gewünschte UI-Element.
3. Lies im HUD den Wert hinter `Hover:` ab, z. B. `Hover:0x8F3A12C49011B2E1(64x24)`.
4. Öffne `lotro_uie.ini` im `x64`-Ordner und füge unter `[ElementScale]` eine Zeile hinzu:
   ```ini
   [ElementScale]
   CharakterLevel = 0x8F3A12C49011B2E1, +15%
   ```
   *(Optional kannst du auch Versatz und maximale Quad-Größe in Pixeln angeben: `CharakterLevel = 0x8F3A12C49011B2E1, +15%, 0, 0, 160, 60`)*
5. Speichere `lotro_uie.ini` und drücke im Spiel **`Ctrl+F6`** — die Änderung ist sofort sichtbar!

---

## Bauen (Visual Studio / CMake, x64)

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Automatisierte Smoke-Tests (prüfen Proxy-Stabilität, Shader-Kompilierung, Schärfe-Presets, 2× Mini-CNN, Offscreen-UI-RTs, `[ElementScale]`, MapMode, 3D-WorldPostFX sowie `.png`/`.dds`/`.bmp`-Export):

```powershell
.\build\Release\lotro_uie_smoke_test.exe .\build\dist\d3d11\d3d11.dll
.\build\Release\lotro_uie_smoke_test.exe --dxgi .\build\dist\dxgi\dxgi.dll
```
