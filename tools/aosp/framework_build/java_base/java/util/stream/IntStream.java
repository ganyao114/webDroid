package java.util.stream;

import java.util.Iterator;
import java.util.NoSuchElementException;
import java.util.Objects;
import java.util.OptionalInt;
import java.util.function.IntPredicate;
import java.util.function.IntUnaryOperator;

public interface IntStream {
    OptionalInt min();
    int sum();
    IntStream filter(IntPredicate predicate);
    IntStream map(IntUnaryOperator mapper);
    int[] toArray();

    static IntStream of(int value) { return new SequentialIntStream(Stream.of(value)); }
    static IntStream empty() { return new SequentialIntStream(Stream.empty()); }
    static IntStream range(int startInclusive, int endExclusive) {
        return new SequentialIntStream(new SequentialStream<>(() -> new Iterator<Integer>() {
            private int next = startInclusive;
            @Override public boolean hasNext() { return next < endExclusive; }
            @Override public Integer next() {
                if (!hasNext()) throw new NoSuchElementException();
                return next++;
            }
        }));
    }
}

final class SequentialIntStream implements IntStream {
    private final Stream<Integer> stream;

    SequentialIntStream(Stream<Integer> stream) { this.stream = stream; }

    @Override public OptionalInt min() {
        Iterator<Integer> cursor = stream.iterator();
        if (!cursor.hasNext()) return OptionalInt.empty();
        int min = cursor.next();
        while (cursor.hasNext()) min = Math.min(min, cursor.next());
        return OptionalInt.of(min);
    }

    @Override public int sum() {
        Iterator<Integer> cursor = stream.iterator();
        int sum = 0;
        while (cursor.hasNext()) sum += cursor.next();
        return sum;
    }

    @Override public IntStream filter(IntPredicate predicate) {
        Objects.requireNonNull(predicate);
        return new SequentialIntStream(stream.filter(value -> predicate.test(value)));
    }

    @Override public IntStream map(IntUnaryOperator mapper) {
        Objects.requireNonNull(mapper);
        return new SequentialIntStream(stream.map(value -> mapper.applyAsInt(value)));
    }

    @Override public int[] toArray() {
        var values = stream.toList();
        int[] result = new int[values.size()];
        for (int i = 0; i < result.length; i++) result[i] = values.get(i);
        return result;
    }
}
