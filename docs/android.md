# Android port — `android-lib` module

The repository ships a Gradle module at `android-lib/` that packages
the C driver + a JNI shim + the `PicoScope2204A` Java bindings as an
Android library (`.aar`). Consumer apps pull it in via JitPack, no
submodule or manual NDK wiring required.

## Pulling the library into an Android app

```kotlin
// settings.gradle.kts
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven("https://jitpack.io")       // ← add this
    }
}

// app/build.gradle.kts
dependencies {
    implementation("com.github.Wasabules:picoscope-libusb:v0.1.0")
}
```

JitPack pulls the tagged commit, runs the build defined in
`jitpack.yml`, and serves the resulting AAR. Your app code then just:

```kotlin
import io.github.wasabules.ps2204.PicoScope2204A

val device = usbManager.deviceList.values.first {
    it.vendorId == 0x0CE9 && it.productId == 0x1007
}
usbManager.requestPermission(device, pendingIntent)

// After the permission broadcast:
val connection = usbManager.openDevice(device)
val handle = PicoScope2204A.nativeOpen(connection.fileDescriptor)

PicoScope2204A.nativeSetChannel(handle,
    PicoScope2204A.CHANNEL_A, true,
    PicoScope2204A.DC, PicoScope2204A.RANGE_5V)
PicoScope2204A.nativeSetTimebase(handle, 5, 1000)
val samples: FloatArray? = PicoScope2204A.nativeCaptureBlock(handle, 1000)

PicoScope2204A.nativeClose(handle)
```

Note the package: `io.github.wasabules.ps2204` — deliberately neutral
so there is no namespace collision or trademark claim with
`com.picotech` / `com.picoscope` packages.

## USB permission + open flow

Android does not expose raw `libusb_open()` — apps must go through
`UsbManager` to get a file descriptor. The JNI layer has a dedicated
entry point `nativeOpen(fd)` that calls the driver's
`ps2204a_open_with_fd()`. Typical flow:

1. Find the `UsbDevice` matching VID `0x0CE9` / PID `0x1007`.
2. `usbManager.requestPermission()` and wait for the broadcast.
3. `usbManager.openDevice(device)` → `UsbDeviceConnection`.
4. Pass `connection.fileDescriptor` to `PicoScope2204A.nativeOpen()`.
5. Do the firmware upload + FPGA init **on a background thread** — it
   takes ~2 s and must not run on the UI thread.

In your app's `AndroidManifest.xml`:

```xml
<uses-feature android:name="android.hardware.usb.host" android:required="true"/>

<activity ...>
    <intent-filter>
        <action android:name="android.hardware.usb.action.USB_DEVICE_ATTACHED"/>
    </intent-filter>
    <meta-data
        android:name="android.hardware.usb.action.USB_DEVICE_ATTACHED"
        android:resource="@xml/device_filter"/>
</activity>
```

And `res/xml/device_filter.xml`:

```xml
<resources>
    <usb-device vendor-id="3305" product-id="4103"/>
    <!-- 3305 = 0x0CE9, 4103 = 0x1007 -->
</resources>
```

## Firmware on Android

Four blobs must be extracted from a device the user owns — see
[firmware-extraction.md](firmware-extraction.md). **Never bundle them
in the APK**: they are copyrighted by Pico Technology Ltd and
redistributing them violates copyright. F-Droid will reject the APK,
and Play Store listings that include them are liable to takedown.

Recommended UX:

1. On first launch, detect missing firmware (driver returns an explicit
   "firmware directory not found" error).
2. Show a wizard that walks the user through one of:
   - copying the files from their desktop via ADB / Share Sheet;
   - running the companion desktop extractor at
     `tools/firmware-extractor/` and transferring the result.
3. Store the blobs in the app's private files dir:
   `getFilesDir()/firmware/`.
4. Either set the env var `PS2204A_FIRMWARE_DIR` before
   `nativeOpen()` (simplest) or patch `find_firmware_dir()` in
   `picoscope2204a.c` to check `getFilesDir()` directly.

## Re-enumeration caveat

After the FX2 firmware upload the device disappears from the USB bus
for ~1 s and reappears with a new address. On Linux `libusb` handles
this transparently; on Android the `UsbDevice` object that produced
your original permission grant becomes stale, so the FD you held is
also stale.

The C driver does the re-find internally using the
`libusb_device_handle` it already has, so from the app's perspective
`nativeOpen()` returns only when the device is fully ready. If you
cached the `UsbDevice` on the Kotlin side (unusual), refresh it from
`usbManager.deviceList` after `nativeOpen()` returns.

## Building the library locally

You don't need to build this yourself when using JitPack — but for
iteration:

```bash
# From the repo root, with the Android SDK + NDK installed.
gradle :android-lib:assembleRelease
# → android-lib/build/outputs/aar/android-lib-release.aar
```

Target ABIs are `arm64-v8a`, `armeabi-v7a`, `x86_64` (set in
`android-lib/build.gradle.kts`). Add `x86` if you need to support older
emulator images.

## Troubleshooting

| Symptom                              | Cause                                    | Fix                                       |
|--------------------------------------|------------------------------------------|-------------------------------------------|
| `UnsatisfiedLinkError: nativeOpen`   | R8 stripped the native methods           | Library ships `consumer-rules.pro` — verify your `minifyEnabled=true` picks it up |
| `-12` (`LIBUSB_ERROR_NOT_SUPPORTED`) | `usbfs_capabilities` disabled            | Requires root or a recent kernel (≥5.2)   |
| Status `0x7b` on every call          | Previous session left state              | Always re-upload FX2 on `nativeOpen`      |
| `libusb_submit_transfer` EIO         | App lost focus → Android detached USB    | Re-request permission, re-open            |
| UI hangs ~2 s on first capture       | Firmware upload blocks `nativeOpen`      | Run `nativeOpen` off the UI thread        |

See [`protocol.md`](protocol.md) for the wire-level USB protocol — it
is identical between the Linux and Android builds.
