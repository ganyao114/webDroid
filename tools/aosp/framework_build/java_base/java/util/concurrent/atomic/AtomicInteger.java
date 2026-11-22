package java.util.concurrent.atomic;

import java.util.function.IntUnaryOperator;
import java.util.function.IntBinaryOperator;

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
    public final int updateAndGet(IntUnaryOperator update) { return update(update, true); }
    public final int getAndUpdate(IntUnaryOperator update) { return update(update, false); }
    public final int accumulateAndGet(int value, IntBinaryOperator operation) {
        return update(current -> operation.applyAsInt(current, value), true);
    }
    private int update(IntUnaryOperator operation, boolean returnUpdated) {
        int expected, updated;
        do {
            expected = get();
            updated = operation.applyAsInt(expected);
        } while (!compareAndSet(expected, updated));
        return returnUpdated ? updated : expected;
    }
    @Override public int intValue() { return value; }
    @Override public long longValue() { return value; }
    @Override public float floatValue() { return value; }
    @Override public double doubleValue() { return value; }
    @Override public String toString() { return Integer.toString(value); }
}
