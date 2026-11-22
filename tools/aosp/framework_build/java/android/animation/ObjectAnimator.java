package android.animation;

import android.util.Property;
import java.lang.reflect.Method;

public class ObjectAnimator extends ValueAnimator {
    private Runnable readStart;
    private Object target;
    private Method floatSetter;

    static ObjectAnimator forFloatKeyframes(String property, float[] fractions, float[] values,
                                            TimeInterpolator[] interpolators) {
        if (property == null || property.isEmpty()) throw new IllegalArgumentException("Missing property name");
        String setter = "set" + Character.toUpperCase(property.charAt(0)) + property.substring(1);
        ObjectAnimator animation = new ObjectAnimator();
        animation.setFloatKeyframes(fractions, values, interpolators);
        animation.addUpdateListener(value -> animation.applyFloat(setter, (Float) value.getAnimatedValue()));
        return animation;
    }

    @Override public void setTarget(Object target) {
        this.target = target;
        floatSetter = null;
    }

    private void applyFloat(String name, Float value) {
        if (target == null) return;
        if (floatSetter == null) {
            for (Class<?> type = target.getClass(); type != null; type = type.getSuperclass()) {
                try {
                    floatSetter = type.getDeclaredMethod(name, float.class);
                    floatSetter.setAccessible(true);
                    break;
                } catch (NoSuchMethodException absent) {
                    // Continue looking for an inherited setter.
                }
            }
            if (floatSetter == null) throw new IllegalArgumentException("Missing animated setter " + name);
        }
        try {
            floatSetter.invoke(target, value);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException("Cannot update animated property " + name, error);
        }
    }

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
