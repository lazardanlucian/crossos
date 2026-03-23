# USB Optical Media Detection on Android - Debug & Fix Guide

## Problem Summary
The disc_burner app does not detect USB optical media (CD/DVD drives) on Android, even when physically connected to the device.

## Root Causes Identified

### 1. Missing USB Device Filter Metadata
**Problem**: Android apps must declare which USB devices they want to interact with. Without a device filter configuration, the system may not report optical drives to the app.

**Fix Applied**: Created `res/xml/usb_device_filter.xml` with:
- USB mass storage class definitions (0x08:0x06:0x50)
- SCSI and SFF-8070i optical drive descriptors
- Vendor IDs for major optical drive manufacturers

### 2. Missing USB Permission Declaration
**Problem**: App needs explicit permission to request USB device access in Android manifest.

**Fix Applied**: Added `android.permission.USB_PERMISSION` to AndroidManifest.xml

### 3. Lack of Debug Visibility
**Problem**: Without logging, it's impossible to identify WHERE detection is failing (device enumeration, interface detection, permission, SCSI probe, etc.).

**Fix Applied**: Added comprehensive debug logging throughout CrossOSUsbBurner.java:
- Device enumeration progress
- Interface class/subclass values
- Permission request/response flow
- SCSI probe results

## Files Changed

### 1. `/android/app/src/main/res/xml/usb_device_filter.xml` (NEW)
Declares USB optical device specifications that Android should recognize:
```xml
<!-- USB Mass Storage Class Devices -->
<usb-device class="0x08" subclass="0x06" protocol="0x50" />

<!-- SCSI CD-ROM, DVD, BD -->
<usb-device class="0xff" subclass="0x02" />

<!-- SFF-8070i CD-ROM/DVD -->
<usb-device class="0xff" subclass="0x05" />

<!-- Known Manufacturer Vendor IDs -->
<usb-device vendor-id="0x0bb8" />  <!-- LG -->
<usb-device vendor-id="0x054c" />  <!-- Sony -->
<!-- ... more vendors ... -->
```

### 2. `/android/app/src/main/AndroidManifest.xml` (MODIFIED)
- Added: `<uses-permission android:name="android.permission.USB_PERMISSION" />`
- Updated activity's USB_DEVICE_ATTACHED intent filter with meta-data reference

### 3. `/android/app/src/main/java/io/crossos/hello/CrossOSUsbBurner.java` (MODIFIED)
**Added Debug Infrastructure**:
- `private static final String TAG = "CrossOSUsbBurner"`
- `private static final boolean DEBUG = true`
- Log imports: `import android.util.Log;`

**Enhanced Methods with Logging**:
- `init()`: Logs UsbManager initialization, BroadcastReceiver registration, and any exceptions
- `requestAttachedDevicePermissions()`: Logs device scan summary (device count, grants, requests, interfaces)
- `listDevices()`: Logs enumerated device counts and details
- `findMassStorageInterface()`: Logs interface attributes for debugging
- `looksLikeOptical()`: Logs detection method (name/subclass/SCSI)
- `ensureUsbPermission()`: Logs permission request/response flow
- `deviceNameHintsOptical()`: Added more keyword patterns and logging

**Example Log Output**:
```
CrossOSUsbBurner: init: Starting initialization
CrossOSUsbBurner: init: UsbManager obtained successfully
CrossOSUsbBurner: init: BroadcastReceiver registered for USB events
CrossOSUsbBurner: init: Initialization complete
CrossOSUsbBurner: requestAttachedDevicePermissions: Scanned 2 devices - 1 granted, 1 requested, 0 no interface
CrossOSUsbBurner: listDevices: Starting device enumeration
CrossOSUsbBurner: listDevices: Found 2 total USB devices
CrossOSUsbBurner: listDevices [1/2]: 0x0bb8:0x0001:1234 - LG Electronics CD-R
CrossOSUsbBurner: findMassStorageInterface: found MS interface at [1]
CrossOSUsbBurner: looksLikeOptical: MATCHED by SCSI device type
CrossOSUsbBurner: ensureUsbPermission: permission response: true
```

## How to Test & Debug

### 1. Build and Deploy
```bash
cd /workspaces/crossos
./scripts/build_all.sh  # or targeted Android build
adb install android/app/build/outputs/apk/debug/app-debug.apk
```

