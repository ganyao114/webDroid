package android.view;

import android.os.Looper;
import android.os.SystemClock;
import java.util.ArrayList;

public final class Choreographer {
    public interface FrameCallback { void doFrame(long frameTimeNanos); }
    public static final int CALLBACK_INPUT = 0, CALLBACK_ANIMATION = 1, CALLBACK_INSETS_ANIMATION = 2;
    public static final int CALLBACK_TRAVERSAL = 3, CALLBACK_COMMIT = 4;
    private static Choreographer main;
    private final ArrayList<Callback> callbacks = new ArrayList<>();
    private long frameTime;
    private boolean inFrame;

    private static final class Callback {
        FrameCallback frame;
        Runnable runnable;
        Object token;
        long due;
        int type;
    }

    private Choreographer() {}
    public static Choreographer getInstance() {
        if (Looper.myLooper() == null) throw new IllegalStateException("Choreographer requires a Looper");
        if (main == null) main = new Choreographer();
        return main;
    }
    public void postFrameCallback(FrameCallback callback) { postFrameCallbackDelayed(callback, 0); }
    public void postFrameCallbackDelayed(FrameCallback callback, long delayMillis) {
        if (callback == null) throw new NullPointerException("callback");
        Callback record = new Callback();
        record.frame = callback;
        record.type = CALLBACK_ANIMATION;
        record.due = due(delayMillis);
        callbacks.add(record);
    }
    public void removeFrameCallback(FrameCallback callback) {
        if (callback == null) throw new NullPointerException("callback");
        for (int i = callbacks.size() - 1; i >= 0; --i) if (callbacks.get(i).frame == callback) callbacks.remove(i);
    }
    public void postCallback(int type, Runnable action, Object token) { postCallbackDelayed(type, action, token, 0); }
    public void postCallbackDelayed(int type, Runnable action, Object token, long delayMillis) {
        if (type < CALLBACK_INPUT || type > CALLBACK_COMMIT) throw new IllegalArgumentException("Callback type");
        if (action == null) throw new NullPointerException("action");
        Callback record = new Callback();
        record.runnable = action;
        record.token = token;
        record.type = type;
        record.due = due(delayMillis);
        callbacks.add(record);
    }
    public void removeCallbacks(int type, Runnable action, Object token) {
        for (int i = callbacks.size() - 1; i >= 0; --i) {
            Callback record = callbacks.get(i);
            if (record.frame == null && record.type == type && (action == null || record.runnable == action)
                    && (token == null || record.token == token)) callbacks.remove(i);
        }
    }
    public long getFrameTimeNanos() {
        if (!inFrame) throw new IllegalStateException("No frame callback is running");
        return frameTime;
    }
    public long getFrameTime() { return getFrameTimeNanos() / 1_000_000; }
    public long getFrameIntervalNanos() { return frameIntervalNanos(); }
    private static long due(long delay) {
        long now = SystemClock.uptimeMillis();
        return now + Math.min(Long.MAX_VALUE - now, Math.max(0, delay));
    }
    public void doFrame(long nanos) {
        frameTime = nanos;
        inFrame = true;
        try {
            for (int type = CALLBACK_INPUT; type <= CALLBACK_COMMIT; type++) dispatch(type, nanos / 1_000_000);
        } finally {
            inFrame = false;
        }
    }
    private void dispatch(int type, long uptime) {
        ArrayList<Callback> ready = new ArrayList<>();
        for (int i = 0; i < callbacks.size();) {
            Callback record = callbacks.get(i);
            if (record.type == type && record.due <= uptime) ready.add(callbacks.remove(i));
            else i++;
        }
        for (Callback record : ready) {
            if (record.frame != null) record.frame.doFrame(frameTime);
            else record.runnable.run();
        }
    }
    private static native long frameIntervalNanos();
}
