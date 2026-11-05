#pragma once

#include <windows.h>
#include <magnification.h>
#include <stdio.h>

constexpr DWORD kRecoverySignature = 0x524C4754; // "RLGT"

struct RecoveryState {
    DWORD signature;
    volatile LONG filterActive;
    MAGCOLOREFFECT originalEffect;
    float originalMagnification;
    int originalXOffset;
    int originalYOffset;
};

inline void MakeRecoveryMappingName(DWORD processId, wchar_t* buffer, size_t capacity) {
    swprintf_s(buffer, capacity, L"Local\\Redlight-Recovery-%lu", processId);
}
