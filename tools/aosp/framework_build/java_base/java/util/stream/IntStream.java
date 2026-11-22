package java.util.stream;

import java.util.OptionalInt;

public interface IntStream {
    OptionalInt min();
    int sum();

    static IntStream of(int value) { return Stream.of(new Integer[] {value}).mapToInt(Integer::intValue); }
    static IntStream empty() { return Stream.of(new Integer[0]).mapToInt(Integer::intValue); }
}
