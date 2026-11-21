package java.util.stream;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.function.BiConsumer;
import java.util.function.Function;
import java.util.function.Supplier;

public final class Collectors {
    private Collectors() {}

    public static <T> Collector<T, ?, List<T>> toList() {
        return new Accumulating<>(ArrayList::new, List::add);
    }

    public static <T> Collector<T, ?, Set<T>> toSet() {
        return new Accumulating<>(HashSet::new, Set::add);
    }

    private static final class Accumulating<T, C> implements Collector<T, C, C> {
        private final Supplier<C> supplier;
        private final BiConsumer<C, T> add;
        Accumulating(Supplier<C> supplier, BiConsumer<C, T> add) {
            this.supplier = supplier;
            this.add = add;
        }
        @Override public Supplier<C> supplier() { return supplier; }
        @Override public BiConsumer<C, T> accumulator() { return add; }
        @Override public Function<C, C> finisher() { return value -> value; }
    }
}
