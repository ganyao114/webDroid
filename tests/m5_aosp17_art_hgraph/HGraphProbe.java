public final class HGraphProbe {
    public static int folded(int x) {
        // With javac -g and d8 --debug, these locals keep both paths in DEX.
        int six = 6;
        int seven = 7;
        int hundred = 100;
        int four = 4;
        int c = (six * seven) + (hundred / four);
        if (c == 67) {
            return x + c;
        }
        return x - 9999;
    }

    public static int sumLoop(int n) {
        int sum = 0;
        for (int i = 1; i <= n; i++) {
            sum += i;
        }
        return sum;
    }

    public static int gcd(int a, int b) {
        while (b != 0) {
            int r = a % b;
            a = b;
            b = r;
        }
        return a;
    }

    public static int fib(int n) {
        if (n <= 1) {
            return n;
        }
        return fib(n - 1) + fib(n - 2);
    }

    private static int twist(int x, int k) {
        int rotated = (x << k) | (x >>> -k);
        int shifted = ((x >>> k) & (x << 3)) | (x >> k);
        return rotated ^ shifted;
    }

    public static int callHelper(int x, int k) {
        return -twist(x, k);
    }

    public static long hashMix(long x, int k) {
        long rotated = (x << k) | (x >>> -k);
        long shifted = (x >>> (k + 1)) ^ ((x << 2) & 0x1111111111111111L);
        return (rotated ^ shifted ^ (x >> k) ^ (long) k) * 0x9e3779b97f4a7c15L
            & 0x7fffffffffffffffL;
    }

    public static float lerp(float a, float b, float t) {
        return a + (b - a) * t;
    }

    public static int choose(int key, int low, int high) {
        return key < 0 ? low : high;
    }

    public static float floatRemainder(float x, float y) {
        return x % y;
    }

    public static double doubleRemainder(double x, double y) {
        return x % y;
    }

    public static int floatOrder(float x, float y) {
        if (x < y) return -1;
        if (x > y) return 1;
        return 0;
    }

    public static int swapLoop(int a, int b, int n) {
        while (n > 0) {
            int temp = a;
            a = b;
            b = temp;
            --n;
        }
        return a * 100 + b;
    }

    public static int quotient(int x, int y) {
        return x / y;
    }

    public static long wideQuotient(long x, long y) {
        return x / y;
    }

    public static int floatToInt(float x) {
        return (int) (x * 0.5f);
    }

    public static int narrow(int x) {
        return (byte) x + (short) x + (char) x;
    }

    public static void noResult() {
    }

    public static void callVoid() {
        noResult();
    }
}
