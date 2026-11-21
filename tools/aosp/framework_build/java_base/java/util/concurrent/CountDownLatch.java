package java.util.concurrent;

import java.util.Objects;

public final class CountDownLatch {
    private int count;
    public CountDownLatch(int count) {
        if (count < 0) throw new IllegalArgumentException("Negative count");
        this.count = count;
    }

    public void countDown() { if (count > 0) --count; }
    public long getCount() { return count; }
    public void await() throws InterruptedException {
        if (count != 0) throw new IllegalStateException("Waiting would block the only interpreter thread");
    }
    public boolean await(long timeout, TimeUnit unit) throws InterruptedException {
        Objects.requireNonNull(unit);
        return count == 0;
    }
}
