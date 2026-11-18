package com.psprecomp.bbs;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Installs a PPSSPP-format texture pack (a ZIP with a textures.ini somewhere inside, e.g. the GitHub archive of
 * BBS HD ReMix, which nests everything in "Birth-by-Sleep-HD-ReMix-master/") into getExternalFilesDir()/textures.
 * 2026-11-18. The pack is never shipped with the app: the player imports a ZIP or downloads it from its own page.
 *
 * One pass: unzip into textures.importing/, find the folder holding textures.ini, move it to textures/ (a rename on
 * the same storage), delete the rest. Entries that would land outside the target folder are refused (zip-slip).
 */
final class TexturePackInstaller {
    interface Progress {
        void onProgress(int files, long bytes);
    }

    static File packDir(File externalFiles) {
        return new File(externalFiles, "textures");
    }

    static boolean installed(File externalFiles) {
        return new File(packDir(externalFiles), "textures.ini").isFile();
    }

    /** Returns null on success, else a message for the player. */
    static String install(InputStream raw, File externalFiles, Progress progress) {
        File staging = new File(externalFiles, "textures.importing");
        deleteTree(staging);
        if (!staging.mkdirs()) return "Couldn't create a folder for the pack (storage full?)";
        String stagingRoot;
        try {
            stagingRoot = staging.getCanonicalPath() + File.separator;
        } catch (IOException e) {
            return "Storage error: " + e.getMessage();
        }
        File iniDir = null;
        int files = 0;
        long bytes = 0;
        byte[] buffer = new byte[1 << 16];
        try (ZipInputStream zip = new ZipInputStream(raw)) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                File out = new File(staging, entry.getName());
                if (!out.getCanonicalPath().startsWith(stagingRoot)) {
                    return "This ZIP contains an unsafe path and was not installed";
                }
                if (entry.isDirectory()) {
                    //noinspection ResultOfMethodCallIgnored
                    out.mkdirs();
                    continue;
                }
                File parent = out.getParentFile();
                if (parent != null) //noinspection ResultOfMethodCallIgnored
                    parent.mkdirs();
                try (OutputStream o = new BufferedOutputStream(new FileOutputStream(out), 1 << 16)) {
                    int n;
                    while ((n = zip.read(buffer)) > 0) {
                        o.write(buffer, 0, n);
                        bytes += n;
                    }
                }
                if (out.getName().equalsIgnoreCase("textures.ini")) {
                    // The shallowest textures.ini is the pack root (packs may carry extra ones in sub-folders).
                    if (iniDir == null || depth(out.getParentFile()) < depth(iniDir)) iniDir = out.getParentFile();
                }
                if ((++files & 63) == 0 && progress != null) progress.onProgress(files, bytes);
            }
        } catch (IOException e) {
            deleteTree(staging);
            return "Couldn't read the ZIP: " + e.getMessage();
        }
        if (iniDir == null) {
            deleteTree(staging);
            return "No textures.ini in this ZIP: it isn't a PPSSPP texture pack";
        }
        File target = packDir(externalFiles);
        deleteTree(target);
        if (!iniDir.renameTo(target)) {
            deleteTree(staging);
            return "Couldn't move the pack into place";
        }
        deleteTree(staging);
        if (progress != null) progress.onProgress(files, bytes);
        return null;
    }

    static void remove(File externalFiles) {
        deleteTree(packDir(externalFiles));
        deleteTree(new File(externalFiles, "textures.importing"));
    }

    private static int depth(File f) {
        int d = 0;
        for (File p = f; p != null; p = p.getParentFile()) d++;
        return d;
    }

    static void deleteTree(File f) {
        if (f == null || !f.exists()) return;
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    private TexturePackInstaller() {}
}
