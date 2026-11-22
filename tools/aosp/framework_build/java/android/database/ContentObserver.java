package android.database;

import android.net.Uri;
import android.os.Handler;

public abstract class ContentObserver {
    private final Handler handler;

    public ContentObserver(Handler handler) { this.handler = handler; }
    public boolean deliverSelfNotifications() { return false; }
    public void onChange(boolean selfChange) {}
    public void onChange(boolean selfChange, Uri uri) { onChange(selfChange); }
    public final void dispatchChange(boolean selfChange) { dispatchChange(selfChange, null); }
    public final void dispatchChange(boolean selfChange, Uri uri) {
        if (handler == null) onChange(selfChange, uri);
        else handler.post(() -> onChange(selfChange, uri));
    }
}
