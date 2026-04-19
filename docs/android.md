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
    implementation("com.github.Wasabules:picoscope-libusb:v0.1.9")
}
```

JitPack pulls the tagged commit, runs the build defined in
`jitpack.yml`, and serves the resulting AAR. The Java API mirrors the
C driver — see [`PicoScope2204A.java`](../android-lib/src/main/java/io/github/wasabules/ps2204/PicoScope2204A.java)
for the full surface (block + dual-channel capture, all three streaming
modes, triggers, range calibration, signal generator).

Note the package: `io.github.wasabules.ps2204` — deliberately neutral
so there is no namespace collision or trademark claim with
`com.picotech` / `com.picoscope` packages.

## USB permission + open flow (two-phase)

Android does not expose raw `libusb_open()` — apps must go through
`UsbManager` for a file descriptor. And because the FX2 firmware upload
re-enumerates the device, the original fd gets invalidated mid-open,
and the Android sandbox forbids scanning `/dev/bus/usb` to find the
post-renum device. The library handles this with a **two-phase open**
driven by the app:

```kotlin
import io.github.wasabules.ps2204.PicoScope2204A

// --- Stage 1 -------------------------------------------------------
val device = usbManager.deviceList.values.first {
    it.vendorId == 0x0CE9 && it.productId == 0x1007
}
usbManager.requestPermission(device, pendingIntent)
// after the permission broadcast fires:
val conn1 = usbManager.openDevice(device)
val handle = PicoScope2204A.nativeOpenStage1(conn1.fileDescriptor)
conn1.close()       // Android emits USB_DEVICE_DETACHED → ATTACHED

// --- Wait for the re-attach broadcast, then: ----------------------
val device2 = /* the newly-attached UsbDevice (same VID/PID) */
usbManager.requestPermission(device2, pendingIntent)
val conn2 = usbManager.openDevice(device2)
val rc = PicoScope2204A.nativeOpenStage2(handle, conn2.fileDescriptor)
if (rc != 0) {
    PicoScope2204A.nativeClose(handle)   // still needed on failure
    throw IOException("stage2 failed: $rc")
}

// --- Device fully ready -------------------------------------------
PicoScope2204A.nativeSetChannel(handle,
    PicoScope2204A.CHANNEL_A, true,
    PicoScope2204A.DC, PicoScope2204A.RANGE_5V)
PicoScope2204A.nativeSetTimebase(handle, 5, 1000)
val samples = PicoScope2204A.nativeCaptureBlock(handle, 1000)

PicoScope2204A.nativeClose(handle)
```

Run stage 1 **on a background thread** — the FX2 upload takes ~1 s.
Stage 2 is fast (ADC init + FPGA bitstream + ~200 ms of waveform LUT
upload) but should also stay off the UI thread.

`nativeOpen(fd)` still exists for environments that don't trigger
re-enumeration (e.g. a device already in post-firmware state), but new
code should use the two-phase flow — it's the only reliable path from
a cold plug.

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

## 16 KB page-size alignment (Android 15+)

Pixel 8/9 and certain tablets ship Android 15 with a 16 KB memory page
size. Any shared library loaded on those devices must be linked with
`-Wl,-z,max-page-size=16384` — we already pass this for `libpicoscope_jni.so`
in `android-lib/src/main/cpp/CMakeLists.txt`, so consumers don't have
to do anything. If you re-link the JNI shim manually, keep the flag.

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
| UI hangs ~2 s on first capture       | Firmware upload blocks `nativeOpenStage1`| Run stage 1 off the UI thread             |
| `stage2 failed: -1` after replug     | Passed the stage-1 fd again (stale)      | Use the fd from the **post-renum** `UsbDeviceConnection` |
| `dlopen` rejects `libpicoscope_jni`  | Target device uses 16 KB pages           | Rebuild with `-Wl,-z,max-page-size=16384` (default in this module) |

See [`protocol.md`](protocol.md) for the wire-level USB protocol — it
is identical between the Linux and Android builds.
