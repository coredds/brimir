// volk's implementation, compiled as C++ so VOLK_NAMESPACE puts every vk* pointer in namespace volk
// (see vendor/volk/BRIMIR_VENDOR.md). volk.c defines the device-level pointers too, so it needs their declarations.
#undef VOLK_NO_DEVICE_PROTOTYPES
#define VOLK_IMPLEMENTATION
#include <volk.h>
