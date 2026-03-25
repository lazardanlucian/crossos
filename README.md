# CrossOS

A cross-platform **display driver / input SDK** written in C that lets you write a single application and produce separate builds for **Windows**, **Linux**, and **Android** – with no porting required.

CrossOS now also includes utility modules for file I/O, HTTP API calls, basic audio playback, optical-disc helpers, and a lightweight software UI toolkit.

On Linux, CrossOS now auto-selects a runtime backend:

- X11 when `DISPLAY` is available
- terminal rendering when running inside an interactive TTY

That fallback stays transparent to application code: the same window, surface, input, and renderer APIs continue to work.

## Features

| Feature | Windows | Linux | Android |
|---|---|---|---|
| Native window creation | Win32 | X11 / terminal fallback | ANativeWindow |
| Software framebuffer | GDI DIBSection | XPutImage / ANSI terminal renderer | ANativeWindow_lock |
| Touch input (multi-touch) | WM_TOUCH / WM_POINTER | XInput2 | AInputEvent |
| Keyboard events | WM_KEYDOWN/UP | XKeyPress/Release / terminal raw input | N/A (on-screen KB) |
| Mouse / pointer events | WM_LBUTTON, WM_MOUSEMOVE | ButtonPress/Release, MotionNotify / terminal mouse reporting | AMotionEvent |
| Scroll events | WM_MOUSEWHEEL | ButtonPress (Button4/5) | Swipe gestures |
| Full-screen mode | ✔ | ✔ (_NET_WM_STATE) | always full-screen |
| Multi-monitor query | ✔ (SM_CMONITORS) | ✔ (ScreenCount) | N/A |
| Native handle escape | `HWND` | `Window` (XID) | `ANativeWindow*` |
| File I/O helpers | ✔ | ✔ | ✔ |
| HTTP API requests | ✔ (libcurl) | ✔ (libcurl) | ✔ (libcurl, if linked) |
| Audio playback | ✔ (PlaySound) | ✔ (`aplay`/`paplay`) | tone playback |
| Optical device scan | ✔ | ✔ | USB host (OTG) |
| Optical burn progress API | simulated | simulated | real USB backend + simulated fallback |
| Optical backend plug-in API | ✔ | ✔ | ✔ |
| SDK drawing primitives | ✔ | ✔ | ✔ |
| CrossOS renderer abstraction (AUTO/software/OpenGL bootstrap) | ✔ | ✔ | ✔ |
| SDK UI widgets (label/button/list/progress/dropdown/tree header) | ✔ | ✔ | ✔ |
| Native file picker API | ✔ | ✔ (zenity/kdialog fallback) | planned |
| UI layout helper (responsive column flow) | ✔ | ✔ | ✔ |
| Camera API fallback on unsupported backend | virtual | virtual | virtual |
| Scanner API (enumeration + scan + film-curve presets) | planned (TWAIN/WIA stub) | SANE backend | planned (USB backend stub) |

---

## Project layout

```
crossos/
├── include/crossos/        Public API headers
│   ├── crossos.h           Main umbrella – include this one
│   ├── types.h             Shared types, enums, event structs
│   ├── window.h            Window management
│   ├── input.h             Event polling, key codes, touch queries
│   ├── display.h           Software framebuffer + display info
│   ├── draw.h              Software drawing primitives (rectangles, text)
│   ├── ui.h                Immediate-mode UI widgets + responsive scaling
│   ├── dialog.h            Native file picker abstraction
│   ├── file.h              File I/O helpers
│   ├── web.h               HTTP request helpers
│   ├── audio.h             Basic audio playback
│   ├── optical.h           Optical-drive and burn-progress helpers
│   └── scanner.h           Scanner + film-processing APIs
│
├── src/
│   ├── core/init.c         Platform-agnostic lifecycle
│   ├── core/file.c         File I/O implementation
│   ├── core/web.c          HTTP public API dispatcher
│   ├── core/web_backend_*.c HTTP transport backends (curl/fallback)
│   ├── core/audio.c        Audio helpers implementation
│   ├── core/draw.c         Drawing helpers implementation
│   ├── core/ui.c           UI helpers implementation
│   ├── core/dialog.c       File picker implementation (Windows first)
│   ├── core/optical.c      Optical device + simulated burn implementation
│   └── platform/
│       ├── windows/        Win32 backend
│       ├── linux/          X11 / XInput2 backend
│       └── android/        Android NDK backend
│
├── examples/hello_world/   Minimal gradient + event-logger app
├── examples/disc_burner/   File browser + burn queue + progress UI demo
├── examples/film_scanner/  Scanner UI demo (preview/scan + film curves)
├── tests/                  Headless unit tests (no display needed)
├── cmake/                  CMake toolchain files
├── .devcontainer/          Dev-container for GitHub Codespaces / VS Code
└── CMakeLists.txt
```

