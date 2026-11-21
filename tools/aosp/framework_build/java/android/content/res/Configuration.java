package android.content.res;

import android.icu.util.ULocale;
import android.os.LocaleList;
import java.util.Locale;

public final class Configuration {
    public static final int ORIENTATION_UNDEFINED = 0;
    public static final int ORIENTATION_PORTRAIT = 1;
    public static final int ORIENTATION_LANDSCAPE = 2;
    public static final int UI_MODE_NIGHT_MASK = 0x30, UI_MODE_NIGHT_NO = 0x10, UI_MODE_NIGHT_YES = 0x20;
    public int smallestScreenWidthDp, screenWidthDp, screenHeightDp, screenLayout;
    public int uiMode, keyboard, navigation, touchscreen, colorMode, orientation;
    public int densityDpi, mcc, mnc, keyboardHidden;
    public float fontScale = 1;
    @Deprecated public Locale locale = Locale.US;
    private LocaleList locales = new LocaleList(Locale.US);

    public Configuration() {}
    public Configuration(Configuration other) { setTo(other); }

    public void setTo(Configuration other) {
        smallestScreenWidthDp = other.smallestScreenWidthDp;
        screenWidthDp = other.screenWidthDp;
        screenHeightDp = other.screenHeightDp;
        screenLayout = other.screenLayout;
        uiMode = other.uiMode;
        keyboard = other.keyboard;
        navigation = other.navigation;
        touchscreen = other.touchscreen;
        colorMode = other.colorMode;
        orientation = other.orientation;
        densityDpi = other.densityDpi;
        mcc = other.mcc;
        mnc = other.mnc;
        keyboardHidden = other.keyboardHidden;
        fontScale = other.fontScale;
        locale = other.locale;
        locales = other.locales;
    }

    public void setLocale(Locale value) {
        setLocales(value == null ? LocaleList.getEmptyLocaleList() : new LocaleList(value));
    }

    /** Host entry point for setting the APK's Android resource configuration. */
    public void setLocaleTag(String tag) {
        // Android's resource qualifier uses rCN where the BCP 47 tag uses CN.
        int r = tag.indexOf("-r");
        if (r >= 0 && tag.length() == r + 4 && tag.charAt(r + 2) >= 'A' && tag.charAt(r + 2) <= 'Z'
                && tag.charAt(r + 3) >= 'A' && tag.charAt(r + 3) <= 'Z') {
            tag = tag.substring(0, r + 1) + tag.substring(r + 2);
        }
        setLocales(LocaleList.forLanguageTags(tag));
    }

    public LocaleList getLocales() { return locales; }
    public void setLocales(LocaleList values) {
        locales = values == null ? LocaleList.getEmptyLocaleList() : values;
        locale = locales.get(0);
        setLayoutDirection(locale);
    }
    public void setLayoutDirection(Locale locale) {
        String script = locale == null ? "" : ULocale.addLikelySubtags(ULocale.forLocale(locale)).getScript();
        boolean rtl = "Arab".equals(script) || "Hebr".equals(script) || "Thaa".equals(script)
                || "Nkoo".equals(script) || "Adlm".equals(script) || "Syrc".equals(script);
        screenLayout = (screenLayout & ~0xc0) | (rtl ? 0x80 : 0x40);
    }
    public String getLocaleTag() { return locale == null ? "" : locale.toLanguageTag(); }
    String[] localeTags() { return locales.isEmpty() ? new String[0] : locales.toLanguageTags().split(","); }
    public int getLayoutDirection() { return (screenLayout & 0xc0) == 0x80 ? 1 : 0; }
}
