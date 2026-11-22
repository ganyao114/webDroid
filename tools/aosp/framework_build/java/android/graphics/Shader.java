package android.graphics;

import java.util.Objects;

public abstract class Shader {
    public enum TileMode { CLAMP, REPEAT, MIRROR, DECAL }

    final TileMode tileMode;
    final int type;
    final float[] geometry, positions;
    final int[] colors;

    Shader(int type, float[] geometry, int[] colors, float[] positions, TileMode tileMode) {
        if (colors.length < 2 || positions != null && positions.length != colors.length) {
            throw new IllegalArgumentException("Gradient needs two or more color stops");
        }
        this.type = type;
        this.geometry = geometry;
        this.colors = colors.clone();
        this.tileMode = Objects.requireNonNull(tileMode);
        if (positions == null) {
            this.positions = new float[colors.length];
            for (int i = 0; i < colors.length; i++) this.positions[i] = i / (float) (colors.length - 1);
        } else {
            this.positions = positions.clone();
            float previous = 0;
            for (float stop : this.positions) {
                if (!(stop >= previous && stop <= 1)) throw new IllegalArgumentException("Gradient stop order");
                previous = stop;
            }
        }
    }
}
