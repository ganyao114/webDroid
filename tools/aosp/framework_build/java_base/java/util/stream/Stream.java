package java.util.stream;

import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.Comparator;
import java.util.Iterator;
import java.util.List;
import java.util.NoSuchElementException;
import java.util.Objects;
import java.util.Optional;
import java.util.OptionalInt;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.function.ToIntFunction;

public interface Stream<T> {
    Iterator<T> iterator();
    <R> Stream<R> map(Function<? super T, ? extends R> mapper);
    <R> Stream<R> flatMap(Function<? super T, ? extends Stream<? extends R>> mapper);
    Stream<T> filter(Predicate<? super T> predicate);
    Stream<T> sorted(Comparator<? super T> order);
    Stream<T> skip(long count);
    Stream<T> limit(long count);
    IntStream mapToInt(ToIntFunction<? super T> mapper);
    <R, A> R collect(Collector<? super T, A, R> collector);
    void forEach(Consumer<? super T> action);
    default void forEachOrdered(Consumer<? super T> action) { forEach(action); }
    boolean anyMatch(Predicate<? super T> predicate);
    long count();
    Optional<T> findFirst();
    default Optional<T> findAny() { return findFirst(); }
    default List<T> toList() {
        ArrayList<T> values = new ArrayList<>();
        forEachOrdered(values::add);
        return Collections.unmodifiableList(values);
    }
    default boolean allMatch(Predicate<? super T> predicate) {
        Objects.requireNonNull(predicate);
        return !anyMatch(value -> !predicate.test(value));
    }

    @SafeVarargs static <T> Stream<T> of(T... values) { return fromArray(values); }
    static <T> Stream<T> of(T value) { return fromCollection(Collections.singletonList(value)); }
    static <T> Stream<T> empty() { return fromCollection(Collections.emptyList()); }
    static <T> Stream<T> concat(Stream<? extends T> first, Stream<? extends T> second) {
        Objects.requireNonNull(first);
        Objects.requireNonNull(second);
        Iterator<? extends T> left = first.iterator(), right = second.iterator();
        return new SequentialStream<>(() -> new Iterator<T>() {
            @Override public boolean hasNext() { return left.hasNext() || right.hasNext(); }
            @Override public T next() { return left.hasNext() ? left.next() : right.next(); }
        });
    }

    static <T> Stream<T> fromCollection(Collection<T> values) {
        return new SequentialStream<T>(values::iterator);
    }

    static <T> Stream<T> fromArray(T[] values) {
        Objects.requireNonNull(values);
        return new SequentialStream<T>(() -> new Iterator<T>() {
            private int index;
            @Override public boolean hasNext() { return index < values.length; }
            @Override public T next() {
                if (!hasNext()) throw new NoSuchElementException();
                return values[index++];
            }
        });
    }
}

final class SequentialStream<T> implements Stream<T> {
    private Supplier<Iterator<T>> source;

    SequentialStream(Supplier<Iterator<T>> source) { this.source = source; }

    private Supplier<Iterator<T>> consume() {
        if (source == null) throw new IllegalStateException("Stream has already been consumed");
        Supplier<Iterator<T>> result = source;
        source = null;
        return result;
    }

    @Override public Iterator<T> iterator() { return consume().get(); }

    @Override public <R> Stream<R> map(Function<? super T, ? extends R> mapper) {
        Objects.requireNonNull(mapper);
        Supplier<Iterator<T>> parent = consume();
        return new SequentialStream<>(() -> new Iterator<R>() {
            private final Iterator<T> cursor = parent.get();
            @Override public boolean hasNext() { return cursor.hasNext(); }
            @Override public R next() { return mapper.apply(cursor.next()); }
        });
    }

    @Override public <R> Stream<R> flatMap(Function<? super T, ? extends Stream<? extends R>> mapper) {
        Objects.requireNonNull(mapper);
        Supplier<Iterator<T>> parent = consume();
        return new SequentialStream<>(() -> new Iterator<R>() {
            private final Iterator<T> cursor = parent.get();
            private Iterator<? extends R> child;
            @Override public boolean hasNext() {
                while (child == null || !child.hasNext()) {
                    if (!cursor.hasNext()) return false;
                    Stream<? extends R> mapped = mapper.apply(cursor.next());
                    child = mapped == null ? null : mapped.iterator();
                }
                return true;
            }
            @Override public R next() {
                if (!hasNext()) throw new NoSuchElementException();
                return child.next();
            }
        });
    }

