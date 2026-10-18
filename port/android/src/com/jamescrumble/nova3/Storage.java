package com.jamescrumble.nova3;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Where the player's files live: the app's user-visible folder on shared storage.
 *
 * Nothing in here is specific to one game: every path is derived from the package name, so the class can be
 * copied as it is into any other bridge app (see docs/STORAGE.md).
 *
 * There are two folders:
 * <ul>
 * <li>shared: {@code <storage root>/BridgeGames/<package>/}, the main one. Needs "all files access" (API 30+) or
 *     the storage permission (API 26-28); the app creates it itself.
 * <li>legacy: {@code Android/data/<package>/files/}, always available to the app, and the only one without
 *     the permission.
 * </ul>
 * With access, everything is moved from legacy to shared and shared is the main folder. Files are read from
 * both, main first, so nothing is lost if the permission is taken away or a file could not be moved.
 */
final class Storage {
    static final String ROOT_NAME = "BridgeGames";

    interface Progress { void status(String text); }

    private Storage() { }

    // ------------------------------------------------------------ access

    /** Can this app read and write the shared folder right now? (API 29 keeps scoped storage: no.) */
    static boolean hasAccess(Context c) {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        if (Build.VERSION.SDK_INT >= 29) return false;
        return c.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    /** API 30+: the system page where the user allows access to all files for this app. */
    static Intent accessIntent(Context c) {
        return new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + c.getPackageName()));
    }

    /** Asks for the access: the system page on API 30+, the storage permission dialog before that. */
    static void requestAccess(Activity a, int requestCode) {
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                a.startActivity(accessIntent(a));
            } catch (RuntimeException e) {
                a.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else if (Build.VERSION.SDK_INT < 29) {
            a.requestPermissions(runtimePermissions(a, true), requestCode);
        }
    }

    /** Runtime permissions of API < 30 that are not granted yet (all of them when {@code all}). */
    static String[] runtimePermissions(Context c, boolean all) {
        List<String> out = new ArrayList<>();
        for (String p : new String[] { Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE }) {
            if (p.equals(Manifest.permission.WRITE_EXTERNAL_STORAGE) && Build.VERSION.SDK_INT >= 29) continue;
            if (all || c.checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) out.add(p);
        }
        return out.toArray(new String[0]);
    }

    // ------------------------------------------------------------ folders

    /** {@code /storage/emulated/0/BridgeGames/<package>}: only meaningful with access. */
    static File shared(Context c) {
        return new File(new File(Environment.getExternalStorageDirectory(), ROOT_NAME), c.getPackageName());
    }

    /** {@code Android/data/<package>/files}, or null when the system gives none. */
    static File legacy(Context c) {
        return c.getExternalFilesDir(null);
    }

    /** The main folder: shared (created when missing) with access, otherwise legacy. */
    static File mainDir(Context c) {
        if (hasAccess(c)) {
            File s = shared(c);
            if ((s.isDirectory() || s.mkdirs()) && s.canWrite()) return s;
        }
        File l = legacy(c);
        return l != null ? l : c.getFilesDir();
    }

    /** Main folder first, then the other one if it exists and can be read. Never empty. */
    static List<File> sources(Context c) {
        List<File> out = new ArrayList<>();
        File main = mainDir(c);
        out.add(main);
        File[] others = { shared(c), legacy(c) };
        for (File o : others) {
            if (o == null || o.equals(main) || !o.isDirectory() || !o.canRead()) continue;
            if (o.equals(shared(c)) && !hasAccess(c)) continue;
            out.add(o);
        }
        return out;
    }

    /** First file with this suffix in any source folder, or null. */
    static File firstWithSuffix(Context c, String suffix) {
        for (File dir : sources(c)) {
            File[] files = dir.listFiles((d, n) -> n.endsWith(suffix));
            if (files == null || files.length == 0) continue;
            Arrays.sort(files);
            return files[0];
        }
        return null;
    }

    /** The existing file with this name in the main folder or the other one; the main folder's path when none. */
    static File find(Context c, String name) {
        for (File dir : sources(c)) {
            File f = new File(dir, name);
            if (f.exists()) return f;
        }
        return new File(mainDir(c), name);
    }

    static void deleteEverywhere(Context c, String name) {
        for (File dir : sources(c)) new File(dir, name).delete();
    }

    /** Both folders for a message to the player. */
    static String describe(Context c) {
        File l = legacy(c);
        return shared(c).getPath() + (l != null ? "\nили " + l.getPath() : "");
    }

    // ------------------------------------------------------------ moving

    /**
     * With access: makes the shared folder and moves everything from the legacy folder into it, file by file
     * (copy under a ".part" name, rename, delete the original, size checked). A file that is already there stays; the
     * legacy copy is dropped only when it has the same size, and a smaller one (a cut-short earlier move) is replaced.
     * Returns the number of files moved. Safe to call often, from any thread, and to be interrupted.
     */
    static synchronized int migrate(Context c, Progress p) {
        File main = mainDir(c), legacy = legacy(c);
        if (legacy == null || !legacy.isDirectory() || !main.equals(shared(c))) return 0;
        File[] list = legacy.listFiles();
        if (list == null) return 0;
        int n = 0;
        for (File f : list) n += moveInto(f, new File(main, f.getName()), p);
        return n;
    }

    private static int moveInto(File src, File dst, Progress p) {
        try {
            if (src.isDirectory()) {
                // Never a directory rename: it may be a copy inside the storage service that an interruption
                // leaves half done. Files are moved one by one below.
                if (!dst.isDirectory() && !dst.mkdirs()) return 0;
                int n = 0;
                File[] list = src.listFiles();
                if (list != null) for (File f : list) n += moveInto(f, new File(dst, f.getName()), p);
                src.delete();
                return n;
            }
            if (!src.isFile()) return 0;
            if (dst.exists()) {
                // Same size: it is the same file, the legacy copy is not needed. Smaller than the original: an earlier
                // move was cut short, the original is still whole, so copy it again. Anything else is left alone.
                if (dst.length() == src.length()) {
                    src.delete();
                    return 0;
                }
                if (dst.length() > src.length()) return 0;
            }
            if (p != null) p.status("Перенос в " + dst.getParentFile().getPath() + "…\n" + src.getName());
            // Copy under a temporary name and rename in the same folder: the final name never holds half a file.
            File tmp = new File(dst.getPath() + ".part");
            tmp.delete();
            copyFile(src, tmp);
            if (tmp.length() != src.length()) {
                tmp.delete();
                return 0;
            }
            dst.delete();
            if (!tmp.renameTo(dst)) {
                tmp.delete();
                return 0;
            }
            dst.setLastModified(src.lastModified());
            src.delete();
            return 1;
        } catch (IOException e) {
            return 0;
        }
    }

    // ------------------------------------------------------------ saves

    /**
     * The folder the game saves into. With access that is {@code <shared>/save}: seeded once from the app's
     * private folder, and the private one is then kept as a mirror of the newest files, so saves stay available
     * under either location. Without access it is the private folder itself.
     */
    static File saveDir(Context c, File privateSave) {
        File main = mainDir(c);
        if (!main.equals(shared(c))) return privateSave;
        File dst = new File(main, "save");
        if (!dst.isDirectory() && !dst.mkdirs()) return privateSave;
        if (!dst.canWrite()) return privateSave;
        try {
            String[] have = dst.list();
            if (have == null || have.length == 0) copyNewer(privateSave, dst);
            copyNewer(dst, privateSave);
        } catch (IOException ignored) { }
        return dst;
    }

    /** Copies files that are missing in {@code dst} or older than the ones in {@code src}, keeping their times. */
    static int copyNewer(File src, File dst) throws IOException {
        File[] list = src.listFiles();
        if (list == null) return 0;
        if (!dst.isDirectory() && !dst.mkdirs()) throw new IOException("mkdir " + dst);
        int n = 0;
        for (File f : list) {
            File t = new File(dst, f.getName());
            if (f.isDirectory()) {
                n += copyNewer(f, t);
            } else if (f.isFile() && (!t.exists() || f.lastModified() > t.lastModified())) {
                copyFile(f, t);
                t.setLastModified(f.lastModified());
                n++;
            }
        }
        return n;
    }

    static void copyFile(File src, File dst) throws IOException {
        try (InputStream in = new FileInputStream(src); OutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[1 << 16];
            for (int r; (r = in.read(buf)) > 0; ) out.write(buf, 0, r);
        }
    }
}
