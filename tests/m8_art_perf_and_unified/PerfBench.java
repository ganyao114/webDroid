package org.wasmandroid.m8;

import java.io.UnsupportedEncodingException;
import java.lang.ref.WeakReference;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.ConcurrentModificationException;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.ListIterator;
import java.util.Map;

public final class PerfBench {
    private static final int PASSES = 120;

    public interface View {
        int measure(int spec);
        int layout(int origin);
        default int draw(int seed) { return seed + 17; }
        static int size(int spec, int margin) { return (spec & 255) + margin; }
    }

    public static class Tile implements View {
        final int id;
        int width, height, x, y;

        public Tile(int id) { this.id = id; }
        @Override public int measure(int spec) {
            width = View.size(spec, id);
            height = (width >>> 1) + 9;
            return width + height;
        }
        @Override public int layout(int origin) {
            x = origin + id;
            y = origin / 3 + height;
            return x + y;
        }
        @Override public int draw(int seed) {
            return View.super.draw(seed) ^ (x * 31 + y * 7 + width + height);
        }
    }

    public static final class Icon extends Tile {
        public Icon(int id) { super(id); }
        @Override public int draw(int seed) { return (x ^ width) + (seed ^ height) + y; }
    }

    public static final class Label extends Tile {
        public Label(int id) { super(id); }
        @Override public int measure(int spec) {
            width = (spec & 127) + 4 * id;
            height = 12;
            return width;
        }
    }

    public static final class Workspace extends Tile {
        final Tile[] children;
        int length;

        public Workspace(int capacity) {
            super(capacity);
            children = new Tile[capacity];
        }
        void add(Tile child) { children[length++] = child; }

        @Override public int measure(int spec) {
            int total = super.measure(spec);
            for (int i = 0; i < length; i++) {
                View child = children[i];
                total += child.measure(spec + i);
            }
            return total;
        }
        @Override public int layout(int origin) {
            int total = super.layout(origin);
            for (int i = 0; i < length; i++) total += children[i].layout(origin + 4 * i);
            return total;
        }
        @Override public int draw(int seed) {
            int total = super.draw(seed);
            for (int i = 0; i < length; i++) {
                View child = children[i];
                total ^= child.draw(seed + i);
            }
            return total;
        }
    }

    public static final class Quiet {
        static { initializations++; }
        public Quiet() {}
    }

    public static final class PrivateTile extends Tile {
        private PrivateTile(long id) { super((int) id); }
    }

    public static final class BrokenTile extends Tile {
        public BrokenTile(int id) {
            super(id);
            throw new IllegalArgumentException("bad view");
        }
    }

    private enum Badge { EMPTY, NEW, IMPORTANT }

    private static final String ROOT_NAME = "org.wasmandroid.m8.PerfBench$Workspace";
    private static final String[] NAMES = {
        "org.wasmandroid.m8.PerfBench$Tile",
        "org.wasmandroid.m8.PerfBench$Icon",
        "org.wasmandroid.m8.PerfBench$Label",
    };
    private static final HashMap<String, Tile> tiles = new HashMap<>();
    private static final ArrayList<Tile> order = new ArrayList<>();
    private static final Comparator<Tile> byId = orderBy(1);
    private static Workspace root;
    private static int frameNumber, initializations;
    private static WeakReference<Object> weak;

    private static Comparator<Tile> orderBy(int direction) {
        return (first, second) -> direction * Integer.compare(first.id, second.id);
    }

    private static Tile inflate(String name, int value) throws ReflectiveOperationException {
        Class<? extends Tile> type = Class.forName(name).asSubclass(Tile.class);
        Constructor<? extends Tile> constructor = type.getConstructor(int.class);
        return constructor.newInstance(value);
    }

    public static void prepare() throws ReflectiveOperationException {
        root = (Workspace) inflate(ROOT_NAME, 4);
        tiles.clear();
        order.clear();
        frameNumber = 0;
        for (int row = 0, id = 1; row < 4; row++) {
            Workspace group = (Workspace) inflate(ROOT_NAME, row == 0 ? 7 : 6);
            root.add(group);
            for (int i = 0; i < group.children.length; i++, id++) {
                Tile child = inflate(NAMES[id % NAMES.length], id);
                tiles.put("tile-" + id, child);
                order.add(child);
                group.add(child);
            }
        }
    }

    public static int callsPerFrame() { return 30 * 3 * PASSES; }

    public static int frame() throws ReflectiveOperationException {
        int phase = ++frameNumber;
        Tile indicator = inflate(NAMES[phase % NAMES.length], phase & 31);
        Collections.reverse(order);
        Collections.sort(order, byId);
        Tile tile = tiles.get("tile-" + ((phase - 1) % 25 + 1));
        String tag = String.format("TILE/%04d/%s", tile.id, "ICON").toLowerCase();
        String[] tokens = tag.split("/");
        String label = tokens[2].concat(tokens[1]);
        int spec = phase + (int) Math.sqrt(tile.id * 5.0)
                + (int) (8 * Math.sin(phase) * Math.cos(phase));
        int checksum = label.contains("icon") ? label.indexOf('n') : label.length();
        for (int pass = 0; pass < PASSES; pass++) {
            checksum = checksum * 31 + root.measure(spec + pass);
            checksum = checksum * 31 + root.layout(pass);
            checksum = checksum * 31 + root.draw(checksum);
        }
        return (checksum ^ indicator.draw(spec)) | 0xff000000;
    }

