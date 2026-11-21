package android.os;

public class Handler {
    public interface Callback { boolean handleMessage(Message message); }
    private final Looper looper;
    private final MessageQueue queue;
    private final Callback callback;
    private final boolean asynchronous;

    public Handler() { this(Looper.myLooper()); }
    public Handler(Callback callback) { this(Looper.myLooper(), callback); }
    public Handler(Looper looper) { this(looper, null); }
    public Handler(Looper looper, Callback callback) { this(looper, callback, false); }
    private Handler(Looper looper, Callback callback, boolean asynchronous) {
        if (looper == null) throw new IllegalStateException("No Looper for this Handler");
        this.looper = looper; queue = looper.getQueue(); this.callback = callback; this.asynchronous = asynchronous;
    }
    public static Handler createAsync(Looper looper) { return createAsync(looper, null); }
    public static Handler createAsync(Looper looper, Callback callback) { return new Handler(looper, callback, true); }
    public final Looper getLooper() { return looper; }
    public void handleMessage(Message message) {}
    public void dispatchMessage(Message message) {
        if (message.callback != null) message.callback.run();
        else if (callback == null || !callback.handleMessage(message)) handleMessage(message);
    }
    public final Message obtainMessage() { return Message.obtain(this); }
    public final Message obtainMessage(int what) { return Message.obtain(this, what); }
    public final Message obtainMessage(int what, Object obj) { return Message.obtain(this, what, obj); }
    public final Message obtainMessage(int what, int arg1, int arg2) { return Message.obtain(this, what, arg1, arg2); }
    public final Message obtainMessage(int what, int arg1, int arg2, Object obj) {
        return Message.obtain(this, what, arg1, arg2, obj);
    }
    public final boolean post(Runnable task) { return postDelayed(task, 0); }
    public final boolean postDelayed(Runnable task, long delay) { return postDelayed(task, null, delay); }
    public final boolean postDelayed(Runnable task, Object token, long delay) {
        return sendMessageDelayed(runnableMessage(task, token), delay);
    }
    public final boolean postAtTime(Runnable task, long time) { return postAtTime(task, null, time); }
    public final boolean postAtTime(Runnable task, Object token, long time) {
        return sendMessageAtTime(runnableMessage(task, token), time);
    }
    public final boolean postAtFrontOfQueue(Runnable task) { return sendMessageAtFrontOfQueue(runnableMessage(task, null)); }
    private Message runnableMessage(Runnable task, Object token) {
        if (task == null) throw new NullPointerException("Runnable");
        Message message = Message.obtain(this, task);
        message.obj = token;
        return message;
    }
    public final boolean sendMessage(Message message) { return sendMessageDelayed(message, 0); }
    public final boolean sendEmptyMessage(int what) { return sendMessage(obtainMessage(what)); }
    public final boolean sendEmptyMessageDelayed(int what, long delay) { return sendMessageDelayed(obtainMessage(what), delay); }
    public final boolean sendEmptyMessageAtTime(int what, long time) { return sendMessageAtTime(obtainMessage(what), time); }
    public final boolean sendMessageDelayed(Message message, long delay) {
        long now = SystemClock.uptimeMillis();
        return sendMessageAtTime(message, Math.min(Long.MAX_VALUE - now, Math.max(0, delay)) + now);
    }
    public boolean sendMessageAtTime(Message message, long time) { return enqueue(message, time, false); }
    public final boolean sendMessageAtFrontOfQueue(Message message) { return enqueue(message, 0, true); }
    private boolean enqueue(Message message, long time, boolean front) {
        if (message.enqueued) throw new IllegalStateException("Message is already enqueued");
        message.target = this;
        if (asynchronous) message.asynchronous = true;
        return queue.enqueue(message, time, front);
    }
    public final void removeCallbacks(Runnable task) { removeCallbacks(task, null); }
    public final void removeCallbacks(Runnable task, Object token) { queue.filter(this, task, token, 0, 0, true); }
    public final void removeMessages(int what) { removeMessages(what, null); }
    public final void removeMessages(int what, Object token) { queue.filter(this, null, token, what, 1, true); }
    public final void removeCallbacksAndMessages(Object token) { queue.filter(this, null, token, 0, 2, true); }
    public final boolean hasCallbacks(Runnable task) { return queue.filter(this, task, null, 0, 0, false); }
    public final boolean hasMessages(int what) { return hasMessages(what, null); }
    public final boolean hasMessages(int what, Object token) { return queue.filter(this, null, token, what, 1, false); }
}
