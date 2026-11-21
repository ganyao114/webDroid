package android.icu.util;

import java.util.Locale;

/** LocaleList's adapter to the Android resource locale tables. */
public final class ULocale {
    private final Locale locale;
    private final String script;
    private ULocale(Locale locale, String script) { this.locale = locale; this.script = script; }
    public static ULocale forLocale(Locale locale) { return new ULocale(locale, locale.getScript()); }
    public static ULocale addLikelySubtags(ULocale locale) {
        if (!locale.script.isEmpty()) return locale;
        return new ULocale(locale.locale, likelyScript(locale.locale.toLanguageTag()));
    }
    public String getScript() { return script; }
    public Locale toLocale() { return locale; }
    private static native String likelyScript(String languageTag);
}
