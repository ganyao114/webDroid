package java.util.concurrent.atomic;

public class AtomicReference<V> {
    private volatile V value;
    public AtomicReference() {}
    public AtomicReference(V initialValue) { value = initialValue; }
    public final V get() { return value; }
    public final void set(V value) { this.value = value; }
    public final void lazySet(V value) { set(value); }
    public final V getAndSet(V value) { V old = this.value; this.value = value; return old; }
    public final boolean compareAndSet(V expected, V updated) {
        if (value != expected) return false;
        value = updated;
        return true;
    }
    @Override public String toString() { return String.valueOf(value); }
}
