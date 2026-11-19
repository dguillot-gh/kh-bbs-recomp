package com.psprecomp.bbs;

import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * First-run setup (2026-11-18): turns the player's own Kingdom Hearts Birth by Sleep ISO into the files the game
 * needs, in getExternalFilesDir(): game.iso (a copy of the image: the game also reads the disc by sector),
 * disc-files/ (every file of the disc, ISO 9660) and EBOOT.BIN.dec (the disc's PSP_GAME/SYSDIR/EBOOT.BIN, decrypted
 * by the bundled GPL helper libbbsdecrypt.so, run as its own process). The PGD-encrypted BBS1-3.DAT are decrypted
 * later by the same helper, the first time the game opens each one with its key (disc_io.cpp, into decrypted/).
 * Nothing is downloaded; nothing from the disc ships with the app.
 */
final class IsoInstaller {
    static final String GAME_ID = "ULUS-10505";
    private static final int SECTOR = 2048;

    interface Progress { void update(String what, long done, long total); }

    static final class SetupException extends Exception {
        SetupException(String message) { super(message); }
    }

    private static final class Entry {
        final String path; final long lba; final long size; final boolean dir;
        Entry(String path, long lba, long size, boolean dir) { this.path = path; this.lba = lba; this.size = size; this.dir = dir; }
    }

    static File eboot(File data) { return new File(data, "EBOOT.BIN.dec"); }
    static File iso(File data) { return new File(data, "game.iso"); }
    /** Room for the decrypted BBS1-3.DAT the game writes on first use. */
    static final long PGD_RESERVE = 700L << 20;
    static File disc(File data) { return new File(data, "disc-files"); }

    /** True when the game files are in place (from an ISO import, or pushed by hand as before). */
    static boolean installed(File data) {
        return eboot(data).isFile() && new File(disc(data), "PSP_GAME/USRDIR/BBS0.DAT").isFile() && anyIso(data) != null;
    }

    /** game.iso, else any *.iso in the folder (setups made by hand before the launcher). */
    static File anyIso(File data) {
        if (iso(data).isFile()) return iso(data);
        File[] all = data.listFiles((d, n) -> n.toLowerCase(java.util.Locale.ROOT).endsWith(".iso"));
        return all != null && all.length > 0 ? all[0] : null;
    }

    /** The disc's game id from UMD_DATA.BIN ("ULUS-10505|..."), or null. */
    static String gameId(File discRoot) {
        File umd = new File(discRoot, "UMD_DATA.BIN");
        if (!umd.isFile()) return null;
        try (InputStream in = new FileInputStream(umd)) {
            byte[] b = new byte[10];
            return in.read(b) == 10 ? new String(b, StandardCharsets.US_ASCII) : null;
        } catch (IOException e) {
            return null;
        }
    }

    static void install(Context context, Uri iso, Progress progress) throws Exception {
        try (ParcelFileDescriptor pfd = context.getContentResolver().openFileDescriptor(iso, "r")) {
            if (pfd == null) throw new SetupException("Can't open the file.");
            try (FileInputStream stream = new FileInputStream(pfd.getFileDescriptor())) {
                install(context, stream.getChannel(), progress);
            }
        }
    }

    static void install(Context context, File iso, Progress progress) throws Exception {
        try (FileInputStream stream = new FileInputStream(iso)) {
            install(context, stream.getChannel(), progress);
        }
    }

