package android.content.pm;

// Lite parsing with flags=0 needs only the unsigned sentinel.
public final class SigningDetails {
    public static final SigningDetails UNKNOWN = new SigningDetails();

    private SigningDetails() {}
}
