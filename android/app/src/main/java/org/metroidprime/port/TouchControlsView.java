package org.metroidprime.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.RelativeLayout;

import org.libsdl.app.SDLActivity;

import java.util.HashMap;
import java.util.Map;

final class TouchControlsView extends View {
    private static final int LEFT_STICK = 1;
    private static final int RIGHT_STICK = 2;
    private static final int BUTTON = 3;
    private static final int HIDE = 4;
    private static final int TOGGLE_DEBUG_OVERLAY = -1;

    private static final int[] LEFT_KEYS = {
        KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_D, KeyEvent.KEYCODE_W, KeyEvent.KEYCODE_S
    };
    private static final int[] RIGHT_KEYS = {
        KeyEvent.KEYCODE_J, KeyEvent.KEYCODE_L, KeyEvent.KEYCODE_I, KeyEvent.KEYCODE_K
    };

    private static final ControlButton[] BUTTONS = {
        new ControlButton("A", KeyEvent.KEYCODE_X, 0.92f, 0.70f, 0.070f),
        new ControlButton("B", KeyEvent.KEYCODE_Z, 0.84f, 0.82f, 0.060f),
        new ControlButton("X", KeyEvent.KEYCODE_C, 0.84f, 0.58f, 0.060f),
        new ControlButton("Y", KeyEvent.KEYCODE_V, 0.76f, 0.70f, 0.060f),
        new ControlButton("L", KeyEvent.KEYCODE_Q, 0.17f, 0.10f, 0.065f),
        new ControlButton("R", KeyEvent.KEYCODE_E, 0.76f, 0.10f, 0.065f),
        new ControlButton("Z", KeyEvent.KEYCODE_F, 0.86f, 0.16f, 0.052f),
        new ControlButton("START", KeyEvent.KEYCODE_ENTER, 0.50f, 0.11f, 0.050f),
        new ControlButton("MENU", TOGGLE_DEBUG_OVERLAY, 0.64f, 0.11f, 0.050f),
        new ControlButton("UP", KeyEvent.KEYCODE_DPAD_UP, 0.08f, 0.25f, 0.043f),
        new ControlButton("DOWN", KeyEvent.KEYCODE_DPAD_DOWN, 0.08f, 0.41f, 0.043f),
        new ControlButton("LEFT", KeyEvent.KEYCODE_DPAD_LEFT, 0.04f, 0.33f, 0.043f),
        new ControlButton("RIGHT", KeyEvent.KEYCODE_DPAD_RIGHT, 0.12f, 0.33f, 0.043f),
    };

    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Map<Integer, TouchTarget> targets = new HashMap<>();
    private final Map<Integer, Integer> heldKeys = new HashMap<>();
    private final RectF hideBounds = new RectF();
    private static native boolean nativeDebugOverlayVisible();
    private static native void nativeToggleDebugOverlay();
    private int leftPointer = -1;
    private int rightPointer = -1;
    private boolean hidden;

