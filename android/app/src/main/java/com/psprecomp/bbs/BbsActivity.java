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
 * Touch: any tap holds CROSS + CIRCLE (passes the auto-save notice's OK).
 */
public class BbsActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "BBS";
    static final int PSP_CIRCLE = 0x2000, PSP_CROSS = 0x4000;

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
        setContentView(view);
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

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                nativeSetButtons(PSP_CROSS | PSP_CIRCLE);
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                nativeSetButtons(0);
                return true;
        }
        return super.onTouchEvent(e);
    }
}