---

## Optical Burn Backends

CrossOS now supports a pluggable optical backend via `crossos_optical_set_backend(...)`.

- If a backend is registered and supports burning, `crossos_optical_burn_start(...)` uses it.
- If no backend is available, it falls back to simulated burn flow.

The Android app in this repository now registers a USB-host backend at startup.

- It requests USB device permission when needed.
- It detects USB mass-storage optical drives, probes media presence/capacity when possible, and starts a physical burn worker.
- The disc burner example now auto-packages queued files/folders into a temporary ISO before starting the physical burn.
- Current Android backend writes a single disc-image file (`.iso`, `.img`, `.bin`) to the selected USB optical drive.
- If backend startup fails, CrossOS still falls back to simulated burn flow so UX can continue.

## Quick start

### Linux (native)

```bash
# Prerequisites (Ubuntu/Debian):
# sudo apt-get install -y cmake build-essential
# Optional for the X11 backend:
# sudo apt-get install -y libx11-dev libxext-dev libxi-dev libxrandr-dev libxinerama-dev libxcursor-dev
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
./hello_world
```

Linux backend selection:

- Default: `X11` when `DISPLAY` is set, otherwise terminal mode on a TTY
- Force terminal mode: `CROSSOS_LINUX_BACKEND=terminal ./hello_world`
- Force X11: `CROSSOS_LINUX_BACKEND=x11 ./hello_world`

**Terminal Mode (Character-based UI rendering):**

The terminal backend converts the pixel-based framebuffer into a character grid using Unicode block elements (█) and colors via ANSI truecolor escape sequences. This allows any CrossOS application to run in the terminal without modification:

- Window resolution is scaled down to character cells (typically 8-16 pixels per character)
- Each character cell gets the average color of its pixel region via box-filter sampling
- Supports keyboard input (raw terminal mode, arrow keys, function keys)
- Supports mouse reporting (SGR protocol if terminal supports it)
- Detects window resize events via SIGWINCH
- Works in both graphical terminal emulators and headless SSH sessions

The existing `crossos_draw_*` API works transparently: apps draw pixels normally, and the terminal backend samples the framebuffer into character cells during present.

### Windows (cross-compile from Linux)

```bash
# Prerequisites: mingw-w64
cmake -B build-win \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-win
# Copy build-win/hello_world.exe to a Windows machine and run it.
```

### Android (NDK library only)

```bash
# Prerequisites: Android NDK r26+ at /opt/android-ndk
cmake -B build-android \
      -DCMAKE_TOOLCHAIN_FILE=/opt/android-ndk/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a \
      -DANDROID_PLATFORM=android-26 \
      -DCROSSOS_BUILD_EXAMPLES=OFF \
      -DCROSSOS_BUILD_TESTS=OFF
cmake --build build-android
# Produces libcrossos.a; link it into an Android NativeActivity project.
```

### Android APKs (disc burner + hello world)

```bash
cd android
./gradlew assembleDebug
# APK outputs:
# android/app/build/outputs/apk/discBurner/debug/app-discBurner-debug.apk
# android/app/build/outputs/apk/helloWorld/debug/app-helloWorld-debug.apk
```

If you are in Codespaces, use the file explorer to download
both APKs from `android/app/build/outputs/apk/...` and install them on your device.

---

## API overview

