package android.content.om;

// The upstream JNI registry resolves this constructor when it loads.
public final class OverlayableInfo {
    public OverlayableInfo(String name, String actor) {
        throw new UnsupportedOperationException("Fixture has no overlayable resources");
    }
}
