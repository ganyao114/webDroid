package com.android.launcher3.fixture;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.util.AttributeSet;
import android.util.TypedValue;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.VelocityTracker;
import android.view.View;
import android.view.ViewConfiguration;
import android.widget.OverScroller;
import org.wasmandroid.uismoke.NativeGraphicsProbe;
import org.wasmandroid.uismoke.R;

public final class TileView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final OverScroller scroller;
    private final int slop, accent;
    private final Bitmap swatch = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888);
    private final Path crown = new Path();
    private VelocityTracker tracker;
    private float startX, pulse;
    private int startScroll, keyCount, motionEvents;
    private boolean dragging;
    private String composed = "", committed = "";

    public TileView(Context context, AttributeSet attrs) {
        super(context, attrs);
        scroller = new OverScroller(context);
        slop = ViewConfiguration.get(context).getScaledTouchSlop();
        TypedValue color = new TypedValue();
        accent = context.getTheme().resolveAttribute(R.attr.tileAccent, color, true) ? color.data : Color.RED;
        swatch.eraseColor(0xff2671c4);
        swatch.setPixel(1, 0, 0xffe94950);
        int[] colors = new int[4];
        swatch.getPixels(colors, 2, -2, 0, 0, 2, 2);
        if (swatch.getPixel(1, 0) != 0xffe94950 || colors[3] != 0xffe94950) {
            throw new IllegalStateException("ARGB pixel round-trip including negative stride");
        }
        crown.moveTo(42, 54);
        crown.quadTo(53, 39, 64, 54);
        crown.cubicTo(72, 55, 80, 35, 88, 53);
        crown.close();
    }

    public void setPulse(float value) { pulse = value; invalidate(); }
    public int swatchPixel() { return swatch.getPixel(1, 0); }
    public boolean isFlinging() { return !scroller.isFinished(); }
    public int keyCount() { return keyCount; }
    public int motionEvents() { return motionEvents; }
    public String composed() { return composed; }
    public String committed() { return committed; }

    @Override public void computeScroll() {
        if (scroller.computeScrollOffset()) {
            scrollTo(scroller.getCurrX(), scroller.getCurrY());
            postInvalidateOnAnimation();
        }
    }

    @Override protected void onDraw(Canvas canvas) {
        int save = canvas.save();
        try {
            canvas.clipRect(getScrollX(), 0, getScrollX() + getWidth(), getHeight());
            canvas.drawColor(0xff203656);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(accent);
            canvas.drawRoundRect(2, 10, 124, 100, 10, 10, paint);
            paint.setColor(0xff10223c);
            canvas.drawPath(crown, paint);
            canvas.drawCircle(104, 38, 6 + pulse * 5, paint);
            canvas.drawLine(18, 72, 104, 72, paint);
            canvas.drawBitmap(swatch, null, new RectF(18, 80, 37, 99), paint);
            paint.setTextSize(17);
            canvas.drawText("Apps", 18, 36, paint);
            paint.setColor(0xff83afed);
            canvas.drawRect(148, 10, 1400, 100, paint);
            paint.setColor(0xff10223c);
            for (int x = 168; x < 1400; x += 122) canvas.drawCircle(x, 48, 14, paint);
            NativeGraphicsProbe.drawRecordedShaders(canvas);
        } finally {
            canvas.restoreToCount(save);
        }
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        motionEvents++;
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                scroller.forceFinished(true);
                startX = event.getX();
                startScroll = getScrollX();
                dragging = false;
                tracker = VelocityTracker.obtain();
                tracker.addMovement(event);
                return super.onTouchEvent(event);
            case MotionEvent.ACTION_MOVE:
                if (tracker == null) return false;
                tracker.addMovement(event);
                float distance = startX - event.getX();
                if (Math.abs(distance) > slop) dragging = true;
                if (dragging) {
                    setPressed(false);
                    scrollTo(Math.max(0, Math.min(1050, startScroll + (int) distance)), 0);
                }
                return true;
            case MotionEvent.ACTION_UP:
                if (tracker != null) {
                    tracker.addMovement(event);
                    tracker.computeCurrentVelocity(1000, 8000);
                    if (dragging) {
                        scroller.fling(getScrollX(), 0, -(int) tracker.getXVelocity(), 0, 0, 1050, 0, 0);
                        postInvalidateOnAnimation();
                    }
                    tracker.recycle();
                    tracker = null;
                }
                return super.onTouchEvent(event);
            case MotionEvent.ACTION_CANCEL:
                if (tracker != null) tracker.recycle();
                tracker = null;
                return super.onTouchEvent(event);
            default: return true;
        }
    }

    @Override public boolean onKeyDown(int code, KeyEvent event) {
        if (code == KeyEvent.KEYCODE_DPAD_RIGHT) { keyCount++; return true; }
        return super.onKeyDown(code, event);
    }

    @Override public boolean onImeText(CharSequence text, boolean composing) {
        if (composing) composed = text.toString();
        else committed = text.toString();
        return true;
    }
}
