package com.psprecomp.bbs;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;

/**
 * Launcher (C1, 2026-11-18): the app's first screen. With the game files in place: Continue (newest save state),
 * Start, Load state, Settings, Game files. Without them: first-run setup (C2) from the player's own ISO
 * (IsoInstaller). The background is the disc's own PSP_GAME/PIC1.PNG, read from the player's files at run time.
 * The game itself runs in BbsActivity; the native game starts once per process, so "back to the launcher" from the
 * game restarts the process.
 */
public class LauncherActivity extends Activity {
    private static final String TAG = "BBS";
    private static final int REQUEST_ISO = 4301;
    static final int STATE_SLOTS = 8;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private SharedPreferences settings;
    private FrameLayout root;
    private boolean importing;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        settings = getSharedPreferences("bbs_settings", MODE_PRIVATE);
        root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        setContentView(root);
        build();
        // adb testing: am start -n com.psprecomp.bbs/.LauncherActivity --es iso_path <file in the app's storage>
        String isoPath = getIntent().getStringExtra("iso_path");
        if (isoPath != null) startImport(null, new File(isoPath));
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        if (!importing) build();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    private void hideSystemBars() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    // Gamepad: A confirms like a tap (D-pad moves the focus by itself), B backs out of dialogs.
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (e.getKeyCode() == KeyEvent.KEYCODE_BUTTON_A) {
            View f = getCurrentFocus();
            if (f != null && e.getAction() == KeyEvent.ACTION_UP) f.performClick();
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    private File data() { return getExternalFilesDir(null); }

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    // ---- screen ----

    /** This build is for Final Mix (ULJM-05775), not the US disc. */
    private boolean swapOX() { return settings.getBoolean("swap_ox", BbsActivity.swapOXDefault(this)); }

    private boolean finalMix() { return "ULJM-05775".equals(IsoInstaller.gameIdFor(this)); }

    private void build() {
        root.removeAllViews();
        File data = data();
        boolean installed = data != null && IsoInstaller.installed(data);

        // A background packed into this build (build-android.sh LAUNCHER_BG) wins; else the disc's own PIC1.PNG.
        Bitmap own = null;
        try (java.io.InputStream s = getAssets().open("launcher_bg")) {
            own = BitmapFactory.decodeStream(s);
        } catch (java.io.IOException ignored) {
        }
        if (own != null || installed) {
            Bitmap pic = own != null ? own
                    : BitmapFactory.decodeFile(new File(IsoInstaller.disc(data), "PSP_GAME/PIC1.PNG").getAbsolutePath());
            final boolean ownBg = own != null;
            if (pic != null) {
                // The logo sits near the top of PIC1 (centre about 300,62 of 480x272). Place it in the middle of the
                // screen height and of the free area right of the menu; the picture's edges that come into view are
                // dark and blend into the scrims below.
                final int menuWidth = dp(MENU_WIDTH_DP);
                // Softens only the picture's top edge (the band above it is plain black), so the logo isn't dimmed.
                final View top = new View(this);
                top.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                        new int[]{0xFF000000, 0x00000000}));
                // Final Mix's PIC1 is a full scene with the logo top-right: fill the screen, right-aligned, so the
                // logo and the characters stay in view (no black band, no top fade).
                final boolean scene = ownBg || finalMix();
                ImageView bg = new ImageView(this) {
                    @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
                        super.onLayout(changed, l, t, r, b);
                        final int w = r - l, h = b - t;
                        float scale = Math.max(w / (float) pic.getWidth(), h / (float) pic.getHeight());
                        if (scene) {
                            android.graphics.Matrix m = new android.graphics.Matrix();
                            m.setScale(scale, scale);
                            m.postTranslate(w - pic.getWidth() * scale, (h - pic.getHeight() * scale) / 2f);
                            setImageMatrix(m);
                            return;
                        }
                        float logoX = pic.getWidth() * (300f / 480f), logoY = pic.getHeight() * (62f / 272f);
                        float targetX = menuWidth + (w - menuWidth) / 2f, targetY = h / 2f;
                        android.graphics.Matrix m = new android.graphics.Matrix();
                        m.setScale(scale, scale);
                        m.postTranslate(targetX - logoX * scale, targetY - logoY * scale);
                        setImageMatrix(m);
                        final int edge = Math.round(targetY - logoY * scale), fade = Math.round(5 * scale);
                        post(() -> {
                            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) top.getLayoutParams();
                            if (lp.topMargin == edge && lp.height == fade) return;
                            lp.topMargin = Math.max(0, edge);
                            lp.height = fade;
                            top.setLayoutParams(lp);
                        });
                    }
                };
                bg.setImageBitmap(pic);
                bg.setScaleType(ImageView.ScaleType.MATRIX);
                root.addView(bg, new FrameLayout.LayoutParams(-1, -1));
                root.addView(top, new FrameLayout.LayoutParams(-1, 0, Gravity.TOP));
            }
        }
        View scrim = new View(this);
        GradientDrawable shade = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[]{0xE6000000, 0xB0000000, 0x20000000});
        scrim.setBackground(shade);
        root.addView(scrim, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setGravity(Gravity.CENTER_VERTICAL);
        column.setPadding(dp(56), dp(24), dp(24), dp(24));
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.addView(column);
        root.addView(scroll, new FrameLayout.LayoutParams(dp(MENU_WIDTH_DP), -1, Gravity.START));

        TextView title = text("Kingdom Hearts\nBirth by Sleep", 28, Color.WHITE, true);
        column.addView(title);
        if (finalMix()) {
            TextView fm = text("FINAL MIX", 18, 0xFFE8C46A, true);
            fm.setLetterSpacing(0.25f);
            fm.setPadding(0, dp(2), 0, 0);
            column.addView(fm);
        }
        TextView sub = text("Native Android port", 14, 0xFFB8C7E0, false);
        sub.setPadding(0, dp(2), 0, dp(16));
        column.addView(sub);

        if (!installed) {
            column.addView(text("First-time setup", 20, Color.WHITE, true));
            TextView info = text("Choose the ISO of your own " + IsoInstaller.gameName(this)
                    + " disc. The app copies the game files and decrypts the game program on this phone. Nothing is "
                    + "downloaded. Needs about 4 GB of free space (the disc image, its files, and decrypted data).", 15, 0xFFDDDDDD, false);
            info.setPadding(0, dp(8), 0, dp(20));
            column.addView(info);
            View choose = button(column, "Choose ISO…", null, v -> pickIso());
            choose.requestFocus();
            return;
        }

        File latest = null;
        int latestSlot = 0;
        for (int i = 1; i <= STATE_SLOTS; i++) {
            File f = stateFile(i);
            if (f.isFile() && (latest == null || f.lastModified() > latest.lastModified())) { latest = f; latestSlot = i; }
        }
        DateFormat fmt = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT);
        View first;
        if (latest != null) {
            final int slot = latestSlot;
            first = button(column, "Continue", "Save state " + slot + " · " + fmt.format(new Date(latest.lastModified())),
                    v -> play(slot));
            button(column, "Start game", "From the title screen (in-game saves)", v -> play(0));
        } else {
            first = button(column, "Start game", null, v -> play(0));
        }
        // Story slots (2026-11-20): one button per character whose automatic slot exists.
        LinearLayout stories = null;
        for (int c = 1; c <= 3; c++) {
            final File f = StorySlots.file(this, c);
            if (!f.isFile()) continue;
            if (stories == null) {
                stories = new LinearLayout(this);
                stories.setOrientation(LinearLayout.HORIZONTAL);
                column.addView(stories, new LinearLayout.LayoutParams(-1, ViewGroup.LayoutParams.WRAP_CONTENT));
            }
            small(stories, "▶ " + StorySlots.NAMES[c], v -> playFile(f));
        }
        // The less used actions share one row, so everything fits on a phone screen without scrolling.
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        column.addView(row, new LinearLayout.LayoutParams(-1, ViewGroup.LayoutParams.WRAP_CONTENT));
        small(row, "Load state", v -> openSlots());
        small(row, "Settings", v -> openSettings());
        small(row, "Game files", v -> openGameFiles());
        TextView status = text(summary(), 12, 0xFF9FB0CC, false);
        column.addView(status);
        first.requestFocus();
    }

    private String summary() {
        return settings.getInt("render_scale", 1) + "x · " + (settings.getBoolean("widescreen", false) ? "phone screen" : "16:9")
                + (settings.getBoolean("fps60", false) ? " · 60 FPS" : "")
                + (settings.getBoolean("texture_pack", true) && TexturePackInstaller.installed(data()) ? " · HD textures" : "");
    }

    private TextView text(String s, float sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return t;
    }

    private static final int MENU_WIDTH_DP = 440;

    private void small(LinearLayout row, String label, View.OnClickListener click) {
        TextView t = text(label, 15, Color.WHITE, true);
        t.setGravity(Gravity.CENTER);
        t.setPadding(dp(6), dp(10), dp(6), dp(10));
        t.setBackground(states());
        t.setFocusable(true);
        t.setClickable(true);
        t.setOnClickListener(click);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        lp.bottomMargin = dp(8);
        if (row.getChildCount() > 0) lp.leftMargin = dp(8);
        row.addView(t, lp);
    }

    private StateListDrawable states() {
        StateListDrawable bg = new StateListDrawable();
        bg.addState(new int[]{android.R.attr.state_pressed}, rounded(0xCC3D6FD6, 0xFFFFFFFF));
        bg.addState(new int[]{android.R.attr.state_focused}, rounded(0xB0284C9C, 0xFFFFFFFF));
        bg.addState(new int[]{}, rounded(0x80101828, 0x60FFFFFF));
        return bg;
    }

    private View button(LinearLayout parent, String label, String detail, View.OnClickListener click) {
        LinearLayout b = new LinearLayout(this);
        b.setOrientation(LinearLayout.VERTICAL);
        b.setPadding(dp(20), dp(10), dp(20), dp(10));
        b.addView(text(label, 19, Color.WHITE, true));
        if (detail != null) b.addView(text(detail, 13, 0xFFC8D2E6, false));
        StateListDrawable bg = new StateListDrawable();
        bg.addState(new int[]{android.R.attr.state_pressed}, rounded(0xCC3D6FD6, 0xFFFFFFFF));
        bg.addState(new int[]{android.R.attr.state_focused}, rounded(0xB0284C9C, 0xFFFFFFFF));
        bg.addState(new int[]{}, rounded(0x80101828, 0x60FFFFFF));
        b.setBackground(bg);
        b.setFocusable(true);
        b.setClickable(true);
        b.setOnClickListener(click);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(8);
        parent.addView(b, lp);
        return b;
    }

    private GradientDrawable rounded(int fill, int stroke) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setCornerRadius(dp(14));
        d.setStroke(dp(1), stroke);
        return d;
    }

    private AlertDialog.Builder dialog() {
        return new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert);
    }

    // ---- actions ----

    private File stateFile(int slot) { return new File(new File(getFilesDir(), "states"), "slot" + slot + ".bbsstate"); }

    private void play(int loadSlot) {
        Intent i = new Intent(this, BbsActivity.class);
        if (loadSlot > 0) i.putExtra(BbsActivity.EXTRA_LOAD_SLOT, loadSlot);
        startActivity(i);
        finish();
    }

    private void playFile(File state) {
        Intent i = new Intent(this, BbsActivity.class);
        i.putExtra(BbsActivity.EXTRA_LOAD_FILE, state.getAbsolutePath());
        startActivity(i);
        finish();
    }

    private void openSlots() {
        List<String> items = new ArrayList<>();
        List<Integer> slots = new ArrayList<>();
        DateFormat fmt = DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT);
        for (int i = 1; i <= STATE_SLOTS; i++) {
            File f = stateFile(i);
            if (!f.isFile()) continue;
            items.add("Slot " + i + "  —  " + fmt.format(new Date(f.lastModified())));
            slots.add(i);
        }
        if (items.isEmpty()) { Toast.makeText(this, "No save states yet (save one from the in-game menu).", Toast.LENGTH_LONG).show(); return; }
        dialog().setTitle("Load state").setItems(items.toArray(new String[0]), (d, w) -> play(slots.get(w)))
                .setNegativeButton("Back", null).show();
    }

    private void openSettings() {
        final int scale = Math.max(1, Math.min(4, settings.getInt("render_scale", 1)));
        final boolean wide = settings.getBoolean("widescreen", false);
        final boolean fps60 = settings.getBoolean("fps60", false);
        final boolean hd = settings.getBoolean("texture_pack", true);
        final boolean hdInstalled = TexturePackInstaller.installed(data());
        String[] items = {
                "Resolution: " + scale + "x" + (scale == 1 ? " (PSP original)" : ""),
                "Aspect ratio: " + (wide ? "Phone screen (wider view)" : "16:9 (PSP original)"),
                (fps60 ? "✓ " : "   ") + "60 FPS mode",
                (hd ? "✓ " : "   ") + "HD textures" + (hdInstalled ? "" : " (no pack installed: import one from the in-game menu)"),
                (settings.getBoolean("invert_cam_x", false) ? "✓ " : "   ") + "Invert camera left/right",
                (settings.getBoolean("invert_cam_y", false) ? "✓ " : "   ") + "Invert camera up/down",
                (swapOX() ? "✓ " : "   ") + "Swap ○ / ✕ (USA layout: ✕ confirms)",
        };
        dialog().setTitle("Settings").setItems(items, (d, w) -> {
            switch (w) {
                case 0: {
                    String[] choices = {"1x — PSP original (480×272)", "2x (960×544)", "3x (1440×816)", "4x (1920×1088)"};
                    dialog().setTitle("Resolution").setSingleChoiceItems(choices, scale - 1, (x, which) -> {
                        x.dismiss();
                        settings.edit().putInt("render_scale", which + 1).apply();
                        build(); openSettings();
                    }).setNegativeButton("Back", null).show();
                    return;
                }
                case 1: {
                    String[] choices = {"16:9 — PSP original (bars at the sides)", "Phone screen — fill it, wider view (experimental)"};
                    dialog().setTitle("Aspect ratio").setSingleChoiceItems(choices, wide ? 1 : 0, (x, which) -> {
                        x.dismiss();
                        settings.edit().putBoolean("widescreen", which == 1).apply();
                        build(); openSettings();
                    }).setNegativeButton("Back", null).show();
                    return;
                }
                case 2: settings.edit().putBoolean("fps60", !fps60).apply(); break;
                case 3: settings.edit().putBoolean("texture_pack", !hd).apply(); break;
                case 4: settings.edit().putBoolean("invert_cam_x", !settings.getBoolean("invert_cam_x", false)).apply(); break;
                case 5: settings.edit().putBoolean("invert_cam_y", !settings.getBoolean("invert_cam_y", false)).apply(); break;
                case 6: settings.edit().putBoolean("swap_ox", !swapOX()).apply(); break;
                default: return;
            }
            build();
            openSettings();
        }).setNegativeButton("Done", null).show();
    }

    private void openGameFiles() {
        File data = data();
        String id = IsoInstaller.gameId(IsoInstaller.disc(data));
        String msg = "Installed: " + (id != null ? id : "game files") + "\nFolder: " + data.getAbsolutePath()
                + "\n\nImport again from an ISO, or delete the game files to free space (save states and settings stay).";
        dialog().setTitle("Game files").setMessage(msg)
                .setPositiveButton("Import ISO…", (d, w) -> pickIso())
                .setNeutralButton("Delete", (d, w) -> dialog().setTitle("Delete the game files?")
                        .setMessage("Removes the disc image and game files (about 4 GB). You'll need your ISO to play again.")
                        .setPositiveButton("Delete", (x, y) -> {
                            IsoInstaller.deleteTree(IsoInstaller.disc(data));
                            IsoInstaller.deleteTree(new File(data, "decrypted"));
                            //noinspection ResultOfMethodCallIgnored
                            IsoInstaller.iso(data).delete();
                            //noinspection ResultOfMethodCallIgnored
                            IsoInstaller.eboot(data).delete();
                            build();
                        }).setNegativeButton("Cancel", null).show())
                .setNegativeButton("Back", null).show();
    }

    private void pickIso() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/x-iso9660-image", "application/octet-stream", "*/*"});
        try {
            startActivityForResult(i, REQUEST_ISO);
        } catch (Exception e) {
            Toast.makeText(this, "No file picker available", Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == REQUEST_ISO && result == RESULT_OK && data != null && data.getData() != null)
            startImport(data.getData(), null);
    }

    private void startImport(Uri uri, File file) {
        importing = true;
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(16), dp(24), dp(8));
        final TextView what = text("Starting…", 15, Color.WHITE, false);
        final ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        box.addView(what);
        box.addView(bar);
        final AlertDialog progress = dialog().setTitle("Setting up the game").setView(box).setCancelable(false).create();
        progress.show();
        final long[] lastUi = {0};
        new Thread(() -> {
            String error = null;
            try {
                IsoInstaller.Progress p = (label, done, total) -> {
                    long now = System.currentTimeMillis();
                    if (now - lastUi[0] < 150 && done < total) return;
                    lastUi[0] = now;
                    final int permille = total > 0 ? (int) (done * 1000 / total) : 0;
                    final String line = total > 1 ? String.format("%s %d%%  (%.2f / %.2f GB)", label, permille / 10,
                            done / 1e9, total / 1e9) : label;
                    handler.post(() -> { what.setText(line); bar.setProgress(permille); });
                };
                if (file != null) IsoInstaller.install(this, file, p);
                else IsoInstaller.install(this, uri, p);
            } catch (IsoInstaller.SetupException e) {
                error = e.getMessage();
            } catch (Throwable e) {
                Log.e(TAG, "ISO import", e);
                error = "Setup failed: " + e;
            }
            final String failure = error;
            handler.post(() -> {
                importing = false;
                progress.dismiss();
                if (failure != null) {
                    dialog().setTitle("Couldn't set up the game").setMessage(failure).setPositiveButton("OK", null).show();
                } else {
                    Toast.makeText(this, "Ready to play!", Toast.LENGTH_LONG).show();
                }
                build();
            });
        }, "iso-import").start();
    }
}
