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

## Remaining high-priority stubs

### Rendering

- OpenGL backend full implementation behind `CROSSOS_RENDER_BACKEND_OPENGL` (context creation, swap, resources).
- Vulkan backend implementation behind `CROSSOS_RENDER_BACKEND_VULKAN`.
- Backend-specific present/swap and resource APIs (textures, pipelines, command buffers).

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

## Suggested execution order

1. Renderer OpenGL backend (unblocks richer 2D/3D apps quickly).
2. Camera Windows + Android real implementations.
3. Android Bluetooth backend.
4. WebSocket TLS support.
5. Android native file picker.
