package com.eaprules.nova3;

import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES30;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructPollfd;
import android.view.Surface;

import java.io.FileDescriptor;
import java.io.InterruptedIOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.Charset;
import java.util.Locale;

/**
 * Host side of the GLES bridge. The game runs under qemu and writes its OpenGL ES calls into a pipe
 * (portbase/src/gl_bridge.cpp describes the format); this thread owns an EGL context on the phone's GPU,
 * replays the stream and answers the synchronous queries over a second pipe.
 *
 * Object names are chosen by the guest; the maps in {@link Ids} translate them to the real ones.
 * Client-side vertex arrays arrive inside the draw message and are streamed through scratch buffers.
 */
final class GlBridge implements Runnable {
    interface Log { void log(String s); }

    static final class IdMap {
        private int[] a = new int[1024];

        int get(int guest) { return guest > 0 && guest < a.length ? a[guest] : 0; }

        void put(int guest, int host) {
            if (guest <= 0) return;
            if (guest >= a.length) {
                int n = a.length;
                while (n <= guest) n *= 2;
                a = java.util.Arrays.copyOf(a, n);
            }
            a[guest] = host;
        }

        int reverse(int host) {
            if (host == 0) return 0;
            for (int i = 1; i < a.length; i++) if (a[i] == host) return i;
            return 0;
        }
    }

    static final class Ids {
        final IdMap tex = new IdMap(), buf = new IdMap(), fbo = new IdMap(), rbo = new IdMap(),
                prog = new IdMap(), shader = new IdMap();
    }

    private static final Charset LATIN1 = Charset.forName("ISO-8859-1");
    private static final int GEN_TEX = 0, GEN_BUF = 1, GEN_FBO = 2, GEN_RBO = 3;
    private static final int POLL_MS = 200;

    private final String cmdPath, repPath;
    private final Log log;
    private final Ids ids = new Ids();
    private final Object lock = new Object();

    private volatile boolean alive;
    private Thread thread;
    private Surface pendingSurface;
    private boolean surfaceDirty;

    private FileDescriptor cmd, rep;
    private final ByteBuffer hdr = ByteBuffer.allocateDirect(8).order(ByteOrder.LITTLE_ENDIAN);
    private ByteBuffer pay = ByteBuffer.allocateDirect(1 << 20).order(ByteOrder.LITTLE_ENDIAN);
    private ByteBuffer out = ByteBuffer.allocateDirect(1 << 16).order(ByteOrder.LITTLE_ENDIAN);

    private EGLDisplay dpy = EGL14.EGL_NO_DISPLAY;
    private EGLContext ctx = EGL14.EGL_NO_CONTEXT;
    private EGLConfig cfg;
    private EGLSurface pbuf = EGL14.EGL_NO_SURFACE;
    private EGLSurface window = EGL14.EGL_NO_SURFACE;

    private int curArray, curElem;
    private final int[] scratchVbo = new int[16];
    private int scratchEbo;
    private float[] fs = new float[256];
    private int[] is = new int[256];

    private volatile long swaps;
    private long cmds, bytes, lastReport, lastSwaps, glErrors, execErrors, busyNanos;
    private final long[] opCounts = new long[256], opBytes = new long[256];

    /** The busiest ops of the last report interval, by count and by bytes, then reset. */
    private String opReport(double frames) {
        StringBuilder sb = new StringBuilder();
        // Synchronous queries stall the whole pipeline, so they are listed whatever their count.
        sb.append(" | sync/frame:");
        for (int op = GlOps.OP_Q_GET_STRING; op <= GlOps.OP_Q_READ_PIXELS; op++) {
            if (opCounts[op] > 0) sb.append(String.format(Locale.US, " %d=%.2f", op, opCounts[op] / Math.max(1.0, frames)));
        }
        for (int pass = 0; pass < 2; pass++) {
            long[] v = pass == 0 ? opCounts : opBytes;
            sb.append(pass == 0 ? " | per frame: " : " | bytes/frame: ");
            boolean[] used = new boolean[v.length];
            for (int k = 0; k < 10; k++) {
                int best = -1;
                for (int i = 0; i < v.length; i++) if (!used[i] && v[i] > 0 && (best < 0 || v[i] > v[best])) best = i;
                if (best < 0) break;
                used[best] = true;
                sb.append(String.format(Locale.US, "%d=%.0f ", best, v[best] / Math.max(1.0, frames)));
            }
        }
        java.util.Arrays.fill(opCounts, 0);
        java.util.Arrays.fill(opBytes, 0);
        return sb.toString();
    }

    interface Telemetry { String line(); }

    private final Runnable onFrame;
    private final Telemetry telemetry;

