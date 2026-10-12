package com.jamescrumble.nova3;

import android.content.Context;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Carries the player's progress from an install under the old package name to this one.
 *
 * The game's saves are in the app's private storage, which nothing outside the app can read. An APK of
 * this same code built under the old package name (build_apk.sh with APP_ID set) updates the old install in
 * place and, on its settings screen, copies the save folders to its own Android/data folder. Once that
 * "migrate" folder has been moved into the new app's Android/data folder, the new app takes it in before
 * the game next starts.
 */
final class SaveMigration {
    /**
     * What the game writes: its home, the port's save folder, the folder used when no save folder is set, and
     * the engine's own files folder (Unity: player prefs and persistent data).
     */
    private static final String[] DIRS = { "home", "save", "game/saves", "game/files" };
    private static final String OUT = "migrate", DONE = "EXPORTED";

    private SaveMigration() { }

    /** Old package name only: copies the save folders out. Returns a line for the screen, or null. */
    static String exportIfOldInstall(Context ctx) {
        if (!ctx.getPackageName().startsWith("com.eaprules.")) return null;
        File ext = ctx.getExternalFilesDir(null);
        if (ext == null) return "Сохранения: нет доступа к папке приложения";
        File out = new File(ext, OUT);
        try {
            deleteTree(out);
            int n = 0;
            for (String d : DIRS) {
                File src = new File(ctx.getFilesDir(), d);
                if (src.isDirectory()) n += copyTree(src, new File(out, d));
            }
            new FileOutputStream(new File(out, DONE)).close();
            return "Сохранения выгружены: файлов " + n + ", папка " + out.getPath();
        } catch (IOException e) {
            return "Сохранения не выгружены: " + e;
        }
    }

    /** New install: takes in a migrate folder placed in Android/data, once. Returns a log line, or null. */
    static String importIfPresent(Context ctx) {
        File ext = ctx.getExternalFilesDir(null);
        if (ext == null) return null;
        File in = new File(ext, OUT);
        if (!new File(in, DONE).isFile()) return null;
        try {
            int n = 0;
            for (String d : DIRS) {
                File src = new File(in, d);
                if (src.isDirectory()) n += copyTree(src, new File(ctx.getFilesDir(), d));
            }
            if (!in.renameTo(new File(ext, OUT + ".done"))) new File(in, DONE).delete();
            return "saves migrated from the old install: " + n + " files";
        } catch (IOException e) {
            return "saves NOT migrated: " + e;
        }
    }

    private static int copyTree(File src, File dst) throws IOException {
        int n = 0;
        File[] list = src.listFiles();
        if (list == null) return 0;
        if (!dst.isDirectory() && !dst.mkdirs()) throw new IOException("mkdir " + dst);
        for (File f : list) {
            File t = new File(dst, f.getName());
            if (f.isDirectory()) {
                n += copyTree(f, t);
            } else if (f.isFile()) {
                try (InputStream i = new FileInputStream(f); OutputStream o = new FileOutputStream(t)) {
                    byte[] buf = new byte[1 << 16];
                    for (int r; (r = i.read(buf)) > 0; ) o.write(buf, 0, r);
                }
                n++;
            }
        }
        return n;
    }

    private static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null) for (File c : list) deleteTree(c);
        f.delete();
    }
}
