package android.os;

public final class Message {
    public int what, arg1, arg2;
    public Object obj;
    Handler target;
    Runnable callback;
    long when;
    boolean enqueued, asynchronous;
    private Bundle data;

    public Message() {}
    public static Message obtain() { return new Message(); }
    public static Message obtain(Message original) {
        Message result = new Message();
        result.copyFrom(original);
        result.target = original.target;
        result.callback = original.callback;
        return result;
    }
    public static Message obtain(Handler handler) { Message m = obtain(); m.target = handler; return m; }
    public static Message obtain(Handler handler, Runnable task) { Message m = obtain(handler); m.callback = task; return m; }
    public static Message obtain(Handler handler, int what) { return obtain(handler, what, 0, 0, null); }
    public static Message obtain(Handler handler, int what, Object obj) { return obtain(handler, what, 0, 0, obj); }
    public static Message obtain(Handler handler, int what, int arg1, int arg2) {
        return obtain(handler, what, arg1, arg2, null);
    }
    public static Message obtain(Handler handler, int what, int arg1, int arg2, Object obj) {
        Message m = obtain(handler);
        m.what = what; m.arg1 = arg1; m.arg2 = arg2; m.obj = obj;
        return m;
    }
    public void copyFrom(Message other) {
        what = other.what; arg1 = other.arg1; arg2 = other.arg2; obj = other.obj;
        data = other.data == null ? null : new Bundle(other.data);
        asynchronous = other.asynchronous;
    }
    public Bundle getData() { if (data == null) data = new Bundle(); return data; }
    public Bundle peekData() { return data; }
    public void setData(Bundle data) { this.data = data; }
    public Handler getTarget() { return target; }
    public void setTarget(Handler handler) { target = handler; }
    public Runnable getCallback() { return callback; }
    public long getWhen() { return when; }
    public void setAsynchronous(boolean value) { asynchronous = value; }
    public boolean isAsynchronous() { return asynchronous; }
    public void sendToTarget() { target.sendMessage(this); }
    public void recycle() {
        if (enqueued) throw new IllegalStateException("Message is in use");
        clear();
    }
    void clear() {
        enqueued = false; target = null; callback = null;
        obj = null; data = null; what = arg1 = arg2 = 0; when = 0; asynchronous = false;
    }
}
