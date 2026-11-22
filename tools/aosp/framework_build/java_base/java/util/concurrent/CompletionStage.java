package java.util.concurrent;

import java.util.function.Consumer;
import java.util.function.Function;

/** CompletionStage methods supported by the cooperative runtime. */
public interface CompletionStage<T> {
    <U> CompletionStage<U> thenApply(Function<? super T, ? extends U> transform);
    <U> CompletionStage<U> thenApplyAsync(Function<? super T, ? extends U> transform, Executor executor);
    <U> CompletionStage<U> thenCompose(Function<? super T, ? extends CompletionStage<U>> transform);
    CompletionStage<Void> thenAcceptAsync(Consumer<? super T> action, Executor executor);
    CompletionStage<Void> thenRunAsync(Runnable action, Executor executor);
    CompletableFuture<T> toCompletableFuture();
}
