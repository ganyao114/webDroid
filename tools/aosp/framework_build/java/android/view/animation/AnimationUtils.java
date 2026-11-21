package android.view.animation;

import android.os.SystemClock;

public final class AnimationUtils {
    private AnimationUtils() {}
    public static long currentAnimationTimeMillis() { return SystemClock.uptimeMillis(); }
}
