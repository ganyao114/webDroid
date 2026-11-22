package android.webkit;

import android.graphics.Bitmap;

/** Navigation callbacks for the embedded WebView. */
public class WebViewClient {
    public static final int ERROR_UNSUPPORTED_SCHEME = -10;

    public void onPageStarted(WebView view, String url, Bitmap favicon) {}
    public void onPageFinished(WebView view, String url) {}
    public boolean shouldOverrideUrlLoading(WebView view, String url) { return false; }
    public void onReceivedError(WebView view, int code, String description, String failingUrl) {}
}
