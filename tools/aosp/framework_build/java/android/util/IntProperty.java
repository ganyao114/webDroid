package android.util;

public abstract class IntProperty<T> extends Property<T, Integer> {
    public IntProperty(String name) { super(Integer.class, name); }
    public abstract void setValue(T target, int value);
    @Override public final void set(T target, Integer value) { setValue(target, value); }
}
