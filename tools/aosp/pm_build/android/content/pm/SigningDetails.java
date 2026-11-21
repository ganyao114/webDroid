// Compile-time declaration for a hidden AOSP API.
package android.content.pm;

public final class SigningDetails {
    public static final SigningDetails UNKNOWN = null;

    public @interface SignatureSchemeVersion {
        int JAR = 1;
        int SIGNING_BLOCK_V2 = 2;
    }
}