    @Override public Stream<T> filter(Predicate<? super T> predicate) {
        Objects.requireNonNull(predicate);
        Supplier<Iterator<T>> parent = consume();
        return new SequentialStream<>(() -> new Iterator<T>() {
            private final Iterator<T> cursor = parent.get();
            private T value;
            private boolean ready;
            @Override public boolean hasNext() {
                while (!ready && cursor.hasNext()) {
                    value = cursor.next();
                    ready = predicate.test(value);
                }
                return ready;
            }
            @Override public T next() {
                if (!hasNext()) throw new NoSuchElementException();
                ready = false;
                return value;
            }
        });
    }

    @Override public IntStream mapToInt(ToIntFunction<? super T> mapper) {
        Objects.requireNonNull(mapper);
        SequentialStream<T> upstream = new SequentialStream<>(consume());
        return new IntStream() {
            @Override public OptionalInt min() {
                Iterator<T> cursor = upstream.consume().get();
                if (!cursor.hasNext()) return OptionalInt.empty();
                int min = mapper.applyAsInt(cursor.next());
                while (cursor.hasNext()) min = Math.min(min, mapper.applyAsInt(cursor.next()));
                return OptionalInt.of(min);
            }

            @Override public int sum() {
                Iterator<T> cursor = upstream.consume().get();
                int sum = 0;
                while (cursor.hasNext()) sum += mapper.applyAsInt(cursor.next());
                return sum;
            }
        };
    }

    @Override public <R, A> R collect(Collector<? super T, A, R> collector) {
        A result = collector.supplier().get();
        var add = collector.accumulator();
        forEach(value -> add.accept(result, value));
        return collector.finisher().apply(result);
    }

    @Override public Stream<T> sorted(Comparator<? super T> order) {
        Objects.requireNonNull(order);
        Supplier<Iterator<T>> parent = consume();
        return new SequentialStream<>(() -> {
            ArrayList<T> values = new ArrayList<>();
            Iterator<T> cursor = parent.get();
            while (cursor.hasNext()) values.add(cursor.next());
            values.sort(order);
            return values.iterator();
        });
    }

    @Override public Stream<T> skip(long count) {
        if (count < 0) throw new IllegalArgumentException("Negative skip count");
        Supplier<Iterator<T>> parent = consume();
        return new SequentialStream<>(() -> {
            Iterator<T> cursor = parent.get();
            for (long i = 0; i < count && cursor.hasNext(); i++) cursor.next();
            return cursor;
        });
    }

    @Override public Stream<T> limit(long count) {
        if (count < 0) throw new IllegalArgumentException("Negative limit");
        Supplier<Iterator<T>> parent = consume();
        if (count == 0) return Stream.empty();
        return new SequentialStream<>(() -> new Iterator<T>() {
            private final Iterator<T> cursor = parent.get();
            private long remaining = count;
            @Override public boolean hasNext() { return remaining > 0 && cursor.hasNext(); }
            @Override public T next() {
                if (!hasNext()) throw new NoSuchElementException();
                remaining--;
                return cursor.next();
            }
        });
    }

    @Override public boolean anyMatch(Predicate<? super T> predicate) {
        Objects.requireNonNull(predicate);
        Iterator<T> cursor = consume().get();
        while (cursor.hasNext()) if (predicate.test(cursor.next())) return true;
        return false;
    }

    @Override public long count() {
        Iterator<T> cursor = consume().get();
        long count = 0;
        while (cursor.hasNext()) {
            cursor.next();
            count++;
        }
        return count;
    }

    @Override public Optional<T> findFirst() {
        Iterator<T> cursor = consume().get();
        return cursor.hasNext() ? Optional.of(cursor.next()) : Optional.empty();
    }

    @Override public void forEach(Consumer<? super T> action) {
        Objects.requireNonNull(action);
        Iterator<T> cursor = consume().get();
        while (cursor.hasNext()) action.accept(cursor.next());
    }
}
