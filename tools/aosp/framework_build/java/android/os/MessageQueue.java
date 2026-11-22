package android.os;

import java.util.ArrayList;

public final class MessageQueue {
    public interface IdleHandler { boolean queueIdle(); }
    private final ArrayList<Message> messages = new ArrayList<>();
    private final ArrayList<IdleHandler> idleHandlers = new ArrayList<>();
    private boolean quitting;

    MessageQueue() {}
    boolean isFinished() { return quitting && messages.isEmpty(); }
    public boolean isIdle() { return messages.isEmpty() || messages.get(0).when > SystemClock.uptimeMillis(); }
    public void addIdleHandler(IdleHandler handler) { if (handler == null) throw new NullPointerException(); idleHandlers.add(handler); }
    public void removeIdleHandler(IdleHandler handler) { idleHandlers.remove(handler); }

    boolean enqueue(Message message, long time, boolean front) {
        if (message.enqueued) throw new IllegalStateException("Message is already enqueued");
        if (quitting) return false;
        message.when = time;
        message.enqueued = true;
        int index = 0;
        if (!front) while (index < messages.size() && messages.get(index).when <= time) index++;
        messages.add(index, message);
        return true;
    }

    int dispatch(long time) {
        int count = 0;
        // Return to the host even when a callback repeatedly posts work for the current tick.
        while (count < 1000 && !messages.isEmpty() && messages.get(0).when <= time) {
            Message message = messages.remove(0);
            try {
                message.target.dispatchMessage(message);
            } finally {
                message.clear();
            }
            count++;
        }
        if (isIdle() && !idleHandlers.isEmpty()) {
            IdleHandler[] pending = new IdleHandler[idleHandlers.size()];
            for (int i = 0; i < pending.length; i++) pending[i] = idleHandlers.get(i);
            for (IdleHandler idle : pending) if (!idle.queueIdle()) idleHandlers.remove(idle);
        }
        return count;
    }

    private static boolean matches(Message message, Handler handler, Runnable callback,
                                   Object token, int what, int mode) {
        if (message.target != handler || token != null && message.obj != token) return false;
        switch (mode) {
            case 0: return message.callback == callback;
            case 1: return message.what == what && message.callback == null;
            default: return true;
        }
    }

    boolean filter(Handler handler, Runnable callback, Object token, int what, int mode, boolean remove) {
        boolean found = false;
        for (int i = messages.size() - 1; i >= 0; i--) {
            Message item = messages.get(i);
            if (!matches(item, handler, callback, token, what, mode)) continue;
            if (!remove) return true;
            messages.remove(i);
            item.clear();
            found = true;
        }
        return found;
    }

    void quit(boolean safely) {
        quitting = true;
        long now = SystemClock.uptimeMillis();
        for (int i = messages.size() - 1; i >= 0; --i) {
            if (!safely || messages.get(i).when > now) messages.remove(i).clear();
        }
    }
}
