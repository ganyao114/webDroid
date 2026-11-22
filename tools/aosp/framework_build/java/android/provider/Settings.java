package android.provider;

import android.content.ContentResolver;
import android.net.Uri;

public final class Settings {
    private Settings() {}
    private static int getInt(ContentResolver resolver, Uri uri, int fallback) {
        String value = resolver.readSetting(uri);
        if (value == null) return fallback;
        try { return Integer.parseInt(value); }
        catch (NumberFormatException error) { return fallback; }
    }

    public static final class Secure {
        private Secure() {}
        public static final Uri CONTENT_URI = Uri.parse("content://settings/secure");
        public static Uri getUriFor(String name) { return Uri.parse("content://settings/secure/" + name); }
        public static String getString(ContentResolver resolver, String name) { return resolver.readSetting(getUriFor(name)); }
        public static boolean putString(ContentResolver resolver, String name, String value) {
            resolver.writeSetting(getUriFor(name), value);
            return true;
        }
        public static int getInt(ContentResolver resolver, String name, int fallback) {
            return Settings.getInt(resolver, getUriFor(name), fallback);
        }
        public static boolean putInt(ContentResolver resolver, String name, int value) {
            return putString(resolver, name, Integer.toString(value));
        }
    }

    public static final class System {
        private System() {}
        public static final Uri CONTENT_URI = Uri.parse("content://settings/system");
        public static Uri getUriFor(String name) { return Uri.parse(CONTENT_URI + "/" + name); }
        public static int getInt(ContentResolver resolver, String name, int fallback) {
            return Settings.getInt(resolver, getUriFor(name), fallback);
        }
    }

    public static final class Global {
        private Global() {}
        public static final Uri CONTENT_URI = Uri.parse("content://settings/global");
        public static Uri getUriFor(String name) { return Uri.parse(CONTENT_URI + "/" + name); }
        public static int getInt(ContentResolver resolver, String name, int fallback) {
            return Settings.getInt(resolver, getUriFor(name), fallback);
        }
    }
}
