package com.eaprules.nova3;

import android.content.Context;
import android.os.Build;
import android.os.PerformanceHintManager;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Tells Android how long the game's frames take, so it clocks up the cores they run on.
 *
 * The emulated game is one busy thread in the qemu child process, and nothing about a child process
 * reads as "game" to the scheduler: on the phone it was left on a mid core with the prime cluster capped
 * at a quarter of its clock. A PerformanceHintManager session over qemu's threads, fed the real frame
 * time against a 60 fps target, is the documented way to ask for more. Sessions over another process's
 * threads are accepted for the same uid on recent Android; where they are not, this logs and does nothing.
 */
final class PerfHint {
    interface Log { void log(String s); }

    private static final long TARGET_NS = 16_666_667L;

    private final Context ctx;
    private final Log log;
    private volatile Object session;   // PerformanceHintManager.Session, kept untyped for API < 31
    private volatile int pid;
    private long lastFrame;
    private int lastThreadCount;

    PerfHint(Context ctx, Log log) {
        this.ctx = ctx;
        this.log = log;
    }

    /** The pid of a java.lang.Process: Android's toString() names it ("Process[pid=123, ...]"). */
    static int pidOf(Process p) {
        Matcher m = Pattern.compile("pid=(\\d+)").matcher(String.valueOf(p));
        if (m.find()) return Integer.parseInt(m.group(1));
        try {
            java.lang.reflect.Field f = p.getClass().getDeclaredField("pid");
            f.setAccessible(true);
            return f.getInt(p);
        } catch (Exception e) {
            return -1;
        }
    }

    void start(Process p) {
        if (Build.VERSION.SDK_INT < 31) return;
        pid = pidOf(p);
        if (pid <= 0) {
            log.log("perf hint: no pid for " + p);
            return;
        }
        Thread t = new Thread(this::watch, "nova3-perfhint");
        t.setDaemon(true);
        t.start();
    }

    /** qemu starts its guest threads over the first seconds; the session follows the thread list. */
    private void watch() {
        try {
            while (new File("/proc/" + pid).exists()) {
                int[] tids = threads();
                if (tids.length > 0 && tids.length != lastThreadCount) {
                    lastThreadCount = tids.length;
                    attach(tids);
                }
                Thread.sleep(session == null ? 1000 : 5000);
            }
        } catch (InterruptedException ignored) {
        }
    }

    private int[] threads() {
        String[] names = new File("/proc/" + pid + "/task").list();
        if (names == null) return new int[0];
        List<Integer> l = new ArrayList<>();
        for (String n : names) {
            try { l.add(Integer.parseInt(n)); } catch (NumberFormatException ignored) { }
        }
        int[] a = new int[l.size()];
        for (int i = 0; i < a.length; i++) a[i] = l.get(i);
        return a;
    }

    private void attach(int[] tids) {
        if (Build.VERSION.SDK_INT < 31) return;
        try {
            PerformanceHintManager m = ctx.getSystemService(PerformanceHintManager.class);
            if (m == null) {
                log.log("perf hint: no PerformanceHintManager");
                return;
            }
            Object s = session;
            if (s != null && Build.VERSION.SDK_INT >= 34) {
                ((PerformanceHintManager.Session) s).setThreads(tids);
            } else {
                if (s != null) ((PerformanceHintManager.Session) s).close();
                session = m.createHintSession(tids, TARGET_NS);
                // Android 15: do not trade this session's speed for power (API 35, so by reflection).
                if (session != null && Build.VERSION.SDK_INT >= 35) {
                    try {
                        session.getClass().getMethod("setPreferPowerEfficiency", boolean.class).invoke(session, false);
                    } catch (Exception e) {
                        log.log("perf hint: setPreferPowerEfficiency: " + e);
                    }
                }
            }
            log.log("perf hint: session over " + tids.length + " qemu threads: " + (session != null ? "ok" : "refused")
                    + ", preferred rate " + m.getPreferredUpdateRateNanos() + " ns");
        } catch (RuntimeException e) {
            log.log("perf hint: " + e);
        }
    }

    /**
     * Where qemu's main thread (tid == pid) last ran and at what clock: "cpu6 2649/3072 MHz".
     * The frequency limiter, not the game, is what most often decides the frame rate, so the log says it.
     */
    String cpuLine() {
        int p = pid;
        if (p <= 0) return "";
        try {
            String stat = readFirstLine("/proc/" + p + "/task/" + p + "/stat");
            // Field 39 (processor) counted after the ")" that closes the command name.
            String[] f = stat.substring(stat.lastIndexOf(')') + 2).split(" ");
            int cpu = Integer.parseInt(f[36]);
            String base = "/sys/devices/system/cpu/cpu" + cpu + "/cpufreq/";
            long cur = Long.parseLong(readFirstLine(base + "scaling_cur_freq").trim()) / 1000;
            long max = Long.parseLong(readFirstLine(base + "scaling_max_freq").trim()) / 1000;
            return "cpu" + cpu + " " + cur + "/" + max + " MHz";
        } catch (Exception e) {
            return "cpu ?";
        }
    }

    private static String readFirstLine(String path) throws java.io.IOException {
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(path))) {
            String l = r.readLine();
            return l == null ? "" : l;
        }
    }

    /** Called once per presented frame. */
    void frame() {
        long now = System.nanoTime();
        long d = now - lastFrame;
        lastFrame = now;
        Object s = session;
        if (s == null || Build.VERSION.SDK_INT < 31 || d <= 0 || d > 1_000_000_000L) return;
        try {
            PerformanceHintManager.Session ss = (PerformanceHintManager.Session) s;
            ss.reportActualWorkDuration(d);
            // Frames well over target: say the load just went up (API 33), at most twice a second.
            if (Build.VERSION.SDK_INT >= 33 && d > TARGET_NS + TARGET_NS / 4 && now - lastLoadUp > 500_000_000L) {
                lastLoadUp = now;
                loadUp(ss);
            }
        } catch (RuntimeException ignored) {
        }
    }

    private long lastLoadUp;
    private java.lang.reflect.Method sendHint;
    private boolean sendHintMissing;

    /** Session.sendHint(CPU_LOAD_UP): not in the SDK stubs this is built against, so by reflection. */
    private void loadUp(PerformanceHintManager.Session s) {
        if (sendHintMissing) return;
        try {
            if (sendHint == null) sendHint = s.getClass().getMethod("sendHint", int.class);
            sendHint.invoke(s, 0 /* Session.CPU_LOAD_UP */);
        } catch (Exception e) {
            sendHintMissing = true;
            log.log("perf hint: sendHint: " + e);
        }
    }
}
