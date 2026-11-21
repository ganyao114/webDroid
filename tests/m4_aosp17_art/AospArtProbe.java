public final class AospArtProbe {
    public static final int MARKER = -40000;
    public static final String LABEL = "ART \uD83D\uDE80";
    private final int counter;

    public AospArtProbe(int counter) {
        this.counter = counter;
    }

    public int snapshot() {
        return counter;
    }

    public static int mix(int x, int y) {
        return (x * 7) ^ (y + 1234);
    }

    public static int sum(int count) {
        int total = 0;
        for (int i = 0; i < count; i++) {
            total += mix(i, 3);
        }
        return total;
    }

    public static long blend(long left, long right, int shift) {
        return (left << shift) ^ right ^ 0x123456789abcdefL;
    }

    public static int guardedDivide(int numerator, int denominator) {
        try {
            return numerator / denominator;
        } catch (ArithmeticException ignored) {
            return -97;
        }
    }

    public static int subtractFromConstant(int value) {
        return 0x12340000 - value;
    }
}
