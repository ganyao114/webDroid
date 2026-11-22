package android.webkit;

/** Settings used by the iframe-backed WebView for explicit script opt-in. */
public final class WebSettings {
    private boolean javascriptEnabled;

    WebSettings() {}

    public boolean getJavaScriptEnabled() { return javascriptEnabled; }
    public void setJavaScriptEnabled(boolean enabled) { javascriptEnabled = enabled; }
    public String getUserAgentString() { return "WasmAndroid WebView"; }
}
