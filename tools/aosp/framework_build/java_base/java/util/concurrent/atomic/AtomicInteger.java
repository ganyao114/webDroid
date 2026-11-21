package java.util.concurrent.atomic;

public class AtomicInteger extends Number {
    private volatile int value;
    public AtomicInteger() {}
    public AtomicInteger(int initialValue) { value = initialValue; }
    public final int get() { return value; }
    public final void set(int value) { this.value = value; }
    public final void lazySet(int value) { set(value); }
    public final int getAndSet(int value) { int old = this.value; this.value = value; return old; }
    public final boolean compareAndSet(int expected, int updated) {
        if (value != expected) return false;
        value = updated;
        return true;
    }
    public final int getAndIncrement() { return value++; }
    public final int getAndDecrement() { return value--; }
    public final int incrementAndGet() { return ++value; }
    public final int decrementAndGet() { return --value; }
    public final int getAndAdd(int delta) { int old = value; value += delta; return old; }
    public final int addAndGet(int delta) { return value += delta; }
    @Override public int intValue() { return value; }
    @Override public long longValue() { return value; }
    @Override public float floatValue() { return value; }
    @Override public double doubleValue() { return value; }
    @Override public String toString() { return Integer.toString(value); }
}
