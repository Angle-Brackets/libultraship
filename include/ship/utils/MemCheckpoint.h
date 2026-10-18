#pragma once

// Startup memory checkpoints on Switch. The port defines SwitchDebug_MemCheckpoint; the weak
// reference keeps LUS linkable without it. Compiles away on other platforms.
#ifdef __SWITCH__
extern "C" __attribute__((weak)) void SwitchDebug_MemCheckpoint(const char* label);
#define LUS_MEM_CHECKPOINT(label)                \
    do {                                         \
        if (SwitchDebug_MemCheckpoint != nullptr) { \
            SwitchDebug_MemCheckpoint(label);    \
        }                                        \
    } while (0)
#else
#define LUS_MEM_CHECKPOINT(label) ((void)0)
#endif
