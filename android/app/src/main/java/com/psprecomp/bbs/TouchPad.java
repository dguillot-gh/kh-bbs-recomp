package com.psprecomp.bbs;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

import org.json.JSONObject;

/**
 * Transparent on-screen PSP pad drawn over the game; multi-touch.
 * 2026-11-16: first version. 2026-11-18: analog stick, camera drag zone (right half = right stick), and a user
 * layout: every control (and the FPS counter) can be moved, resized and hidden in edit mode; the layout and the
 * opacity are saved in SharedPreferences "bbs_pad_layout" / "layout_v1" (JSON).
 */
final class TouchPad extends View {
    interface Listener {
        void onButtons(int buttons);
        /** Left stick, -1..1, y down. */
        void onStick(float x, float y);
        /** Camera drag (right half, away from controls) as a right stick, -1..1. */
        void onCamera(float x, float y);
        void onMenu();
        void onEditDone();
    }

    // Control ids. FPS and MENU are not PSP buttons.
    // SHOTLOCK (2026-11-20): one button that holds L+R, BBS's Shotlock aim, so the other thumb is free to fire.
    static final int DPAD = 0, FACE = 1, STICK = 2, L = 3, R = 4, START = 5, SELECT = 6, FPS = 7, MENU = 8, CAMSTICK = 9,
            SHOTLOCK = 10, COUNT = 11;
    private static final String[] KEYS = {"dpad", "face", "stick", "l", "r", "start", "select", "fps", "menu", "camstick",
            "shotlock"};
    static final String[] NAMES = {"D-pad", "Face buttons", "Analog stick", "L", "R", "START", "SELECT",
            "FPS counter", "Menu button", "Camera stick", "Shotlock (L+R)"};
    // Default centres as fractions of width/height (landscape) and default scale.
    private static final float[][] DEFAULTS = {
            {0.105f, 0.33f, 1f}, {0.895f, 0.70f, 1f}, {0.135f, 0.73f, 1f}, {0.075f, 0.07f, 1f},
            {0.925f, 0.07f, 1f}, {0.565f, 0.93f, 1f}, {0.435f, 0.93f, 1f}, {0.30f, 0.06f, 1f},
            {0.50f, 0.06f, 1f}, {0.68f, 0.72f, 0.8f}, {0.895f, 0.40f, 1f}};

    private final float[] cx = new float[COUNT], cy = new float[COUNT], scale = new float[COUNT];
    private final boolean[] visible = new boolean[COUNT];
    private float opacity = 1f;

