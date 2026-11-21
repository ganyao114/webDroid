package java.util.concurrent.atomic;

public class AtomicBoolean {
    private volatile boolean value;
    public AtomicBoolean() {}
    public AtomicBoolean(boolean initialValue) { value = initialValue; }
    public final boolean get() { return value; }
    public final void set(boolean value) { this.value = value; }
    public final void lazySet(boolean value) { set(value); }
    public final boolean getAndSet(boolean value) { boolean old = this.value; this.value = value; return old; }
    public final boolean compareAndSet(boolean expected, boolean updated) {
        if (value != expected) return false;
        value = updated;
        return true;
    }
    @Override public String toString() { return Boolean.toString(value); }
}
