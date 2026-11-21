package android.animation;

import android.util.Property;

public class ObjectAnimator extends ValueAnimator {
    private Runnable readStart;

    public static <T> ObjectAnimator ofFloat(T target, Property<T, Float> property, float... values) {
        ObjectAnimator animation = new ObjectAnimator();
        float[] keyframes = values.clone();
        animation.readStart = () -> animation.setFloatValues(keyframes.length == 1
                ? new float[] {property.get(target), keyframes[0]} : keyframes);
        animation.addUpdateListener(animator -> property.set(target, (Float) animator.getAnimatedValue()));
        return animation;
    }

    public static <T> ObjectAnimator ofInt(T target, Property<T, Integer> property, int... values) {
        ObjectAnimator animation = new ObjectAnimator();
        int[] keyframes = values.clone();
        animation.readStart = () -> animation.setIntValues(keyframes.length == 1
                ? new int[] {property.get(target), keyframes[0]} : keyframes);
        animation.addUpdateListener(animator -> property.set(target, (Integer) animator.getAnimatedValue()));
        return animation;
    }

    @Override public void start() { if (readStart != null) readStart.run(); super.start(); }
    @Override public void end() { if (!isStarted() && readStart != null) readStart.run(); super.end(); }
    @Override public ObjectAnimator setDuration(long duration) { super.setDuration(duration); return this; }
}