```c
#include <crossos/crossos.h>

/* 1. Initialise */
crossos_init();

/* 2. Create a window */
crossos_window_t *win = crossos_window_create("My App", 800, 600,
                                               CROSSOS_WINDOW_RESIZABLE);
crossos_window_show(win);

/* 3. Software framebuffer */
crossos_surface_t *surf = crossos_surface_get(win);
crossos_framebuffer_t fb;
if (crossos_surface_lock(surf, &fb) == CROSSOS_OK) {
    /* Write BGRA pixels to fb.pixels */
    crossos_surface_unlock(surf);
    crossos_surface_present(surf);
}

/* 4. Event loop */
static void on_event(const crossos_event_t *ev, void *ud) {
    if (ev->type == CROSSOS_EVENT_QUIT) crossos_quit();
    if (ev->type == CROSSOS_EVENT_TOUCH_BEGIN)
        printf("touch at (%.0f, %.0f)\n",
               ev->touch.points[0].x, ev->touch.points[0].y);
}
crossos_run_loop(win, on_event, NULL);

/* 5. Clean up */
crossos_window_destroy(win);
crossos_shutdown();
```

For hardware-accelerated rendering (Vulkan, OpenGL ES), retrieve the native window handle and create your own surface:

```c
void *native = crossos_window_get_native_handle(win);
/* Windows → HWND, Linux → Window (XID), Android → ANativeWindow* */
```

---

## Event reference

| Event type | Payload field | Description |
|---|---|---|
| `CROSSOS_EVENT_QUIT` | – | App-wide quit signal |
| `CROSSOS_EVENT_WINDOW_CLOSE` | – | X button / back button |
| `CROSSOS_EVENT_WINDOW_RESIZE` | `ev.resize.{width,height}` | Client area resized |
| `CROSSOS_EVENT_KEY_DOWN/UP` | `ev.key.{keycode,scancode,mods,repeat}` | Physical key press |
| `CROSSOS_EVENT_POINTER_DOWN/UP/MOVE` | `ev.pointer.{x,y,button}` | Mouse / trackpad |
| `CROSSOS_EVENT_POINTER_SCROLL` | `ev.pointer.{scroll_x,scroll_y}` | Scroll wheel |
| `CROSSOS_EVENT_TOUCH_BEGIN/UPDATE/END` | `ev.touch.{count,points[]}` | Multi-touch |

---

## Dev container (Codespaces / VS Code)

The repository ships with a `.devcontainer/` configuration that installs:

- `cmake`, `ninja`, `gcc`, `clang`, `clang-format`, `clang-tidy`
- X11 / XInput2 dev libraries for Linux builds
- `mingw-w64` for Windows cross-compilation
- Android NDK r26 for Android cross-compilation

Open the repository in **GitHub Codespaces** or **VS Code with the Dev Containers extension** and the environment is ready automatically.

---

## Running tests

Tests are headless (no display required):

```bash
cmake -B build -DCROSSOS_BUILD_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure
```

## Build all platforms/apps (SDK compile check)

Use the repository script to rebuild all major targets after SDK updates:

```bash
./scripts/build_all.sh --clean
```

What it runs by default:

- Linux CMake build + tests
- Windows cross-compile build (mingw toolchain)
- Android NDK CMake build (`libcrossos.a`)
- Android Gradle APK build (`assembleDebug`)

Binary artifact outputs:

- `artifacts/linux/` (Linux executables)
- `artifacts/windows/` (`*.exe` files)

APK artifact outputs:

- `artifacts/android-apk/disc_burner.apk`
- `artifacts/android-apk/hello_world.apk`

Useful options:

```bash
./scripts/build_all.sh --stop-on-error
./scripts/build_all.sh --android-apk-only
./scripts/build_all.sh --no-android-apk
```

Environment overrides:

- `BUILD_TYPE` (default `Release`)
- `ANDROID_NDK` (default `/opt/android-ndk`)
- `ANDROID_ABI` (default `arm64-v8a`)
- `ANDROID_PLATFORM` (default `android-26`)

---

## Why not Electron / SDL?

- **Electron** packages a browser and a website – great for web tech, but not for native C/Rust code.
- **SDL2** is excellent but brings audio/joystick/OpenGL dependencies. CrossOS is deliberately minimal: just window + framebuffer + input.
- **C (or Rust via FFI)** compiles natively to all targets including Android (via NDK), producing small, dependency-free binaries.

---

## Roadmap

- [ ] Wayland backend (Linux)
- [ ] macOS / iOS backend (Cocoa / UIKit)
- [ ] OpenGL ES / Vulkan surface helper
- [ ] Clipboard read/write
- [ ] IME / text-input events
- [ ] Android Studio project template
