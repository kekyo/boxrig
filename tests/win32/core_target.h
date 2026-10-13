/* Verify the SDK contract used to build the portable Windows consumers. */
#include <sdkddkver.h>

#ifdef _WIN64
#define BOXRIG_TEST_CORE_WINVER 0x0502
#define BOXRIG_TEST_CORE_NTDDI 0x05020000
#else
#define BOXRIG_TEST_CORE_WINVER 0x0500
#define BOXRIG_TEST_CORE_NTDDI 0x05000000
#endif

#if WINVER != BOXRIG_TEST_CORE_WINVER || \
    _WIN32_WINNT != BOXRIG_TEST_CORE_WINVER || \
    NTDDI_VERSION != BOXRIG_TEST_CORE_NTDDI
#error Portable core consumers must target the architecture-specific minimum OS
#endif
