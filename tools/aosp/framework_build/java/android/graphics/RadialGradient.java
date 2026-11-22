package android.graphics;

public class RadialGradient extends Shader {
    public RadialGradient(float x, float y, float radius, int[] colors, float[] positions, TileMode tile) {
        super(2, new float[] {x, y, radius, 0}, colors, positions, tile);
        if (!(radius > 0)) throw new IllegalArgumentException("The radius must be positive");
    }

    public RadialGradient(float x, float y, float radius, int innerColor, int outerColor, TileMode tile) {
        this(x, y, radius, new int[] {innerColor, outerColor}, null, tile);
    }
}
