package android.content;

import android.net.Uri;
import java.util.HashMap;

/** In-memory settings associated with the current application context. */
public class ContentResolver {
    private final HashMap<Uri, String> settings = new HashMap<>();
    public String readSetting(Uri uri) { return settings.get(uri); }
    public void writeSetting(Uri uri, String value) {
        if (value == null) settings.remove(uri);
        else settings.put(uri, value);
    }
}
