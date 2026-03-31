# CrossOS unfinished features tracker

This file tracks APIs that are still partially implemented or stubbed.

## Recently completed

- Linux file picker fallback implemented in `src/core/dialog.c` using `zenity` or `kdialog`.
- Renderer abstraction now supports backend availability probing and auto/software fallback behavior in `src/core/render.c`.
- Renderer OpenGL bootstrap implemented in `src/core/render.c`:
	- `crossos_renderer_create(..., CROSSOS_RENDER_BACKEND_OPENGL, ...)` now succeeds.
	- renderer exposes native render target via `crossos_renderer_get_native_target(...)`.
	- backend implementation/availability checks are now explicit.
- Core camera API now includes a functional virtual-camera fallback in `src/core/camera.c`
	when platform camera backends return unsupported.
- Scanner subsystem bootstrap added:
	- new public API in `include/crossos/scanner.h` (device enumeration, scan, film curves).
	- Linux SANE backend in `src/platform/linux/scanner_sane.c`.
	- Windows (`scanner_twain.c`) and Android (`scanner_android.c`) stubs with documented implementation path.
	- new `examples/film_scanner` demo app for preview + scan + curve/exposure UI.
- **OpenGL renderer backend implemented (v0.6.0)**:
	- Linux: GLX backend in `src/platform/linux/render_glx.c` (`glXCreateNewContext`, `glXMakeCurrent`, `glXSwapBuffers`).
	- Windows: WGL backend in `src/platform/windows/render_wgl.c` (`wglCreateContext`, `wglMakeCurrent`, `SwapBuffers`).
	- Android: EGL stub in `src/platform/android/render_egl.c` (documented implementation path, returns `CROSSOS_ERR_UNSUPPORT`).
	- New public API: `crossos_renderer_make_current()`, `crossos_renderer_present()`, `crossos_renderer_get_gl_context()`.
	- `CROSSOS_RENDER_BACKEND_AUTO` now selects OpenGL on Linux and Windows.

## Remaining high-priority stubs

### Rendering

- OpenGL backend full implementation: resource APIs (textures, pipelines, command buffers), swap interval, context sharing.
- Vulkan backend implementation behind `CROSSOS_RENDER_BACKEND_VULKAN`.
- Android EGL/GLES2 backend in `src/platform/android/render_egl.c` (see documented implementation path in that file).

### Camera

- Windows Media Foundation capture path in `src/platform/windows/camera_win32.c`.
- Android Camera2 capture path in `src/platform/android/camera_android.c`.
- Virtual fallback exists for unsupported platforms; remaining work is native hardware capture parity.

### Bluetooth

- Android Bluetooth JNI backend in `src/platform/android/bluetooth_android.c`.
- Linux Bluetooth support when BlueZ is unavailable (currently unsupported fallback in `src/platform/linux/bluetooth_linux.c`).

### Networking

- WebSocket TLS (`wss://`) support in `src/core/websocket.c` (currently requires OpenSSL integration).

### Dialogs

- Android native picker implementation for `crossos_dialog_pick_files(...)`.

### Scanner

- Windows WIA/TWAIN backend implementation in `src/platform/windows/scanner_twain.c`.
- Android USB backend implementation in `src/platform/android/scanner_android.c`.
- Linux SANE backend improvements:
	- asynchronous scan progress callback.
	- exact 16-bit pipeline validation per backend frame format.
	- model-specific defaults for Plustek OpticFilm variants.


## Suggested execution order

1. Android EGL/GLES2 renderer backend (unblocks OpenGL on mobile).
2. Camera Windows + Android real implementations.
3. Android Bluetooth backend.
4. WebSocket TLS support.
5. Android native file picker.
