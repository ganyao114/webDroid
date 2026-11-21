package android.view.inputmethod;

import android.os.IBinder;
import android.view.View;

public final class InputMethodManager {
    public static final int SHOW_IMPLICIT = 1;
    private View inputView;

    public boolean showSoftInput(View view, int flags) {
        if (!view.isFocused() || view.getWindowToken() == null) return false;
        inputView = view;
        return true;
    }

    public boolean hideSoftInputFromWindow(IBinder token, int flags) {
        if (inputView == null || token == null || inputView.getWindowToken() != token) return false;
        inputView = null;
        return true;
    }

    public boolean isActive(View view) { return view == inputView && view.isFocused(); }
}