    GlBridge(String cmdPath, String repPath, Log log, Runnable onFrame, Telemetry telemetry) {
        this.cmdPath = cmdPath;
        this.repPath = repPath;
        this.log = log;
        this.onFrame = onFrame;
        this.telemetry = telemetry;
    }

    long swaps() { return swaps; }

    void start() {
        alive = true;
        thread = new Thread(this, "nova3-gl");
        thread.start();
    }

    void stop() {
        alive = false;
    }

    /** The surface to present on (null while the app is in the background). */
    void setSurface(Surface s) {
        synchronized (lock) {
            pendingSurface = s;
            surfaceDirty = true;
        }
    }

    // ---------------------------------------------------------------- thread

    @Override public void run() {
        try {
            cmd = Os.open(cmdPath, OsConstants.O_RDWR | OsConstants.O_NONBLOCK, 0);
            rep = Os.open(repPath, OsConstants.O_RDWR, 0);
            try { Os.fcntlInt(cmd, 1031 /* F_SETPIPE_SZ */, 1 << 20); } catch (ErrnoException ignored) { }
            initEgl();
            lastReport = System.nanoTime();
            loop();
        } catch (Throwable t) {
            log.log("bridge thread died: " + t);
        } finally {
            alive = false;
            teardown();
        }
    }

    private void initEgl() {
        dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] ver = new int[2];
        if (!EGL14.eglInitialize(dpy, ver, 0, ver, 1)) throw new IllegalStateException("eglInitialize failed");

        // The game never uses destination alpha on screen; an opaque surface avoids see-through output.
        int[][] wanted = {
            { 8, 8, 8, 0, 24, 8 }, { 8, 8, 8, 8, 24, 8 }, { 8, 8, 8, 0, 16, 0 }, { 8, 8, 8, 8, 16, 0 },
        };
        EGLConfig[] configs = new EGLConfig[1];
        int[] n = new int[1];
        for (int[] w : wanted) {
            int[] attr = {
                EGL14.EGL_RENDERABLE_TYPE, EGLExt.EGL_OPENGL_ES3_BIT_KHR,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT | EGL14.EGL_PBUFFER_BIT,
                EGL14.EGL_RED_SIZE, w[0], EGL14.EGL_GREEN_SIZE, w[1], EGL14.EGL_BLUE_SIZE, w[2],
                EGL14.EGL_ALPHA_SIZE, w[3], EGL14.EGL_DEPTH_SIZE, w[4], EGL14.EGL_STENCIL_SIZE, w[5],
                EGL14.EGL_NONE,
            };
            if (EGL14.eglChooseConfig(dpy, attr, 0, configs, 0, 1, n, 0) && n[0] > 0) {
                cfg = configs[0];
                log.log(String.format(Locale.US, "EGL config: rgba %d%d%d%d depth %d stencil %d", w[0], w[1], w[2], w[3], w[4], w[5]));
                break;
            }
        }
        if (cfg == null) throw new IllegalStateException("no usable EGL config");

        int[] ctxAttr = { EGL14.EGL_CONTEXT_CLIENT_VERSION, 3, EGL14.EGL_NONE };
        ctx = EGL14.eglCreateContext(dpy, cfg, EGL14.EGL_NO_CONTEXT, ctxAttr, 0);
        if (ctx == EGL14.EGL_NO_CONTEXT) throw new IllegalStateException("eglCreateContext failed");
        pbuf = EGL14.eglCreatePbufferSurface(dpy, cfg, new int[] { EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE }, 0);
        if (!EGL14.eglMakeCurrent(dpy, pbuf, pbuf, ctx)) throw new IllegalStateException("eglMakeCurrent failed");

