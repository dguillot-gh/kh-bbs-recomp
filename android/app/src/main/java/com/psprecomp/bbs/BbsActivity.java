package com.psprecomp.bbs;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;

/**
 * Birth by Sleep bring-up shell (2026-11-16): a full-screen SurfaceView whose ANativeWindow goes to the
 * Vulkan GE backend; the game runs on a native thread (libbbs.so). Game data is pushed with adb into
 * /sdcard/Android/data/com.psprecomp.bbs/files/ (EBOOT.BIN.dec + disc-files/).
 * Controls: on-screen pad (TouchPad: D-pad left, face buttons right, START/SELECT, L/R) and any
 * Bluetooth/USB gamepad (onKeyDown/onKeyUp).
 */
public class BbsActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "BBS";
    static final int PSP_SELECT = 0x1, PSP_START = 0x8, PSP_UP = 0x10, PSP_RIGHT = 0x20, PSP_DOWN = 0x40,
            PSP_LEFT = 0x80, PSP_L = 0x100, PSP_R = 0x200, PSP_TRIANGLE = 0x1000, PSP_CIRCLE = 0x2000,
            PSP_CROSS = 0x4000, PSP_SQUARE = 0x8000;
    private int touchButtons, keyButtons;

    static { System.loadLibrary("bbs"); }

    static native void nativeSetSurface(Object surface);
    static native boolean nativeStart(String filesDir, String eboot, String disc, long maxDispatches);
    static native void nativeSetButtons(int buttons);

    private boolean started;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        android.widget.FrameLayout root = new android.widget.FrameLayout(this);
        root.addView(view);
        root.addView(new TouchPad(this, b -> { touchButtons = b; nativeSetButtons(touchButtons | keyButtons); }));
        setContentView(root);
        hideSystemBars();
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
            out.write("[Rendering]\nBackend=Vulkan\nNativeGE=true\n".getBytes("UTF-8"));
        } catch (Exception e) {
            Log.e(TAG, "VCSNative.ini", e);
        }
        started = nativeStart(files.getAbsolutePath(), eboot.getAbsolutePath(), disc.getAbsolutePath(),
                4_000_000_000_000L);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSetSurface(null);
    }

    private static int keyToPsp(int code) {
        switch (code) {
            case android.view.KeyEvent.KEYCODE_DPAD_UP: return PSP_UP;
            case android.view.KeyEvent.KEYCODE_DPAD_DOWN: return PSP_DOWN;
            case android.view.KeyEvent.KEYCODE_DPAD_LEFT: return PSP_LEFT;
            case android.view.KeyEvent.KEYCODE_DPAD_RIGHT: return PSP_RIGHT;
            case android.view.KeyEvent.KEYCODE_BUTTON_A: return PSP_CROSS;
            case android.view.KeyEvent.KEYCODE_BUTTON_B: return PSP_CIRCLE;
            case android.view.KeyEvent.KEYCODE_BUTTON_X: return PSP_SQUARE;
            case android.view.KeyEvent.KEYCODE_BUTTON_Y: return PSP_TRIANGLE;
            case android.view.KeyEvent.KEYCODE_BUTTON_L1: return PSP_L;
            case android.view.KeyEvent.KEYCODE_BUTTON_R1: return PSP_R;
            case android.view.KeyEvent.KEYCODE_BUTTON_START: return PSP_START;
            case android.view.KeyEvent.KEYCODE_BUTTON_SELECT: return PSP_SELECT;
            default: return 0;
        }
    }

    @Override
    public boolean onKeyDown(int code, android.view.KeyEvent e) {
        int b = keyToPsp(code);
        if (b == 0) return super.onKeyDown(code, e);
        keyButtons |= b; nativeSetButtons(touchButtons | keyButtons); return true;
    }

    @Override
    public boolean onKeyUp(int code, android.view.KeyEvent e) {
        int b = keyToPsp(code);
        if (b == 0) return super.onKeyUp(code, e);
        keyButtons &= ~b; nativeSetButtons(touchButtons | keyButtons); return true;
    }
}
