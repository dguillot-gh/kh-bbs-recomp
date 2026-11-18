package com.psprecomp.bbs;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.SeekBar;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.text.DateFormat;
import java.util.Date;

/**
 * Birth by Sleep shell (2026-11-16): a full-screen SurfaceView whose ANativeWindow goes to the Vulkan GE backend;
 * the game runs on a native thread (libbbs.so). Game data is pushed with adb into
 * /sdcard/Android/data/com.psprecomp.bbs/files/ (EBOOT.BIN.dec + disc-files/).
 * Controls: on-screen pad (TouchPad, user layout) and any Bluetooth/USB gamepad.
 * 2026-11-18: in-game menu (☰ button, Back, gamepad Select+Start or Mode): save/load state slots, layout editor,
 * opacity, FPS counter, camera inversion, stick deadzone. The game is paused while the menu is open.
 */
public class BbsActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "BBS";
    static final int PSP_SELECT = 0x1, PSP_START = 0x8, PSP_UP = 0x10, PSP_RIGHT = 0x20, PSP_DOWN = 0x40,
            PSP_LEFT = 0x80, PSP_L = 0x100, PSP_R = 0x200, PSP_TRIANGLE = 0x1000, PSP_CIRCLE = 0x2000,
            PSP_CROSS = 0x4000, PSP_SQUARE = 0x8000;
    private static final int STATE_SLOTS = 8;

    private int touchButtons, keyButtons, hatButtons, triggerButtons;
    // Stick state in -1..1 (y down): touch stick, gamepad left/right sticks (raw; deadzone applied when sent).
    private float touchLx, touchLy, touchRx, touchRy, padLx, padLy, padRx, padRy;

    private SharedPreferences settings;
    private float deadzone;
    private boolean invertCamX, invertCamY;

    static { System.loadLibrary("bbs"); }

    static native void nativeSetSurface(Object surface);
    static native boolean nativeStart(String filesDir, String eboot, String disc, long maxDispatches);
    static native void nativeSetButtons(int buttons);
    static native void nativeSetAnalog(int lx, int ly, int rx, int ry);
    static native void nativeRequestState(int op, String path);
    static native int nativeStateResult();
    static native void nativeSetPaused(boolean paused);
    static native long nativeFlipCount();
    static native boolean nativeSetTextureReplacement(boolean on);
    static native void nativeSet60Fps(boolean on);

    private boolean started;
    private TouchPad pad;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean menuOpen, stateBusy, activityPaused;
    /** Texture pack (PPSSPP textures.ini) in getExternalFilesDir()/textures; on/off from the menu. */
    private boolean texturePack = true, texturePackLoaded;
    /** 60 FPS mode (experimental): keeps BBS's vblank frame-interval word at 0 (native, see main.cpp). */
    private boolean fps60;

    // FPS: game frame flips per half second.
    private long lastFlips = -1, lastFlipTime;
    private final Runnable fpsTick = new Runnable() {
        @Override public void run() {
            long now = System.nanoTime(), flips = started ? nativeFlipCount() : 0;
            if (lastFlips >= 0 && now > lastFlipTime) {
                pad.setFps((flips - lastFlips) * 1e9f / (now - lastFlipTime));
            }
            lastFlips = flips;
            lastFlipTime = now;
            handler.postDelayed(this, 500);
        }
    };

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        settings = getSharedPreferences("bbs_settings", MODE_PRIVATE);
        deadzone = settings.getFloat("deadzone", 0.15f);
        invertCamX = settings.getBoolean("invert_cam_x", false);
        invertCamY = settings.getBoolean("invert_cam_y", false);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        android.widget.FrameLayout root = new android.widget.FrameLayout(this);
        root.addView(view);
        pad = new TouchPad(this, new TouchPad.Listener() {
            @Override public void onButtons(int b) { touchButtons = b; pushButtons(); }
            @Override public void onStick(float x, float y) { touchLx = x; touchLy = y; pushAnalog(); }
            @Override public void onCamera(float x, float y) { touchRx = x; touchRy = y; pushAnalog(); }
            @Override public void onMenu() { openMenu(); }
            @Override public void onEditDone() { menuOpen = false; updatePause(); hideSystemBars(); }
        });
        root.addView(pad);
        setContentView(root);
        hideSystemBars();
        handler.post(fpsTick);
    }

    @Override
    protected void onPause() {
        super.onPause();
        activityPaused = true;
        updatePause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        activityPaused = false;
        updatePause();
    }

    private void updatePause() {
        if (started) nativeSetPaused(menuOpen || activityPaused);
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

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        nativeSetSurface(holder.getSurface());
        if (started) return;
        File data = getExternalFilesDir(null);
        File eboot = new File(data, "EBOOT.BIN.dec");
        File disc = new File(data, "disc-files");
        if (!eboot.isFile() || !disc.isDirectory()) {
            Log.e(TAG, "Game data missing: push EBOOT.BIN.dec and disc-files/ to " + data);
            return;
        }
        File files = getFilesDir();
        try (FileOutputStream out = new FileOutputStream(new File(files, "VCSNative.ini"))) {
            // The FPS counter is drawn by TouchPad now (movable, toggled from the menu), not the Vulkan overlay.
            // Internal resolution (menu "Resolution"): the Vulkan GE backend renders at 480x272 x scale
            // (vcs_config Rendering.InternalResolutionMode/InternalScale). Read once at startup.
            int scale = Math.max(1, Math.min(4, settings.getInt("render_scale", 1)));
            String rendering = "[Rendering]\nBackend=Vulkan\nNativeGE=true\n"
                    + (scale > 1 ? "InternalResolutionMode=Scale\nInternalScale=" + scale + "\n" : "")
                    // Aspect (menu "Aspect ratio"): Original = PSP 480:272 with side bars; Widescreen = the
                    // renderer's Hor+ projection widening to the screen's ratio (vcs_config [Widescreen]).
                    + (settings.getBoolean("widescreen", false) ? widescreenIni() : "");
            out.write(rendering.getBytes("UTF-8"));
        } catch (Exception e) {
            Log.e(TAG, "VCSNative.ini", e);
        }
        started = nativeStart(files.getAbsolutePath(), eboot.getAbsolutePath(), disc.getAbsolutePath(),
                4_000_000_000_000L);
        texturePack = settings.getBoolean("texture_pack", true);
        if (started) texturePackLoaded = nativeSetTextureReplacement(texturePack);
        fps60 = settings.getBoolean("fps60", false);
        if (started) nativeSet60Fps(fps60);
        updatePause();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSetSurface(null);
    }

    // ---- menu ----

    private AlertDialog.Builder dialog() {
        return new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert);
    }

    private void showDialog(AlertDialog d) {
        openDialogs++;
        d.setOnDismissListener(x -> {
            openDialogs--;
            // A follow-up dialog may have opened from this one; resume only when nothing else is up.
            handler.post(this::maybeResume);
        });
        d.show();
    }

    private int openDialogs;

    private void maybeResume() {
        if (openDialogs == 0 && !pad.isEditing() && !stateBusy) { menuOpen = false; updatePause(); hideSystemBars(); }
    }

    private void openMenu() {
        if (pad.isEditing()) return;
        menuOpen = true;
        updatePause();
        touchButtons = 0; keyButtons = 0; pushButtons();
        String[] items = {
                "Resume",
                "Save state…",
                "Load state…",
                "Edit controls layout",
                "Controls opacity…",
                pad.controlsHidden() ? "Show touch controls" : "Hide touch controls (gamepad)",
                (pad.isShown(TouchPad.FPS) ? "✓ " : "   ") + "FPS counter",
                (invertCamX ? "✓ " : "   ") + "Invert camera left/right",
                (invertCamY ? "✓ " : "   ") + "Invert camera up/down",
                "Stick deadzone… (" + Math.round(deadzone * 100) + "%)",
                "HD textures… (" + (!TexturePackInstaller.installed(getExternalFilesDir(null)) ? "not installed"
                        : texturePack ? "on" : "off") + ")",
                (fps60 ? "✓ " : "   ") + "60 FPS mode (experimental)",
                "Resolution… (" + settings.getInt("render_scale", 1) + "x)",
                "Aspect ratio… (" + (settings.getBoolean("widescreen", false) ? "Phone screen" : "16:9") + ")",
        };
        AlertDialog d = dialog().setTitle("Birth by Sleep").setItems(items, (x, which) -> {
            switch (which) {
                case 1: openSlots(true); break;
                case 2: openSlots(false); break;
                case 3:
                    pad.setEditing(true);
                    Toast.makeText(this, "Drag controls to move them. Tap Done when finished.", Toast.LENGTH_LONG).show();
                    break;
                case 4: openOpacity(); break;
                case 5: pad.setControlsHidden(!pad.controlsHidden()); break;
                case 6: pad.setShown(TouchPad.FPS, !pad.isShown(TouchPad.FPS)); break;
                case 7: invertCamX = !invertCamX; settings.edit().putBoolean("invert_cam_x", invertCamX).apply(); pushAnalog(); break;
                case 8: invertCamY = !invertCamY; settings.edit().putBoolean("invert_cam_y", invertCamY).apply(); pushAnalog(); break;
                case 9: openDeadzone(); break;
                case 10: openTextures(); break;
                case 11:
                    fps60 = !fps60;
                    settings.edit().putBoolean("fps60", fps60).apply();
                    nativeSet60Fps(fps60);
                    break;
                case 12: openResolution(); break;
                case 13: openAspect(); break;
                default: break;
            }
        }).create();
        showDialog(d);
    }

    /** Internal render resolution, 1x (PSP 480x272) to 4x. Applied at startup, so offer a restart. */
    /**
     * Widescreen = fill the whole screen with a wider 3D view. The renderer's Hor+ factor comes from
     * [Widescreen] AspectRatio (on Android its "desktop" surface is still 480:272, so give it the real screen),
     * and the presenter must fill the screen ([Display] AspectRatio=Fill) instead of pillarboxing; the
     * projection is narrowed by the same factor, so nothing looks stretched. 2D HUD draws are re-squeezed.
     */
    private String widescreenIni() {
        android.graphics.Rect b = getWindowManager().getCurrentWindowMetrics().getBounds();
        int w = Math.max(b.width(), b.height()), h = Math.min(b.width(), b.height());
        // vcs_config caps each side at 1000: write the ratio as N:100 (2712x1220 -> 222:100).
        int x = Math.max(100, Math.min(1000, Math.round(100f * w / Math.max(1, h))));
        return "[Display]\nAspectRatio=Fill\n[Widescreen]\nEnabled=true\nAspectRatio=" + x + ":100\n"
                + "GenericProjection=true\n";
    }

    // ---- HD textures (2026-11-18) ----
    // A PPSSPP texture pack (BBS HD ReMix) is never shipped with the app: the player downloads the ZIP themselves and
    // imports it here; TexturePackInstaller unpacks it into getExternalFilesDir()/textures, which the native texture
    // replacer reads at startup. (An in-app download was built 11-18 and taken out for now at the user's request.)
    private static final int REQUEST_IMPORT_ZIP = 4201;
    private boolean installingTextures;

    private void openTextures() {
        final File ext = getExternalFilesDir(null);
        final boolean installed = TexturePackInstaller.installed(ext);
        java.util.List<String> items = new java.util.ArrayList<>();
        java.util.List<Runnable> actions = new java.util.ArrayList<>();
        if (installed) {
            items.add((texturePack ? "✓ " : "   ") + "Use HD textures");
            actions.add(() -> {
                texturePack = !texturePack;
                settings.edit().putBoolean("texture_pack", texturePack).apply();
                texturePackLoaded = nativeSetTextureReplacement(texturePack);
            });
        }
        items.add("Import a texture pack ZIP…");
        actions.add(() -> {
            android.content.Intent pick = new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(android.content.Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
            pick.putExtra(android.content.Intent.EXTRA_MIME_TYPES,
                    new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"});
            try {
                startActivityForResult(pick, REQUEST_IMPORT_ZIP);
            } catch (android.content.ActivityNotFoundException e) {
                Toast.makeText(this, "No file picker on this phone", Toast.LENGTH_LONG).show();
            }
        });
        if (installed) {
            items.add("Remove HD textures");
            actions.add(() -> showDialog(dialog().setTitle("Remove the HD texture pack?")
                    .setMessage("Frees its storage. You can import it again later.")
                    .setPositiveButton("Remove", (y, w) -> {
                        TexturePackInstaller.remove(ext);
                        askRestart("HD textures removed");
                    })
                    .setNegativeButton("Cancel", null).create()));
        }
        String title = installed ? "HD textures" : "HD textures (not installed)";
        showDialog(dialog().setTitle(title)
                .setItems(items.toArray(new String[0]), (x, which) -> actions.get(which).run())
                .setNegativeButton("Back", null).create());
    }

    private interface StreamOpener { java.io.InputStream open() throws Exception; }

    /** Unpacks on a background thread with a progress dialog; deleteAfter (the downloaded ZIP) is removed after. */
    private void installTextures(StreamOpener opener, File deleteAfter) {
        installingTextures = true;
        final AlertDialog progress = dialog().setTitle("Installing HD textures…")
                .setMessage("Unpacking…").setCancelable(false).create();
        showDialog(progress);
        final File ext = getExternalFilesDir(null);
        new Thread(() -> {
            String error;
            try (java.io.InputStream in = new java.io.BufferedInputStream(opener.open(), 1 << 16)) {
                error = TexturePackInstaller.install(in, ext, (files, bytes) -> handler.post(() ->
                        progress.setMessage("Unpacking… " + files + " files, " + (bytes >> 20) + " MB")));
            } catch (Exception e) {
                error = "Couldn't open the ZIP: " + e.getMessage();
            }
            if (deleteAfter != null) //noinspection ResultOfMethodCallIgnored
                deleteAfter.delete();
            final String result = error;
            handler.post(() -> {
                installingTextures = false;
                progress.dismiss();
                if (result != null) {
                    showDialog(dialog().setTitle("HD textures not installed").setMessage(result)
                            .setPositiveButton("OK", null).create());
                    return;
                }
                texturePack = true;
                settings.edit().putBoolean("texture_pack", true).apply();
                Toast.makeText(this, "HD textures installed", Toast.LENGTH_LONG).show();
                // Shown a moment after the progress dialog is gone: opened in the same frame it never appeared.
                handler.postDelayed(() -> askRestart("HD textures installed"), 300);
            });
        }, "texture-install").start();
    }

    private void askRestart(String what) {
        showDialog(dialog().setTitle(what)
                .setMessage("Restart the game to apply? Save a state first if you want to keep your progress.")
                .setPositiveButton("Restart now", (y, w) -> restartApp())
                .setNegativeButton("Later", null).create());
    }

    @Override
    protected void onActivityResult(int request, int result, android.content.Intent data) {
        super.onActivityResult(request, result, data);
        if (request != REQUEST_IMPORT_ZIP || result != RESULT_OK || data == null || data.getData() == null) return;
        final android.net.Uri uri = data.getData();
        installTextures(() -> getContentResolver().openInputStream(uri), null);
    }

    /** Original PSP aspect (bars at the sides) or Widescreen (wider view, experimental). Applied at startup. */
    private void openAspect() {
        final boolean current = settings.getBoolean("widescreen", false);
        String[] choices = {"16:9 — PSP original (bars at the sides)", "Phone screen — fill it, wider view (experimental)"};
        AlertDialog d = dialog().setTitle("Aspect ratio").setSingleChoiceItems(choices, current ? 1 : 0, (x, which) -> {
            x.dismiss();
            boolean wide = which == 1;
            if (wide == current) return;
            settings.edit().putBoolean("widescreen", wide).apply();
            showDialog(dialog().setTitle("Restart to apply?")
                    .setMessage("The game restarts. Save a state first if you want to keep your progress.")
                    .setPositiveButton("Restart now", (y, w) -> restartApp())
                    .setNegativeButton("Later", null).create());
        }).setNegativeButton("Back", null).create();
        showDialog(d);
    }

    private void openResolution() {
        final int current = Math.max(1, Math.min(4, settings.getInt("render_scale", 1)));
        String[] choices = {
                "1x — PSP original (480×272)",
                "2x (960×544)",
                "3x (1440×816)",
                "4x (1920×1088)",
        };
        AlertDialog d = dialog().setTitle("Resolution").setSingleChoiceItems(choices, current - 1, (x, which) -> {
            x.dismiss();
            int scale = which + 1;
            if (scale == current) return;
            settings.edit().putInt("render_scale", scale).apply();
            AlertDialog r = dialog().setTitle("Restart to apply " + scale + "x?")
                    .setMessage("The game restarts. Save a state first if you want to keep your progress.")
                    .setPositiveButton("Restart now", (y, w) -> restartApp())
                    .setNegativeButton("Later", null).create();
            showDialog(r);
        }).setNegativeButton("Back", null).create();
        showDialog(d);
    }

    /** The native game can only start once per process: relaunch the activity in a fresh process. */
    private void restartApp() {
        // Start the activity while we're still in the foreground, then exit: the system brings it up in a
        // new process.
        android.content.Intent intent = android.content.Intent.makeRestartActivityTask(
                new android.content.ComponentName(this, BbsActivity.class));
        startActivity(intent);
        Runtime.getRuntime().exit(0);
    }

    private File stateFile(int slot) {
        File dir = new File(getFilesDir(), "states");
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        return new File(dir, "slot" + slot + ".bbsstate");
    }

    private void openSlots(boolean save) {
        String[] items = new String[STATE_SLOTS];
        DateFormat fmt = DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT);
        for (int i = 0; i < STATE_SLOTS; i++) {
            File f = stateFile(i + 1);
            items[i] = "Slot " + (i + 1) + "  —  " + (f.isFile() ? fmt.format(new Date(f.lastModified())) : "empty");
        }
        AlertDialog d = dialog().setTitle(save ? "Save state" : "Load state").setItems(items, (x, which) -> {
            File f = stateFile(which + 1);
            if (!save && !f.isFile()) {
                Toast.makeText(this, "Slot " + (which + 1) + " is empty", Toast.LENGTH_SHORT).show();
                return;
            }
            if (save && f.isFile()) {
                AlertDialog c = dialog().setTitle("Overwrite slot " + (which + 1) + "?")
                        .setPositiveButton("Overwrite", (y, w) -> runState(true, which + 1))
                        .setNegativeButton("Cancel", null).create();
                showDialog(c);
                return;
            }
            runState(save, which + 1);
        }).setNegativeButton("Back", null).create();
        showDialog(d);
    }

    /** Asks the game thread to save/load at its next vblank wait and reports the result. */
    private void runState(boolean save, int slot) {
        if (!started || stateBusy) return;
        stateBusy = true;
        menuOpen = true;          // stay paused until it is done
        updatePause();
        nativeRequestState(save ? 1 : 2, stateFile(slot).getAbsolutePath());
        final long deadline = System.currentTimeMillis() + 15000;
        handler.postDelayed(new Runnable() {
            @Override public void run() {
                int r = nativeStateResult();
                if (r == 0 && System.currentTimeMillis() < deadline) { handler.postDelayed(this, 100); return; }
                stateBusy = false;
                String what = save ? "Saved to slot " + slot : "Loaded slot " + slot;
                String msg = r == 1 ? what
                        : r == 0 ? "The game is busy (loading?). Try again in a moment."
                        : (save ? "Save failed" : "Load failed: the state is from an incompatible build");
                Toast.makeText(BbsActivity.this, msg, Toast.LENGTH_SHORT).show();
                maybeResume();
            }
        }, 100);
    }

    private SeekBar slider(int max, int value) {
        SeekBar bar = new SeekBar(this);
        bar.setMax(max);
        bar.setProgress(value);
        int p = Math.round(16 * getResources().getDisplayMetrics().density);
        bar.setPadding(p * 2, p, p * 2, p);
        return bar;
    }

    private void openOpacity() {
        SeekBar bar = slider(90, Math.round(pad.getOpacity() * 100) - 10);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int v, boolean user) { pad.setOpacity((v + 10) / 100f); }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        showDialog(dialog().setTitle("Controls opacity").setView(bar).setPositiveButton("OK", null).create());
    }

    private void openDeadzone() {
        SeekBar bar = slider(40, Math.round(deadzone * 100));
        AlertDialog d = dialog().setTitle("Stick deadzone: " + Math.round(deadzone * 100) + "%").setView(bar)
                .setPositiveButton("OK", null).create();
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int v, boolean user) {
                deadzone = v / 100f;
                settings.edit().putFloat("deadzone", deadzone).apply();
                d.setTitle("Stick deadzone: " + v + "%");
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        showDialog(d);
    }

    // ---- gamepad ----

    private static int keyToPsp(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_UP: return PSP_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return PSP_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return PSP_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return PSP_RIGHT;
            case KeyEvent.KEYCODE_BUTTON_A: return PSP_CROSS;
            case KeyEvent.KEYCODE_BUTTON_B: return PSP_CIRCLE;
            case KeyEvent.KEYCODE_BUTTON_X: return PSP_SQUARE;
            case KeyEvent.KEYCODE_BUTTON_Y: return PSP_TRIANGLE;
            case KeyEvent.KEYCODE_BUTTON_L1: return PSP_L;
            case KeyEvent.KEYCODE_BUTTON_R1: return PSP_R;
            case KeyEvent.KEYCODE_BUTTON_L2: return PSP_L;
            case KeyEvent.KEYCODE_BUTTON_R2: return PSP_R;
            case KeyEvent.KEYCODE_BUTTON_START: return PSP_START;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return PSP_SELECT;
            default: return 0;
        }
    }

    @Override
    public boolean onKeyDown(int code, KeyEvent e) {
        if (code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_BUTTON_MODE) {
            if (pad.isEditing()) { pad.setEditing(false); menuOpen = false; updatePause(); }
            else if (!menuOpen) openMenu();
            return true;
        }
        int b = keyToPsp(code);
        if (b == 0) return super.onKeyDown(code, e);
        keyButtons |= b;
        // Select + Start together opens the menu (gamepads without a Mode button).
        if ((keyButtons & (PSP_SELECT | PSP_START)) == (PSP_SELECT | PSP_START) && !menuOpen) {
            keyButtons = 0;
            pushButtons();
            openMenu();
            return true;
        }
        pushButtons();
        return true;
    }

    @Override
    public boolean onKeyUp(int code, KeyEvent e) {
        if (code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_BUTTON_MODE) return true;
        int b = keyToPsp(code);
        if (b == 0) return super.onKeyUp(code, e);
        keyButtons &= ~b; pushButtons(); return true;
    }

    private void pushButtons() {
        nativeSetButtons(touchButtons | keyButtons | hatButtons | triggerButtons);
    }

    private static int toByte(float v) {
        int b = Math.round(128f + v * 127.5f);
        return b < 0 ? 0 : (b > 255 ? 255 : b);
    }

    /** Radial deadzone, rescaled so the stick still reaches full tilt. */
    private float[] dead(float x, float y) {
        float len = (float) Math.sqrt(x * x + y * y);
        if (len < deadzone || len == 0f) return new float[] {0f, 0f};
        float scale = Math.min(1f, (len - deadzone) / (1f - deadzone)) / len;
        return new float[] {x * scale, y * scale};
    }

    private void pushAnalog() {
        float[] pl = dead(padLx, padLy), pr = dead(padRx, padRy);
        boolean padLeft = pl[0] != 0f || pl[1] != 0f;
        float lx = padLeft ? pl[0] : touchLx, ly = padLeft ? pl[1] : touchLy;
        boolean padRight = pr[0] != 0f || pr[1] != 0f;
        float rx = padRight ? pr[0] : touchRx, ry = padRight ? pr[1] : touchRy;
        if (invertCamX) rx = -rx;
        if (invertCamY) ry = -ry;
        nativeSetAnalog(toByte(lx), toByte(ly), toByte(rx), toByte(ry));
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent e) {
        if ((e.getSource() & android.view.InputDevice.SOURCE_JOYSTICK) != android.view.InputDevice.SOURCE_JOYSTICK
                || e.getActionMasked() != MotionEvent.ACTION_MOVE) {
            return super.onGenericMotionEvent(e);
        }
        padLx = e.getAxisValue(MotionEvent.AXIS_X); padLy = e.getAxisValue(MotionEvent.AXIS_Y);
        padRx = e.getAxisValue(MotionEvent.AXIS_Z); padRy = e.getAxisValue(MotionEvent.AXIS_RZ);
        pushAnalog();
        float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        int hat = 0;
        if (hx < -0.5f) hat |= PSP_LEFT; else if (hx > 0.5f) hat |= PSP_RIGHT;
        if (hy < -0.5f) hat |= PSP_UP; else if (hy > 0.5f) hat |= PSP_DOWN;
        float lt = Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE));
        float rt = Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS));
        int trig = (lt > 0.5f ? PSP_L : 0) | (rt > 0.5f ? PSP_R : 0);
        if (hat != hatButtons || trig != triggerButtons) { hatButtons = hat; triggerButtons = trig; pushButtons(); }
        return true;
    }
}
