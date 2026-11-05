#include "recovery_state.h"

#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments || argumentCount < 2) {
        if (arguments) LocalFree(arguments);
        return 2;
    }

    const DWORD parentId = wcstoul(arguments[1], nullptr, 10);
    LocalFree(arguments);
    if (parentId == 0) return 2;

    wchar_t mappingName[96]{};
    MakeRecoveryMappingName(parentId, mappingName, _countof(mappingName));

    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentId);
    if (!parent) return 3;

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mappingName);
    if (!mapping) {
        CloseHandle(parent);
        return 4;
    }

    auto* state = static_cast<RecoveryState*>(MapViewOfFile(
        mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(RecoveryState)));
    if (!state) {
        CloseHandle(mapping);
        CloseHandle(parent);
        return 5;
    }

    const DWORD waitResult = WaitForSingleObject(parent, INFINITE);
    bool restored = false;
    const bool needsRestore = waitResult == WAIT_OBJECT_0 && state->signature == kRecoverySignature &&
        InterlockedCompareExchange(&state->filterActive, 0, 0) != 0;
    if (needsRestore) {
        // The UI process disappeared while the filter was active. Restore the exact
        // color effect and magnification transform that it saved before applying tint.
        if (MagInitialize()) {
            const BOOL transformRestored = MagSetFullscreenTransform(state->originalMagnification,
                state->originalXOffset, state->originalYOffset);
            const BOOL effectRestored = MagSetFullscreenColorEffect(&state->originalEffect);
            const BOOL uninitialized = MagUninitialize();
            restored = transformRestored && effectRestored && uninitialized;
            if (restored) InterlockedExchange(&state->filterActive, 0);
        }
    }

    UnmapViewOfFile(state);
    CloseHandle(mapping);
    CloseHandle(parent);
    if (waitResult != WAIT_OBJECT_0) return 6;
    return needsRestore && !restored ? 7 : 0;
}
