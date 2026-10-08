package com.eaprules.nova3;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Game settings. They are kept in settings.txt (KEY=VALUE per line) next to the game log; GameProcess
 * adds them to the engine's environment after engine_env.txt, so a menu value wins over a hand-edited one.
 * This is the app's launcher screen: "Играть" saves and starts the game (the game process is restarted
 * so that it always begins with the values shown here).
 */
public final class SettingsActivity extends Activity {
    private static final int BG = Color.rgb(18, 22, 26), FG = Color.rgb(230, 235, 238), DIM = Color.rgb(140, 150, 156),
            ACCENT = Color.rgb(128, 203, 196);

    private final Map<String, String> values = new LinkedHashMap<>();
    private float dp;

    static File file(Context c) {
        File d = c.getExternalFilesDir(null);
        return new File(d != null ? d : c.getFilesDir(), "settings.txt");
    }

    static Map<String, String> load(Context c) {
        Map<String, String> m = new LinkedHashMap<>();
        File f = file(c);
        if (!f.isFile()) return m;
        try (FileInputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] b = new byte[2048];
            int n;
            while ((n = in.read(b)) > 0) out.write(b, 0, n);
            for (String line : out.toString("UTF-8").split("\n")) {
                int eq = line.indexOf('=');
                if (eq > 0) m.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
            }
        } catch (IOException ignored) { }
        return m;
    }

    private boolean save() {
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<String, String> e : values.entrySet()) sb.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        try (FileOutputStream out = new FileOutputStream(file(this))) {
            out.write(sb.toString().getBytes("UTF-8"));
            return true;
        } catch (IOException e) {
            Toast.makeText(this, "Не удалось сохранить: " + e.getMessage(), Toast.LENGTH_LONG).show();
            return false;
        }
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        dp = getResources().getDisplayMetrics().density;
        values.putAll(load(this));

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(px(20), px(16), px(20), px(24));

        col.addView(text("N.O.V.A. 3 — настройки", 22, FG, true));
        col.addView(text("Настрой и нажми «Играть» — игра стартует с этими значениями.", 13, DIM, false));

        col.addView(section("Картинка"));
        col.addView(slider("Угол обзора (FOV)", "NOVA3_FOV_PERCENT", 100, 220, 5, 160, " %",
                "100 % — как в оригинале. Больше — шире обзор; интерфейс может немного смещаться."));
        col.addView(choice("Разрешение рендера (высота)", "RENDER_HEIGHT", new int[] { 480, 540, 720 }, 720, "p",
                "Ширина считается по пропорциям экрана. Ниже — быстрее. Выше 720p интерфейс игры ломается (мелкий текст), поэтому 720p — максимум."));

        col.addView(section("Качество (влияет на FPS)"));
        col.addView(toggle("Тени", "Q_SHADOWS", true, "Выключение заметно разгружает процессор и видеокарту."));
        col.addView(toggle("Пост-эффекты (DOF, туман, блюр, искажения)", "Q_POST", true,
                "Дополнительные проходы рендера; без них меньше вызовов отрисовки."));
        col.addView(slider("Дальность детализации (LOD)", "Q_LOD", 30, 100, 10, 100, " %",
                "Меньше — упрощённые модели ближе к камере, меньше геометрии и скиннинга."));
        col.addView(choice("Уровень деталей", "Q_DETAILS", new int[] { 0, 1, 2 }, 0, "",
                "0 — полный, 2 — минимальный."));

        col.addView(section("Производительность"));
        col.addView(choice("Кадров «в полёте»", "NOVA3_GL_INFLIGHT", new int[] { 1, 2, 3 }, 2, "",
                "Сколько кадров игра готовит вперёд, пока телефон рисует предыдущий. 2 — плавнее, 1 — меньше задержка."));
        col.addView(toggle("Физика в отдельном потоке", "NOVA3_PHYSICS_THREAD", false,
                "Разгружает главный поток; на части сцен даёт прирост."));
        col.addView(toggle("Только быстрые ядра (6-7)", "CPU_PRIME", false,
                "Держит игру на двух самых быстрых ядрах: система реже переносит главный поток на медленные."));
        col.addView(toggle("Профилирование в логе", "QEMU_GUEST_PROF", true,
                "Раз в 10 секунд пишет в лог, где игра тратит время. Для отладки."));

        ScrollView sv = new ScrollView(this);
        sv.addView(col, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // The buttons stay at the bottom whatever the scroll position.
        Button reset = button("Вернуть как было");
        reset.setOnClickListener(v -> {
            values.clear();
            file(this).delete();
            recreate();
        });
        Button saveOnly = button("Сохранить");
        saveOnly.setOnClickListener(v -> {
            if (save()) Toast.makeText(this, "Сохранено", Toast.LENGTH_SHORT).show();
        });
        Button play = button("Играть");
        play.setOnClickListener(v -> { if (save()) restartGame(); });
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setPadding(px(16), px(4), px(16), px(8));
        for (Button bt : new Button[] { reset, saveOnly, play }) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            lp.setMargins(px(4), 0, px(4), 0);
            bar.addView(bt, lp);
        }
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(BG);
        root.addView(sv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        root.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(root);
    }

    /**
     * Starts the game with the saved values. CLEAR_TASK destroys a MainActivity that is still alive, so its
     * GameProcess (which reads settings.txt when it is created) is rebuilt from scratch.
     */
    private void restartGame() {
        startActivity(new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
        finish();
    }

    // ------------------------------------------------------------ widgets

    private int px(int v) { return Math.round(v * dp); }

    private TextView text(String s, int sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextColor(color);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        if (bold) t.setTypeface(t.getTypeface(), android.graphics.Typeface.BOLD);
        return t;
    }

    private View spacer(int h) {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(1, px(h)));
        return v;
    }

    private View section(String title) {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setPadding(0, px(22), 0, px(4));
        l.addView(text(title.toUpperCase(), 12, ACCENT, true));
        return l;
    }

    private Button button(String label) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        return b;
    }

    private int intOf(String key, int def) {
        try { return Integer.parseInt(values.getOrDefault(key, String.valueOf(def))); } catch (NumberFormatException e) { return def; }
    }

    private View slider(String title, String key, int min, int max, int step, int def, String unit, String hint) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(0, px(10), 0, px(4));
        TextView head = text("", 16, FG, false);
        SeekBar bar = new SeekBar(this);
        bar.setMax((max - min) / step);
        int cur = Math.max(min, Math.min(max, intOf(key, def)));
        bar.setProgress((cur - min) / step);
        Runnable show = () -> head.setText(title + ": " + (min + bar.getProgress() * step) + unit);
        show.run();
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean user) {
                show.run();
                values.put(key, String.valueOf(min + p * step));
            }
            @Override public void onStartTrackingTouch(SeekBar s) { }
            @Override public void onStopTrackingTouch(SeekBar s) { }
        });
        row.addView(head);
        row.addView(bar);
        row.addView(text(hint, 12, DIM, false));
        return row;
    }

    private View choice(String title, String key, int[] options, int def, String unit, String hint) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(0, px(10), 0, px(4));
        row.addView(text(title, 16, FG, false));
        LinearLayout line = new LinearLayout(this);
        line.setOrientation(LinearLayout.HORIZONTAL);
        line.setGravity(Gravity.START);
        int cur = intOf(key, def);
        Button[] bs = new Button[options.length];
        for (int i = 0; i < options.length; i++) {
            final int o = options[i];
            Button b = new Button(this);
            b.setAllCaps(false);
            b.setText(o + unit);
            b.setMinWidth(0);
            b.setMinimumWidth(0);
            bs[i] = b;
            b.setOnClickListener(v -> {
                values.put(key, String.valueOf(o));
                for (int j = 0; j < bs.length; j++) bs[j].setAlpha(options[j] == o ? 1f : 0.45f);
            });
            b.setAlpha(o == cur ? 1f : 0.45f);
            line.addView(b, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        }
        row.addView(line);
        row.addView(text(hint, 12, DIM, false));
        return row;
    }

    private View toggle(String title, String key, boolean def, String hint) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(0, px(10), 0, px(4));
        Switch sw = new Switch(this);
        sw.setText(title);
        sw.setTextColor(FG);
        sw.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        sw.setChecked(intOf(key, def ? 1 : 0) != 0);
        sw.setOnCheckedChangeListener((b, on) -> values.put(key, on ? "1" : "0"));
        row.addView(sw);
        row.addView(text(hint, 12, DIM, false));
        return row;
    }
}
