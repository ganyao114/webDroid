package android.graphics;

public final class Outline {
    private final Rect bounds = new Rect();
    private float radius, alpha = 1;
    public void setEmpty() { bounds.setEmpty(); radius = 0; }
    public boolean isEmpty() { return bounds.isEmpty(); }
    public void setRect(int l, int t, int r, int b) { setRoundRect(l, t, r, b, 0); }
    public void setRoundRect(Rect rect, float radius) {
        setRoundRect(rect.left, rect.top, rect.right, rect.bottom, radius);
    }
    public void setRoundRect(int l, int t, int r, int b, float radius) {
        bounds.set(l, t, r, b);
        this.radius = radius;
    }
    public boolean getRect(Rect out) { out.set(bounds); return !isEmpty(); }
    public float getRadius() { return radius; }
    public void setAlpha(float alpha) { this.alpha = alpha; }
    public float getAlpha() { return alpha; }
}
