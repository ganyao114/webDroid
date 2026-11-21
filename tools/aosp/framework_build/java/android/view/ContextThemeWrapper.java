package android.view;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.res.Resources;

public class ContextThemeWrapper extends ContextWrapper {
    private int themeId;
    private Resources.Theme theme;
    private LayoutInflater inflater;
    public ContextThemeWrapper() { super(null); }
    public ContextThemeWrapper(Context base, int id) { super(base); themeId = id; }
    public ContextThemeWrapper(Context base, Resources.Theme theme) { super(base); this.theme = theme; }
    public int getThemeResId() { return themeId; }
    @Override public void setTheme(int id) {
        if (themeId != id) { themeId = id; theme = null; }
    }
    @Override public Resources.Theme getTheme() {
        if (theme == null) {
            theme = getResources().newTheme();
            Resources.Theme base = getBaseContext().getTheme();
            if (base != null) theme.setTo(base);
            if (themeId != 0) theme.applyStyle(themeId, true);
        }
        return theme;
    }
    @Override public Object getSystemService(String name) {
        if (LAYOUT_INFLATER_SERVICE.equals(name)) {
            if (inflater == null) inflater = LayoutInflater.from(getBaseContext()).cloneInContext(this);
            return inflater;
        }
        return super.getSystemService(name);
    }
}
