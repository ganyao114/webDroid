package java.util.concurrent;

import java.util.ArrayList;
import java.util.Objects;
import java.util.function.Consumer;
import java.util.function.Function;

/** Completion stages on a cooperative interpreter thread. */
public class CompletableFuture<T> implements Future<T> {
    private final ArrayList<Runnable> continuations = new ArrayList<>();
    private boolean done;
    private T value;
    private Throwable failure;

    public static <T> CompletableFuture<T> completedFuture(T value) {
        CompletableFuture<T> future = new CompletableFuture<>();
        future.complete(value);
        return future;
    }
    public boolean complete(T value) { return finish(value, null); }
    public boolean completeExceptionally(Throwable failure) {
        return finish(null, Objects.requireNonNull(failure));
    }
    private boolean finish(T value, Throwable failure) {
        if (done) return false;
        this.value = value;
        this.failure = failure;
        done = true;
        for (Runnable task : continuations) task.run();
        continuations.clear();
        return true;
    }
    @Override public boolean cancel(boolean interrupt) {
        return finish(null, new CancellationException()) || isCancelled();
    }
    @Override public boolean isCancelled() { return failure instanceof CancellationException; }
    @Override public boolean isDone() { return done; }

    @Override public T get() throws ExecutionException {
        if (!done) throw new IllegalStateException("Waiting would block the interpreter thread");
        if (isCancelled()) throw (CancellationException) failure;
        if (failure != null) throw new ExecutionException(failure);
        return value;
    }
    @Override public T get(long timeout, TimeUnit unit) throws ExecutionException, TimeoutException {
        Objects.requireNonNull(unit);
        if (!done) throw new TimeoutException();
        return get();
    }
    public T join() {
        if (!done) throw new IllegalStateException("Waiting would block the interpreter thread");
        if (isCancelled()) throw (CancellationException) failure;
        if (failure instanceof CompletionException completion) throw completion;
        if (failure != null) throw new CompletionException(failure);
        return value;
    }
    public T getNow(T fallback) { return done ? join() : fallback; }

    private void onComplete(Runnable continuation) {
        if (done) continuation.run();
        else continuations.add(continuation);
    }

    private static Throwable propagatedFailure(Throwable error) {
        return error == null || error instanceof CompletionException ? error : new CompletionException(error);
    }

    public <U> CompletableFuture<U> thenApply(Function<? super T, ? extends U> transform) {
        return thenApplyAsync(transform, Runnable::run);
    }
    public <U> CompletableFuture<U> thenApplyAsync(Function<? super T, ? extends U> transform, Executor executor) {
        Objects.requireNonNull(transform);
        Objects.requireNonNull(executor);
        CompletableFuture<U> next = new CompletableFuture<>();
        Runnable continuation = () -> {
            if (failure != null) {
                next.completeExceptionally(propagatedFailure(failure));
                return;
            }
            try {
                executor.execute(() -> {
                    try {
                        next.complete(transform.apply(value));
                    } catch (Throwable error) {
                        next.completeExceptionally(error);
                    }
                });
            } catch (RuntimeException error) {
                next.completeExceptionally(error);
            }
        };
        onComplete(continuation);
        return next;
    }
    public <U> CompletableFuture<U> thenCompose(
            Function<? super T, ? extends CompletableFuture<U>> transform) {
        Objects.requireNonNull(transform);
        CompletableFuture<U> next = new CompletableFuture<>();
        onComplete(() -> {
            if (failure != null) {
                next.completeExceptionally(propagatedFailure(failure));
                return;
            }
            try {
                CompletableFuture<U> stage = Objects.requireNonNull(transform.apply(value));
                stage.onComplete(() -> next.finish(stage.value, propagatedFailure(stage.failure)));
            } catch (Throwable error) {
                next.completeExceptionally(error);
            }
        });
        return next;
    }
    public CompletableFuture<Void> thenAcceptAsync(Consumer<? super T> consumer, Executor executor) {
        Objects.requireNonNull(consumer);
        return thenApplyAsync(result -> { consumer.accept(result); return null; }, executor);
    }
    public CompletableFuture<Void> thenRunAsync(Runnable action, Executor executor) {
        Objects.requireNonNull(action);
        return thenAcceptAsync(result -> action.run(), executor);
    }
}
