package com.eaprules.nova3;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.RectF;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;

import java.util.Locale;

/**
 * Full-screen shell around the emulated game. The picture comes from {@link GlBridge}, which renders the
 * game's OpenGL ES calls straight onto {@link #glView}; the overlay on top shows the loading screen and the
 * on-screen controls and turns touches and gamepad input into commands for the port.
 */
public final class MainActivity extends Activity {
    private enum Phase { SETUP, LOADING, PLAYING, ERROR }

    private static final int MAX_POINTERS = 20;
    private static final int ZONE_NONE = 0, ZONE_LEFT = 1, ZONE_RIGHT = 2, ZONE_CURSOR = 3, ZONE_BUTTON = 4;

    /*
     * One game per app process. The activity can be recreated while qemu keeps running (a config change the
     * manifest does not list, a second launch); starting another copy would leave two emulated games
     * sharing the CPU, so a new activity attaches to the running one instead.
     */
    private static GameProcess running;

    private GameProcess game;
    private SurfaceView glView;
    private Overlay overlay;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile Phase phase = Phase.SETUP;
    private volatile String message = "Подготовка файлов…";
    private volatile boolean menuMode = true;
    private volatile int modeEpoch;
    private volatile long loadingSince;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28) {
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        GameProcess.Listener listener = new GameProcess.Listener() {
            @Override public void onStatus(String text) { message = text; }
            @Override public void onExit(int code) {
                message = "Игра завершилась (код " + code + ").\nЛог: " + game.logFile().getPath();
                phase = Phase.ERROR;
            }
        };
        boolean attach = running != null && running.isRunning();
        if (attach) {
            game = running;
            game.setListener(listener);
            game.log("activity recreated; attaching to the running game");
            phase = game.gl().swaps() > 0 ? Phase.PLAYING : Phase.LOADING;
            loadingSince = SystemClock.uptimeMillis();
        } else {
            game = new GameProcess(getApplicationContext(), listener);
            running = game;
        }

        FrameLayout root = new FrameLayout(this);
        glView = new SurfaceView(this);
        SurfaceHolder holder = glView.getHolder();
        holder.setFormat(PixelFormat.OPAQUE);
        // The game renders at its own size and the compositor scales it to the panel.
        holder.setFixedSize(game.renderWidth(), game.renderHeight());
        holder.addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) { game.gl().setSurface(h.getSurface()); }
            @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int hgt) { game.gl().setSurface(h.getSurface()); }
            @Override public void surfaceDestroyed(SurfaceHolder h) { game.gl().setSurface(null); }
        });
        root.addView(glView, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        overlay = new Overlay(this);
        root.addView(overlay, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(root);

        InputManager im = (InputManager) getSystemService(Context.INPUT_SERVICE);
        if (im != null) im.registerInputDeviceListener(padWatcher, ui);

        if (attach) {
            if (phase == Phase.PLAYING) game.setMuted(false);
        } else if (Build.VERSION.SDK_INT < 30
                && checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE }, 1);
        } else {
            startGame();
        }
        ui.post(poller);
    }

    @Override public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        startGame();
    }

    private void startGame() {
        new Thread(() -> {
            try {
                game.start();
                if (game.isRunning()) {
                    message = "Загрузка игры…\nПервый запуск может занять несколько минут.";
                    loadingSince = SystemClock.uptimeMillis();
                    phase = Phase.LOADING;
                }
            } catch (Exception e) {
                message = e.getMessage() != null ? e.getMessage() : e.toString();
                phase = Phase.ERROR;
            }
        }, "nova3-setup").start();
    }

    /** Follows the port's status file: leaves the loading screen once the main menu is up. */
    private volatile String fpsText = "";

    private final Runnable poller = new Runnable() {
        private long lastReport = SystemClock.uptimeMillis();
        private long fpsAt = SystemClock.uptimeMillis(), fpsSwaps;

        @Override public void run() {
            Phase p = phase;
            long t = SystemClock.uptimeMillis();
            if (p == Phase.PLAYING && t - fpsAt >= 500) {
                long s = game.gl().swaps();
                fpsText = String.format(Locale.US, "%.1f FPS", (s - fpsSwaps) * 1000.0 / (t - fpsAt));
                fpsSwaps = s;
                fpsAt = t;
                overlay.invalidate();
            }
            if (p == Phase.LOADING || p == Phase.PLAYING) {
                int[] s = game.readStatus();
                if (s != null) {
                    boolean menu = s[1] == 1;
                    if (p == Phase.LOADING && game.gl().swaps() > 0 && (menu || s[0] > 900)) {
                        phase = Phase.PLAYING;
                        game.setMuted(false);
                        overlay.invalidate();
                    }
                    if (menu != menuMode) {
                        menuMode = menu;
                        modeEpoch++;
                        overlay.invalidate();
                    }
                    long now = SystemClock.uptimeMillis();
                    if (now - lastReport >= 5000) {
                        lastReport = now;
                        game.log(String.format(Locale.US, "phase=%s frame=%d menu=%d presented=%d", phase, s[0], s[1], game.gl().swaps()));
                    }
                }
            }
            if (phase != Phase.PLAYING) overlay.invalidate();
            ui.postDelayed(this, phase == Phase.PLAYING ? 250 : 40);
        }
    };

    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) {
            glView.setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }

    @Override protected void onPause() {
        game.setMuted(true);
        super.onPause();
    }

    @Override protected void onResume() {
        super.onResume();
        if (phase == Phase.PLAYING) game.setMuted(false);
    }

    @Override protected void onDestroy() {
        ui.removeCallbacks(poller);
        InputManager im = (InputManager) getSystemService(Context.INPUT_SERVICE);
        if (im != null) im.unregisterInputDeviceListener(padWatcher);
        // Only a real exit ends the game; a recreated activity attaches to it again.
        if (isFinishing()) {
            game.stop();
            if (running == game) running = null;
        }
        super.onDestroy();
    }

    // ------------------------------------------------------------ gamepad

    private boolean padOn;

    /** The engine is told about a pad the first time one is used, as the phone build's MOGA listener does. */
    private void padUsed() {
        if (!game.touchMode() || padOn) return;
        padOn = true;
        overlay.releaseFingers();
        game.command("pad 1", null);
    }

    private static boolean isPad(InputDevice d) {
        if (d == null || d.isVirtual()) return false;
        int s = d.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private final InputManager.InputDeviceListener padWatcher = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) { }
        @Override public void onInputDeviceChanged(int id) { }
        @Override public void onInputDeviceRemoved(int id) {
            if (!padOn) return;
            for (int other : InputDevice.getDeviceIds()) {
                if (isPad(InputDevice.getDevice(other))) return;
            }
            padOn = false;
            game.command("pad 0", null);
        }
    };

    private static String padButton(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return "a";
            case KeyEvent.KEYCODE_BUTTON_B: return "b";
            case KeyEvent.KEYCODE_BUTTON_X: return "x";
            case KeyEvent.KEYCODE_BUTTON_Y: return "y";
            case KeyEvent.KEYCODE_BUTTON_L1: return "l1";
            case KeyEvent.KEYCODE_BUTTON_R1: return "r1";
            case KeyEvent.KEYCODE_BUTTON_L2: return "l2";
            case KeyEvent.KEYCODE_BUTTON_R2: return "r2";
            case KeyEvent.KEYCODE_BUTTON_START: return "start";
            case KeyEvent.KEYCODE_DPAD_UP: return "up";
            case KeyEvent.KEYCODE_DPAD_DOWN: return "down";
            case KeyEvent.KEYCODE_DPAD_LEFT: return "left";
            case KeyEvent.KEYCODE_DPAD_RIGHT: return "right";
            default: return null;
        }
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int action = e.getAction();
        boolean edge = (action == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) || action == KeyEvent.ACTION_UP;
        // On a phone Back belongs to the game (its menus and pause), as in the original activity.
        if (e.getKeyCode() == KeyEvent.KEYCODE_BACK && game.touchMode() && phase == Phase.PLAYING) {
            if (edge) game.command("key 4 " + (action == KeyEvent.ACTION_DOWN ? "down" : "up"), null);
            return true;
        }
        String name = padButton(e.getKeyCode());
        int src = e.getSource();
        boolean pad = (src & (InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_DPAD | InputDevice.SOURCE_JOYSTICK)) != 0;
        if (name == null || !pad) return super.dispatchKeyEvent(e);
        padUsed();
        if (edge) {
            game.command("button " + name + (action == KeyEvent.ACTION_DOWN ? " down" : " up"), null);
        }
        return true;
    }

    private float padLx, padLy, padRx, padRy;
    private int hatX, hatY;

    @Override public boolean onGenericMotionEvent(MotionEvent e) {
        if ((e.getSource() & InputDevice.SOURCE_JOYSTICK) == 0 || e.getActionMasked() != MotionEvent.ACTION_MOVE) {
            return super.onGenericMotionEvent(e);
        }
        padUsed();
        float lx = dead(e.getAxisValue(MotionEvent.AXIS_X)), ly = dead(e.getAxisValue(MotionEvent.AXIS_Y));
        float rx = dead(e.getAxisValue(MotionEvent.AXIS_Z)), ry = dead(e.getAxisValue(MotionEvent.AXIS_RZ));
        if (changed(lx, padLx) || changed(ly, padLy)) {
            padLx = lx; padLy = ly;
            game.command(String.format(Locale.US, "stick left %.3f %.3f", lx, ly), "stick left");
        }
        if (changed(rx, padRx) || changed(ry, padRy)) {
            padRx = rx; padRy = ry;
            game.command(String.format(Locale.US, "stick right %.3f %.3f", rx, ry), "stick right");
        }
        int hx = Math.round(e.getAxisValue(MotionEvent.AXIS_HAT_X));
        int hy = Math.round(e.getAxisValue(MotionEvent.AXIS_HAT_Y));
        if (hx != hatX) {
            if (hatX != 0) game.command("button " + (hatX < 0 ? "left" : "right") + " up", null);
            if (hx != 0) game.command("button " + (hx < 0 ? "left" : "right") + " down", null);
            hatX = hx;
        }
        if (hy != hatY) {
            if (hatY != 0) game.command("button " + (hatY < 0 ? "up" : "down") + " up", null);
            if (hy != 0) game.command("button " + (hy < 0 ? "up" : "down") + " down", null);
            hatY = hy;
        }
        return true;
    }

    private static float dead(float v) { return Math.abs(v) < 0.15f ? 0f : v; }
    private static boolean changed(float a, float b) { return Math.abs(a - b) > 0.02f; }

    // ------------------------------------------------------------ overlay

    private static final class Button {
        final String label, name;
        final RectF rect = new RectF();
        Button(String label, String name) { this.label = label; this.name = name; }
    }

    private final class Overlay extends View {
        private final float density = getResources().getDisplayMetrics().density;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF bar = new RectF();
        private final Button[] buttons = {
            new Button("L1", "l1"), new Button("R1", "r1"), new Button("A", "a"),
            new Button("B", "b"), new Button("X", "x"), new Button("Y", "y"),
            new Button("II", "start"),
        };

        private final int[] zone = new int[MAX_POINTERS];
        private final int[] buttonOf = new int[MAX_POINTERS];
        private final float[] originX = new float[MAX_POINTERS], originY = new float[MAX_POINTERS];
        private int cursorPointer = -1;
        private int seenEpoch;

        Overlay(Context c) {
            super(c);
            setFocusable(true);
        }

        @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
            float s = 48 * density, gap = 8 * density, margin = 16 * density;
            float rowW = 3 * s + 2 * gap;
            float left = (w - rowW) / 2f;
            float bottom = h - margin;
            for (int i = 0; i < 6; i++) {
                int row = 1 - i / 3, col = i % 3;
                float x = left + col * (s + gap), y = bottom - (row + 1) * s - row * gap;
                buttons[i].rect.set(x, y, x + s, y + s);
            }
            buttons[6].rect.set((w - s) / 2f, margin, (w + s) / 2f, margin + s);
        }

        @Override protected void onDraw(Canvas c) {
            Phase p = phase;
            if (p == Phase.PLAYING) {
                // In phone mode the engine draws its own controls.
                if (!menuMode && (!game.touchMode() || padOn)) drawButtons(c);
                drawFps(c);
            } else {
                drawLoading(c, p, SystemClock.uptimeMillis());
            }
        }

        /** Presented frames per second, top centre, small enough to stay out of the game's HUD. */
        private void drawFps(Canvas c) {
            String s = fpsText;
            if (s.isEmpty()) return;
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(13 * density);
            float x = getWidth() / 2f, y = 18 * density;
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0x99000000);
            float w = paint.measureText(s) / 2 + 6 * density;
            c.drawRect(x - w, y - 14 * density, x + w, y + 5 * density, paint);
            paint.setColor(0xff7cfc00);
            c.drawText(s, x, y, paint);
        }

        private void drawButtons(Canvas c) {
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(16 * density);
            for (Button b : buttons) {
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(0x55ffffff);
                c.drawRoundRect(b.rect, 12 * density, 12 * density, paint);
                paint.setColor(0xddffffff);
                c.drawText(b.label, b.rect.centerX(), b.rect.centerY() + 6 * density, paint);
            }
        }

        private void drawLoading(Canvas c, Phase p, long now) {
            int w = getWidth(), h = getHeight();
            c.drawColor(0xff0b0f14);
            paint.setStyle(Paint.Style.FILL);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setColor(0xfff2f5f7);
            paint.setTextSize(36 * density);
            c.drawText("N.O.V.A. 3", w / 2f, h * 0.30f, paint);

            paint.setTextSize(16 * density);
            paint.setColor(p == Phase.ERROR ? 0xffff8a80 : 0xffb7c1c9);
            float y = h * 0.42f;
            for (String line : message.split("\n")) {
                c.drawText(line, w / 2f, y, paint);
                y += 24 * density;
            }
            if (p == Phase.ERROR) return;

            float barW = Math.min(w * 0.5f, 320 * density), barH = 6 * density;
            float x0 = (w - barW) / 2f, y0 = Math.max(y + 12 * density, h * 0.60f);
            paint.setColor(0xff1f2933);
            bar.set(x0, y0, x0 + barW, y0 + barH);
            c.drawRoundRect(bar, barH, barH, paint);
            float seg = barW * 0.3f;
            float t = (now % 1400) / 1400f;
            float sx = x0 + (barW + seg) * t - seg;
            float l = Math.max(x0, sx), r = Math.min(x0 + barW, sx + seg);
            paint.setColor(0xff4fc3f7);
            if (r > l) {
                bar.set(l, y0, r, y0 + barH);
                c.drawRoundRect(bar, barH, barH, paint);
            }
            if (p == Phase.LOADING) {
                paint.setColor(0xff7b8794);
                paint.setTextSize(13 * density);
                c.drawText("Прошло: " + (now - loadingSince) / 1000 + " с", w / 2f, y0 + 30 * density, paint);
            }
        }

        // -------------------------------------------------- touch

        private float clamp(float v) { return Math.max(-1f, Math.min(1f, v)); }

        /** Touch position in the game's own pixel grid. */
        private void sendCursor(float x, float y) {
            float gx = x * game.renderWidth() / Math.max(1, getWidth());
            float gy = y * game.renderHeight() / Math.max(1, getHeight());
            game.command(String.format(Locale.US, "cursor %.1f %.1f", gx, gy), "cursor");
        }

        private void pointerDown(int id, float x, float y) {
            if (id >= MAX_POINTERS) return;
            zone[id] = ZONE_NONE;
            if (!menuMode) {
                for (int i = 0; i < buttons.length; i++) {
                    if (buttons[i].rect.contains(x, y)) {
                        zone[id] = ZONE_BUTTON;
                        buttonOf[id] = i;
                        game.command("button " + buttons[i].name + " down", null);
                        return;
                    }
                }
                float frac = x / Math.max(1, getWidth());
                if (frac < 0.35f || frac > 0.65f) {
                    zone[id] = frac < 0.35f ? ZONE_LEFT : ZONE_RIGHT;
                    originX[id] = x;
                    originY[id] = y;
                    return;
                }
            }
            if (cursorPointer < 0) {
                cursorPointer = id;
                zone[id] = ZONE_CURSOR;
                sendCursor(x, y);
                game.command("click down", null);
            }
        }

        private void pointerMove(int id, float x, float y) {
            if (id >= MAX_POINTERS) return;
            switch (zone[id]) {
                case ZONE_CURSOR:
                    sendCursor(x, y);
                    break;
                case ZONE_LEFT:
                case ZONE_RIGHT: {
                    float radius = 70 * density;
                    boolean left = zone[id] == ZONE_LEFT;
                    game.command(String.format(Locale.US, "stick %s %.3f %.3f", left ? "left" : "right",
                            clamp((x - originX[id]) / radius), clamp((y - originY[id]) / radius)),
                            left ? "stick left" : "stick right");
                    break;
                }
                default:
                    break;
            }
        }

        private void pointerUp(int id) {
            if (id >= MAX_POINTERS) return;
            switch (zone[id]) {
                case ZONE_CURSOR:
                    cursorPointer = -1;
                    game.command("click up", null);
                    break;
                case ZONE_LEFT:
                    game.command("stick left 0 0", "stick left");
                    break;
                case ZONE_RIGHT:
                    game.command("stick right 0 0", "stick right");
                    break;
                case ZONE_BUTTON:
                    game.command("button " + buttons[buttonOf[id]].name + " up", null);
                    break;
                default:
                    break;
            }
            zone[id] = ZONE_NONE;
        }

        private void releaseAll() {
            for (int id = 0; id < MAX_POINTERS; id++) if (zone[id] != ZONE_NONE) pointerUp(id);
        }

        // ------------------------------------------- touch, phone style

        private final boolean[] fingerDown = new boolean[MAX_POINTERS];
        private final int[] fingerX = new int[MAX_POINTERS], fingerY = new int[MAX_POINTERS];

        /** Lifts every finger the engine still thinks is down (a pad took over). */
        void releaseFingers() {
            for (int id = 0; id < MAX_POINTERS; id++) {
                if (!fingerDown[id]) continue;
                fingerDown[id] = false;
                game.command("touch 0 " + fingerX[id] + " " + fingerY[id] + " " + id, null);
            }
        }

        private void touch(int action, MotionEvent e, int index) {
            int id = e.getPointerId(index);
            if (id >= MAX_POINTERS) return;
            if (action == 1) fingerDown[id] = true;
            else if (action == 0) { if (!fingerDown[id]) return; fingerDown[id] = false; }
            else if (!fingerDown[id]) return;
            int gx = Math.round(e.getX(index) * game.renderWidth() / Math.max(1, getWidth()));
            int gy = Math.round(e.getY(index) * game.renderHeight() / Math.max(1, getHeight()));
            fingerX[id] = gx;
            fingerY[id] = gy;
            // Moves of one finger replace each other while queued; presses and releases never do.
            game.command("touch " + action + " " + gx + " " + gy + " " + id, action == 2 ? "touch " + id : null);
        }

        /** The phone build's onTouchEvent: 1 for a finger down, 2 for every finger on a move, 0 for a finger up. */
        private void nativeTouch(MotionEvent e) {
            int idx = e.getActionIndex();
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                case MotionEvent.ACTION_POINTER_DOWN:
                    touch(1, e, idx);
                    break;
                case MotionEvent.ACTION_MOVE:
                    for (int i = 0; i < e.getPointerCount(); i++) touch(2, e, i);
                    break;
                case MotionEvent.ACTION_POINTER_UP:
                    touch(0, e, idx);
                    break;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    for (int i = 0; i < e.getPointerCount(); i++) touch(0, e, i);
                    break;
                default:
                    break;
            }
        }

        @Override public boolean onTouchEvent(MotionEvent e) {
            if (phase != Phase.PLAYING) return true;
            if (game.touchMode() && !padOn) {
                nativeTouch(e);
                return true;
            }
            if (seenEpoch != modeEpoch) {
                seenEpoch = modeEpoch;
                releaseAll();
            }
            int idx = e.getActionIndex();
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                case MotionEvent.ACTION_POINTER_DOWN:
                    pointerDown(e.getPointerId(idx), e.getX(idx), e.getY(idx));
                    break;
                case MotionEvent.ACTION_MOVE:
                    for (int i = 0; i < e.getPointerCount(); i++) pointerMove(e.getPointerId(i), e.getX(i), e.getY(i));
                    break;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_POINTER_UP:
                    pointerUp(e.getPointerId(idx));
                    break;
                case MotionEvent.ACTION_CANCEL:
                    releaseAll();
                    break;
                default:
                    break;
            }
            return true;
        }
    }
}
