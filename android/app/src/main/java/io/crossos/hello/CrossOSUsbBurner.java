package io.crossos.hello;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.SystemClock;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

public final class CrossOSUsbBurner {
    private static final String ACTION_USB_PERMISSION = "io.crossos.hello.USB_PERMISSION";
    private static final String TAG = "CrossOSUsbBurner";
    private static final boolean DEBUG = true;
    private static final int BULK_TIMEOUT_MS = 10000;
    private static final int SCSI_DIR_OUT = 0;
    private static final int SCSI_DIR_IN = 1;

    private static UsbManager usbManager;
    private static volatile boolean receiverRegistered;
    private static volatile String lastError = "";
    private static final AtomicBoolean s_device_list_changed = new AtomicBoolean(true);

    private static final Object permissionLock = new Object();
    private static final Map<Integer, Boolean> permissionResults = new HashMap<>();

    private static final AtomicLong nextJobId = new AtomicLong(1);
    private static final Map<Long, BurnSession> sessions = new HashMap<>();

    private CrossOSUsbBurner() {
    }

    public static synchronized boolean init(Activity activity) {
        if (DEBUG) Log.i(TAG, "init: Starting initialization");
        
        usbManager = (UsbManager) activity.getSystemService(Context.USB_SERVICE);
        if (usbManager == null) {
            lastError = "UsbManager unavailable";
            if (DEBUG) Log.e(TAG, "init: " + lastError);
            return false;
        }
        
        if (DEBUG) Log.d(TAG, "init: UsbManager obtained successfully");

        if (!receiverRegistered) {
            try {
                IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
                filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
                filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
                if (Build.VERSION.SDK_INT >= 33) {
                    activity.registerReceiver(permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
                } else {
                    activity.registerReceiver(permissionReceiver, filter);
                }
                receiverRegistered = true;
                if (DEBUG) Log.i(TAG, "init: BroadcastReceiver registered for USB events");
            } catch (Exception ex) {
                if (DEBUG) Log.e(TAG, "init: Failed to register BroadcastReceiver: " + ex.getMessage());
                lastError = "Failed to register BroadcastReceiver: " + ex.getMessage();
                return false;
            }
        } else {
            if (DEBUG) Log.d(TAG, "init: BroadcastReceiver already registered");
        }

        requestAttachedDevicePermissions(activity);
        
        if (DEBUG) Log.i(TAG, "init: Initialization complete");
        return true;
    }

    public static synchronized void requestAttachedDevicePermissions(Activity activity) {
        if (activity == null && usbManager == null) {
            if (DEBUG) Log.w(TAG, "requestAttachedDevicePermissions: both activity and usbManager are null");
            return;
        }

        if (usbManager == null) {
            usbManager = (UsbManager) activity.getSystemService(Context.USB_SERVICE);
            if (usbManager == null) {
                if (DEBUG) Log.e(TAG, "requestAttachedDevicePermissions: cannot obtain UsbManager");
                return;
            }
        }

        int deviceCount = 0;
        int permissionRequested = 0;
        int alreadyGranted = 0;
        int noInterface = 0;
        
        for (UsbDevice dev : usbManager.getDeviceList().values()) {
            deviceCount++;
            String devId = makeDeviceId(dev);
            
            UsbInterface intf = findMassStorageInterface(dev);
            if (intf == null) {
                noInterface++;
                if (DEBUG) Log.v(TAG, "requestAttachedDevicePermissions: " + devId + " - no MS interface");
                continue;
            }

            if (usbManager.hasPermission(dev)) {
                alreadyGranted++;
                if (DEBUG) Log.v(TAG, "requestAttachedDevicePermissions: " + devId + " - already granted");
                continue;
            }

            PendingIntent pi = PendingIntent.getBroadcast(
                activity,
                dev.getDeviceId(),
                new Intent(ACTION_USB_PERMISSION),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
            );
            usbManager.requestPermission(dev, pi);
            permissionRequested++;
            if (DEBUG) Log.i(TAG, "requestAttachedDevicePermissions: " + devId + " - permission requested");
        }
        
        if (DEBUG) Log.i(TAG, "requestAttachedDevicePermissions: Scanned " + deviceCount + " devices - " +
                       alreadyGranted + " granted, " + permissionRequested + " requested, " + noInterface + " no interface");
    }

    public static synchronized String getLastError() {
        return lastError;
    }

    /**
     * Returns {@code true} if the USB device list has changed since the last
     * call and resets the flag.  The flag starts as {@code true} so that the
     * first call always triggers an initial device scan.
     */
    public static boolean pollDeviceListChanged() {
        return s_device_list_changed.getAndSet(false);
    }

    public static synchronized String listDevices(Activity activity) {
        if (!init(activity)) {
            if (DEBUG) Log.e(TAG, "listDevices: init failed");
            return "";
        }

        if (DEBUG) Log.i(TAG, "listDevices: Starting device enumeration");
        
        int totalDevices = usbManager.getDeviceList().size();
        if (DEBUG) Log.i(TAG, "listDevices: Found " + totalDevices + " total USB devices");

        StringBuilder sb = new StringBuilder();
        int deviceIndex = 0;
        
        for (UsbDevice dev : usbManager.getDeviceList().values()) {
            deviceIndex++;
            String devId = makeDeviceId(dev);
            String devName = dev.getDeviceName();
            String devLabel = "unknown";
            if (Build.VERSION.SDK_INT >= 21) {
                String prod = dev.getProductName();
                String mfr = dev.getManufacturerName();
                devLabel = (mfr != null ? mfr : "") + " " + (prod != null ? prod : "");
            }
            
            if (DEBUG) Log.i(TAG, "listDevices [" + deviceIndex + "/" + totalDevices + "]: " + devId + " - " + devLabel.trim());
            
            UsbInterface mass = findMassStorageInterface(dev);
            if (mass == null) {
                if (DEBUG) Log.d(TAG, "listDevices: Device " + devId + " - no mass storage interface found");
                continue;
            }
            
            if (DEBUG) Log.d(TAG, "listDevices: Device " + devId + " - found mass storage interface");

            boolean isOptical = looksLikeOptical(activity, dev, mass);
            if (DEBUG) Log.i(TAG, "listDevices: Device " + devId + " - isOptical=" + isOptical);

            String id = devId;
            String label = buildDeviceLabel(dev, isOptical);
            DeviceProbe probe = probeDevice(activity, dev, mass);
            
            if (DEBUG) Log.d(TAG, "listDevices: Device " + devId + " - probe: hasMedia=" + probe.hasMedia + 
                           ", capacity=" + probe.capacityBytes + ", free=" + probe.freeBytes);

            sb.append(id).append('\t')
                .append(escape(label)).append('\t')
                .append(1).append('\t')                        /* is_usb     */
                .append(1).append('\t')                        /* can_read   */
                .append(1).append('\t')                        /* can_write  */
                .append(probe.hasMedia ? 1 : 0).append('\t')  /* has_media  */
                .append(probe.capacityBytes).append('\t')
                .append(probe.freeBytes).append('\n');
        }

        if (DEBUG) Log.i(TAG, "listDevices: Enumeration complete, found optical devices: " + (sb.length() > 0 ? "yes" : "no"));
        return sb.toString();
    }

    private static String buildDeviceLabel(UsbDevice dev, boolean isOptical) {
        String product = null;
        String manufacturer = null;

        if (Build.VERSION.SDK_INT >= 21) {
            product = dev.getProductName();
            manufacturer = dev.getManufacturerName();
        }

        StringBuilder label = new StringBuilder();
        label.append(isOptical ? "USB Optical " : "USB Candidate ");

        if (manufacturer != null && !manufacturer.isEmpty()) {
            label.append(manufacturer).append(' ');
        }
        if (product != null && !product.isEmpty()) {
            label.append(product);
        }

        if (label.charAt(label.length() - 1) == ' ') {
            label.setLength(label.length() - 1);
        }

        if (label.toString().equals("USB Optical") || label.toString().equals("USB Candidate")) {
            label.append(' ').append(String.format("%04x:%04x", dev.getVendorId(), dev.getProductId()));
        }

        return label.toString();
    }

    public static synchronized long startBurn(Activity activity, String targetId, String[] paths) {
        if (!init(activity)) {
            return -1;
        }

        if (paths == null || paths.length == 0) {
            lastError = "No source files to burn";
            return -1;
        }

        File image = resolveImagePath(paths);
        if (image == null) {
            lastError = "Android USB burner currently supports burning a single .iso/.img/.bin file";
            return -1;
        }

        UsbDevice dev = findTargetDevice(targetId);
        if (dev == null) {
            lastError = "Target USB optical drive not found";
            return -1;
        }

        UsbInterface mass = findMassStorageInterface(dev);
        if (mass == null) {
            lastError = "Target device has no mass-storage interface";
            return -1;
        }

        if (!ensureUsbPermission(activity, dev)) {
            lastError = "USB permission denied for optical drive";
            return -1;
        }

        BurnSession session = new BurnSession();
        session.id = nextJobId.getAndIncrement();
        session.state = 1; // PREPARING
        session.message = "Preparing USB optical session";
        session.totalBytes = image.length();

        sessions.put(session.id, session);

        Thread worker = new Thread(() -> runBurnWorker(session, dev, mass, image), "crossos-burn-" + session.id);
        session.worker = worker;
        worker.start();

        return session.id;
    }

    public static synchronized String pollBurn(long jobId) {
        BurnSession session = sessions.get(jobId);
        if (session == null) {
            return "5\t0\t0\t0\t0\tUnknown burn job";
        }

        return session.state + "\t"
            + session.percent + "\t"
            + session.speedMiBS + "\t"
            + session.bytesWritten + "\t"
            + session.totalBytes + "\t"
            + escape(session.message);
    }

    public static synchronized int cancelBurn(long jobId) {
        BurnSession session = sessions.get(jobId);
        if (session == null) {
            return 0;
        }

        session.cancelRequested = true;
        session.message = "Cancel requested";
        return 1;
    }

    public static synchronized void freeBurn(long jobId) {
        sessions.remove(jobId);
    }

    private static UsbDevice findTargetDevice(String targetId) {
        if (usbManager == null) {
            return null;
        }

        if (targetId != null && !targetId.isEmpty()) {
            for (UsbDevice dev : usbManager.getDeviceList().values()) {
                if (targetId.equals(makeDeviceId(dev))) {
                    return dev;
                }
            }
        }

        for (UsbDevice dev : usbManager.getDeviceList().values()) {
            if (findMassStorageInterface(dev) != null) {
                return dev;
            }
        }

        return null;
    }

    private static String makeDeviceId(UsbDevice device) {
        return device.getVendorId() + ":" + device.getProductId() + ":" + device.getDeviceId();
    }

    private static UsbInterface findMassStorageInterface(UsbDevice dev) {
        String devId = makeDeviceId(dev);
        if (DEBUG) Log.d(TAG, "findMassStorageInterface: " + devId + " - checking " + dev.getInterfaceCount() + " interfaces");
        
        for (int i = 0; i < dev.getInterfaceCount(); i++) {
            UsbInterface intf = dev.getInterface(i);
            if (DEBUG) Log.v(TAG, "findMassStorageInterface: " + devId + " [" + i + "] class=0x" + 
                           String.format("%02x", intf.getInterfaceClass()) + 
                           " subclass=0x" + String.format("%02x", intf.getInterfaceSubclass()));
            
            if (intf.getInterfaceClass() == UsbConstants.USB_CLASS_MASS_STORAGE) {
                if (DEBUG) Log.i(TAG, "findMassStorageInterface: " + devId + " - found MS interface at [" + i + "]");
                return intf;
            }
        }

        if (DEBUG) Log.d(TAG, "findMassStorageInterface: " + devId + " - no MS class, checking for bulk endpoints");
        
        for (int i = 0; i < dev.getInterfaceCount(); i++) {
            UsbInterface intf = dev.getInterface(i);
            if (hasBulkEndpoints(intf)) {
                if (DEBUG) Log.i(TAG, "findMassStorageInterface: " + devId + " - found bulk endpoints at [" + i + "]");
                return intf;
            }
        }

        if (DEBUG) Log.w(TAG, "findMassStorageInterface: " + devId + " - no suitable interface found");
        return null;
    }

    private static boolean hasBulkEndpoints(UsbInterface intf) {
        boolean hasIn = false;
        boolean hasOut = false;

        for (int i = 0; i < intf.getEndpointCount(); i++) {
            UsbEndpoint ep = intf.getEndpoint(i);
            if (ep.getType() != UsbConstants.USB_ENDPOINT_XFER_BULK) {
                continue;
            }
            if (ep.getDirection() == UsbConstants.USB_DIR_IN) {
                hasIn = true;
            } else if (ep.getDirection() == UsbConstants.USB_DIR_OUT) {
                hasOut = true;
            }
        }

        return hasIn && hasOut;
    }

    private static boolean looksLikeOptical(Activity activity, UsbDevice dev, UsbInterface intf) {
        String devId = makeDeviceId(dev);
        
        if (deviceNameHintsOptical(dev)) {
            if (DEBUG) Log.i(TAG, "looksLikeOptical: " + devId + " - MATCHED by device name");
            return true;
        }

        if (intf.getInterfaceSubclass() == 0x02 || intf.getInterfaceSubclass() == 0x05) {
            if (DEBUG) Log.i(TAG, "looksLikeOptical: " + devId + " - MATCHED by subclass (0x" + 
                           String.format("%02x", intf.getInterfaceSubclass()) + ")");
            return true;
        }

        if (!ensureUsbPermission(activity, dev)) {
            if (DEBUG) Log.w(TAG, "looksLikeOptical: " + devId + " - cannot get permission, assuming optical");
            return true;
        }

        UsbEndpoint in = findEndpoint(intf, UsbConstants.USB_DIR_IN);
        UsbEndpoint out = findEndpoint(intf, UsbConstants.USB_DIR_OUT);
        if (in == null || out == null) {
            if (DEBUG) Log.w(TAG, "looksLikeOptical: " + devId + " - no bulk endpoints (in=" + 
                           (in != null) + ", out=" + (out != null) + ")");
            return false;
        }

        UsbDeviceConnection conn = usbManager.openDevice(dev);
        if (conn == null) {
            if (DEBUG) Log.w(TAG, "looksLikeOptical: " + devId + " - cannot open device, assuming optical");
            return true;
        }

        boolean claimed = conn.claimInterface(intf, true);
        if (!claimed) {
            if (DEBUG) Log.w(TAG, "looksLikeOptical: " + devId + " - cannot claim interface, assuming optical");
            conn.close();
            return true;
        }

        try {
            MmcProbe mmc = probeMmcCapability(conn, in, out);
            if (mmc.supported) {
                boolean profileOptical = isOpticalProfile(mmc.currentProfile);
                if (DEBUG) {
                    Log.i(TAG, "looksLikeOptical: " + devId + " - MMC supported, profile=0x"
                        + String.format("%04x", mmc.currentProfile)
                        + " opticalProfile=" + profileOptical
                        + " writable=" + mmc.writable);
                }
                if (profileOptical) {
                    return true;
                }
            }

            byte[] inquiry = scsiInquiry(conn, in, out);
            if (inquiry == null || inquiry.length < 36) {
                if (DEBUG) Log.w(TAG, "looksLikeOptical: " + devId + " - SCSI INQUIRY failed or too short (len=" + 
                               (inquiry != null ? inquiry.length : 0) + ")");
                return true;
            }
            int type = inquiry[0] & 0x1F;
            boolean isOptical = (type == 0x05);
            if (DEBUG) Log.i(TAG, "looksLikeOptical: " + devId + " - SCSI device type=0x" + 
                           String.format("%02x", type) + " -> isOptical=" + isOptical);
            return isOptical;
        } catch (Exception ex) {
            if (DEBUG) Log.e(TAG, "looksLikeOptical: " + devId + " - SCSI probe exception: " + ex.getMessage());
            return true;
        } finally {
            conn.releaseInterface(intf);
            conn.close();
        }
    }

    private static boolean deviceNameHintsOptical(UsbDevice dev) {
        StringBuilder text = new StringBuilder();
        if (Build.VERSION.SDK_INT >= 21) {
            if (dev.getManufacturerName() != null) {
                text.append(dev.getManufacturerName()).append(' ');
            }
            if (dev.getProductName() != null) {
                text.append(dev.getProductName()).append(' ');
            }
        }
        if (dev.getDeviceName() != null) {
            text.append(dev.getDeviceName());
        }

        String lower = text.toString().toLowerCase();
        
        if (DEBUG) Log.v(TAG, "deviceNameHintsOptical: Checking device name: " + lower);
        
        boolean isOptical = lower.contains("dvd") || lower.contains("cd") || lower.contains("cdr") ||
               lower.contains("cdrw") || lower.contains("bluray") || lower.contains("bd") ||
               lower.contains("optical") || lower.contains("writer") || lower.contains("burner") ||
               lower.contains("drive") || lower.contains("recorder") || lower.contains("combo") ||
               lower.contains("multi") || lower.contains("rw");  // RW = ReWriter
        
        if (DEBUG && isOptical) Log.d(TAG, "deviceNameHintsOptical: MATCHED optical keyword in name");
        
        return isOptical;
    }

    private static DeviceProbe probeDevice(Activity activity, UsbDevice dev, UsbInterface intf) {
        DeviceProbe probe = new DeviceProbe();

        if (!ensureUsbPermission(activity, dev)) {
            return probe;
        }

        UsbEndpoint in = findEndpoint(intf, UsbConstants.USB_DIR_IN);
        UsbEndpoint out = findEndpoint(intf, UsbConstants.USB_DIR_OUT);
        if (in == null || out == null) {
            return probe;
        }

        UsbDeviceConnection conn = usbManager.openDevice(dev);
        if (conn == null) {
            return probe;
        }

        boolean claimed = conn.claimInterface(intf, true);
        if (!claimed) {
            conn.close();
            return probe;
        }

        try {
            MmcProbe mmc = probeMmcCapability(conn, in, out);
            probe.mmcSupported = mmc.supported;
            probe.mmcCurrentProfile = mmc.currentProfile;
            probe.mmcWritable = mmc.writable;

            testUnitReady(conn, in, out);
            long[] cap = readCapacity(conn, in, out);
            long lastLba = cap[0];
            long blockSize = cap[1];
            if (lastLba > 0 && blockSize > 0) {
                probe.hasMedia = true;
                probe.capacityBytes = (lastLba + 1L) * blockSize;
                probe.freeBytes = probe.capacityBytes;
            }

            if (!probe.hasMedia && mmc.supported && isOpticalProfile(mmc.currentProfile)) {
                probe.hasMedia = true;
            }
        } catch (Exception ignored) {
        } finally {
            conn.releaseInterface(intf);
            conn.close();
        }

        return probe;
    }

    private static boolean ensureUsbPermission(Activity activity, UsbDevice dev) {
        if (usbManager == null) {
            if (DEBUG) Log.w(TAG, "ensureUsbPermission: usbManager is null");
            return false;
        }
        
        String devId = makeDeviceId(dev);
        
        if (usbManager.hasPermission(dev)) {
            if (DEBUG) Log.d(TAG, "ensureUsbPermission: " + devId + " - already has permission");
            return true;
        }

        if (DEBUG) Log.i(TAG, "ensureUsbPermission: " + devId + " - requesting permission");
        
        PendingIntent pi = PendingIntent.getBroadcast(
            activity,
            dev.getDeviceId(),
            new Intent(ACTION_USB_PERMISSION),
            PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
        );

        synchronized (permissionLock) {
            permissionResults.remove(dev.getDeviceId());
            usbManager.requestPermission(dev, pi);

            long waitUntil = SystemClock.uptimeMillis() + 8000;
            int waitCount = 0;
            while (SystemClock.uptimeMillis() < waitUntil) {
                Boolean granted = permissionResults.get(dev.getDeviceId());
                if (granted != null) {
                    if (DEBUG) Log.i(TAG, "ensureUsbPermission: " + devId + " - permission response: " + granted);
                    permissionResults.remove(dev.getDeviceId());
                    return granted;
                }
                waitCount++;
                try {
                    permissionLock.wait(250);
                } catch (InterruptedException ignored) {
                    Thread.currentThread().interrupt();
                    if (DEBUG) Log.w(TAG, "ensureUsbPermission: " + devId + " - interrupted after " + waitCount + " waits");
                    break;
                }
            }
            
            if (DEBUG) Log.w(TAG, "ensureUsbPermission: " + devId + " - timeout after " + waitCount + " waits");
        }

        boolean hasIt = usbManager.hasPermission(dev);
        if (DEBUG) Log.i(TAG, "ensureUsbPermission: " + devId + " - final check: " + hasIt);
        return hasIt;
    }

    private static File resolveImagePath(String[] paths) {
        if (paths.length != 1 || paths[0] == null) {
            return null;
        }

        File f = new File(paths[0]);
        if (!f.isFile()) {
            return null;
        }

        String n = f.getName().toLowerCase();
        if (n.endsWith(".iso") || n.endsWith(".img") || n.endsWith(".bin")) {
            return f;
        }

        return null;
    }

    private static void runBurnWorker(BurnSession session, UsbDevice dev, UsbInterface intf, File image) {
        UsbDeviceConnection conn = null;
        boolean claimed = false;

        try {
            UsbEndpoint in = findEndpoint(intf, UsbConstants.USB_DIR_IN);
            UsbEndpoint out = findEndpoint(intf, UsbConstants.USB_DIR_OUT);
            if (in == null || out == null) {
                fail(session, "Missing USB bulk endpoints");
                return;
            }

            conn = usbManager.openDevice(dev);
            if (conn == null) {
                fail(session, "Unable to open USB optical drive");
                return;
            }

            claimed = conn.claimInterface(intf, true);
            if (!claimed) {
                fail(session, "Cannot claim USB interface");
                return;
            }

            testUnitReady(conn, in, out);
            long[] cap = readCapacity(conn, in, out);
            int blockSize = (int) cap[1];
            if (blockSize <= 0 || blockSize > 65536) {
                blockSize = 2048;
            }

            synchronized (CrossOSUsbBurner.class) {
                session.state = 2; // BURNING
                session.message = "Writing image to optical media";
            }

            writeImage(session, conn, in, out, image, blockSize);
            if (session.cancelRequested) {
                synchronized (CrossOSUsbBurner.class) {
                    session.state = 6; // CANCELED
                    session.message = "Burn canceled";
                }
                return;
            }

            synchronizeCache(conn, in, out);
            closeSession(conn, in, out);

            synchronized (CrossOSUsbBurner.class) {
                session.state = 4; // DONE
                session.percent = 100.0f;
                session.message = "Disc burn completed";
            }
        } catch (Exception ex) {
            fail(session, "Burn failed: " + ex.getMessage());
        } finally {
            if (conn != null) {
                if (claimed) {
                    conn.releaseInterface(intf);
                }
                conn.close();
            }
        }
    }

    private static void writeImage(BurnSession session,
                                   UsbDeviceConnection conn,
                                   UsbEndpoint in,
                                   UsbEndpoint out,
                                   File image,
                                   int blockSize) throws IOException {
        try (FileInputStream fis = new FileInputStream(image)) {
            final int maxBlocksPerTransfer = 16;
            byte[] chunk = new byte[blockSize * maxBlocksPerTransfer];
            long lba = 0;
            long started = SystemClock.uptimeMillis();

            while (true) {
                if (session.cancelRequested) {
                    return;
                }

                int read = fis.read(chunk);
                if (read < 0) {
                    break;
                }

                int blocks = (read + blockSize - 1) / blockSize;
                int transferBytes = blocks * blockSize;
                if (transferBytes > read) {
                    Arrays.fill(chunk, read, transferBytes, (byte) 0);
                }

                byte[] transfer = chunk;
                if (transferBytes != chunk.length) {
                    transfer = Arrays.copyOf(chunk, transferBytes);
                }

                scsiWrite10(conn, in, out, lba, blocks, transfer);
                lba += blocks;

                synchronized (CrossOSUsbBurner.class) {
                    session.bytesWritten += read;
                    if (session.totalBytes > 0) {
                        session.percent = Math.min(100.0f,
                            (session.bytesWritten * 100.0f) / session.totalBytes);
                    }

                    long elapsedMs = Math.max(1, SystemClock.uptimeMillis() - started);
                    session.speedMiBS = (float) ((session.bytesWritten / 1048576.0) / (elapsedMs / 1000.0));
                    session.message = "Burning (" + session.bytesWritten + "/" + session.totalBytes + " bytes)";
                }
            }
        }
    }

    private static UsbEndpoint findEndpoint(UsbInterface intf, int direction) {
        for (int i = 0; i < intf.getEndpointCount(); i++) {
            UsbEndpoint ep = intf.getEndpoint(i);
            if (ep.getType() == UsbConstants.USB_ENDPOINT_XFER_BULK && ep.getDirection() == direction) {
                return ep;
            }
        }
        return null;
    }

    private static byte[] scsiInquiry(UsbDeviceConnection conn,
                                      UsbEndpoint in,
                                      UsbEndpoint out) {
        byte[] cdb = new byte[6];
        cdb[0] = 0x12;
        cdb[4] = 36;
        return scsiTransfer(conn, in, out, cdb, null, 36, SCSI_DIR_IN);
    }

    private static byte[] scsiRequestSense(UsbDeviceConnection conn,
                                           UsbEndpoint in,
                                           UsbEndpoint out) {
        byte[] cdb = new byte[6];
        cdb[0] = 0x03;
        cdb[4] = 18;

        ScsiResult result = scsiTransferRaw(conn, in, out, cdb, null, 18, SCSI_DIR_IN);
        if (result == null || result.status != 0 || result.data == null || result.data.length == 0) {
            return null;
        }
        return result.data;
    }

    private static MmcProbe probeMmcCapability(UsbDeviceConnection conn,
                                               UsbEndpoint in,
                                               UsbEndpoint out) {
        MmcProbe probe = new MmcProbe();

        byte[] cdb = new byte[10];
        cdb[0] = 0x46; // GET CONFIGURATION
        cdb[1] = 0x00; // RT=0: current values
        cdb[2] = 0x00;
        cdb[3] = 0x00;
        cdb[7] = 0x00;
        cdb[8] = 0x20; // initial read

        byte[] data = scsiTransfer(conn, in, out, cdb, null, 0x20, SCSI_DIR_IN);
        if (data == null || data.length < 8) {
            return probe;
        }

        probe.supported = true;
        ByteBuffer bb = ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN);
        int dataLength = bb.getInt();
        int currentProfile = Short.toUnsignedInt(bb.getShort());
        probe.currentProfile = currentProfile;

        int totalLen = Math.min(data.length, dataLength + 4);
        int offset = 8;
        while (offset + 4 <= totalLen) {
            int featureCode = Short.toUnsignedInt(bb.getShort(offset));
            int flags = bb.get(offset + 2) & 0xFF;
            int additionalLen = bb.get(offset + 3) & 0xFF;
            boolean current = (flags & 0x01) != 0;

            if (featureCode == 0x0021 && current) { // Random Writable
                probe.writable = true;
            }
            if (featureCode == 0x002F && current) { // DVD-R/RW Write
                probe.writable = true;
            }
            if (featureCode == 0x0040 && current) { // BD Write
                probe.writable = true;
            }

            offset += 4 + additionalLen;
        }

        return probe;
    }