        log.log("GL_RENDERER " + GLES30.glGetString(GLES30.GL_RENDERER) + ", GL_VERSION " + GLES30.glGetString(GLES30.GL_VERSION));
        log.log("GL_EXTENSIONS " + GLES30.glGetString(GLES30.GL_EXTENSIONS));
    }

    private void teardown() {
        try {
            if (dpy != EGL14.EGL_NO_DISPLAY) {
                EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                if (window != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(dpy, window);
                if (pbuf != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(dpy, pbuf);
                if (ctx != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(dpy, ctx);
                EGL14.eglTerminate(dpy);
            }
        } catch (RuntimeException ignored) {
        }
        try { if (cmd != null) Os.close(cmd); } catch (ErrnoException ignored) { }
        try { if (rep != null) Os.close(rep); } catch (ErrnoException ignored) { }
    }

    private void loop() throws ErrnoException, InterruptedIOException {
        while (alive) {
            if (!readFully(hdr, 8)) return;
            int len = hdr.getInt(0), op = hdr.getInt(4);
            int plen = len - 8;
            if (len < 8 || (len & 3) != 0 || plen > (128 << 20)) {
                log.log("bad message length " + len + " (op " + op + "), stopping");
                return;
            }
            if (plen > pay.capacity()) {
                int c = pay.capacity();
                while (c < plen) c *= 2;
                pay = ByteBuffer.allocateDirect(c).order(ByteOrder.LITTLE_ENDIAN);
            }
            if (plen > 0 && !readFully(pay, plen)) return;
            pay.clear();
            pay.limit(plen);
            cmds++;
            bytes += len;
            if (op >= 0 && op < opCounts.length) {
                opCounts[op]++;
                opBytes[op] += len;
            }
            long t0 = System.nanoTime();
            try {
                dispatch(op, plen);
                busyNanos += System.nanoTime() - t0;
            } catch (RuntimeException e) {
                if (execErrors++ < 20) log.log("op " + op + " failed: " + e);
            }
        }
    }

    /** Fills the first n bytes of b from the command pipe; false when asked to stop. */
    private boolean readFully(ByteBuffer b, int n) throws ErrnoException, InterruptedIOException {
        b.clear();
        b.limit(n);
        while (b.hasRemaining()) {
            try {
                int r = Os.read(cmd, b);
                if (r == 0) pollReadable();
            } catch (ErrnoException e) {
                if (e.errno != OsConstants.EAGAIN) throw e;
                if (!pollReadable() && !alive) return false;
            }
            if (!alive) return false;
        }
        return true;
    }

    private boolean pollReadable() throws ErrnoException, InterruptedIOException {
        StructPollfd p = new StructPollfd();
        p.fd = cmd;
        p.events = (short) OsConstants.POLLIN;
        return Os.poll(new StructPollfd[] { p }, POLL_MS) > 0;
    }

    // ---------------------------------------------------------------- replies

    private void reply(byte[] data, int n) {
        int total = 4 + n;
        if (out.capacity() < total) out = ByteBuffer.allocateDirect(Math.max(total, out.capacity() * 2)).order(ByteOrder.LITTLE_ENDIAN);
        out.clear();
        out.putInt(n);
        if (n > 0) out.put(data, 0, n);
        out.flip();
        try {
            while (out.hasRemaining()) Os.write(rep, out);
        } catch (ErrnoException | InterruptedIOException e) {
            log.log("reply failed: " + e);
            alive = false;
        }
    }

    private void reply(byte[] data) { reply(data, data.length); }

    private void replyInts(int[] v, int n) {
        ByteBuffer b = ByteBuffer.allocate(n * 4).order(ByteOrder.LITTLE_ENDIAN);
        for (int i = 0; i < n; i++) b.putInt(v[i]);
        reply(b.array());
    }

    private void replyString(String s) { reply(s == null ? new byte[0] : s.getBytes(LATIN1)); }

    /** Tells the guest the frame has been presented, so it can start the one after next. */
    private void ackFrame() {
        out.clear();
        out.putInt(-1);
        out.flip();
        try {
            while (out.hasRemaining()) Os.write(rep, out);
        } catch (ErrnoException | InterruptedIOException e) {
            alive = false;
        }
    }

    // ---------------------------------------------------------------- decoding helpers

    private int i() { return pay.getInt(); }

    /** The next n bytes as a direct buffer (null when n < 0), consuming them padded to 4. */
    private ByteBuffer data(int n) {
        if (n < 0) return null;
        ByteBuffer d = pay.slice().order(ByteOrder.LITTLE_ENDIAN);
        d.limit(n);
        pay.position(pay.position() + ((n + 3) & ~3));
        return d;
    }

    private static int[] one() { return new int[1]; }

    // ---------------------------------------------------------------- dispatch

    private void dispatch(int op, int plen) {
        ByteBuffer b = pay;
        if (GlOps.exec(op, b, ids)) return;
        switch (op) {
            case GlOps.OP_SWAP: swap(); break;
            case GlOps.OP_GEN: gen(); break;
            case GlOps.OP_DELETE: delete(); break;
            case GlOps.OP_BIND_BUFFER: {
                int target = i(), id = i();
                if (target == GLES30.GL_ARRAY_BUFFER) curArray = id;
                else if (target == GLES30.GL_ELEMENT_ARRAY_BUFFER) curElem = id;
                GLES30.glBindBuffer(target, ids.buf.get(id));
                break;
            }
            case GlOps.OP_BUFFER_DATA: {
                int target = i(), size = i(), usage = i(), n = i();
                GLES30.glBufferData(target, size, data(n), usage);
                break;
            }
            case GlOps.OP_BUFFER_SUB_DATA: {
                int target = i(), offset = i(), size = i(), n = i();
                GLES30.glBufferSubData(target, offset, size, data(n));
                break;
            }
            case GlOps.OP_TEX_IMAGE_2D: {
                int t = i(), lv = i(), ifmt = i(), w = i(), h = i(), bd = i(), f = i(), ty = i(), n = i();
                GLES30.glTexImage2D(t, lv, ifmt, w, h, bd, f, ty, data(n));
                break;
            }
            case GlOps.OP_TEX_SUB_IMAGE_2D: {
                int t = i(), lv = i(), x = i(), y = i(), w = i(), h = i(), f = i(), ty = i(), n = i();
                GLES30.glTexSubImage2D(t, lv, x, y, w, h, f, ty, data(n));
                break;
            }
            case GlOps.OP_COMPRESSED_TEX_IMAGE_2D: {
                int t = i(), lv = i(), ifmt = i(), w = i(), h = i(), bd = i(), size = i(), n = i();
                GLES30.glCompressedTexImage2D(t, lv, ifmt, w, h, bd, size, data(n));
                break;
            }
            case GlOps.OP_COMPRESSED_TEX_SUB_IMAGE_2D: {
                int t = i(), lv = i(), x = i(), y = i(), w = i(), h = i(), f = i(), size = i(), n = i();
                GLES30.glCompressedTexSubImage2D(t, lv, x, y, w, h, f, size, data(n));
                break;
            }
            case GlOps.OP_TEX_IMAGE_3D: {
                int t = i(), lv = i(), ifmt = i(), w = i(), h = i(), d = i(), bd = i(), f = i(), ty = i(), n = i();
                GLES30.glTexImage3D(t, lv, ifmt, w, h, d, bd, f, ty, data(n));
                break;
            }
            case GlOps.OP_TEX_SUB_IMAGE_3D: {
                int t = i(), lv = i(), x = i(), y = i(), z = i(), w = i(), h = i(), d = i(), f = i(), ty = i(), n = i();
                GLES30.glTexSubImage3D(t, lv, x, y, z, w, h, d, f, ty, data(n));
                break;
            }
            case GlOps.OP_COMPRESSED_TEX_IMAGE_3D: {
                int t = i(), lv = i(), ifmt = i(), w = i(), h = i(), d = i(), bd = i(), size = i(), n = i();
                GLES30.glCompressedTexImage3D(t, lv, ifmt, w, h, d, bd, size, data(n));
                break;
            }
            case GlOps.OP_COMPRESSED_TEX_SUB_IMAGE_3D: {
                int t = i(), lv = i(), x = i(), y = i(), z = i(), w = i(), h = i(), d = i(), f = i(), size = i(), n = i();
                GLES30.glCompressedTexSubImage3D(t, lv, x, y, z, w, h, d, f, size, data(n));
                break;
            }
            case GlOps.OP_CREATE_SHADER: {
                int type = i(), id = i();
                ids.shader.put(id, GLES30.glCreateShader(type));
                break;
            }
            case GlOps.OP_CREATE_PROGRAM: {
                ids.prog.put(i(), GLES30.glCreateProgram());
                break;
            }
            case GlOps.OP_SHADER_SOURCE: {
                int id = i(), n = i();
                byte[] src = new byte[n];
                pay.get(src);
                GLES30.glShaderSource(ids.shader.get(id), new String(src, LATIN1));
                break;
            }
            case GlOps.OP_UNIFORM_V: uniformV(); break;
            case GlOps.OP_ATTRIB_POINTER_VBO: {
                int idx = i(), size = i(), type = i(), norm = i(), stride = i(), offset = i();
                GLES30.glVertexAttribPointer(idx, size, type, norm != 0, stride, offset);
                break;
            }
            case GlOps.OP_DRAW_ARRAYS: drawArrays(); break;
            case GlOps.OP_DRAW_ELEMENTS: drawElements(); break;
            case GlOps.OP_INVALIDATE_FB: invalidate(); break;
            default:
                if (!query(op)) { if (execErrors++ < 20) log.log("unknown op " + op); }
        }
    }

    private void gen() {
        int kind = i(), n = i();
        int[] guest = new int[n], host = new int[n];
        for (int k = 0; k < n; k++) guest[k] = i();
        switch (kind) {
            case GEN_TEX: GLES30.glGenTextures(n, host, 0); for (int k = 0; k < n; k++) ids.tex.put(guest[k], host[k]); break;
            case GEN_BUF: GLES30.glGenBuffers(n, host, 0); for (int k = 0; k < n; k++) ids.buf.put(guest[k], host[k]); break;
            case GEN_FBO: GLES30.glGenFramebuffers(n, host, 0); for (int k = 0; k < n; k++) ids.fbo.put(guest[k], host[k]); break;
            case GEN_RBO: GLES30.glGenRenderbuffers(n, host, 0); for (int k = 0; k < n; k++) ids.rbo.put(guest[k], host[k]); break;
            default: break;
        }
    }

    private void delete() {
        int kind = i(), n = i();
        for (int k = 0; k < n; k++) {
            int g = i();
            int[] h = one();
            switch (kind) {
                case GEN_TEX: h[0] = ids.tex.get(g); GLES30.glDeleteTextures(1, h, 0); ids.tex.put(g, 0); break;
                case GEN_BUF: h[0] = ids.buf.get(g); GLES30.glDeleteBuffers(1, h, 0); ids.buf.put(g, 0); break;
                case GEN_FBO: h[0] = ids.fbo.get(g); GLES30.glDeleteFramebuffers(1, h, 0); ids.fbo.put(g, 0); break;
                case GEN_RBO: h[0] = ids.rbo.get(g); GLES30.glDeleteRenderbuffers(1, h, 0); ids.rbo.put(g, 0); break;
                default: break;
            }
            if (kind == GEN_BUF) {
                if (g == curArray) curArray = 0;
                if (g == curElem) curElem = 0;
            }
        }
    }

    private void uniformV() {
        int kind = i(), loc = i(), count = i(), transpose = i();
        int per = kind == 8 ? 16 : (kind / 2) + 1;
        int n = count * per;
        boolean isFloat = kind == 8 || (kind & 1) == 0;
        if (isFloat) {
            if (fs.length < n) fs = new float[n];
            for (int k = 0; k < n; k++) fs[k] = pay.getFloat();
        } else {
            if (is.length < n) is = new int[n];
            for (int k = 0; k < n; k++) is[k] = pay.getInt();
        }
        switch (kind) {
            case 0: GLES30.glUniform1fv(loc, count, fs, 0); break;
            case 1: GLES30.glUniform1iv(loc, count, is, 0); break;
            case 2: GLES30.glUniform2fv(loc, count, fs, 0); break;
            case 3: GLES30.glUniform2iv(loc, count, is, 0); break;
            case 4: GLES30.glUniform3fv(loc, count, fs, 0); break;
            case 5: GLES30.glUniform3iv(loc, count, is, 0); break;
            case 6: GLES30.glUniform4fv(loc, count, fs, 0); break;
            case 7: GLES30.glUniform4iv(loc, count, is, 0); break;
            case 8: GLES30.glUniformMatrix4fv(loc, count, transpose != 0, fs, 0); break;
            default: break;
        }
    }

    /** Uploads the client arrays of one draw into scratch buffers and points the attributes at them. */
    private void clientBlocks(int nclient, int firstVertex) {
        for (int k = 0; k < nclient; k++) {
            int idx = i(), size = i(), type = i(), norm = i(), stride = i(), len = i();
            ByteBuffer d = data(len);
            if (idx < 0 || idx >= scratchVbo.length) continue;
            if (scratchVbo[idx] == 0) {
                int[] t = one();
                GLES30.glGenBuffers(1, t, 0);
                scratchVbo[idx] = t[0];
            }
            GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, scratchVbo[idx]);
            int eb = size * (type == GLES30.GL_FLOAT || type == GLES30.GL_INT || type == GLES30.GL_UNSIGNED_INT ? 4
                    : (type == GLES30.GL_SHORT || type == GLES30.GL_UNSIGNED_SHORT ? 2 : 1));
            int strideBytes = stride != 0 ? stride : eb;
            int lead = firstVertex * strideBytes;
            if (lead == 0) {
                GLES30.glBufferData(GLES30.GL_ARRAY_BUFFER, len, d, GLES30.GL_STREAM_DRAW);
            } else {
                GLES30.glBufferData(GLES30.GL_ARRAY_BUFFER, lead + len, null, GLES30.GL_STREAM_DRAW);
                GLES30.glBufferSubData(GLES30.GL_ARRAY_BUFFER, lead, len, d);
            }
            GLES30.glVertexAttribPointer(idx, size, type, norm != 0, stride, 0);
        }
        if (nclient > 0) GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, ids.buf.get(curArray));
    }

    private void drawArrays() {
        int mode = i(), first = i(), count = i(), nclient = i();
        clientBlocks(nclient, first);
        GLES30.glDrawArrays(mode, first, count);
    }

    private void drawElements() {
        int mode = i(), count = i(), type = i(), idxMode = i(), idxVal = i(), nclient = i();
        clientBlocks(nclient, 0);
        if (idxMode == 1) {
            ByteBuffer d = data(idxVal);
            if (scratchEbo == 0) {
                int[] t = one();
                GLES30.glGenBuffers(1, t, 0);
                scratchEbo = t[0];
            }
            GLES30.glBindBuffer(GLES30.GL_ELEMENT_ARRAY_BUFFER, scratchEbo);
            GLES30.glBufferData(GLES30.GL_ELEMENT_ARRAY_BUFFER, idxVal, d, GLES30.GL_STREAM_DRAW);
            GLES30.glDrawElements(mode, count, type, 0);
            GLES30.glBindBuffer(GLES30.GL_ELEMENT_ARRAY_BUFFER, ids.buf.get(curElem));
        } else {
            GLES30.glDrawElements(mode, count, type, idxVal);
        }
    }

    private void invalidate() {
        int target = i(), num = i(), sub = i(), x = i(), y = i(), w = i(), h = i();
        int[] att = new int[num];
        for (int k = 0; k < num; k++) att[k] = i();
        if (sub != 0) GLES30.glInvalidateSubFramebuffer(target, num, att, 0, x, y, w, h);
        else GLES30.glInvalidateFramebuffer(target, num, att, 0);
    }

    // ---------------------------------------------------------------- queries

    private static int intCount(int pname) {
        switch (pname) {
            case 0x0BA2: case 0x0C10: return 4;            // viewport, scissor box
            case 0x0D3A: case 0x846D: case 0x846E: return 2; // max viewport dims, aliased ranges
            default: return 1;
        }
    }

    private static int floatCount(int pname) {
        switch (pname) {
            case 0x0C22: case 0x8005: return 4;            // clear colour, blend colour
            case 0x0B70: case 0x846D: case 0x846E: return 2; // depth range, aliased ranges
            default: return 1;
        }
    }

    private static int imageBytes(int w, int h, int format, int type, int align) {
        int comps;
        switch (format) {
            case 0x1906: case 0x1909: case 0x1903: comps = 1; break;
            case 0x190A: case 0x8227: comps = 2; break;
            case 0x1907: comps = 3; break;
            default: comps = 4;
        }
        int bpp;
        switch (type) {
            case 0x1401: bpp = comps; break;
            case 0x8363: case 0x8033: case 0x8034: bpp = 2; break;
            case 0x1406: bpp = 4 * comps; break;
            default: bpp = comps;
        }
        if (w <= 0 || h <= 0) return 0;
        int row = w * bpp;
        if (align > 1) row = (row + align - 1) / align * align;
        return row * (h - 1) + w * bpp;
    }

    /** Handles the synchronous ops; every one of them must answer, or the guest would wait forever. */
    private boolean query(int op) {
        switch (op) {
            case GlOps.OP_Q_GET_STRING:
            case GlOps.OP_Q_GET_INTEGERV:
            case GlOps.OP_Q_GET_FLOATV:
            case GlOps.OP_Q_GET_SHADERIV:
            case GlOps.OP_Q_GET_PROGRAMIV:
            case GlOps.OP_Q_SHADER_LOG:
            case GlOps.OP_Q_PROGRAM_LOG:
            case GlOps.OP_Q_SHADER_SOURCE:
            case GlOps.OP_Q_ACTIVE_ATTRIB:
            case GlOps.OP_Q_ACTIVE_UNIFORM:
            case GlOps.OP_Q_ATTRIB_LOC:
            case GlOps.OP_Q_UNIFORM_LOC:
            case GlOps.OP_Q_CHECK_FB:
            case GlOps.OP_Q_READ_PIXELS:
            case OP_Q_PROGRAM_INFO:
                break;
            default:
                return false;
        }
        try {
            answer(op);
        } catch (RuntimeException e) {
            if (execErrors++ < 20) log.log("query " + op + " failed: " + e);
            reply(new byte[4]);
        }
        return true;
    }

    private void answer(int op) {
        switch (op) {
            case GlOps.OP_Q_GET_STRING:
                replyString(GLES30.glGetString(i()));
                break;
            case GlOps.OP_Q_GET_INTEGERV: {
                int pname = i();
                int[] v = new int[64];
                int n = intCount(pname);
                if (pname == 0x86A3) { // GL_COMPRESSED_TEXTURE_FORMATS
                    int[] num = one();
                    GLES30.glGetIntegerv(0x86A2, num, 0);
                    n = Math.min(num[0], v.length);
                }
                GLES30.glGetIntegerv(pname, v, 0);
                switch (pname) {
                    case 0x8CA6: v[0] = ids.fbo.reverse(v[0]); break;
                    case 0x8CA7: v[0] = ids.rbo.reverse(v[0]); break;
                    case 0x8069: case 0x8514: v[0] = ids.tex.reverse(v[0]); break;
                    case 0x8894: case 0x8895: v[0] = ids.buf.reverse(v[0]); break;
                    case 0x8B8D: v[0] = ids.prog.reverse(v[0]); break;
                    default: break;
                }
                replyInts(v, n);
                break;
            }
            case GlOps.OP_Q_GET_FLOATV: {
                int pname = i();
                float[] v = new float[16];
                GLES30.glGetFloatv(pname, v, 0);
                int n = floatCount(pname);
                ByteBuffer b = ByteBuffer.allocate(n * 4).order(ByteOrder.LITTLE_ENDIAN);
                for (int k = 0; k < n; k++) b.putFloat(v[k]);
                reply(b.array());
                break;
            }
            case GlOps.OP_Q_GET_SHADERIV: {
                int id = i(), pname = i();
                int[] v = one();
                GLES30.glGetShaderiv(ids.shader.get(id), pname, v, 0);
                replyInts(v, 1);
                break;
            }
            case GlOps.OP_Q_GET_PROGRAMIV: {
                int id = i(), pname = i();
                int[] v = one();
                GLES30.glGetProgramiv(ids.prog.get(id), pname, v, 0);
                replyInts(v, 1);
                break;
            }
            case GlOps.OP_Q_SHADER_LOG:
                replyString(GLES30.glGetShaderInfoLog(ids.shader.get(i())));
                break;
            case GlOps.OP_Q_PROGRAM_LOG:
                replyString(GLES30.glGetProgramInfoLog(ids.prog.get(i())));
                break;
            case GlOps.OP_Q_SHADER_SOURCE:
                replyString(GLES30.glGetShaderSource(ids.shader.get(i())));
                break;
            case GlOps.OP_Q_ACTIVE_ATTRIB:
            case GlOps.OP_Q_ACTIVE_UNIFORM: {
                int prog = ids.prog.get(i()), index = i();
                int[] size = one(), type = one();
                String name = op == GlOps.OP_Q_ACTIVE_ATTRIB
                        ? GLES30.glGetActiveAttrib(prog, index, size, 0, type, 0)
                        : GLES30.glGetActiveUniform(prog, index, size, 0, type, 0);
                byte[] nb = name == null ? new byte[0] : name.getBytes(LATIN1);
                ByteBuffer b = ByteBuffer.allocate(8 + nb.length).order(ByteOrder.LITTLE_ENDIAN);
                b.putInt(size[0]).putInt(type[0]).put(nb);
                reply(b.array());
                break;
            }
            case GlOps.OP_Q_ATTRIB_LOC:
            case GlOps.OP_Q_UNIFORM_LOC: {
                int prog = ids.prog.get(i());
                int start = pay.position(), end = start;
                while (end < pay.limit() && pay.get(end) != 0) end++;
                byte[] nb = new byte[end - start];
                pay.get(nb);
                String name = new String(nb, LATIN1);
                int loc = op == GlOps.OP_Q_ATTRIB_LOC ? GLES30.glGetAttribLocation(prog, name)
                                                      : GLES30.glGetUniformLocation(prog, name);
                replyInts(new int[] { loc }, 1);
                break;
            }
            case GlOps.OP_Q_CHECK_FB:
                replyInts(new int[] { GLES30.glCheckFramebufferStatus(i()) }, 1);
                break;
            case GlOps.OP_Q_READ_PIXELS: {
                int x = i(), y = i(), w = i(), h = i(), format = i(), type = i(), align = i();
                int n = imageBytes(w, h, format, type, align);
                ByteBuffer px = ByteBuffer.allocateDirect(Math.max(n, 4));
                GLES30.glReadPixels(x, y, w, h, format, type, px);
                byte[] bytes = new byte[n];
                px.get(bytes, 0, n);
                reply(bytes);
                break;
            }
            case OP_Q_PROGRAM_INFO:
                programInfo(ids.prog.get(i()));
                break;
            default:
                reply(new byte[0]);
        }
    }

    /** Guest-side op (portbase/src/gl_bridge.cpp): everything about a linked program, in one reply. */
    static final int OP_Q_PROGRAM_INFO = 250;

    private void programInfo(int prog) {
        int[] v = new int[1];
        int[] st = new int[6];
        GLES30.glGetProgramiv(prog, GLES30.GL_LINK_STATUS, v, 0); st[0] = v[0];
        GLES30.glGetProgramiv(prog, GLES30.GL_INFO_LOG_LENGTH, v, 0); st[1] = v[0];
        GLES30.glGetProgramiv(prog, GLES30.GL_ACTIVE_ATTRIBUTES, v, 0); st[2] = v[0];
        GLES30.glGetProgramiv(prog, GLES30.GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, v, 0); st[3] = v[0];
        GLES30.glGetProgramiv(prog, GLES30.GL_ACTIVE_UNIFORMS, v, 0); st[4] = v[0];
        GLES30.glGetProgramiv(prog, GLES30.GL_ACTIVE_UNIFORM_MAX_LENGTH, v, 0); st[5] = v[0];
        if (st[0] == 0) { st[2] = 0; st[4] = 0; }
        java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
        ByteBuffer b = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN);
        for (int x : st) b.putInt(x);
        bo.write(b.array(), 0, 24);
        int[] size = one(), type = one();
        for (int pass = 0; pass < 2; pass++) {
            int n = pass == 0 ? st[2] : st[4];
            for (int k = 0; k < n; k++) {
                String name = pass == 0 ? GLES30.glGetActiveAttrib(prog, k, size, 0, type, 0)
                                        : GLES30.glGetActiveUniform(prog, k, size, 0, type, 0);
                if (name == null) name = "";
                int loc = pass == 0 ? GLES30.glGetAttribLocation(prog, name) : GLES30.glGetUniformLocation(prog, name);
                byte[] nb = name.getBytes(LATIN1);
                ByteBuffer e = ByteBuffer.allocate(16 + ((nb.length + 3) & ~3)).order(ByteOrder.LITTLE_ENDIAN);
                e.putInt(size[0]).putInt(type[0]).putInt(loc).putInt(nb.length).put(nb);
                bo.write(e.array(), 0, e.capacity());
            }
        }
        reply(bo.toByteArray());
    }

    // ---------------------------------------------------------------- presentation

    private void attach(Surface s) {
        if (window != EGL14.EGL_NO_SURFACE) {
            EGL14.eglMakeCurrent(dpy, pbuf, pbuf, ctx);
            EGL14.eglDestroySurface(dpy, window);
            window = EGL14.EGL_NO_SURFACE;
        }
        if (s != null && s.isValid()) {
            EGLSurface w = EGL14.eglCreateWindowSurface(dpy, cfg, s, new int[] { EGL14.EGL_NONE }, 0);
            if (w != EGL14.EGL_NO_SURFACE && EGL14.eglMakeCurrent(dpy, w, w, ctx)) {
                window = w;
                EGL14.eglSwapInterval(dpy, 1);
                log.log("presenting on a window surface");
            } else {
                log.log("eglCreateWindowSurface failed: 0x" + Integer.toHexString(EGL14.eglGetError()));
                EGL14.eglMakeCurrent(dpy, pbuf, pbuf, ctx);
            }
        }
    }

    private void swap() {
        Surface s;
        boolean dirty;
        synchronized (lock) {
            dirty = surfaceDirty;
            s = pendingSurface;
            surfaceDirty = false;
        }
        if (dirty) attach(s);

        if (window != EGL14.EGL_NO_SURFACE) {
            if (!EGL14.eglSwapBuffers(dpy, window)) {
                log.log("eglSwapBuffers failed: 0x" + Integer.toHexString(EGL14.eglGetError()));
                EGL14.eglMakeCurrent(dpy, pbuf, pbuf, ctx);
                EGL14.eglDestroySurface(dpy, window);
                window = EGL14.EGL_NO_SURFACE;
            }
        } else {
            // Nothing to present on (app in the background): do not let the game spin.
            try { Thread.sleep(16); } catch (InterruptedException ignored) { }
        }
        swaps++;
        ackFrame();
        if (onFrame != null) onFrame.run();

        int e = GLES30.glGetError();
        if (e != 0 && glErrors++ < 40) log.log("glGetError 0x" + Integer.toHexString(e) + " at frame " + swaps);

        long now = System.nanoTime();
        if (now - lastReport >= 5_000_000_000L) {
            double secs = (now - lastReport) / 1e9;
            log.log(String.format(Locale.US, "frames %.1f/s, %d cmds, %.1f MB/s, total frames %d, busy %.0f%%",
                    (swaps - lastSwaps) / secs, cmds, bytes / 1e6 / secs, swaps, 100.0 * busyNanos / (now - lastReport))
                    + (telemetry != null ? " | " + telemetry.line() : "")
                    + opReport(swaps - lastSwaps));
            busyNanos = 0;
            lastReport = now;
            lastSwaps = swaps;
            bytes = 0;
            cmds = 0;
        }
    }
}