### 2. Connect USB Optical Media Device
Plug in a USB CD/DVD drive to the Android device.

### 3. View Debug Logs
```bash
# In a terminal, capture logs from CrossOSUsbBurner
adb logcat -s CrossOSUsbBurner

# In another terminal, open the disc_burner app and navigate to optical device list
adb shell am start -n io.crossos.hello/.CrossOSNativeActivity
```

### 4. Analyze Log Output
Check logcat for these patterns:

**✓ Initialization Success**:
```
init: Starting initialization
init: UsbManager obtained successfully
init: BroadcastReceiver registered for USB events
init: Initialization complete
requestAttachedDevicePermissions: Scanned N devices
```

**✗ Initialization Failed - UsbManager**:
```
init: UsbManager unavailable
→ System USB service not available (very rare)
→ Indicates OS-level USB issue
```

**✗ Initialization Failed - BroadcastReceiver**:
```
init: Failed to register BroadcastReceiver: [error message]
→ Permission issue or incorrect manifest configuration
→ Verify Android manifest has proper permissions
```

**✗ Device Not Enumerated**:
```
Found 0 total USB devices
→ USB connection issue or device not recognized by Android
→ Check: adb shell dumpsys usb
```

**✗ Wrong Interface Type**:
```
Device has no mass storage interface
checked 2 interfaces: class=0x09, class=0x0a
→ Device uses non-standard USB class
→ Add to usb_device_filter.xml with vendor ID
```

**✗ Permission Denied**:
```
requesting permission
permission response: false
→ User declined in permission dialog
→ Or app not in foreground when dialog appeared
```

**✗ SCSI Probe Failed**:
```
cannot open device, assuming optical
→ Device doesn't respond to USB control commands
→ May need SCSI retry logic
```

## Next Steps if Device Still Not Detected

### Option 1: Add Device-Specific Vendor ID
Edit `usb_device_filter.xml`:
```xml
<!-- Add your device's vendor ID -->
<usb-device vendor-id="0xYYYY" product-id="0xZZZZ" />
```
Find vendor/product ID: `adb shell dumpsys usb | grep "iProduct"`

### Option 2: Broaden USB Class Filters
The current filter is fairly comprehensive, but very old devices might use different classes. Check device descriptors:
```bash
adb shell dumpsys usb | grep -A30 "USB Device"
```

### Option 3: Improve Permission Handling
The permission request waits 8 seconds. For devices with slow permission dialogs:
1. Increase timeout in `ensureUsbPermission()` from 8000ms to 15000ms
2. Or show explicit UI prompting user to grant permission

### Option 4: SCSI Probe Robustness
If SCSI INQUIRY command fails unexpectedly:
1. Add retry logic (retry up to 3 times)
2. Accept more device types (currently only accepts 0x05; could accept 0x00)
3. Add error recovery for short responses

## Technical Details

### USB Device Detection Flow
1. Android enumerates USB devices via `UsbManager.getDeviceList()`
2. App filters for mass storage interfaces (class 0x08 or bulk endpoints)
3. For each candidate, probe with SCSI INQUIRY command
4. SCSI response device type = 0x05 confirms it's optical
5. Device advertised to user with capacity/media info

### Permission Model
- User grants permission to app only when requested PendingIntent
- Permission persists unless user revokes in Settings > Apps > Permissions
- App must request fresh permission for each device per Android lifecycle

### SCSI Probe Details
- Uses BOT (Bulk-Only Transfer) protocol
- Sends INQUIRY (0x12) to get device type
- Device type 0x05 = CD-ROM/DVD drive
- Reads capacity with READ_CAPACITY (0x25)

## References
- Android UsbManager: https://developer.android.com/reference/android/hardware/usb/UsbManager
- USB Device Filter: https://developer.android.com/guide/topics/connectivity/usb/host
- SCSI Commands: https://en.wikipedia.org/wiki/SCSI_command

## Log Tags for Filtering
```bash
# Just CrossOSUsbBurner logs
adb logcat -s CrossOSUsbBurner

# CrossOS logs including C native code
adb logcat -s "CrossOS"

# Full logs (verbose)
adb logcat

# Persist logs to file
adb logcat -v threadtime > /tmp/android_optical.log
```