    private static void install(Context context, FileChannel ch, Progress progress) throws Exception {
        File data = context.getExternalFilesDir(null);
        if (data == null) throw new SetupException("No app storage available.");
        progress.update("Reading the disc…", 0, 1);
        byte[] pvd = read(ch, 16L * SECTOR, SECTOR);
        if (pvd[0] != 1 || !new String(pvd, 1, 5, StandardCharsets.US_ASCII).equals("CD001"))
            throw new SetupException("This isn't an ISO disc image. Pick the .iso of your UMD (CSO isn't supported).");
        List<Entry> entries = new ArrayList<>();
        walk(ch, pvd, 156, "", entries, 0);
        Entry umd = find(entries, "UMD_DATA.BIN");
        String id = umd == null ? null : new String(read(ch, umd.lba * SECTOR, 10), StandardCharsets.US_ASCII);
        if (!GAME_ID.equals(id))
            throw new SetupException("This port needs Kingdom Hearts Birth by Sleep, USA (" + GAME_ID + "). "
                    + "This disc is " + (id == null ? "not a PSP game" : id) + ".");
        Entry ebootEntry = find(entries, "PSP_GAME/SYSDIR/EBOOT.BIN");
        if (ebootEntry == null) throw new SetupException("The disc has no PSP_GAME/SYSDIR/EBOOT.BIN.");
        long total = 0;
        for (Entry e : entries) if (!e.dir) total += e.size;
        final long isoSize = ch.size();
        long free = new StatFs(data.getAbsolutePath()).getAvailableBytes();
        File oldDisc = disc(data);  // removed only after the new copy is complete, so it doesn't count as free
        long need = total + isoSize + PGD_RESERVE + (64L << 20);
        if (free < need)
            throw new SetupException(String.format("Not enough free space: %.1f GB needed, %.1f GB free.",
                    need / 1e9, free / 1e9));

        // Copy into disc-files.importing, then swap it in: a cancelled or failed import leaves the old files alone.
        File staging = new File(data, "disc-files.importing");
        deleteTree(staging);
        try {
            copyAndDecrypt(context, ch, entries, staging, data, total, isoSize, progress);
        } catch (Exception e) {
            deleteTree(staging);
            //noinspection ResultOfMethodCallIgnored
            new File(data, "game.iso.importing").delete();
            //noinspection ResultOfMethodCallIgnored
            new File(data, "EBOOT.BIN.dec.importing").delete();
            throw e;
        }
        progress.update("Finishing…", total, total);
        deleteTree(oldDisc);
        if (!staging.renameTo(oldDisc)) throw new SetupException("Couldn't move the game files into place.");
        File eboot = eboot(data);
        //noinspection ResultOfMethodCallIgnored
        eboot.delete();
        if (!new File(data, "EBOOT.BIN.dec.importing").renameTo(eboot))
            throw new SetupException("Couldn't move the game program into place.");
        File iso = iso(data);
        //noinspection ResultOfMethodCallIgnored
        iso.delete();
        if (!new File(data, "game.iso.importing").renameTo(iso))
            throw new SetupException("Couldn't move the disc image into place.");
        // Decrypted data from an earlier disc copy belongs to that copy; the game makes it again on first use.
        deleteTree(new File(data, "decrypted"));
    }

    private static void copyAndDecrypt(Context context, FileChannel ch, List<Entry> entries, File staging, File data,
                                       long total, long isoSize, Progress progress) throws Exception {
        // 1. The disc image itself (raw sector reads).
        long done = 0;
        final long all = isoSize + total;
        byte[] buffer = new byte[1 << 20];
        try (FileOutputStream out = new FileOutputStream(new File(data, "game.iso.importing"))) {
            long at = 0;
            while (at < isoSize) {
                int n = (int) Math.min(buffer.length, isoSize - at);
                ByteBuffer bb = ByteBuffer.wrap(buffer, 0, n);
                while (bb.hasRemaining()) {
                    int r = ch.read(bb, at + bb.position());
                    if (r < 0) throw new SetupException("The ISO ends early: is the file complete?");
                }
                out.write(buffer, 0, n);
                at += n; done += n;
                progress.update("Copying the disc image…", done, all);
            }
        }
        // 2. Every file of the disc.
        for (Entry e : entries) {
            File target = new File(staging, e.path);
            if (e.dir) { mkdirs(target); continue; }
            mkdirs(target.getParentFile());
            try (FileOutputStream out = new FileOutputStream(target)) {
                long at = e.lba * SECTOR, left = e.size;
                while (left > 0) {
                    int n = (int) Math.min(buffer.length, left);
                    ByteBuffer bb = ByteBuffer.wrap(buffer, 0, n);
                    while (bb.hasRemaining()) {
                        int r = ch.read(bb, at + bb.position());
                        if (r < 0) throw new SetupException("The ISO ends early: is the file complete?");
                    }
                    out.write(buffer, 0, n);
                    at += n; left -= n; done += n;
                    progress.update("Copying game files…", done, all);
                }
            }
        }

        progress.update("Decrypting the game program…", all, all);
        decrypt(context, new File(staging, "PSP_GAME/SYSDIR/EBOOT.BIN"), new File(data, "EBOOT.BIN.dec.importing"));
    }