    private static void testUnitReady(UsbDeviceConnection conn,
                                      UsbEndpoint in,
                                      UsbEndpoint out) {
        byte[] cdb = new byte[6];
        cdb[0] = 0x00;
        scsiTransfer(conn, in, out, cdb, null, 0, SCSI_DIR_OUT);
    }

    private static long[] readCapacity(UsbDeviceConnection conn,
                                       UsbEndpoint in,
                                       UsbEndpoint out) {
        byte[] cdb = new byte[10];
        cdb[0] = 0x25;
        byte[] data = scsiTransfer(conn, in, out, cdb, null, 8, SCSI_DIR_IN);
        if (data == null || data.length < 8) {
            return new long[] {0, 2048};
        }

        ByteBuffer bb = ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN);
        long lastLba = Integer.toUnsignedLong(bb.getInt());
        long block = Integer.toUnsignedLong(bb.getInt());
        if (block == 0) {
            block = 2048;
        }
        return new long[] {lastLba, block};
    }

    private static void scsiWrite10(UsbDeviceConnection conn,
                                    UsbEndpoint in,
                                    UsbEndpoint out,
                                    long lba,
                                    int blocks,
                                    byte[] dataOut) {
        byte[] cdb = new byte[10];
        cdb[0] = 0x2A;
        cdb[2] = (byte) ((lba >> 24) & 0xFF);
        cdb[3] = (byte) ((lba >> 16) & 0xFF);
        cdb[4] = (byte) ((lba >> 8) & 0xFF);
        cdb[5] = (byte) (lba & 0xFF);
        cdb[7] = (byte) ((blocks >> 8) & 0xFF);
        cdb[8] = (byte) (blocks & 0xFF);

        byte[] ret = scsiTransfer(conn, in, out, cdb, dataOut, 0, SCSI_DIR_OUT);
        if (ret == null) {
            throw new IllegalStateException("WRITE10 command failed");
        }
    }

    private static void synchronizeCache(UsbDeviceConnection conn,
                                         UsbEndpoint in,
                                         UsbEndpoint out) {
        byte[] cdb = new byte[10];
        cdb[0] = 0x35;
        scsiTransfer(conn, in, out, cdb, null, 0, SCSI_DIR_OUT);
    }

    private static void closeSession(UsbDeviceConnection conn,
                                     UsbEndpoint in,
                                     UsbEndpoint out) {
        byte[] cdb = new byte[6];
        cdb[0] = 0x1B;
        cdb[4] = 0x02;
        scsiTransfer(conn, in, out, cdb, null, 0, SCSI_DIR_OUT);
    }

    private static byte[] scsiTransfer(UsbDeviceConnection conn,
                                       UsbEndpoint in,
                                       UsbEndpoint out,
                                       byte[] cdb,
                                       byte[] dataOut,
                                       int dataInLen,
                                       int direction) {
        ScsiResult result = scsiTransferRaw(conn, in, out, cdb, dataOut, dataInLen, direction);
        if (result == null) {
            return null;
        }

        if (result.status == 0) {
            return result.data;
        }

        if ((cdb[0] & 0xFF) != 0x03) {
            byte[] sense = scsiRequestSense(conn, in, out);
            SenseInfo info = parseSense(sense);
            if (DEBUG) {
                if (info.valid) {
                    Log.w(TAG, "scsiTransfer: command 0x" + String.format("%02x", cdb[0] & 0xFF)
                        + " failed status=" + result.status
                        + " sense=" + info.toShortString());
                } else {
                    Log.w(TAG, "scsiTransfer: command 0x" + String.format("%02x", cdb[0] & 0xFF)
                        + " failed status=" + result.status + " without valid sense");
                }
            }

            if (shouldRetryFromSense(info)) {
                if (DEBUG) {
                    Log.i(TAG, "scsiTransfer: retrying command 0x"
                        + String.format("%02x", cdb[0] & 0xFF)
                        + " after recoverable sense");
                }
                ScsiResult retry = scsiTransferRaw(conn, in, out, cdb, dataOut, dataInLen, direction);
                if (retry != null && retry.status == 0) {
                    return retry.data;
                }
            }
        }

        return null;
    }

    private static ScsiResult scsiTransferRaw(UsbDeviceConnection conn,
                                              UsbEndpoint in,
                                              UsbEndpoint out,
                                              byte[] cdb,
                                              byte[] dataOut,
                                              int dataInLen,
                                              int direction) {
        int transferLen = dataOut != null ? dataOut.length : dataInLen;

        byte[] cbw = buildCbw(nextTag(), transferLen, direction == SCSI_DIR_IN, cdb);
        int wrote = conn.bulkTransfer(out, cbw, cbw.length, BULK_TIMEOUT_MS);
        if (wrote != cbw.length) {
            return null;
        }

        byte[] dataIn = null;
        if (dataOut != null && dataOut.length > 0) {
            int sent = conn.bulkTransfer(out, dataOut, dataOut.length, BULK_TIMEOUT_MS);
            if (sent != dataOut.length) {
                return null;
            }
        } else if (dataInLen > 0) {
            dataIn = new byte[dataInLen];
            int got = conn.bulkTransfer(in, dataIn, dataInLen, BULK_TIMEOUT_MS);
            if (got < 0) {
                return null;
            }
            if (got != dataInLen) {
                dataIn = Arrays.copyOf(dataIn, got);
            }
        }

        byte[] csw = new byte[13];
        int gotCsw = conn.bulkTransfer(in, csw, csw.length, BULK_TIMEOUT_MS);
        if (gotCsw != csw.length) {
            return null;
        }

        ByteBuffer cswBuf = ByteBuffer.wrap(csw).order(ByteOrder.LITTLE_ENDIAN);
        int signature = cswBuf.getInt();
        if (signature != 0x53425355) {
            return null;
        }

        cswBuf.getInt(); // tag
        cswBuf.getInt(); // data residue
        int status = cswBuf.get() & 0xFF;
        ScsiResult result = new ScsiResult();
        result.data = dataIn;
        result.status = status;
        return result;
    }

    private static SenseInfo parseSense(byte[] sense) {
        SenseInfo info = new SenseInfo();
        if (sense == null || sense.length < 14) {
            return info;
        }

        int responseCode = sense[0] & 0x7F;
        if (responseCode != 0x70 && responseCode != 0x71) {
            return info;
        }

        info.valid = true;
        info.senseKey = sense[2] & 0x0F;
        info.asc = sense[12] & 0xFF;
        info.ascq = sense[13] & 0xFF;
        return info;
    }

    private static boolean shouldRetryFromSense(SenseInfo info) {
        if (!info.valid) {
            return false;
        }

        // UNIT ATTENTION (e.g. media changed/reset) – one retry often succeeds.
        if (info.senseKey == 0x06) {
            return true;
        }

        // NOT READY with "becoming ready" style conditions.
        return info.senseKey == 0x02 && info.asc == 0x04
            && (info.ascq == 0x01 || info.ascq == 0x02 || info.ascq == 0x07);
    }

    private static boolean isOpticalProfile(int profile) {
        switch (profile) {
            case 0x0008: // CD-ROM
            case 0x0009: // CD-R
            case 0x000A: // CD-RW
            case 0x0010: // DVD-ROM
            case 0x0011: // DVD-R Sequential
            case 0x0012: // DVD-RAM
            case 0x0013: // DVD-RW Restricted Overwrite
            case 0x0014: // DVD-RW Sequential
            case 0x0015: // DVD-R DL Sequential
            case 0x0016: // DVD-R DL Layer Jump
            case 0x001A: // DVD+RW
            case 0x001B: // DVD+R
            case 0x002A: // DVD+RW DL
            case 0x002B: // DVD+R DL
            case 0x0040: // BD-ROM
            case 0x0041: // BD-R SRM
            case 0x0042: // BD-R RRM
            case 0x0043: // BD-RE
                return true;
            default:
                return false;
        }
    }

    private static final AtomicLong nextTag = new AtomicLong(1);

    private static int nextTag() {
        return (int) nextTag.getAndIncrement();
    }

    private static byte[] buildCbw(int tag,
                                   int transferLen,
                                   boolean dirIn,
                                   byte[] cdb) {
        byte[] cbw = new byte[31];
        ByteBuffer bb = ByteBuffer.wrap(cbw).order(ByteOrder.LITTLE_ENDIAN);
        bb.putInt(0x43425355);
        bb.putInt(tag);
        bb.putInt(transferLen);
        bb.put((byte) (dirIn ? 0x80 : 0x00));
        bb.put((byte) 0);
        bb.put((byte) cdb.length);
        bb.put(cdb, 0, cdb.length);
        return cbw;
    }

    private static String escape(String s) {
        if (s == null) {
            return "";
        }
        return s.replace("\\", "\\\\").replace("\t", " ").replace("\n", " ");
    }

    private static void fail(BurnSession session, String message) {
        synchronized (CrossOSUsbBurner.class) {
            session.state = 5; // FAILED
            session.message = message;
            lastError = message;
        }
    }

    private static final BroadcastReceiver permissionReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();

            if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action) ||
                    UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
                s_device_list_changed.set(true);
                return;
            }

            if (!ACTION_USB_PERMISSION.equals(action)) {
                return;
            }

            UsbDevice device;
            if (Build.VERSION.SDK_INT >= 33) {
                device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice.class);
            } else {
                device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            }
            boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
            if (device == null) {
                return;
            }

            synchronized (permissionLock) {
                permissionResults.put(device.getDeviceId(), granted);
                permissionLock.notifyAll();
            }
        }
    };

    private static final class BurnSession {
        long id;
        volatile int state;
        volatile float percent;
        volatile float speedMiBS;
        volatile long bytesWritten;
        volatile long totalBytes;
        volatile String message;
        volatile boolean cancelRequested;
        Thread worker;
    }

    private static final class DeviceProbe {
        boolean hasMedia;
        long capacityBytes;
        long freeBytes;
        boolean mmcSupported;
        int mmcCurrentProfile;
        boolean mmcWritable;
    }

    private static final class MmcProbe {
        boolean supported;
        int currentProfile;
        boolean writable;
    }

    private static final class ScsiResult {
        byte[] data;
        int status;
    }

    private static final class SenseInfo {
        boolean valid;
        int senseKey;
        int asc;
        int ascq;

        String toShortString() {
            return "KEY=0x" + String.format("%02x", senseKey)
                + " ASC=0x" + String.format("%02x", asc)
                + " ASCQ=0x" + String.format("%02x", ascq);
        }
    }
}
