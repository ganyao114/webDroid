public final class HGraphObjectsProbe {
    private interface HasArea {
        int area();
    }

    private abstract static class Shape implements HasArea {
        final int offset;

        Shape(int offset) {
            this.offset = offset;
        }

        int bias() {
            return offset;
        }

        public abstract int area();
    }

    private static final class Rect extends Shape {
        final int width;
        final int height;

        Rect(int width, int height) {
            super(1);
            this.width = width;
            this.height = height;
        }

        public int area() {
            return width * height + super.bias();
        }
    }

    private static final class Circle extends Shape {
        final int radius;

        Circle(int radius) {
            super(2);
            this.radius = radius;
        }

        public int area() {
            return 3 * radius * radius + offset;
        }
    }

    private static final class Node {
        final int value;
        final Node next;

        Node(int value, Node next) {
            this.value = value;
            this.next = next;
        }
    }

    private static final class ProbeFailure extends RuntimeException {
        final int code;

        ProbeFailure(int code) {
            this.code = code;
        }
    }

    private static int counter;
    private static Node saved;

    public static int polymorphism(int rounds) {
        Shape[] shapes = {new Rect(3, 7), new Circle(4)};
        int sum = 0;
        for (int i = 0; i < rounds; ++i) {
            sum += shapes[i % shapes.length].area();
        }
        return sum;
    }

    private static int interfaceArea(HasArea shape) {
        return shape.area();
    }

    public static int throughInterface(boolean circle) {
        return interfaceArea(circle ? new Circle(3) : new Rect(2, 5));
    }

    public static int staticFields(int value) {
        counter = value;
        saved = new Node(value + 1, null);
        return readStatic();
    }

    public static int readStatic() {
        return counter + saved.value;
    }

    public static int arrays(int length) {
        try {
            int[] values = new int[length];
            int sum = 0;
            for (int i = 0; i < values.length; ++i) {
                values[i] = 3 * i - 1;
                sum += values[i];
            }
            return sum;
        } catch (NegativeArraySizeException expected) {
            return -1000;
        }
    }

    public static double floatingArrays(double wide, float narrow) {
        double[] wideValues = {1.5, 2.5};
        float[] narrowValues = {1.25f, 2.5f};
        wideValues[0] = wide;
        narrowValues[0] = narrow;
        return wideElement(wideValues, 0) + narrowValues[0] + wideValues[1] + narrowValues[1];
    }

    private static double wideElement(Object array, int index) {
        return ((double[]) array)[index];
    }

    public static int literalNullArray(int mode) {
        try {
            if (mode == 0) {
                int[] ints = null;
                return ints[0];
            }
            double[] doubles = null;
            if (mode == 1) {
                return (int) doubles[0];
            }
            doubles[0] = 1.25;
            return -1;
        } catch (NullPointerException expected) {
            return 71;
        }
    }

    public static int castsAndStrings(int mode) {
        Object value = mode == 0 ? "ART" : mode == 1 ? new Object() : null;
        int size = value instanceof String ? 10 : 0;
        try {
            String string = (String) value;
            return size + (string == "ART" ? 1 : 2);
        } catch (ClassCastException expected) {
            return 3;
        }
    }

    public static int arrayStore(boolean compatible) {
        Object[] shapes = new Shape[1];
        try {
            shapes[0] = compatible ? new Rect(1, 2) : new Object();
            return ((Shape) shapes[0]).area();
        } catch (ArrayStoreException expected) {
            return -1;
        }
    }

    public static void escapes(int code) {
        throw new ProbeFailure(code);
    }

    public static int exceptions(int mode, int index) {
        int[] values = mode == 0 ? null : new int[3];
        Node marker = new Node(3, null);
        try {
            if (mode == 2) {
                marker = new Node(7, null);
                escapes(41);
                return -1;
            }
            values[1] = 9;
            return values[index] + marker.value;
        } catch (NullPointerException expected) {
            return 100 + marker.value;
        } catch (ArrayIndexOutOfBoundsException expected) {
            return 200 + marker.value;
        } catch (ProbeFailure failure) {
            return 300 + marker.value + failure.code;
        }
    }

    public static int multiCatch(int mode, int index) {
        int[] values = mode == 0 ? null : new int[1];
        try {
            if (mode == 2) {
                escapes(17);
            }
            return values[index];
        } catch (NullPointerException | ArrayIndexOutOfBoundsException expected) {
            return 11;
        } catch (RuntimeException expected) {
            return 22;
        }
    }

    public static int gcStress(int allocations, int chainLength) {
        Node head = null;
        for (int i = 0; i < chainLength; ++i) {
            head = new Node(i * 17 - 9, head);
        }
        Node[] anchors = {new Node(13579, null), new Node(-24680, null)};
        int sum = 0;
        for (int i = 0; i < allocations; ++i) {
            Node garbage = new Node(i ^ 23, null);
            sum += garbage.value;
        }

        for (int i = chainLength - 1; i >= 0; --i) {
            if (head == null || head.value != i * 17 - 9) {
                return -1;
            }
            head = head.next;
        }
        if (head != null || anchors.length != 2 || anchors[0].value != 13579 ||
                anchors[1].value != -24680 || anchors[0].next != null || anchors[1].next != null) {
            return -2;
        }
        return sum;
    }

    public static int keepInt(int value) {
        return value;
    }
}
