package java.util.stream;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
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

    public static <T, K, U> Collector<T, ?, Map<K, U>> toMap(
            Function<? super T, ? extends K> keyMapper, Function<? super T, ? extends U> valueMapper) {
        Objects.requireNonNull(keyMapper);
        Objects.requireNonNull(valueMapper);
        return new Accumulating<>(HashMap::new, (values, item) -> {
            K key = keyMapper.apply(item);
            U value = Objects.requireNonNull(valueMapper.apply(item));
            if (values.putIfAbsent(key, value) != null) throw new IllegalStateException("Duplicate key " + key);
        });
    }

    public static <T, U, A, R> Collector<T, ?, R> mapping(
            Function<? super T, ? extends U> mapper, Collector<? super U, A, R> downstream) {
        Objects.requireNonNull(mapper);
        BiConsumer<A, ? super U> add = downstream.accumulator();
        return new Collector<T, A, R>() {
            @Override public Supplier<A> supplier() { return downstream.supplier(); }
            @Override public BiConsumer<A, T> accumulator() { return (result, item) -> add.accept(result, mapper.apply(item)); }
            @Override public Function<A, R> finisher() { return downstream.finisher(); }
        };
    }

    public static <T, K, A, D> Collector<T, ?, Map<K, D>> groupingBy(
            Function<? super T, ? extends K> classifier, Collector<? super T, A, D> downstream) {
        Objects.requireNonNull(classifier);
        Supplier<A> create = downstream.supplier();
        BiConsumer<A, ? super T> add = downstream.accumulator();
        Function<A, D> finish = downstream.finisher();
        return new Collector<T, Map<K, A>, Map<K, D>>() {
            @Override public Supplier<Map<K, A>> supplier() { return HashMap::new; }
            @Override public BiConsumer<Map<K, A>, T> accumulator() {
                return (groups, item) -> {
                    K key = Objects.requireNonNull(classifier.apply(item));
                    add.accept(groups.computeIfAbsent(key, ignored -> create.get()), item);
                };
            }
            @Override public Function<Map<K, A>, Map<K, D>> finisher() {
                return groups -> {
                    Map<K, D> result = new HashMap<>();
                    groups.forEach((key, values) -> result.put(key, finish.apply(values)));
                    return result;
                };
            }
        };
    }

    public static Collector<CharSequence, ?, String> joining(CharSequence delimiter) {
        String separator = Objects.requireNonNull(delimiter).toString();
        return new Collector<CharSequence, JoinedText, String>() {
            @Override public Supplier<JoinedText> supplier() { return JoinedText::new; }
            @Override public BiConsumer<JoinedText, CharSequence> accumulator() {
                return (joined, value) -> {
                    if (joined.hasValue) joined.text.append(separator);
                    joined.text.append(value);
                    joined.hasValue = true;
                };
            }
            @Override public Function<JoinedText, String> finisher() { return joined -> joined.text.toString(); }
        };
    }

    private static final class JoinedText {
        private final StringBuilder text = new StringBuilder();
        private boolean hasValue;
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
