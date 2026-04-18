/* Compatibility shim.
 *
 * On Linux desktop, libusb's header sits at <libusb-1.0/libusb.h> (the
 * conventional system-install layout). libusb-cmake exposes it as plain
 * <libusb.h> through its target_include_directories. This shim lets the
 * single shared driver source compile against both without an #ifdef. */
#include <libusb.h>