    TouchControlsView(Context context) {
        super(context);
        setClickable(true);
        setFocusable(false);
        fillPaint.setStyle(Paint.Style.FILL);
        strokePaint.setStyle(Paint.Style.STROKE);
        strokePaint.setStrokeWidth(dp(2));
        textPaint.setColor(Color.WHITE);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setFakeBoldText(true);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        // The debug overlay is drawn into the surface below; stay out of its way.
        if (nativeDebugOverlayVisible()) {
            return;
        }
        if (hidden) {
            drawPill(canvas, "SHOW", 0, 0, getWidth(), getHeight(), false);
            return;
        }

        float width = getWidth();
        float height = getHeight();
        float stickRadius = height * 0.16f;
        drawStick(canvas, "MOVE", width * 0.18f, height * 0.73f, stickRadius, leftPointer);
        drawStick(canvas, "LOOK", width * 0.58f, height * 0.73f, stickRadius, rightPointer);

        for (ControlButton button : BUTTONS) {
            drawButton(canvas, button, width, height);
        }

        hideBounds.set(width - dp(94), dp(10), width - dp(10), dp(50));
        drawPill(canvas, "HIDE", hideBounds.left, hideBounds.top,
                 hideBounds.right, hideBounds.bottom, false);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int actionIndex = event.getActionIndex();

        // While the debug overlay is open the game is paused and the touches
        // are for it, so decline them and let the SDL surface below have them.
        if (nativeDebugOverlayVisible()) {
            return false;
        }

        if (hidden) {
            if (action == MotionEvent.ACTION_UP) {
                setHidden(false);
                performClick();
            }
            return true;
        }

        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            int pointerId = event.getPointerId(actionIndex);
            float x = event.getX(actionIndex);
            float y = event.getY(actionIndex);
            if (hideBounds.contains(x, y)) {
                targets.put(pointerId, new TouchTarget(HIDE, 0));
                return true;
            }
            assignPointer(pointerId, x, y);
        } else if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < event.getPointerCount(); ++i) {
                TouchTarget target = targets.get(event.getPointerId(i));
                if (target != null &&
                    (target.type == LEFT_STICK || target.type == RIGHT_STICK)) {
                    updateStick(target, event.getX(i), event.getY(i));
                }
            }
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            releasePointer(event.getPointerId(actionIndex));
        } else if (action == MotionEvent.ACTION_CANCEL) {
            releaseAll();
        }
        invalidate();
        return true;
    }

    @Override
    public boolean performClick() {
        super.performClick();
        return true;
    }

    void releaseAll() {
        for (TouchTarget target : targets.values()) {
            releaseTarget(target);
        }
        targets.clear();
        heldKeys.clear();
        leftPointer = -1;
        rightPointer = -1;
        invalidate();
    }

    private void assignPointer(int pointerId, float x, float y) {
        float width = getWidth();
        float height = getHeight();
        for (ControlButton button : BUTTONS) {
            float radius = button.radius * height;
            float dx = x - button.x * width;
            float dy = y - button.y * height;
            if (dx * dx + dy * dy <= radius * radius) {
                TouchTarget target = new TouchTarget(BUTTON, button.keyCode);
                targets.put(pointerId, target);
                pressKey(button.keyCode);
                return;
            }
        }

        if (x < width * 0.38f && y > height * 0.43f && leftPointer == -1) {
            TouchTarget target = new TouchTarget(LEFT_STICK, 0);
            leftPointer = pointerId;
            targets.put(pointerId, target);
            updateStick(target, x, y);
        } else if (x >= width * 0.38f && x < width * 0.72f &&
                   y > height * 0.43f && rightPointer == -1) {
            TouchTarget target = new TouchTarget(RIGHT_STICK, 0);
            rightPointer = pointerId;
            targets.put(pointerId, target);
            updateStick(target, x, y);
        }
    }

    private void updateStick(TouchTarget target, float x, float y) {
        target.x = x;
        target.y = y;
        float centerX = getWidth() * (target.type == LEFT_STICK ? 0.18f : 0.58f);
        float centerY = getHeight() * 0.73f;
        float threshold = getHeight() * 0.045f;
        int[] keys = target.type == LEFT_STICK ? LEFT_KEYS : RIGHT_KEYS;
        setTargetKey(target, 0, keys[0], x < centerX - threshold);
        setTargetKey(target, 1, keys[1], x > centerX + threshold);
        setTargetKey(target, 2, keys[2], y < centerY - threshold);
        setTargetKey(target, 3, keys[3], y > centerY + threshold);
    }

    private void setTargetKey(TouchTarget target, int index, int keyCode, boolean down) {
        if (target.directions[index] == down) {
            return;
        }
        target.directions[index] = down;
        if (down) {
            pressKey(keyCode);
        } else {
            releaseKey(keyCode);
        }
    }

    private void releasePointer(int pointerId) {
        TouchTarget target = targets.remove(pointerId);
        if (target == null) {
            return;
        }
        if (target.type == HIDE) {
            setHidden(true);
            performClick();
            return;
        }
        releaseTarget(target);
        if (pointerId == leftPointer) {
            leftPointer = -1;
        }
        if (pointerId == rightPointer) {
            rightPointer = -1;
        }
    }

    private void releaseTarget(TouchTarget target) {
        if (target.type == HIDE) {
            return;
        }
        if (target.type == BUTTON) {
            releaseKey(target.keyCode);
            return;
        }
        int[] keys = target.type == LEFT_STICK ? LEFT_KEYS : RIGHT_KEYS;
        for (int i = 0; i < target.directions.length; ++i) {
            if (target.directions[i]) {
                target.directions[i] = false;
                releaseKey(keys[i]);
            }
        }
    }

    private void pressKey(int keyCode) {
        if (keyCode == TOGGLE_DEBUG_OVERLAY) {
            // Drive the overlay directly; polling a synthetic F1 key press is
            // unreliable and depends on SDL having keyboard focus.
            if (!heldKeys.containsKey(keyCode)) {
                heldKeys.put(keyCode, 1);
                nativeToggleDebugOverlay();
                // The toggle lands on the next game frame; redraw after it so
                // the controls get out of the overlay's way.
                postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        invalidate();
                    }
                }, 150);
            }
            return;
        }
        int count = heldKeys.containsKey(keyCode) ? heldKeys.get(keyCode) : 0;
        if (count == 0) {
            SDLActivity.onNativeKeyDown(keyCode);
        }
        heldKeys.put(keyCode, count + 1);
    }

    private void releaseKey(int keyCode) {
        Integer current = heldKeys.get(keyCode);
        if (current == null) {
            return;
        }
        if (keyCode == TOGGLE_DEBUG_OVERLAY) {
            heldKeys.remove(keyCode);
            return;
        }
        if (current <= 1) {
            heldKeys.remove(keyCode);
            SDLActivity.onNativeKeyUp(keyCode);
        } else {
            heldKeys.put(keyCode, current - 1);
        }
    }

    private void setHidden(boolean hide) {
        releaseAll();
        hidden = hide;
        RelativeLayout.LayoutParams params;
        if (hidden) {
            params = new RelativeLayout.LayoutParams(dp(88), dp(44));
            params.addRule(RelativeLayout.ALIGN_PARENT_TOP);
            params.addRule(RelativeLayout.ALIGN_PARENT_END);
            params.setMargins(0, dp(10), dp(10), 0);
        } else {
            params = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT);
        }
        setLayoutParams(params);
        invalidate();
    }

    private void drawStick(Canvas canvas, String label, float x, float y,
                           float radius, int pointerId) {
        boolean active = pointerId != -1;
        fillPaint.setColor(active ? 0x8848C8E8 : 0x66081218);
        strokePaint.setColor(active ? 0xFFE1F8FF : 0xAAFFFFFF);
        canvas.drawCircle(x, y, radius, fillPaint);
        canvas.drawCircle(x, y, radius, strokePaint);
        float knobX = x;
        float knobY = y;
        TouchTarget target = targets.get(pointerId);
        if (target != null) {
            float dx = target.x - x;
            float dy = target.y - y;
            float distance = (float) Math.hypot(dx, dy);
            float limit = radius * 0.58f;
            if (distance > limit) {
                dx *= limit / distance;
                dy *= limit / distance;
            }
            knobX += dx;
            knobY += dy;
        }
        canvas.drawCircle(knobX, knobY, radius * 0.42f, strokePaint);
        drawLabel(canvas, label, x, y + radius + dp(18), dp(13));
    }

    private void drawButton(Canvas canvas, ControlButton button, float width, float height) {
        float x = button.x * width;
        float y = button.y * height;
        float radius = button.radius * height;
        boolean active = heldKeys.containsKey(button.keyCode);
        fillPaint.setColor(active ? 0xCC48C8E8 : 0x77081218);
        strokePaint.setColor(active ? 0xFFE1F8FF : 0xBBFFFFFF);
        canvas.drawCircle(x, y, radius, fillPaint);
        canvas.drawCircle(x, y, radius, strokePaint);
        drawCenteredLabel(canvas, button.label, x, y,
                          button.label.length() > 2 ? dp(10) : dp(15));
    }

    private void drawPill(Canvas canvas, String label, float left, float top,
                          float right, float bottom, boolean active) {
        fillPaint.setColor(active ? 0xCC48C8E8 : 0x99081218);
        strokePaint.setColor(0xCCFFFFFF);
        RectF bounds = new RectF(left, top, right, bottom);
        float radius = Math.min(bounds.width(), bounds.height()) * 0.25f;
        canvas.drawRoundRect(bounds, radius, radius, fillPaint);
        canvas.drawRoundRect(bounds, radius, radius, strokePaint);
        drawCenteredLabel(canvas, label, bounds.centerX(), bounds.centerY(), dp(11));
    }

    private void drawLabel(Canvas canvas, String label, float x, float y, float size) {
        textPaint.setTextSize(size);
        canvas.drawText(label, x, y, textPaint);
    }

    private void drawCenteredLabel(Canvas canvas, String label, float x, float y, float size) {
        textPaint.setTextSize(size);
        canvas.drawText(label, x, y - (textPaint.ascent() + textPaint.descent()) / 2, textPaint);
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static final class ControlButton {
        final String label;
        final int keyCode;
        final float x;
        final float y;
        final float radius;

        ControlButton(String label, int keyCode, float x, float y, float radius) {
            this.label = label;
            this.keyCode = keyCode;
            this.x = x;
            this.y = y;
            this.radius = radius;
        }
    }

    private static final class TouchTarget {
        final int type;
        final int keyCode;
        final boolean[] directions = new boolean[4];
        float x;
        float y;

        TouchTarget(int type, int keyCode) {
            this.type = type;
            this.keyCode = keyCode;
        }
    }
}
