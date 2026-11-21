package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.Paint;

public class ColorDrawable extends Drawable {
    private final Paint paint = new Paint();
    private int color;

    public ColorDrawable() {}
    public ColorDrawable(int color) { this.color = color; }
    public void setColor(int color) { this.color = color; invalidateSelf(); }
    public int getColor() { return modulateAlpha(color); }
    @Override public void draw(Canvas canvas) {
        paint.setColor(getColor());
        canvas.drawRect(getBounds(), paint);
    }
    @Override public int getOpacity() {
        int alpha = getColor() >>> 24;
        return alpha == 0 ? -2 : alpha == 255 ? -1 : -3;
    }
    @Override public ConstantState getConstantState() {
        int baseColor = color;
        return new ConstantState() {
            @Override public Drawable newDrawable() { return new ColorDrawable(baseColor); }
            @Override public int getChangingConfigurations() { return 0; }
        };
    }
}
