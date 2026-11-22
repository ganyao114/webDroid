package android.content;

import android.net.Uri;
import android.database.ContentObserver;
import java.util.ArrayList;
import java.util.HashMap;

/** In-memory settings associated with the current application context. */
public class ContentResolver {
    private final HashMap<Uri, String> settings = new HashMap<>();
    private static final class Registration {
        final Uri uri;
        final boolean descendants;
        final ContentObserver observer;
        Registration(Uri uri, boolean descendants, ContentObserver observer) {
            this.uri = uri;
            this.descendants = descendants;
            this.observer = observer;
        }
    }
    private final ArrayList<Registration> observers = new ArrayList<>();
    public String readSetting(Uri uri) { return settings.get(uri); }
    public void writeSetting(Uri uri, String value) {
        if (value == null) settings.remove(uri);
        else settings.put(uri, value);
        notifyChange(uri, null);
    }
    public int getUserId() { return 0; }
    public void registerContentObserver(Uri uri, boolean descendants, ContentObserver observer) {
        registerContentObserver(uri, descendants, observer, getUserId());
    }
    public void registerContentObserver(Uri uri, boolean descendants, ContentObserver observer, int user) {
        if (uri == null || observer == null) throw new NullPointerException();
        if (user == getUserId()) observers.add(new Registration(uri, descendants, observer));
    }
    public void unregisterContentObserver(ContentObserver observer) {
        for (int i = observers.size() - 1; i >= 0; i--) {
            if (observers.get(i).observer == observer) observers.remove(i);
        }
    }
    public void notifyChange(Uri uri, ContentObserver sender) {
        for (Registration entry : new ArrayList<>(observers)) {
            boolean self = entry.observer == sender;
            if ((!self || sender.deliverSelfNotifications()) && (entry.uri.equals(uri)
                    || entry.descendants && uri.toString().startsWith(entry.uri.toString() + "/"))) {
                entry.observer.dispatchChange(self, uri);
            }
        }
    }
}