    private static void require(boolean condition, int test) {
        if (!condition) throw new AssertionError("Wave 8 Java assertion " + test);
    }

    public static void installWeak() {
        Object live = new Object();
        weak = new WeakReference<>(live);
        require(weak.get() == live, 1);
    }

    public static int weakCleared() { return weak.get() == null ? 1 : 0; }

    public static void allocateDuringFrame() {
        for (int i = 0; i < 26000; ++i) {
            WeakReference<Object> reference = new WeakReference<>(new Object());
            reference.clear();
        }
    }

    private static int divide(int zero) { return 14 / zero; }

    @SuppressWarnings("deprecation")
    public static void verify() throws ReflectiveOperationException, UnsupportedEncodingException {
        Class<?> type = Class.forName(ROOT_NAME);
        require(type.getName().equals(ROOT_NAME) && type.getSimpleName().equals("Workspace"), 2);
        require(type.getSuperclass() == Tile.class && Tile.class.getInterfaces()[0] == View.class, 3);
        require(View.class.isAssignableFrom(type) && View.class.isInstance(inflate(ROOT_NAME, 0)), 4);
        require(new Tile(1) {}.getClass().getSimpleName().isEmpty(), 5);
        Object array = new Icon[2][1];
        require(array instanceof View[][] && ((View[][]) array).length == 2, 6);
        require(Class.forName("[I") == int[].class && !Object[].class.isAssignableFrom(int[].class), 7);
        require(Class.forName("[Ljava.lang.String;").getSimpleName().equals("String[]"), 8);
        Class<?> quiet = Class.forName("org.wasmandroid.m8.PerfBench$Quiet", false, type.getClassLoader());
        require(initializations == 0 && quiet.newInstance() instanceof Quiet && initializations == 1, 9);

        try {
            PrivateTile.class.getConstructor(long.class);
            require(false, 10);
        } catch (NoSuchMethodException expected) {}
        Constructor<PrivateTile> hidden = PrivateTile.class.getDeclaredConstructor(long.class);
        hidden.setAccessible(true);
        require(hidden.newInstance(15).id == 15, 11);
        try {
            inflate(BrokenTile.class.getName(), 0);
            require(false, 12);
        } catch (InvocationTargetException expected) {
            require(expected.getCause() instanceof IllegalArgumentException, 13);
        }
        try {
            divide(0);
            require(false, 14);
        } catch (IllegalArgumentException | ArithmeticException expected) {
            require(expected instanceof ArithmeticException, 15);
        }
        try {
            Class.forName("org.wasmandroid.m8.Missing");
            require(false, 16);
        } catch (ClassNotFoundException expected) {}
        require(Badge.values()[2].ordinal() == 2 && Badge.valueOf("NEW").name().equals("NEW")
                && Badge.class.getEnumConstants()[2] == Badge.IMPORTANT, 17);

        verifyText();
        verifyCollections();
        verifyArraysAndMath();
    }

    private static void verifyText() throws UnsupportedEncodingException {
        String text = "  Alpha:β:ALPHA  ".trim();
        require(text.length() == 13 && text.charAt(6) == 'β' && text.startsWith("β", 6), 18);
        require(text.lastIndexOf("ALPHA", 13) == 8 && text.lastIndexOf('a') == 4
                && text.indexOf(':', 6) == 7 && text.endsWith("ALPHA"), 19);
        require(text.regionMatches(true, 0, "ALPHA", 0, 5) && text.substring(0, 5).equalsIgnoreCase("alpha")
                && text.substring(8).compareToIgnoreCase("alpha") == 0, 20);
        require(text.replace(':', '/').replace("β", "gamma").split("/")[1].equals("gamma")
                && "α::γ".split(":+")[1].equals("γ"), 21);
        require("Straße".toUpperCase().equals("STRASSE") && "É".toLowerCase().equals("é")
                && "É".getBytes().length == 2 && text.toCharArray()[6] == 'β', 22);
        require(String.valueOf(new char[]{'b', 'e', 'e'}, 1, 2).equals("ee")
                && String.valueOf(123456789L).equals("123456789"), 23);
        StringBuilder builder = new StringBuilder();
        builder.append('a').append(new char[]{'b','c','d'}, 1, 2).insert(1, "β").delete(2, 40);
        builder.setLength(3);
        require(builder.length() == 3 && builder.charAt(2) == 0
                && new StringBuffer(builder).append(true).toString().equals("aβ\0true"), 24);
        require(String.format("%2$s:%1$04d:%3$.2f", 7, "tag", 1.25).equals("tag:0007:1.25"), 25);
        require(new String("Alpha").intern() == "Alpha" && "ø".getBytes("US-ASCII")[0] == '?'
                && "ø".getBytes(StandardCharsets.ISO_8859_1)[0] == (byte) 0xf8, 26);
    }

