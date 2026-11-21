public final class BenchmarkApp {
    static class Node {
        int value;
        Node next;
        int[] values;

        Node(int value, Node next) {
            this.value = value;
            this.next = next;
        }

        int get() {
            return value;
        }

        static int classValue(int value) {
            return value + 9;
        }
    }

    static final class SpecialNode extends Node {
        SpecialNode(int value) {
            super(value, null);
        }

        @Override int get() {
            return value + 19;
        }
    }

    static final class CustomException extends RuntimeException {}

    static Node anchor;
    static int visits;
    static int initial = 7;

    public static int fib(int n) {
        if (n <= 1) return n;
        return fib(n - 1) + fib(n - 2);
    }

    public static int tightSum(int n) {
        int result = 0;
        for (int i = 0; i < n; ++i) result += i;
        return result;
    }

    public static int allocateAndSum(int count) {
        Node tail = new Node(31, null);
        Node root = new Node(73, tail);
        root.values = new int[3];
        root.values[2] = 97;
        anchor = new Node(17, null);
        int result = 0;
        for (int i = 0; i < count; ++i) {
            Node temporary = new Node(i, root);
            int[] values = new int[3];
            temporary.values = values;
            values[0] = i * 3;
            values[1] = temporary.next.value;
            values[2] = root.next.value;
            result += values[0] + values[1] + values[2] + temporary.value;
        }
        ++visits;
        return result + root.values[2] + anchor.value;
    }

    public static int referenceArray() {
        Node[] array = new Node[2];
        array[1] = new Node(41, null);
        return array.length + array[1].value;
    }

    static int dereference(Node obj) {
        return obj.value;
    }

    public static int testTryCatch(Node obj) {
        int result;
        try {
            result = dereference(obj);
        } catch (NullPointerException e) {
            result = 42;
        }
        try {
            throw new CustomException();
        } catch (CustomException e) {
            return result + 58;
        }
    }

    public static int catchAcrossFrames() {
        try {
            allocateAndThrow(new Node(11, null));
            return -1;
        } catch (CustomException e) {
            return 13;
        }
    }

    static void allocateAndThrow(Node node) {
        anchor = node;
        throw new CustomException();
    }

    public static int catchLiveRoot() {
        Node onlyInHandler = new Node(55, null);
        try {
            failAfterAllocating();
            return -1;
        } catch (CustomException e) {
            return onlyInHandler.value;
        }
    }

    static void failAfterAllocating() {
        allocateAndSum(4096);
        throw new CustomException();
    }

    public static int testNonNull() {
        return testTryCatch(new Node(27, null));
    }

    public static int testVirtual() {
        return readVirtual(new SpecialNode(23));
    }

    static int readVirtual(Node node) {
        return node.get();
    }

    public static int testInheritedStatic() {
        return SpecialNode.classValue(19);
    }

    public static int testCast(boolean succeed) {
        Object value;
        if (succeed) value = new SpecialNode(11);
        else value = new int[1];
        try {
            return ((Node) value).get();
        } catch (ClassCastException e) {
            return -1;
        }
    }

    public static int testBounds(int length, int index) {
        try {
            return (new int[length])[index];
        } catch (ArrayIndexOutOfBoundsException e) {
            return -5;
        } catch (NegativeArraySizeException e) {
            return -6;
        }
    }

    public static int testDivide(int a, int b) {
        try {
            return a / b + a % b;
        } catch (ArithmeticException e) {
            return 99;
        }
    }

    public static int integerOps(int x, int y) {
        int a = -x;
        int b = 30131 - y;
        a = ((a + 301) & -19) ^ (b | 29);
        a = a * -3 + (a << (y & 37));
        a = (a >>> 5) ^ (a >> y);
        return (a ^ 0x45a17655) - ((x & y) >>> 1);
    }

    public static long wide(int x, long y) {
        return ((long) x + y - 0x17_0000_0000L) * 3L;
    }

    public static int lowWord(long value) {
        return (int) value;
    }

    public static int floatBits() {
        return (int) floatMath(3.25f, 2.0f);
    }

    static float floatMath(float a, float b) {
        return ((a + b) * a - b) / b;
    }

    public static int profileEntry(int value) {
        return tightSum(value);
    }

    public static int initialValue() {
        return initial;
    }

    public static native int nativeBoost(int x, String tag);
    private static native int nativeEcho(int x);

    public static int nativeRoundTrip(int value) {
        return nativeBoost(value, "tag-\0-\u96ea");
    }

    public static int nativeRoundTripCaught() {
        try {
            return nativeRoundTrip(-1);
        } catch (CustomException e) {
            return 73;
        }
    }

    static int fromNative(int value, String tag) {
        if (tag == null) return -1000;
        if (value < 0) throw new CustomException();
        return nativeEcho(tightSum(value)) + 19;
    }
}
