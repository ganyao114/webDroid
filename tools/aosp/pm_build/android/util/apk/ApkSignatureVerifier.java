// Compile-time declaration for a hidden AOSP API.
package android.util.apk;

import android.content.pm.SigningDetails;
import android.content.pm.parsing.result.ParseInput;
import android.content.pm.parsing.result.ParseResult;

public final class ApkSignatureVerifier {
    public static native int getMinimumSignatureSchemeVersionForTargetSdk(int targetSdk);
    public static native ParseResult<SigningDetails> unsafeGetCertsWithoutVerification(
            ParseInput input, String path, int minSignatureSchemeVersion);
    public static native ParseResult<SigningDetails> verify(
            ParseInput input, String path, int minSignatureSchemeVersion);
}
