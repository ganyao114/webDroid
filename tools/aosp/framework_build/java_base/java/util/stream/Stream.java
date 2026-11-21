package java.util.stream;

import java.util.Collection;
import java.util.Iterator;
import java.util.NoSuchElementException;
import java.util.Objects;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.Supplier;

public interface Stream<T> {
    <R> Stream<R> map(Function<? super T, ? extends R> mapper);
    Stream<T> filter(Predicate<? super T> predicate);
    <R, A> R collect(Collector<? super T, A, R> collector);
    void forEach(Consumer<? super T> action);

    static <T> Stream<T> fromCollection(Collection<T> values) {
        return new SequentialStream<T>(values::iterator);
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

    @Override public <R> Stream<R> map(Function<? super T, ? extends R> mapper) {
        Objects.requireNonNull(mapper);
        Supplier<Iterator<T>> parent = consume();
        return new SequentialStream<>(() -> new Iterator<R>() {
            private final Iterator<T> cursor = parent.get();
            @Override public boolean hasNext() { return cursor.hasNext(); }
            @Override public R next() { return mapper.apply(cursor.next()); }
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

    @Override public <R, A> R collect(Collector<? super T, A, R> collector) {
        A result = collector.supplier().get();
        var add = collector.accumulator();
        forEach(value -> add.accept(result, value));
        return collector.finisher().apply(result);
    }

    @Override public void forEach(Consumer<? super T> action) {
        Objects.requireNonNull(action);
        Iterator<T> cursor = consume().get();
        while (cursor.hasNext()) action.accept(cursor.next());
    }
}
