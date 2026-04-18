# Android port — driver + JNI

The C driver is designed to be dropped into an Android app as a native
library. The USB layer stays `libusb-1.0`; only the device-open path
differs because Android apps cannot call `libusb_open()` directly.

## What ships in `driver/jni/`

| File                   | Purpose                                        |
|------------------------|------------------------------------------------|
| `picoscope_jni.c`      | JNI wrappers over the public C API             |
| `PicoScope2204A.java`  | Kotlin/Java-friendly class mapping the wrappers |

The final `libpicoscope_jni.so` statically links `libpicoscope2204a.a`,
so the APK only needs one native library (plus `libusb-1.0.so`, which
NDK builds ship).

## Building

```bash
cmake -S driver -B driver/build-android \
      -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a \
      -DANDROID_PLATFORM=android-24 \
      -DPS2204A_BUILD_JNI=ON
cmake --build driver/build-android
```

Repeat per ABI (`armeabi-v7a`, `x86_64`) and copy each `libpicoscope_jni.so`
into `app/src/main/jniLibs/<abi>/`.

## USB permission + open flow

Android does not expose raw `libusb_open()` — apps receive a Linux file
descriptor via `UsbManager`. The driver has a dedicated entry point
for this:

```c
ps_status_t ps2204a_open_with_fd(ps2204a_device_t **dev, int usb_fd);
```

Typical Kotlin flow:

```kotlin
val device = usbManager.deviceList.values.first {
    it.vendorId == 0x0CE9 && it.productId == 0x1007
}
// 1. Request permission (intent-based)
usbManager.requestPermission(device, pendingIntent)

// 2. After the broadcast arrives:
val connection = usbManager.openDevice(device)
val fd = connection.fileDescriptor           // <-- pass this to JNI
scope.openWithFd(fd)                         // calls ps2204a_open_with_fd
```

The Java side `PicoScope2204A.openWithFd(int fd)` returns immediately;
do the firmware upload + FPGA init on a background thread because it
takes ~2 s.

## Firmware on Android

Same four blobs as Linux — they must live somewhere the native side can
`fopen()`. Recommended layout:

```
app/src/main/assets/firmware/
    fx2_firmware.bin
    fpga_firmware.bin
    ep06_000_32768B.bin … ep06_007_8192B.bin
```

At first launch, copy the assets to the app's private files directory
and set the env var `PS2204A_FIRMWARE_DIR` (or wire up
`find_firmware_dir()` in `picoscope2204a.c` to check
`getFilesDir()/firmware`).

> The firmware blobs are **not** distributed in this repository — each
> end-user extracts them from their own device with the provided tool
> (`tools/firmware-extractor/`) and the APK packages the resulting
> files. Do not ship Pico Technology firmware unless your distribution
> channel is legally covered for it.

## Re-enumeration caveat

After the FX2 firmware upload the device disappears from the USB bus
for ~1 s and reappears with a new address. On Linux `libusb` handles
this transparently; on Android the `UsbDevice` object that generated
your original permission grant becomes stale, so the FD you held is
also stale.

The driver does the re-find internally using the `libusb_device_handle`
it already has. If you cache the `UsbDevice` on the Kotlin side
(unusual) you will need to refresh it from `usbManager.deviceList`
after `ps2204a_open_with_fd()` returns.

## Troubleshooting

| Symptom                             | Cause                                          | Fix                                     |
|-------------------------------------|------------------------------------------------|-----------------------------------------|
| `-12` (`LIBUSB_ERROR_NOT_SUPPORTED`) | `usbfs_capabilities` disabled on the device    | Requires root or a recent kernel (≥5.2) |
| Status `0x7b` on every call          | Previous session left state; FX2 upload skipped| Always re-upload FX2 on `openWithFd`    |
| `libusb_submit_transfer` EIO         | App lost focus → Android detached USB          | Re-request permission, re-open          |
| Long hang on first capture           | Firmware upload blocks the caller ~2 s         | Run `openWithFd` off the UI thread      |

See [`protocol.md`](protocol.md) for the wire-level protocol — it is
100 % identical between the Linux and Android builds.
