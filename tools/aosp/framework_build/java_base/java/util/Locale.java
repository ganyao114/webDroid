package java.util;

import java.io.Serializable;

/** Immutable locale values for Android resource configuration. */
public final class Locale implements Cloneable, Serializable {
    public static final Locale ROOT = new Locale("");
    public static final Locale ENGLISH = new Locale("en"), US = new Locale("en", "US");
    public static final Locale UK = new Locale("en", "GB");
    public static final Locale CHINESE = new Locale("zh"), CHINA = new Locale("zh", "CN");
    public static final Locale TAIWAN = new Locale("zh", "TW");
    public static final Locale SIMPLIFIED_CHINESE = CHINA, TRADITIONAL_CHINESE = TAIWAN;
    public static final Locale JAPANESE = new Locale("ja"), JAPAN = new Locale("ja", "JP");
    public static final Locale KOREAN = new Locale("ko"), KOREA = new Locale("ko", "KR");
    public static final Locale FRENCH = new Locale("fr"), FRANCE = new Locale("fr", "FR");
    public static final Locale GERMAN = new Locale("de"), GERMANY = new Locale("de", "DE");
    private static Locale defaultLocale = US;

    private final String language, country, script, variant;
    public Locale(String language) { this(language, ""); }
    public Locale(String language, String country) { this(language, country, ""); }
    public Locale(String language, String country, String variant) { this(language, country, "", variant); }
    private Locale(String language, String country, String script, String variant) {
        this.language = language.toLowerCase();
        this.country = upper(country);
        this.script = script;
        this.variant = variant;
    }

    public String getLanguage() { return language; }
    public String getCountry() { return country; }
    public String getScript() { return script; }
    public String getVariant() { return variant; }
    public static Locale getDefault() { return defaultLocale; }
    public static void setDefault(Locale locale) {
        if (locale == null) throw new NullPointerException("locale");
        defaultLocale = locale;
    }

    public static Locale forLanguageTag(String tag) {
        String[] parts = tag.split("-");
        if (parts.length == 0 || !letters(parts[0], 2, 8)) return ROOT;
        String language = "und".equals(parts[0].toLowerCase()) ? "" : parts[0];
        String script = "", region = "", variant = "";
        int next = 1;
        if (next < parts.length && letters(parts[next], 4, 4)) {
            String part = parts[next++].toLowerCase();
            script = upper(part.substring(0, 1)) + part.substring(1);
        }
        if (next < parts.length && isRegion(parts[next])) region = parts[next++];
        while (next < parts.length && parts[next].length() >= 4 && parts[next].length() <= 8) {
            if (!variant.isEmpty()) variant += "_";
            variant += parts[next++];
        }
        return new Locale(language, region, script, variant);
    }

    public String toLanguageTag() {
        StringBuilder result = new StringBuilder(language.isEmpty() ? "und" : language);
        if (!script.isEmpty()) result.append('-').append(script);
        if (!country.isEmpty()) result.append('-').append(country);
        if (!variant.isEmpty()) result.append('-').append(variant.replace("_", "-"));
        return result.toString();
    }

    private static boolean letters(String value, int min, int max) {
        if (value.length() < min || value.length() > max) return false;
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z')) return false;
        }
        return true;
    }

    private static boolean isRegion(String value) {
        if (letters(value, 2, 2)) return true;
        return value.length() == 3 && digit(value.charAt(0)) && digit(value.charAt(1)) && digit(value.charAt(2));
    }

    private static boolean digit(char c) { return c >= '0' && c <= '9'; }

    private static String upper(String value) {
        StringBuilder result = new StringBuilder(value.length());
        for (int i = 0; i < value.length(); ++i) {
            char c = value.charAt(i);
            result.append(c >= 'a' && c <= 'z' ? (char) (c - ('a' - 'A')) : c);
        }
        return result.toString();
    }

    @Override public Object clone() { return this; }

    @Override public boolean equals(Object object) {
        if (!(object instanceof Locale)) return false;
        Locale other = (Locale) object;
        return language.equals(other.language) && country.equals(other.country)
                && script.equals(other.script) && variant.equals(other.variant);
    }

    @Override public int hashCode() {
        return ((language.hashCode() * 31 + country.hashCode()) * 31 + script.hashCode()) * 31 + variant.hashCode();
    }

    @Override public String toString() {
        String result = language;
        if (!country.isEmpty() || !variant.isEmpty()) result += "_" + country;
        if (!variant.isEmpty()) result += "_" + variant;
        return script.isEmpty() ? result : result + (country.isEmpty() && variant.isEmpty() ? "_" : "") + "_#" + script;
    }
}
