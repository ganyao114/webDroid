package org.wasmandroid.interp;

public final class RuntimeProbe {
    static int initialized;
    public static Object saved;

    interface Step {
        default int step(int value) { return value + 3; }
    }

    static class Parent {
        protected int value;
        int read() { return value; }
    }

    static final class Node extends Parent implements Step {
        static { initialized++; }
        long wide;
        Node peer;

        Node(int value, long wide) {
            this.value = value;
            this.wide = wide;
        }

        @Override int read() { return super.read() + 1; }
    }

    private static native Object echo(Object value, long wide, double floating, int[] values);

    static int inspect(long wide, double floating, int[] values) {
        System.gc();
        return (int) (wide >>> 40) + (int) floating + values[2];
    }

    public static long exercise(int seed, double number) {
        long[] contents = {0xfedcba9876543210L, 4, -90, 123};
        Node first = new Node(seed, contents[0]);
        Node second = new Node(onPackedSwitch(seed), contents[2]);
        first.peer = second;
        second.peer = first;
        Step step = second;
        int[] ints = {1, 3, 5};
        Object[] covariant = new Node[] { first, second };
        try {
            covariant[0] = new Object();
            return -1;
        } catch (ArrayStoreException expected) {
            // The first element must remain the original Node through the JNI callback and GC.
        }
        synchronized (first) {
            if (echo(covariant[0], 9L << 40, number, ints) != first) return -2;
            System.gc();
        }
        int zero = seed - 2;
        try {
            seed /= zero;
        } catch (ArithmeticException expected) {
            seed += onSparseSwitch(7000);
        }
        saved = first;
        return first.wide + step.step(first.peer.read()) + seed + initialized + (int) number;
    }

    public static Object runArrayChecks(Object[] array, int index) {
        try {
            return (String) array[index];
        } catch (ArrayIndexOutOfBoundsException | ClassCastException expected) {
            return expected;
        }
    }

    private static int onPackedSwitch(int seed) {
        switch (seed) {
            case 0: return 8;
            case 1: return 23;
            case 2: return 11;
            case 3: return 45;
            case 4: return 9;
            default: return 0;
        }
    }

    private static int onSparseSwitch(int seed) {
        switch (seed) {
            case -900: return 46;
            case 10: return 2;
            case 7000: return 19;
            case 12000: return 4;
            default: return -6;
        }
    }
}