    /** Runs the bundled decrypter (GPL-3.0, android/decrypt) as a separate process. */
    static void decrypt(Context context, File in, File out) throws Exception {
        File helper = new File(context.getApplicationInfo().nativeLibraryDir, "libbbsdecrypt.so");
        if (!helper.isFile()) throw new SetupException("This build has no EBOOT decrypter (libbbsdecrypt.so).");
        Process p = new ProcessBuilder(helper.getAbsolutePath(), in.getAbsolutePath(), out.getAbsolutePath())
                .redirectErrorStream(true).start();
        StringBuilder log = new StringBuilder();
        try (InputStream s = p.getInputStream()) {
            byte[] b = new byte[512];
            int n;
            while ((n = s.read(b)) > 0) log.append(new String(b, 0, n, StandardCharsets.UTF_8));
        }
        int code = p.waitFor();
        if (code != 0 || !out.isFile()) throw new SetupException("Decrypting EBOOT.BIN failed (" + code + "): " + log.toString().trim());
        byte[] magic = new byte[4];
        try (InputStream s = new FileInputStream(out)) {
            if (s.read(magic) != 4 || magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F')
                throw new SetupException("The decrypted EBOOT isn't a valid program.");
        }
    }

    // ---- ISO 9660 ----

    private static void walk(FileChannel ch, byte[] sector, int recordAt, String prefix, List<Entry> out, int depth)
            throws Exception {
        if (depth > 16) return;
        ByteBuffer rec = ByteBuffer.wrap(sector, recordAt, 34).order(ByteOrder.LITTLE_ENDIAN);
        long lba = rec.getInt(recordAt + 2) & 0xFFFFFFFFL;
        long size = rec.getInt(recordAt + 10) & 0xFFFFFFFFL;
        byte[] dir = read(ch, lba * SECTOR, (int) size);
        int at = 0;
        while (at < dir.length) {
            int len = dir[at] & 0xFF;
            if (len == 0) { at = (at / SECTOR + 1) * SECTOR; continue; }  // records don't cross sectors
            int nameLen = dir[at + 32] & 0xFF;
            boolean isDir = (dir[at + 25] & 0x02) != 0;
            if (!(nameLen == 1 && (dir[at + 33] == 0 || dir[at + 33] == 1))) {  // skip "." and ".."
                String name = new String(dir, at + 33, nameLen, StandardCharsets.US_ASCII);
                int semi = name.indexOf(';');
                if (semi >= 0) name = name.substring(0, semi);
                if (name.endsWith(".")) name = name.substring(0, name.length() - 1);
                if (name.contains("/") || name.contains("\\") || name.equals("..") || name.isEmpty())
                    throw new SetupException("Unexpected file name on the disc: " + name);
                String path = prefix + name;
                ByteBuffer r = ByteBuffer.wrap(dir).order(ByteOrder.LITTLE_ENDIAN);
                long childLba = r.getInt(at + 2) & 0xFFFFFFFFL, childSize = r.getInt(at + 10) & 0xFFFFFFFFL;
                out.add(new Entry(path, childLba, childSize, isDir));
                if (isDir) walk(ch, dir, at, path + "/", out, depth + 1);
            }
            at += len;
        }
    }

    private static Entry find(List<Entry> entries, String path) {
        for (Entry e : entries) if (!e.dir && e.path.equalsIgnoreCase(path)) return e;
        return null;
    }

    private static byte[] read(FileChannel ch, long at, int size) throws Exception {
        byte[] b = new byte[size];
        ByteBuffer bb = ByteBuffer.wrap(b);
        while (bb.hasRemaining()) {
            int r = ch.read(bb, at + bb.position());
            if (r < 0) throw new SetupException("The ISO ends early: is the file complete?");
        }
        return b;
    }

    private static void mkdirs(File dir) throws IOException {
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("mkdir " + dir);
    }

    static void deleteTree(File f) {
        if (f == null || !f.exists()) return;
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
