package android.webkit;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.util.AttributeSet;
import android.widget.FrameLayout;
import java.util.ArrayList;

/** Adapts Android's web view to the browser-owned SurfaceFlinger iframe plane. */
public class WebView extends FrameLayout {
    private static final String BLANK = "about:blank";
    private static int nextId = 2;

    private final int id = nextId++;
    private final WebSettings settings = new WebSettings();
    private final ArrayList<String> history = new ArrayList<>();
    private final int[] location = new int[2];
    private WebViewClient client;
    private int page = -1;
    private boolean destroyed;

    public WebView(Context context) { this(context, null); }

    public WebView(Context context, AttributeSet attrs) {
        super(context, attrs);
        setFocusableInTouchMode(true);
        setBackgroundColor(Color.WHITE);
        setWillNotDraw(false);
    }

    public WebSettings getSettings() { return settings; }
    public void setWebViewClient(WebViewClient client) { this.client = client; }
    public String getUrl() { return page < 0 ? null : history.get(page); }

    public void loadUrl(String url) {
        if (!navigate(url)) return;
        while (history.size() > page + 1) history.remove(history.size() - 1);
        history.add(url);
        ++page;
    }

    public boolean canGoBack() { return page > 0; }

    public void goBack() {
        if (canGoBack() && navigate(history.get(page - 1))) --page;
    }

    public void reload() {
        if (page >= 0) navigate(history.get(page));
    }

    private boolean navigate(String url) {
        if (destroyed) return false;
        // The iframe provider allows page scripts. Require the caller to enable them explicitly.
        if (url == null || (!BLANK.equals(url) && !settings.getJavaScriptEnabled()) ||
                !nativeNavigate(id, url)) {
            if (client != null) client.onReceivedError(this, WebViewClient.ERROR_UNSUPPORTED_SCHEME,
                    "The embedded WebView requires an HTTP(S) URL and JavaScript permission", url);
            return false;
        }
        if (client != null) {
            client.onPageStarted(this, url, null);
            if (BLANK.equals(url)) client.onPageFinished(this, url);
        }
        invalidate();
        return true;
    }

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        if (!destroyed && page >= 0) nativeNavigate(id, history.get(page));
    }

    @Override protected void onDetachedFromWindow() {
        nativeClose(id);
        super.onDetachedFromWindow();
    }

    @Override public void setVisibility(int visibility) {
        super.setVisibility(visibility);
        if (destroyed || page < 0) return;
        if (visibility == VISIBLE) nativeNavigate(id, history.get(page));
        else nativeClose(id);
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (destroyed || page < 0) return;
        getLocationInWindow(location);
        nativePlace(id, location[0], location[1], getWidth(), getHeight());
    }

    public void destroy() {
        if (destroyed) return;
        nativeClose(id);
        destroyed = true;
        client = null;
        history.clear();
        page = -1;
        invalidate();
    }

    private static native boolean nativeNavigate(int id, String url);
    private static native void nativePlace(int id, int x, int y, int width, int height);
    private static native void nativeClose(int id);
}
