package android.provider;

import android.content.ContentResolver;
import android.net.Uri;

public final class Settings {
    private Settings() {}

    public static final class Secure {
        private Secure() {}
        public static Uri getUriFor(String name) { return Uri.parse("content://settings/secure/" + name); }
        public static String getString(ContentResolver resolver, String name) { return resolver.readSetting(getUriFor(name)); }
        public static boolean putString(ContentResolver resolver, String name, String value) {
            resolver.writeSetting(getUriFor(name), value);
            return true;
        }
        public static int getInt(ContentResolver resolver, String name, int fallback) {
            String value = getString(resolver, name);
            if (value == null) return fallback;
            try { return Integer.parseInt(value); }
            catch (NumberFormatException error) { return fallback; }
        }
        public static boolean putInt(ContentResolver resolver, String name, int value) {
            return putString(resolver, name, Integer.toString(value));
        }
    }
}
