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

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

public final class CrossOSUsbBurner {
    private static final String ACTION_USB_PERMISSION = "io.crossos.hello.USB_PERMISSION";
    private static final int BULK_TIMEOUT_MS = 10000;
    private static final int SCSI_DIR_OUT = 0;
    private static final int SCSI_DIR_IN = 1;

    private static UsbManager usbManager;
    private static volatile boolean receiverRegistered;
    private static volatile String lastError = "";

    private static final Object permissionLock = new Object();
    private static final Map<Integer, Boolean> permissionResults = new HashMap<>();

    private static final AtomicLong nextJobId = new AtomicLong(1);
    private static final Map<Long, BurnSession> sessions = new HashMap<>();

    private CrossOSUsbBurner() {
    }

    public static synchronized boolean init(Activity activity) {
        usbManager = (UsbManager) activity.getSystemService(Context.USB_SERVICE);
        if (usbManager == null) {
            lastError = "UsbManager unavailable";
            return false;
        }

        if (!receiverRegistered) {
            IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
            if (Build.VERSION.SDK_INT >= 33) {
                activity.registerReceiver(permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
            } else {
                activity.registerReceiver(permissionReceiver, filter);
            }
            receiverRegistered = true;
        }

        return true;
    }

    public static synchronized String getLastError() {
        return lastError;
    }

    public static synchronized String listDevices(Activity activity) {
        if (!init(activity)) {
            return "";
        }

        StringBuilder sb = new StringBuilder();
        for (UsbDevice dev : usbManager.getDeviceList().values()) {
            UsbInterface mass = findMassStorageInterface(dev);
            if (mass == null) {
                continue;
            }

            boolean isOptical = looksLikeOptical(activity, dev, mass);
            if (!isOptical) {
                continue;
            }

            String id = makeDeviceId(dev);
            String label = "USB Optical "
                + String.format("%04x:%04x", dev.getVendorId(), dev.getProductId());
            DeviceProbe probe = probeDevice(activity, dev, mass);

            sb.append(id).append('\t')
                .append(escape(label)).append('\t')
                .append(1).append('\t')
                .append(1).append('\t')
                .append(probe.hasMedia ? 1 : 0).append('\t')
                .append(1).append('\t')
                .append(probe.capacityBytes).append('\t')
                .append(probe.freeBytes).append('\n');
        }

        return sb.toString();
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
        for (int i = 0; i < dev.getInterfaceCount(); i++) {
            UsbInterface intf = dev.getInterface(i);
            if (intf.getInterfaceClass() == UsbConstants.USB_CLASS_MASS_STORAGE) {
                return intf;
            }
        }
        return null;
    }

    private static boolean looksLikeOptical(Activity activity, UsbDevice dev, UsbInterface intf) {
        if (intf.getInterfaceSubclass() == 0x02 || intf.getInterfaceSubclass() == 0x05) {
            return true;
        }

        if (!ensureUsbPermission(activity, dev)) {
            return true;
        }

        UsbEndpoint in = findEndpoint(intf, UsbConstants.USB_DIR_IN);
        UsbEndpoint out = findEndpoint(intf, UsbConstants.USB_DIR_OUT);
        if (in == null || out == null) {
            return false;
        }

        UsbDeviceConnection conn = usbManager.openDevice(dev);
        if (conn == null) {
            return true;
        }

        boolean claimed = conn.claimInterface(intf, true);
        if (!claimed) {
            conn.close();
            return true;
        }

        try {
            byte[] inquiry = scsiInquiry(conn, in, out);
            if (inquiry == null || inquiry.length < 36) {
                return true;
            }
            int type = inquiry[0] & 0x1F;
            return type == 0x05;
        } finally {
            conn.releaseInterface(intf);
            conn.close();
        }
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
            testUnitReady(conn, in, out);
            long[] cap = readCapacity(conn, in, out);
            long lastLba = cap[0];
            long blockSize = cap[1];
            if (lastLba > 0 && blockSize > 0) {
                probe.hasMedia = true;
                probe.capacityBytes = (lastLba + 1L) * blockSize;
                probe.freeBytes = probe.capacityBytes;
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
            return false;
        }
        if (usbManager.hasPermission(dev)) {
            return true;
        }

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
            while (SystemClock.uptimeMillis() < waitUntil) {
                Boolean granted = permissionResults.get(dev.getDeviceId());
                if (granted != null) {
                    permissionResults.remove(dev.getDeviceId());
                    return granted;
                }
                try {
                    permissionLock.wait(250);
                } catch (InterruptedException ignored) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
        }

        return usbManager.hasPermission(dev);
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
        if (status != 0) {
            return null;
        }

        return dataIn;
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
            if (!ACTION_USB_PERMISSION.equals(intent.getAction())) {
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
    }
}