    private final Listener listener;
    private final SharedPreferences prefs;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG),
            knob = new Paint(Paint.ANTI_ALIAS_FLAG), outline = new Paint(Paint.ANTI_ALIAS_FLAG),
            fpsText = new Paint(Paint.ANTI_ALIAS_FLAG), fpsBack = new Paint(Paint.ANTI_ALIAS_FLAG),
            toolFill = new Paint(Paint.ANTI_ALIAS_FLAG), latchFill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private float u; // unit = screen height / 10

    private int stickPointer = -1;
    private float knobDx, knobDy;
    private int camPointer = -1;
    private float camX0, camY0;
    private int camStickPointer = -1;
    private float camKnobDx, camKnobDy;

    private String fpsLabel = "FPS --";

    // Shotlock button: a short tap latches L+R on (tap again, fire, or open the menu to release); held longer it
    // works as hold-to-aim and releases when lifted.
    private static final long SHOTLOCK_TAP_MS = 300;
    private static final int FACE_BITS = BbsActivity.PSP_CROSS | BbsActivity.PSP_CIRCLE | BbsActivity.PSP_SQUARE
            | BbsActivity.PSP_TRIANGLE;
    private int shotlockPointer = -1;
    private long shotlockDownTime;
    private boolean shotlockLatched, shotlockWasLatched;
    private int lastFace;

    /** Drops a latched Shotlock (the in-game menu opens). */
    void releaseShotlock() {
        if (!shotlockLatched) return;
        shotlockLatched = false;
        invalidate();
    }

    // Edit mode.
    private boolean editing;
    private int selected = -1, dragPointer = -1;
    private float dragDx, dragDy;
    private final RectF[] tools = new RectF[5];
    private static final String[] TOOL_LABELS = {"−", "+", "Hide/Show", "Reset", "Done"};

    TouchPad(Context c, Listener l) {
        super(c);
        listener = l;
        prefs = c.getSharedPreferences("bbs_pad_layout", Context.MODE_PRIVATE);
        text.setTextAlign(Paint.Align.CENTER);
        fpsText.setTextAlign(Paint.Align.CENTER);
        fpsText.setColor(0xFFF0F8FF);
        fpsText.setFakeBoldText(true);
        outline.setStyle(Paint.Style.STROKE);
        toolFill.setColor(0xC0202830);
        for (int i = 0; i < tools.length; i++) tools[i] = new RectF();
        resetLayout(false);
        load();
        applyOpacity();
    }

    // ---- layout persistence ----

    void resetLayout(boolean save) {
        for (int i = 0; i < COUNT; i++) {
            cx[i] = DEFAULTS[i][0]; cy[i] = DEFAULTS[i][1]; scale[i] = DEFAULTS[i][2]; visible[i] = true;
        }
        opacity = 1f;
        applyOpacity();
        if (save) save();
        invalidate();
    }

    private void load() {
        String json = prefs.getString("layout_v1", null);
        if (json == null) return;
        try {
            JSONObject o = new JSONObject(json);
            opacity = (float) o.optDouble("opacity", 1.0);
            JSONObject cs = o.optJSONObject("controls");
            if (cs == null) return;
            for (int i = 0; i < COUNT; i++) {
                JSONObject c = cs.optJSONObject(KEYS[i]);
                if (c == null) continue;
                cx[i] = clamp((float) c.optDouble("x", cx[i]), 0f, 1f);
                cy[i] = clamp((float) c.optDouble("y", cy[i]), 0f, 1f);
                scale[i] = clamp((float) c.optDouble("s", scale[i]), 0.5f, 2.5f);
                visible[i] = c.optBoolean("v", true);
            }
            visible[MENU] = true; // never lose the way back into the menu
        } catch (Exception ignored) {
            resetLayout(false);
        }
    }

    void save() {
        try {
            JSONObject o = new JSONObject();
            o.put("opacity", opacity);
            JSONObject cs = new JSONObject();
            for (int i = 0; i < COUNT; i++) {
                JSONObject c = new JSONObject();
                c.put("x", cx[i]); c.put("y", cy[i]); c.put("s", scale[i]); c.put("v", visible[i]);
                cs.put(KEYS[i], c);
            }
            o.put("controls", cs);
            prefs.edit().putString("layout_v1", o.toString()).apply();
        } catch (Exception ignored) {
        }
    }

    float getOpacity() { return opacity; }

    void setOpacity(float value) {
        opacity = clamp(value, 0.1f, 1f);
        applyOpacity();
        save();
        invalidate();
    }

    boolean isShown(int id) { return visible[id]; }

    void setShown(int id, boolean v) {
        visible[id] = v;
        save();
        invalidate();
    }

    /** Touch buttons hidden (gamepad users); FPS counter and menu button stay. */
    boolean controlsHidden() { return !visible[DPAD] && !visible[FACE] && !visible[STICK]; }

    void setControlsHidden(boolean hide) {
        for (int i : new int[] {DPAD, FACE, STICK, L, R, START, SELECT, CAMSTICK}) visible[i] = !hide;
        save();
        invalidate();
    }

    private void applyOpacity() {
        fill.setColor(alpha(0x40, 0xFFFFFF));
        knob.setColor(alpha(0x70, 0xFFFFFF));
        latchFill.setColor(alpha(0xA0, 0xFFC040));
        text.setColor(alpha(0x90, 0xFFFFFF));
        fpsBack.setColor(alpha(0x70, 0x000000));
    }

    private int alpha(int a, int rgb) {
        return (Math.round(a * opacity) << 24) | rgb;
    }

    private static float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    void setFps(float fps) {
        fpsLabel = fps > 0f ? String.format(java.util.Locale.US, "FPS %.1f", fps) : "FPS --";
        if (visible[FPS] || editing) invalidate();
    }

    void setEditing(boolean e) {
        editing = e;
        selected = -1;
        dragPointer = -1;
        releaseStick();
        releaseCamStick();
        if (camPointer >= 0) { camPointer = -1; listener.onCamera(0, 0); }
        listener.onButtons(0);
        if (!e) save();
        invalidate();
    }

    boolean isEditing() { return editing; }

    // ---- geometry ----

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        u = Math.min(w, h) / 10f;
        float tw = 1.9f * u, th = 0.9f * u, gap = 0.2f * u;
        float total = tools.length * tw + (tools.length - 1) * gap, x = (w - total) / 2f, y = h / 2f - th / 2f;
        for (RectF r : tools) { r.set(x, y, x + tw, y + th); x += tw + gap; }
    }

    private float px(int id) { return cx[id] * getWidth(); }
    private float py(int id) { return cy[id] * getHeight(); }

    /** Radius of the control's grab/hit circle (edit mode and layout). */
    private float reach(int id) {
        float s = scale[id] * u;
        switch (id) {
            case DPAD: return 1.7f * s;
            case FACE: return 2.3f * s;
            case STICK: case CAMSTICK: return 2.3f * s;
            case L: case R: case SHOTLOCK: return 1.3f * s;
            case START: case SELECT: return 1.0f * s;
            case FPS: return 1.4f * s;
            default: return 0.6f * s;
        }
    }

    private boolean inRect(int id, float x, float y, float halfW, float halfH) {
        float s = scale[id] * u;
        return Math.abs(x - px(id)) < halfW * s && Math.abs(y - py(id)) < halfH * s;
    }

    private boolean inStickZone(float x, float y) {
        if (!visible[STICK]) return false;
        float dx = x - px(STICK), dy = y - py(STICK), r = reach(STICK);
        return dx * dx + dy * dy < r * r;
    }

    private boolean inCamStickZone(float x, float y) {
        if (!visible[CAMSTICK]) return false;
        float dx = x - px(CAMSTICK), dy = y - py(CAMSTICK), r = reach(CAMSTICK);
        return dx * dx + dy * dy < r * r;
    }

    private void updateCamStick(float x, float y) {
        float r = 1.9f * scale[CAMSTICK] * u;
        float dx = x - px(CAMSTICK), dy = y - py(CAMSTICK);
        float len = (float) Math.sqrt(dx * dx + dy * dy);
        if (len > r) { dx = dx * r / len; dy = dy * r / len; }
        camKnobDx = dx; camKnobDy = dy;
        listener.onCamera(dx / r, dy / r);
    }

    private void releaseCamStick() {
        if (camStickPointer < 0 && camKnobDx == 0 && camKnobDy == 0) return;
        camStickPointer = -1;
        camKnobDx = camKnobDy = 0;
        listener.onCamera(0, 0);
    }

    private boolean onMenuButton(float x, float y) {
        if (!visible[MENU]) return false;
        float dx = x - px(MENU), dy = y - py(MENU), r = 0.7f * scale[MENU] * u;
        return dx * dx + dy * dy < r * r;
    }

    private int hit(float x, float y) {
        if (visible[L] && inRect(L, x, y, 1.3f, 0.6f)) return BbsActivity.PSP_L;
        if (visible[R] && inRect(R, x, y, 1.3f, 0.6f)) return BbsActivity.PSP_R;
        if (visible[SHOTLOCK] && inRect(SHOTLOCK, x, y, 1.3f, 0.6f)) return BbsActivity.PSP_L | BbsActivity.PSP_R;
        if (visible[START] && inRect(START, x, y, 1.1f, 0.5f)) return BbsActivity.PSP_START;
        if (visible[SELECT] && inRect(SELECT, x, y, 1.1f, 0.5f)) return BbsActivity.PSP_SELECT;
        if (visible[DPAD]) {
            float dx = x - px(DPAD), dy = y - py(DPAD), r = reach(DPAD), dead = 0.25f * scale[DPAD] * u;
            if (dx * dx + dy * dy < r * r) {
                if (dx * dx + dy * dy < dead * dead) return 0;
                int b = 0;
                double a = Math.toDegrees(Math.atan2(-dy, dx)); // 0 = right, 90 = up
                if (a > -67.5 && a < 67.5) b |= BbsActivity.PSP_RIGHT;
                if (a > 22.5 && a < 157.5) b |= BbsActivity.PSP_UP;
                if (a > 112.5 || a < -112.5) b |= BbsActivity.PSP_LEFT;
                if (a < -22.5 && a > -157.5) b |= BbsActivity.PSP_DOWN;
                return b;
            }
        }
        if (visible[FACE]) {
            float dx = x - px(FACE), dy = y - py(FACE), r = reach(FACE) + 0.3f * scale[FACE] * u;
            if (dx * dx + dy * dy < r * r) {
                if (Math.abs(dx) > Math.abs(dy)) return dx > 0 ? BbsActivity.PSP_CIRCLE : BbsActivity.PSP_SQUARE;
                return dy > 0 ? BbsActivity.PSP_CROSS : BbsActivity.PSP_TRIANGLE;
            }
        }
        return 0;
    }

    /** True if (x, y) is on any visible control (the camera drag zone excludes these). */
    private boolean onControl(float x, float y) {
        if (hit(x, y) != 0 || inStickZone(x, y) || inCamStickZone(x, y) || onMenuButton(x, y)) return true;
        if (visible[DPAD]) {
            float dx = x - px(DPAD), dy = y - py(DPAD), r = reach(DPAD);
            if (dx * dx + dy * dy < r * r) return true; // D-pad centre dead spot
        }
        return false;
    }

    private void updateStick(float x, float y) {
        float r = 1.9f * scale[STICK] * u;
        float dx = x - px(STICK), dy = y - py(STICK);
        float len = (float) Math.sqrt(dx * dx + dy * dy);
        if (len > r) { dx = dx * r / len; dy = dy * r / len; }
        knobDx = dx; knobDy = dy;
        listener.onStick(dx / r, dy / r);
    }

    private void releaseStick() {
        if (stickPointer < 0 && knobDx == 0 && knobDy == 0) return;
        stickPointer = -1;
        knobDx = knobDy = 0;
        listener.onStick(0, 0);
    }

    // ---- touch ----

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (editing) return onEditTouch(e);
        int action = e.getActionMasked();
        int ai = e.getActionIndex();
        if (action == MotionEvent.ACTION_CANCEL) {
            releaseStick();
            releaseCamStick();
            if (camPointer >= 0) { camPointer = -1; listener.onCamera(0, 0); }
            listener.onButtons(0);
            invalidate();
            return true;
        }
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            float x = e.getX(ai), y = e.getY(ai);
            if (onMenuButton(x, y)) {
                listener.onMenu();
                return true;
            }
            if (shotlockPointer < 0 && visible[SHOTLOCK] && inRect(SHOTLOCK, x, y, 1.3f, 0.6f)) {
                shotlockPointer = e.getPointerId(ai);
                shotlockDownTime = e.getEventTime();
                shotlockWasLatched = shotlockLatched;
            } else if (stickPointer < 0 && inStickZone(x, y)) {
                stickPointer = e.getPointerId(ai);
            } else if (camStickPointer < 0 && inCamStickZone(x, y)) {
                camStickPointer = e.getPointerId(ai);
            } else if (camPointer < 0 && x > getWidth() / 2f && !onControl(x, y)) {
                camPointer = e.getPointerId(ai);
                camX0 = x; camY0 = y;
            }
        }
        int b = 0;
        boolean stickSeen = false, camStickSeen = false;
        for (int i = 0; i < e.getPointerCount(); i++) {
            boolean lifting = (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) && i == ai;
            int id = e.getPointerId(i);
            if (id == shotlockPointer) {
                if (lifting) {
                    shotlockPointer = -1;
                    // Lifting always ends a latch that was on at touch-down; a short tap from off latches it.
                    shotlockLatched = !shotlockWasLatched && e.getEventTime() - shotlockDownTime < SHOTLOCK_TAP_MS;
                } else {
                    b |= BbsActivity.PSP_L | BbsActivity.PSP_R;
                }
                continue;
            }
            if (id == camPointer) {
                if (lifting) { camPointer = -1; listener.onCamera(0, 0); }
                else {
                    float cr = 1.5f * u;
                    listener.onCamera(clamp((e.getX(i) - camX0) / cr, -1f, 1f),
                            clamp((e.getY(i) - camY0) / cr, -1f, 1f));
                }
                continue;
            }
            if (id == camStickPointer) {
                if (lifting) releaseCamStick();
                else { updateCamStick(e.getX(i), e.getY(i)); camStickSeen = true; }
                continue;
            }
            if (id == stickPointer) {
                if (lifting) releaseStick();
                else { updateStick(e.getX(i), e.getY(i)); stickSeen = true; }
                continue;
            }
            if (lifting) continue;
            b |= hit(e.getX(i), e.getY(i));
        }
        if (!stickSeen && stickPointer >= 0) releaseStick();
        if (!camStickSeen && camStickPointer >= 0) releaseCamStick();
        // Latched Shotlock: firing (a face button pressed, then released) ends it, as the shot ends the aim.
        int face = b & FACE_BITS;
        if (shotlockLatched && lastFace != 0 && face == 0 && shotlockPointer < 0) shotlockLatched = false;
        lastFace = face;
        if (shotlockLatched) b |= BbsActivity.PSP_L | BbsActivity.PSP_R;
        listener.onButtons(b);
        invalidate();
        return true;
    }

    private boolean onEditTouch(MotionEvent e) {
        int action = e.getActionMasked();
        int ai = e.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            float x = e.getX(ai), y = e.getY(ai);
            for (int t = 0; t < tools.length; t++) {
                if (!tools[t].contains(x, y)) continue;
                onTool(t);
                return true;
            }
            int best = -1;
            float bestD = Float.MAX_VALUE;
            for (int i = 0; i < COUNT; i++) {
                float dx = x - px(i), dy = y - py(i), d = dx * dx + dy * dy, r = reach(i);
                if (d < r * r && d < bestD) { best = i; bestD = d; }
            }
            if (best >= 0 && dragPointer < 0) {
                selected = best;
                dragPointer = e.getPointerId(ai);
                dragDx = px(best) - x; dragDy = py(best) - y;
            }
            invalidate();
            return true;
        }
        if (action == MotionEvent.ACTION_MOVE && dragPointer >= 0 && selected >= 0) {
            int i = e.findPointerIndex(dragPointer);
            if (i >= 0) {
                cx[selected] = clamp((e.getX(i) + dragDx) / getWidth(), 0f, 1f);
                cy[selected] = clamp((e.getY(i) + dragDy) / getHeight(), 0f, 1f);
                invalidate();
            }
            return true;
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL
                || (action == MotionEvent.ACTION_POINTER_UP && e.getPointerId(ai) == dragPointer)) {
            dragPointer = -1;
        }
        return true;
    }

    private void onTool(int t) {
        switch (t) {
            case 0: if (selected >= 0) scale[selected] = clamp(scale[selected] - 0.1f, 0.5f, 2.5f); break;
            case 1: if (selected >= 0) scale[selected] = clamp(scale[selected] + 0.1f, 0.5f, 2.5f); break;
            case 2:
                if (selected >= 0 && selected != MENU) visible[selected] = !visible[selected];
                break;
            case 3: resetLayout(true); break;
            default:
                setEditing(false);
                listener.onEditDone();
                return;
        }
        invalidate();
    }

    // ---- drawing ----

    private void circle(Canvas c, float x, float y, float r, String label, float ts) {
        c.drawCircle(x, y, r, fill);
        text.setTextSize(ts);
        c.drawText(label, x, y + ts * 0.36f, text);
    }

    private void drawControl(Canvas c, int id) {
        float s = scale[id] * u, x = px(id), y = py(id);
        switch (id) {
            case DPAD: {
                float o = 1.0f * s, r = 0.55f * s, ts = 0.5f * s;
                circle(c, x, y - o, r, "▲", ts); circle(c, x, y + o, r, "▼", ts);
                circle(c, x - o, y, r, "◀", ts); circle(c, x + o, y, r, "▶", ts);
                break;
            }
            case FACE: {
                float o = 1.4f * s, r = 0.75f * s, ts = 0.55f * s;
                circle(c, x, y - o, r, "△", ts); circle(c, x, y + o, r, "✕", ts);
                circle(c, x - o, y, r, "□", ts); circle(c, x + o, y, r, "○", ts);
                break;
            }
            case STICK:
                c.drawCircle(x, y, 1.9f * s, fill);
                c.drawCircle(x + knobDx, y + knobDy, 0.8f * s, knob);
                break;
            case CAMSTICK:
                c.drawCircle(x, y, 1.9f * s, fill);
                c.drawCircle(x + camKnobDx, y + camKnobDy, 0.8f * s, knob);
                text.setTextSize(0.4f * s);
                c.drawText("CAM", x, y + 0.14f * s, text);
                break;
            case L: case R:
                c.drawRoundRect(x - 1.3f * s, y - 0.5f * s, x + 1.3f * s, y + 0.5f * s, 0.4f * s, 0.4f * s, fill);
                text.setTextSize(0.5f * s);
                c.drawText(id == L ? "L" : "R", x, y + 0.18f * s, text);
                break;
            case SHOTLOCK:
                c.drawRoundRect(x - 1.3f * s, y - 0.5f * s, x + 1.3f * s, y + 0.5f * s, 0.5f * s, 0.5f * s,
                        shotlockLatched ? latchFill : fill);   // gold while latched
                text.setTextSize(0.36f * s);
                c.drawText("SHOTLOCK", x, y + 0.13f * s, text);
                break;
            case START: case SELECT:
                c.drawRoundRect(x - 1.0f * s, y - 0.4f * s, x + 1.0f * s, y + 0.4f * s, 0.4f * s, 0.4f * s, fill);
                text.setTextSize(0.4f * s);
                c.drawText(id == START ? "START" : "SELECT", x, y + 0.14f * s, text);
                break;
            case FPS: {
                fpsText.setTextSize(0.5f * s);
                float w = fpsText.measureText("FPS 00.0") / 2f + 0.2f * s;
                c.drawRoundRect(x - w, y - 0.38f * s, x + w, y + 0.38f * s, 0.15f * s, 0.15f * s, fpsBack);
                c.drawText(fpsLabel, x, y + 0.18f * s, fpsText);
                break;
            }
            default:
                circle(c, x, y, 0.45f * s, "☰", 0.5f * s);
                break;
        }
    }

    @Override
    protected void onDraw(Canvas c) {
        for (int i = 0; i < COUNT; i++) {
            if (visible[i]) drawControl(c, i);
            else if (editing) {
                float o = opacity;
                opacity = 0.35f; applyOpacity();
                drawControl(c, i);
                opacity = o; applyOpacity();
            }
        }
        if (!editing) return;
        for (int i = 0; i < COUNT; i++) {
            outline.setStrokeWidth(i == selected ? 4f : 2f);
            outline.setColor(i == selected ? 0xFFFFD040 : 0x80FFFFFF);
            c.drawCircle(px(i), py(i), reach(i), outline);
        }
        int prev = text.getColor();
        text.setColor(0xFFFFFFFF);
        text.setTextSize(0.4f * u);
        String hint = selected >= 0 ? NAMES[selected] + (visible[selected] ? "" : " (hidden)")
                : "Drag a control to move it; tap one to select it";
        c.drawText(hint, getWidth() / 2f, tools[0].top - 0.4f * u, text);
        for (int t = 0; t < tools.length; t++) {
            c.drawRoundRect(tools[t], 0.2f * u, 0.2f * u, toolFill);
            c.drawText(TOOL_LABELS[t], tools[t].centerX(), tools[t].centerY() + 0.15f * u, text);
        }
        text.setColor(prev);
    }
}
