package java.util.stream;

import java.util.function.BiConsumer;
import java.util.function.Function;
import java.util.function.Supplier;

public interface Collector<T, A, R> {
    Supplier<A> supplier();
    BiConsumer<A, T> accumulator();
    Function<A, R> finisher();
}
