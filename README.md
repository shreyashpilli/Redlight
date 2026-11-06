# Redlight

Redlight is a small Windows desktop accessibility filter. It uses the Windows full-screen Magnification API to apply a color transformation to the desktop. It does not create a tinted overlay window, so it does not cover the screen with a separate layer or intercept mouse clicks.

## Build

Requirements:

- Windows 10 or 11
- 64-bit Windows SDK
- Visual Studio 2022 C++ build tools
- CMake 3.24 or newer

From the repository root, run:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --install build --config Release --prefix dist/Redlight
```

Keep `Redlight.exe` and `RedlightRecovery.exe` in the same folder. Launch `dist/Redlight/Redlight.exe`.

## Use

- **Enable red filter** applies the current transform to the desktop.
- **Intensity** adjusts it from 0% to 100% and saves the value between launches.
- **Red-only filter** preserves the red channel and progressively reduces green and blue.
- **Red tint blend** adds a red bias while reducing green and blue. It is a simple red-tint approximation, not a claim of pixel-for-pixel matching with Apple's implementation.
- **Ctrl+Alt+R** toggles the filter by default. Use **Change shortcut** to choose another global combination.
- **Ctrl+Alt+Shift+R** is a fixed emergency shortcut that restores the saved desktop colors immediately.
- The tray menu can enable or disable the filter, reopen settings, or exit. The filter stays active while the settings window is minimized or hidden to the tray.
- **Start with Windows** adds or removes a per-user startup entry. **Minimize to system tray** controls whether minimizing or closing the settings window hides it instead of exiting.
- **Restore original colors** restores the color transform and magnification level saved when the filter was enabled.

The filter starts off each time the app launches. This avoids leaving an unexpected color effect active after Windows signs in.

## Recovery and limitations

Redlight saves the active full-screen color effect and magnification transform before changing them. Normal disable, app exit, and the emergency shortcut restore those saved values. A small companion process waits without polling and attempts the same restoration if `Redlight.exe` exits unexpectedly while the filter is active. If both processes are forcibly terminated, Windows crashes, or the display driver stops responding, restoration cannot be guaranteed. Signing out or restarting Windows clears the session state.

The full-screen Magnification API is supported on Windows 8 and later and is not supported by 32-bit applications running on 64-bit Windows. This project therefore builds x64 only. The API changes a shared Windows full-screen magnifier transform, so using it at the same time as Windows Magnifier or another app that changes the Magnification API's full-screen effect can conflict. Secure desktop screens, protected video, HDR output, and particular display drivers may not be recolored consistently. Test those cases on the target PC.

Microsoft documents that `MagSetFullscreenColorEffect` applies its 5-by-5 matrix to the full-screen magnifier and that the full-screen API needs initialization with `MagInitialize`. See the [full-screen color effect reference](https://learn.microsoft.com/en-us/windows/win32/api/magnification/nf-magnification-magsetfullscreencoloreffect), [Magnification API overview](https://learn.microsoft.com/en-us/windows/win32/winauto/magapi/magapi-intro), and [Magnification API requirements](https://learn.microsoft.com/en-us/windows/win32/winauto/magapi/entry-magapi-sdk).
