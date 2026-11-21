package java.net;

import java.util.Objects;

public class URISyntaxException extends Exception {
    private final String input;
    private final int index;
    public URISyntaxException(String input, String reason, int index) {
        super(Objects.requireNonNull(reason));
        this.input = Objects.requireNonNull(input);
        if (index < -1) throw new IllegalArgumentException("index");
        this.index = index;
    }
    public URISyntaxException(String input, String reason) { this(input, reason, -1); }
    public String getInput() { return input; }
    public String getReason() { return super.getMessage(); }
    public int getIndex() { return index; }
    @Override public String getMessage() {
        return getReason() + (index < 0 ? ": " : " at index " + index + ": ") + input;
    }
}
