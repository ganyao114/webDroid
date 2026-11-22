package java.util.stream;

import java.util.Objects;
import java.util.Spliterator;
import java.util.Spliterators;

public final class StreamSupport {
    private StreamSupport() {}

    public static <T> Stream<T> stream(Spliterator<T> source, boolean parallel) {
        Objects.requireNonNull(source);
        // The embedded interpreter traverses streams on one thread.
        return new SequentialStream<>(() -> Spliterators.iterator(source));
    }
}
