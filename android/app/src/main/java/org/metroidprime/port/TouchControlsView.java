package org.metroidprime.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;
import android.widget.RelativeLayout;

import java.util.HashMap;
import java.util.Map;

/**
 * On-screen controller drawn over the game surface.
 *
 * It feeds an SDL virtual gamepad rather than synthesising keyboard keys, so the
 * game's own controller mapping, prompts and rebinding treat it as a real pad,
 * and its sticks are sticks rather than four keys pretending to be one.
 *
 * Two layouts: the GameCube pad's face layout while twin-stick is off, and the
 * Xbox arrangement while it is on, because that mode plays like a modern
 * shooter. Only arrangement and labels change; the game assigns the actions.
 */
final class TouchControlsView extends View {
    private static final int LEFT_STICK = 1;
    private static final int RIGHT_STICK = 2;
    private static final int BUTTON = 3;
    private static final int HIDE = 4;

    // Port-only actions, not game inputs.
    private static final int TOGGLE_DEBUG_OVERLAY = -1;
    private static final int HIDE_KEY = -2;
    // Axis-held controls are tracked with ids above this, to share one press map.
    private static final int AXIS_ID_BASE = 100;

    // SDL gamepad ids. The port maps these onto the GameCube pad itself: SOUTH is
    // A, EAST is B, WEST is X, NORTH is Y, Start is Start, the right shoulder is
    // Z, the D-pad is the D-pad, the left stick is the main stick, the right
    // stick is the C-stick and the triggers are L and R.
    private static final int BTN_SOUTH = 0;
    private static final int BTN_EAST = 1;
    private static final int BTN_WEST = 2;
    private static final int BTN_NORTH = 3;
    private static final int BTN_START = 6;
    private static final int BTN_LEFT_SHOULDER = 9;
    private static final int BTN_RIGHT_SHOULDER = 10;
    private static final int BTN_DPAD_UP = 11;
    private static final int BTN_DPAD_DOWN = 12;
    private static final int BTN_DPAD_LEFT = 13;
    private static final int BTN_DPAD_RIGHT = 14;

    private static final int AXIS_LEFTX = 0;
    private static final int AXIS_LEFTY = 1;
    private static final int AXIS_RIGHTX = 2;
    private static final int AXIS_RIGHTY = 3;
    private static final int AXIS_TRIGGER_L = 4;
    private static final int AXIS_TRIGGER_R = 5;

    // Sticks are drawn and read through these, so the two cannot disagree.
    private static final float STICK_LEFT_X = 0.18f;
    private static final float STICK_RIGHT_X = 0.58f;
    private static final float STICK_Y = 0.73f;
    private static final float STICK_RADIUS = 0.16f;
    private static final float STICK_DEAD_ZONE = 0.12f;

    // The GameCube pad's face layout, with its larger A, kept while twin-stick is
    // off so the overlay matches the pad the game was authored for.
    private static final ControlButton[] GAMECUBE_FACE = {
        new ControlButton("A", BTN_SOUTH, 0.925f, 0.700f, 0.070f),
        new ControlButton("B", BTN_EAST, 0.850f, 0.775f, 0.058f),
        new ControlButton("X", BTN_WEST, 0.850f, 0.625f, 0.058f),
        new ControlButton("Y", BTN_NORTH, 0.775f, 0.700f, 0.058f),
    };

    // Twin-stick reads like a modern shooter pad: the Xbox diamond, all four the
    // same size, because a bigger A is a GameCube trait rather than an Xbox one.
    private static final ControlButton[] XBOX_FACE = {
        new ControlButton("A", BTN_SOUTH, 0.875f, 0.795f, 0.055f),
        new ControlButton("B", BTN_EAST, 0.950f, 0.720f, 0.055f),
        new ControlButton("X", BTN_WEST, 0.800f, 0.720f, 0.055f),
        new ControlButton("Y", BTN_NORTH, 0.875f, 0.645f, 0.055f),
    };

