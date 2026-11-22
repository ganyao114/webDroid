package android.os;

/** Haptic capability API for hosts without a guest vibrator service. */
public abstract class Vibrator {
    public abstract boolean hasVibrator();
    public abstract boolean hasAmplitudeControl();
    public abstract void cancel();

    public boolean isVibrating() { return false; }

    public boolean[] arePrimitivesSupported(int... primitiveIds) {
        return new boolean[primitiveIds.length];
    }

    public boolean areAllPrimitivesSupported(int... primitiveIds) {
        for (boolean supported : arePrimitivesSupported(primitiveIds)) {
            if (!supported) return false;
        }
        return true;
    }
}
