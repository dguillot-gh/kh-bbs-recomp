package com.psprecomp.bbs;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.MotionEvent;
import android.view.View;

/** Transparent on-screen PSP pad drawn over the game; multi-touch. 2026-11-16. */
final class TouchPad extends View {
    interface Listener { void onButtons(int buttons); }

    private final Listener listener;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private float u; // unit = screen height / 10

    TouchPad(Context c, Listener l) {
        super(c);
        listener = l;
        fill.setColor(0x40FFFFFF);
        text.setColor(0x90FFFFFF);
        text.setTextAlign(Paint.Align.CENTER);
    }

    // Centres in units from the left/right edges, set in onSizeChanged.
    private float dpadX, dpadY, faceX, faceY;

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        u = h / 10f;
        dpadX = 2.6f * u; dpadY = h - 3.0f * u;
        faceX = w - 2.6f * u; faceY = h - 3.0f * u;
        text.setTextSize(0.55f * u);
    }

    private int hit(float x, float y) {
        int w = getWidth();
        // L / R shoulders: top corners.
        if (y < 1.4f * u && x < 3.0f * u) return BbsActivity.PSP_L;
        if (y < 1.4f * u && x > w - 3.0f * u) return BbsActivity.PSP_R;
        // START / SELECT: bottom centre.
        if (y > getHeight() - 1.2f * u) {
            if (Math.abs(x - (w / 2f + 1.3f * u)) < 1.1f * u) return BbsActivity.PSP_START;
            if (Math.abs(x - (w / 2f - 1.3f * u)) < 1.1f * u) return BbsActivity.PSP_SELECT;
        }
        float dx = x - dpadX, dy = y - dpadY;
        if (dx * dx + dy * dy < 2.6f * u * 2.6f * u) {
            if (dx * dx + dy * dy < 0.35f * u * 0.35f * u) return 0;
            int b = 0;
            double a = Math.toDegrees(Math.atan2(-dy, dx)); // 0 = right, 90 = up
            if (a > -67.5 && a < 67.5) b |= BbsActivity.PSP_RIGHT;
            if (a > 22.5 && a < 157.5) b |= BbsActivity.PSP_UP;
            if (a > 112.5 || a < -112.5) b |= BbsActivity.PSP_LEFT;
            if (a < -22.5 && a > -157.5) b |= BbsActivity.PSP_DOWN;
            return b;
        }
        dx = x - faceX; dy = y - faceY;
        if (dx * dx + dy * dy < 2.6f * u * 2.6f * u) {
            if (Math.abs(dx) > Math.abs(dy)) return dx > 0 ? BbsActivity.PSP_CIRCLE : BbsActivity.PSP_SQUARE;
            return dy > 0 ? BbsActivity.PSP_CROSS : BbsActivity.PSP_TRIANGLE;
        }
        return 0;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int b = 0;
        int action = e.getActionMasked();
        for (int i = 0; i < e.getPointerCount(); i++) {
            boolean lifting = (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP)
                    && i == e.getActionIndex();
            if (action == MotionEvent.ACTION_CANCEL || lifting) continue;
            b |= hit(e.getX(i), e.getY(i));
        }
        listener.onButtons(b);
        return true;
    }

    private void circle(Canvas c, float x, float y, String label) {
        c.drawCircle(x, y, 0.75f * u, fill);
        c.drawText(label, x, y + 0.2f * u, text);
    }

    @Override
    protected void onDraw(Canvas c) {
        float o = 1.4f * u;
        circle(c, dpadX, dpadY - o, "▲"); circle(c, dpadX, dpadY + o, "▼");
        circle(c, dpadX - o, dpadY, "◀"); circle(c, dpadX + o, dpadY, "▶");
        circle(c, faceX, faceY - o, "△"); circle(c, faceX, faceY + o, "✕");
        circle(c, faceX - o, faceY, "□"); circle(c, faceX + o, faceY, "○");
        float w = getWidth(), h = getHeight();
        c.drawRoundRect(0.2f * u, 0.2f * u, 2.8f * u, 1.2f * u, 0.4f * u, 0.4f * u, fill);
        c.drawText("L", 1.5f * u, 0.9f * u, text);
        c.drawRoundRect(w - 2.8f * u, 0.2f * u, w - 0.2f * u, 1.2f * u, 0.4f * u, 0.4f * u, fill);
        c.drawText("R", w - 1.5f * u, 0.9f * u, text);
        c.drawRoundRect(w / 2f + 0.3f * u, h - 1.1f * u, w / 2f + 2.3f * u, h - 0.3f * u, 0.4f * u, 0.4f * u, fill);
        c.drawText("START", w / 2f + 1.3f * u, h - 0.5f * u, text);
        c.drawRoundRect(w / 2f - 2.3f * u, h - 1.1f * u, w / 2f - 0.3f * u, h - 0.3f * u, 0.4f * u, 0.4f * u, fill);
        c.drawText("SELECT", w / 2f - 1.3f * u, h - 0.5f * u, text);
    }
}