    // A square cross: equal spacing both ways, equal sizes.
    private static final ControlButton[] DPAD = {
        new ControlButton("\u25B2", BTN_DPAD_UP, 0.115f, 0.245f, 0.042f),
        new ControlButton("\u25BC", BTN_DPAD_DOWN, 0.115f, 0.395f, 0.042f),
        new ControlButton("\u25C0", BTN_DPAD_LEFT, 0.040f, 0.320f, 0.042f),
        new ControlButton("\u25B6", BTN_DPAD_RIGHT, 0.190f, 0.320f, 0.042f),
    };

    // Shoulders stack vertically: the trigger above the bumper, both sides.
    // L and R are the pad's analog triggers; Z is its digital shoulder. In Xbox
    // mode they read LT/RT and LB/RB, and RB carries Z while LB carries the
    // twin-stick beam modifier, which is what the port reads the left shoulder
    // for. X-Box mode has no Z label because the pad has no Z button.
    private static final PillButton[] GAMECUBE_PILLS = {
        new PillButton("L", AXIS_TRIGGER_L, -1, 0.020f, 0.030f, 0.150f, 0.100f),
        new PillButton("Z", -1, BTN_RIGHT_SHOULDER, 0.020f, 0.115f, 0.150f, 0.185f),
        new PillButton("R", AXIS_TRIGGER_R, -1, 0.850f, 0.030f, 0.980f, 0.100f),
        new PillButton("START", -1, BTN_START, 0.400f, 0.030f, 0.490f, 0.100f),
        new PillButton("MENU", TOGGLE_DEBUG_OVERLAY, -1, 0.510f, 0.030f, 0.600f, 0.100f),
        new PillButton("HIDE", HIDE_KEY, -1, 0.620f, 0.030f, 0.710f, 0.100f),
    };

    private static final PillButton[] XBOX_PILLS = {
        new PillButton("LT", AXIS_TRIGGER_L, -1, 0.020f, 0.030f, 0.150f, 0.100f),
        new PillButton("LB", -1, BTN_LEFT_SHOULDER, 0.020f, 0.115f, 0.150f, 0.185f),
        new PillButton("RT", AXIS_TRIGGER_R, -1, 0.850f, 0.030f, 0.980f, 0.100f),
        new PillButton("RB", -1, BTN_RIGHT_SHOULDER, 0.850f, 0.115f, 0.980f, 0.185f),
        new PillButton("START", -1, BTN_START, 0.400f, 0.030f, 0.490f, 0.100f),
        new PillButton("MENU", TOGGLE_DEBUG_OVERLAY, -1, 0.510f, 0.030f, 0.600f, 0.100f),
        new PillButton("HIDE", HIDE_KEY, -1, 0.620f, 0.030f, 0.710f, 0.100f),
    };

    private ControlButton[] face = GAMECUBE_FACE;
    private PillButton[] pills = GAMECUBE_PILLS;
    private boolean twinStickMode;

    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Map<Integer, TouchTarget> targets = new HashMap<>();
    private final Map<Integer, Integer> held = new HashMap<>();
    private final RectF hideBounds = new RectF();
    private static native boolean nativeDebugOverlayVisible();
    private static native boolean nativeTwinStick();
    private static native void nativeSetTouchDevice(boolean xboxLayout);
    private static native void nativeToggleDebugOverlay();
    private static native void nativeVirtualButton(int button, boolean down);
    private static native void nativeVirtualAxis(int axis, float value);
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
        // The layout follows the twin-stick setting, which the player can change
        // mid-session, so re-check it every draw.
        if (nativeTwinStick() != twinStickMode) {
            twinStickMode = !twinStickMode;
            face = twinStickMode ? XBOX_FACE : GAMECUBE_FACE;
            pills = twinStickMode ? XBOX_PILLS : GAMECUBE_PILLS;
        }
        for (PillButton pill : pills) {
            if (pill.id() == HIDE_KEY) {
                hideBounds.set(pill.left * width, pill.top * height,
                               pill.right * width, pill.bottom * height);
            }
        }
        drawStick(canvas, width * STICK_LEFT_X, height * STICK_Y, height * STICK_RADIUS,
                  leftPointer);
        drawStick(canvas, width * STICK_RIGHT_X, height * STICK_Y, height * STICK_RADIUS,
                  rightPointer);