    private static void verifyCollections() {
        ArrayList<Integer> list = new ArrayList<>(Arrays.asList(5, 3, 1));
        Collections.sort(list);
        require(list.get(0) == 1 && list.get(2) == 5, 27);
        ListIterator<Integer> cursor = list.listIterator(1);
        require(cursor.previous() == 1 && cursor.nextIndex() == 0, 28);
        cursor.set(8);
        cursor.add(4);
        require(cursor.previous() == 4, 29);
        cursor.remove();
        List<Integer> view = Collections.unmodifiableList(list);
        list.add(9);
        require(view.size() == 4 && view.get(0) == 8 && view.equals(Arrays.asList(8, 3, 5, 9)), 30);
        try {
            view.add(10);
            require(false, 31);
        } catch (UnsupportedOperationException expected) {}
        Iterator<Integer> stale = list.iterator();
        list.add(10);
        try {
            stale.next();
            require(false, 32);
        } catch (ConcurrentModificationException expected) {}
        require(Collections.emptyList().isEmpty() && Collections.emptyMap().isEmpty()
                && Collections.emptySet().isEmpty() && Collections.singletonList(null).get(0) == null, 33);

        Map<String, Integer> map = new LinkedHashMap<>(4, .75f, true);
        map.put("Aa", 7);
        map.put("BB", 3);
        map.put(null, 1);
        require(map.get("Aa") == 7 && map.keySet().iterator().next().equals("BB"), 34);
        Map.Entry<String, Integer> entry = map.entrySet().iterator().next();
        require(entry.setValue(12) == 3 && map.get("BB") == 12 && map.containsValue(12), 35);
        require(map.values().remove(1) && !map.containsKey(null) && map.size() == 2, 36);
        HashSet<String> set = new HashSet<>(map.keySet());
        require(!set.add("Aa") && set.add(null) && set.contains("BB"), 37);
        Iterator<String> iter = set.iterator();
        String first = iter.next();
        iter.remove();
        require(set.size() == 2 && !set.contains(first), 38);
    }

    private static void verifyArraysAndMath() {
        int[] numbers = {6, 3, 2, 7, 5, 1};
        System.arraycopy(numbers, 0, numbers, 1, 3);
        require(Arrays.equals(numbers, new int[]{6, 6, 3, 2, 5, 1}), 39);
        int[] part = Arrays.copyOfRange(numbers, 2, 7);
        Arrays.fill(part, 2, 4, 3);
        Arrays.sort(part);
        require(Arrays.binarySearch(part, 0) == 0 && Arrays.binarySearch(part, 4) == -6
                && Arrays.hashCode(Arrays.copyOf(part, 2)) == 963, 40);
        String[] target = new String[2];
        try {
            System.arraycopy(new Object[]{"ok", 3}, 0, target, 0, 2);
            require(false, 41);
        } catch (ArrayStoreException expected) {
            require(target[0].equals("ok") && target[1] == null, 42);
        }
        String[] strings = {"z", "a", "m"};
        Arrays.sort(strings, Comparator.reverseOrder());
        require(Arrays.binarySearch(strings, "m", Comparator.reverseOrder()) == 1
                && Arrays.copyOf(strings, 4).getClass() == String[].class, 43);
        float[] reals = {1, Float.NaN, 0f, -0f, -1};
        Arrays.sort(reals);
        require(Arrays.binarySearch(reals, Float.NaN) == 4 && Float.floatToIntBits(reals[1]) == 0x80000000, 44);
        require(Integer.parseInt("-7f", 16) == -127 && Long.parseLong("-9223372036854775808") == Long.MIN_VALUE
                && Float.parseFloat(" 0x1.8p1f ") == 3 && Double.isInfinite(Double.parseDouble("Infinity")), 45);
        Number boxed = Float.valueOf("3.5");
        require(boxed.doubleValue() == 3.5 && boxed.intValue() == 3 && Character.isWhitespace(0x3000)
                && Character.isDigit(0x1d7cf) && Character.toUpperCase('é') == 'É', 46);
        require(StrictMath.hypot(3, 4) == 5 && StrictMath.round(-1.5) == -1 && StrictMath.cbrt(8) == 2
                && Double.doubleToLongBits(Math.min(-0.0, 0.0)) == Long.MIN_VALUE, 47);
        require(mix(1, 2L, 3, 4L, 5, 6L) == 654321L, 48);
    }

    private static long mix(int a, long b, int c, long d, int e, long f) {
        return a + 10 * b + 100 * c + 1000 * d + 10000 * e + 100000 * f;
    }

    public static String summary() { return "reflection, lambdas, strings, collections, arrays, math"; }
}
