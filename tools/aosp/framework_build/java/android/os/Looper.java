package android.os;

/** A cooperative main looper driven by the embedded runtime's frame pump. */
public final class Looper {
    private static Looper main;
    private final MessageQueue queue = new MessageQueue();
    private Looper() {}

    public static void prepareMainLooper() { prepare(); }
    public static void prepare() {
        if (main != null) throw new RuntimeException("Only one Looper can be created per VM");
        main = new Looper();
    }
    public static Looper getMainLooper() { return main; }
    public static Looper myLooper() { return main; }
    public static MessageQueue myQueue() { return requireLooper().queue; }
    public MessageQueue getQueue() { return queue; }
    public boolean isCurrentThread() { return this == main; }
    public void quit() { queue.quit(false); }
    public void quitSafely() { queue.quit(true); }
    public static void loop() { requireLooper().dispatchDueMessages(SystemClock.uptimeMillis()); }

    public int dispatchDueMessages(long now) { return queue.dispatch(now); }

    private static Looper requireLooper() {
        if (main == null) throw new IllegalStateException("No main Looper");
        return main;
    }
}