        for (PillButton pill : pills) {
            drawPillButton(canvas, pill, width, height);
        }
        for (ControlButton button : face) {
            drawButton(canvas, button, width, height);
        }
        for (ControlButton button : DPAD) {
            drawButton(canvas, button, width, height);
        }
    }

    // A mouse is not a finger on the overlay. Its clicks are dispatched as
    // touches, and hover goes to the topmost hoverable view (this one, being
    // clickable), so without these the SDL surface below saw neither and a
    // click landed on whatever on-screen button was under the cursor. Declining
    // passes them down to the surface. Once SDL captures the pointer for mouse
    // aim, events go to the focused surface and never reach here.
    private static boolean fromMouse(MotionEvent event) {
        return event.getToolType(event.getActionIndex()) == MotionEvent.TOOL_TYPE_MOUSE;
    }

    @Override
    public boolean onHoverEvent(MotionEvent event) {
        return !fromMouse(event) && super.onHoverEvent(event);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (fromMouse(event)) {
            return false;
        }
        int action = event.getActionMasked();
        int actionIndex = event.getActionIndex();

        // While the debug overlay is open the game is paused and the touches
        // are for it, so decline them and let the SDL surface below have them.
        // Anything still held has to go first: once this view stops claiming
        // touches the matching releases never arrive, which left whatever was
        // down (a trigger, say) held for the rest of the session.
        if (nativeDebugOverlayVisible()) {
            if (!targets.isEmpty() || !held.isEmpty()) {
                releaseAll();
            }
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
            // The in-game prompts follow the input the player reached for, so
            // tell the port which set this overlay is showing.
            nativeSetTouchDevice(twinStickMode);
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
        held.clear();
        leftPointer = -1;
        rightPointer = -1;
        invalidate();
    }

    private void assignPointer(int pointerId, float x, float y) {
        float width = getWidth();
        float height = getHeight();
        for (PillButton pill : pills) {
            if (x >= pill.left * width && x <= pill.right * width &&
                y >= pill.top * height && y <= pill.bottom * height) {
                if (pill.id() == HIDE_KEY) {
                    targets.put(pointerId, new TouchTarget(HIDE, 0));
                    return;
                }
                targets.put(pointerId, new TouchTarget(BUTTON, pill.id()));
                pressControl(pill.id());
                return;
            }
        }
        for (ControlButton button : face) {
            if (hitCircle(button, x, y, width, height)) {
                targets.put(pointerId, new TouchTarget(BUTTON, button.button));
                pressControl(button.button);
                return;
            }
        }
        for (ControlButton button : DPAD) {
            if (hitCircle(button, x, y, width, height)) {
                targets.put(pointerId, new TouchTarget(BUTTON, button.button));
                pressControl(button.button);
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
        final boolean left = target.type == LEFT_STICK;
        float centreX = getWidth() * (left ? STICK_LEFT_X : STICK_RIGHT_X);
        float centreY = getHeight() * STICK_Y;
        float radius = getHeight() * STICK_RADIUS;
        // SDL's gamepad axes are +X right and +Y *down* (Aurora inverts Y for the
        // GameCube stick, whose +Y is up), so screen coordinates apply as-is.
        float dx = (x - centreX) / radius;
        float dy = (y - centreY) / radius;
        float length = (float) Math.hypot(dx, dy);
        if (length > 1f) {
            dx /= length;
            dy /= length;
            length = 1f;
        }
        if (length < STICK_DEAD_ZONE) {
            dx = 0f;
            dy = 0f;
        }
        nativeVirtualAxis(left ? AXIS_LEFTX : AXIS_RIGHTX, dx);
        nativeVirtualAxis(left ? AXIS_LEFTY : AXIS_RIGHTY, dy);
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
            releaseControl(target.id);
            return;
        }
        final boolean left = target.type == LEFT_STICK;
        nativeVirtualAxis(left ? AXIS_LEFTX : AXIS_RIGHTX, 0f);
        nativeVirtualAxis(left ? AXIS_LEFTY : AXIS_RIGHTY, 0f);
    }

    private void pressControl(int id) {
        if (id == TOGGLE_DEBUG_OVERLAY) {
            // Drive the overlay directly; polling a synthetic F1 key press is
            // unreliable and depends on SDL having keyboard focus.
            if (!held.containsKey(id)) {
                held.put(id, 1);
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
        int count = held.containsKey(id) ? held.get(id) : 0;
        if (count == 0) {
            if (id >= AXIS_ID_BASE) {
                nativeVirtualAxis(id - AXIS_ID_BASE, 1f);
            } else {
                nativeVirtualButton(id, true);
            }
        }
        held.put(id, count + 1);
    }

    private void releaseControl(int id) {
        Integer current = held.get(id);
        if (current == null) {
            return;
        }
        if (id == TOGGLE_DEBUG_OVERLAY) {
            held.remove(id);
            return;
        }
        if (current <= 1) {
            held.remove(id);
            if (id >= AXIS_ID_BASE) {
                // Triggers are axes, and SDL's joystick axes run -32768..32767
                // with a trigger resting at the minimum: releasing with 0 left
                // the trigger half pressed, so the game stayed locked on (and
                // strafing) after the player let go.
                nativeVirtualAxis(id - AXIS_ID_BASE, -1f);
            } else {
                nativeVirtualButton(id, false);
            }
        } else {
            held.put(id, current - 1);
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

    private void drawStick(Canvas canvas, float x, float y, float radius, int pointerId) {
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
    }

    private boolean hitCircle(ControlButton button, float x, float y, float width, float height) {
        float radius = button.radius * height;
        float dx = x - button.x * width;
        float dy = y - button.y * height;
        return dx * dx + dy * dy <= radius * radius;
    }

    private void drawPillButton(Canvas canvas, PillButton pill, float width, float height) {
        float left = pill.left * width;
        float top = pill.top * height;
        float right = pill.right * width;
        float bottom = pill.bottom * height;
        boolean active = held.containsKey(pill.id());
        fillPaint.setColor(active ? 0xCC48C8E8 : 0x99081218);
        strokePaint.setColor(active ? 0xFFE1F8FF : 0xCCFFFFFF);
        RectF bounds = new RectF(left, top, right, bottom);
        float radius = Math.min(bounds.width(), bounds.height()) * 0.28f;
        canvas.drawRoundRect(bounds, radius, radius, fillPaint);
        canvas.drawRoundRect(bounds, radius, radius, strokePaint);
        drawCenteredLabel(canvas, pill.label, bounds.centerX(), bounds.centerY(),
                          pill.label.length() > 2 ? dp(11) : dp(13));
    }

    private void drawButton(Canvas canvas, ControlButton button, float width, float height) {
        float x = button.x * width;
        float y = button.y * height;
        float radius = button.radius * height;
        boolean active = held.containsKey(button.button);
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

    private void drawCenteredLabel(Canvas canvas, String label, float x, float y, float size) {
        textPaint.setTextSize(size);
        canvas.drawText(label, x, y - (textPaint.ascent() + textPaint.descent()) / 2, textPaint);
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static final class ControlButton {
        final String label;
        final int button;
        final float x;
        final float y;
        final float radius;

        ControlButton(String label, int button, float x, float y, float radius) {
            this.label = label;
            this.button = button;
            this.x = x;
            this.y = y;
            this.radius = radius;
        }
    }

    private static final class PillButton {
        final String label;
        final int axis;
        final int button;
        final float left;
        final float top;
        final float right;
        final float bottom;

        PillButton(String label, int axis, int button, float left, float top, float right,
                   float bottom) {
            this.label = label;
            this.axis = axis;
            this.button = button;
            this.left = left;
            this.top = top;
            this.right = right;
            this.bottom = bottom;
        }

        // One identity for the press map and the held-highlight, whether this is
        // a button or an analog trigger sent as an axis.
        int id() {
            return axis >= 0 ? AXIS_ID_BASE + axis : button;
        }
    }

    private static final class TouchTarget {
        final int type;
        final int id;
        float x;
        float y;

        TouchTarget(int type, int id) {
            this.type = type;
            this.id = id;
        }
    }
}
