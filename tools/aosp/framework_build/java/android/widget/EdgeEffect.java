package android.widget;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.SystemClock;

public class EdgeEffect {
    private int width, height, color = 0x805264af;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private float distance, displacement = 0.5f;
    private long released = -1;

    public EdgeEffect(Context context) {}
    public void setColor(int color) { this.color = color; }
    public int getColor() { return color; }
    public void setSize(int width, int height) { this.width = width; this.height = height; }
    public void finish() { distance = 0; released = -1; }
    public boolean isFinished() { return getDistance() == 0; }
    public float getDistance() {
        if (released < 0) return distance;
        float remaining = 1 - (SystemClock.uptimeMillis() - released) / 240f;
        if (remaining <= 0) { finish(); return 0; }
        return distance * remaining * remaining;
    }
    public void onPull(float delta) { onPull(delta, 0.5f); }
    public void onPull(float delta, float displacement) { onPullDistance(delta, displacement); }
    public float onPullDistance(float delta, float displacement) {
        float before = getDistance();
        distance = Math.max(0, Math.min(1, before + delta));
        this.displacement = Math.max(0, Math.min(1, displacement));
        released = -1;
        return distance - before;
    }
    public void onRelease() {
        if (distance != 0 && released < 0) released = SystemClock.uptimeMillis();
    }
    public void onAbsorb(int velocity) {
        distance = Math.min(1f, 0.08f + Math.abs(velocity) / 9000f);
        released = SystemClock.uptimeMillis();
    }
    public boolean draw(Canvas canvas) {
        float amount = getDistance();
        if (amount == 0) return false;
        paint.setColor((color & 0xffffff) | (int) ((color >>> 24) * amount) << 24);
        float radius = Math.min(width * 0.6f, height * 0.12f) * amount;
        canvas.drawOval(width * displacement - width * 0.5f, -radius,
                width * displacement + width * 0.5f, radius, paint);
        return true;
    }
}
