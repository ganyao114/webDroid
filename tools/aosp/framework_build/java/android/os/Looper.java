package android.os;

import java.util.ArrayList;
import org.wasmandroid.ui.ThreadContext;

/** Cooperative loopers driven by the embedded runtime's frame pump. */
public final class Looper {
    private static Looper main;
    private static Looper dispatching;
    private static final ArrayList<Looper> workers = new ArrayList<>();
    private final MessageQueue queue = new MessageQueue();
    private final Thread thread;

    private Looper(Thread thread) { this.thread = thread; }

    public static void prepareMainLooper() { prepare(); }
    public static void prepare() {
        if (main != null) throw new RuntimeException("Only one Looper can be created per VM");
        main = new Looper(Thread.currentThread());
    }
    static Looper createWorker(Thread thread) {
        Looper looper = new Looper(thread);
        workers.add(looper);
        return looper;
    }
    void prepareWorker(Runnable onPrepared) {
        Looper previous = dispatching;
        Thread previousThread = ThreadContext.swap(thread);
        dispatching = this;
        try {
            onPrepared.run();
        } finally {
            dispatching = previous;
            ThreadContext.swap(previousThread);
        }
    }
    public static Looper getMainLooper() { return main; }
    public static Looper myLooper() {
        Looper looper = dispatching != null ? dispatching : main;
        return looper != null && looper.thread == Thread.currentThread() ? looper : null;
    }
    public static MessageQueue myQueue() { return requireLooper().queue; }
    public Thread getThread() { return thread; }
    public MessageQueue getQueue() { return queue; }
    public boolean isCurrentThread() { return this == myLooper(); }
    public void quit() { queue.quit(false); }
    public void quitSafely() { queue.quit(true); }
    public static void loop() { requireLooper().dispatchDueMessages(SystemClock.uptimeMillis()); }

    public int dispatchDueMessages(long now) {
        Looper previous = dispatching;
        Thread previousThread = ThreadContext.swap(thread);
        dispatching = this;
        try {
            return queue.dispatch(now);
        } finally {
            dispatching = previous;
            ThreadContext.swap(previousThread);
        }
    }
    public static void dispatchAllDueMessages(long now) {
        int count = workers.size();
        for (int i = 0; i < count; i++) {
            Looper worker = workers.get(i);
            worker.dispatchDueMessages(now);
            if (worker.queue.isFinished()) {
                workers.remove(i--);
                count--;
            }
        }
        if (main != null) main.dispatchDueMessages(now);
    }
    boolean isFinished() { return queue.isFinished(); }

    private static Looper requireLooper() {
        Looper looper = myLooper();
        if (looper == null) throw new IllegalStateException("No Looper");
        return looper;
    }
}
