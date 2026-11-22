package android.graphics;

public final class BlurMaskFilter extends MaskFilter {
    public enum Blur { NORMAL, SOLID, OUTER, INNER }

    final float radius;
    final Blur style;

    public BlurMaskFilter(float radius, Blur style) {
        if (!(radius > 0) || Float.isInfinite(radius)) throw new IllegalArgumentException("radius");
        this.radius = radius;
        this.style = java.util.Objects.requireNonNull(style);
    }
}
