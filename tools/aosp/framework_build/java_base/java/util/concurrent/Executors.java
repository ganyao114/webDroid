package java.util.concurrent;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

/** A serial executor whose tasks are drained by the owning interpreter thread. */
public final class Executors {
    private Executors() {}

    public static ExecutorService newSingleThreadExecutor() { return new SerialExecutor(); }

    private static final class SerialExecutor implements ExecutorService {
        private final ArrayList<Runnable> queue = new ArrayList<>();
        private boolean shutdown, running;

        @Override public void execute(Runnable task) {
            Objects.requireNonNull(task);
            if (shutdown) throw new RejectedExecutionException("Executor is shut down");
            queue.add(task);
            if (running) return;
            running = true;
            try {
                while (!queue.isEmpty()) queue.remove(0).run();
            } finally {
                running = false;
            }
        }

        @Override public <T> Future<T> submit(Callable<T> task) {
            Task<T> future = new Task<>(Objects.requireNonNull(task));
            execute(future);
            return future;
        }

        @Override public <T> Future<T> submit(Runnable task, T value) {
            Objects.requireNonNull(task);
            return submit(() -> { task.run(); return value; });
        }

        @Override public Future<?> submit(Runnable task) { return submit(task, null); }
        @Override public void shutdown() { shutdown = true; }
        @Override public List<Runnable> shutdownNow() {
            shutdown = true;
            ArrayList<Runnable> pending = new ArrayList<>(queue);
            queue.clear();
            return pending;
        }
        @Override public boolean isShutdown() { return shutdown; }
        @Override public boolean isTerminated() { return shutdown && !running && queue.isEmpty(); }
        @Override public boolean awaitTermination(long timeout, TimeUnit unit) {
            Objects.requireNonNull(unit);
            return isTerminated();
        }
    }

    private static final class Task<T> implements Runnable, Future<T> {
        private Callable<T> callable;
        private T value;
        private Throwable failure;
        private boolean running, done, cancelled;

        Task(Callable<T> callable) { this.callable = callable; }

        @Override public void run() {
            if (done || running) return;
            running = true;
            try { value = callable.call(); }
            catch (Throwable error) { failure = error; }
            finally {
                callable = null;
                running = false;
                done = true;
            }
        }

        @Override public boolean cancel(boolean interrupt) {
            if (done || running) return false;
            callable = null;
            cancelled = done = true;
            return true;
        }
        @Override public boolean isCancelled() { return cancelled; }
        @Override public boolean isDone() { return done; }

        private T result() throws ExecutionException {
            if (cancelled) throw new CancellationException();
            if (failure != null) throw new ExecutionException(failure);
            return value;
        }

        @Override public T get() throws ExecutionException {
            if (!done) throw new IllegalStateException("Cannot wait for a queued task on its executor");
            return result();
        }

        @Override public T get(long timeout, TimeUnit unit) throws ExecutionException, TimeoutException {
            Objects.requireNonNull(unit);
            if (!done) throw new TimeoutException();
            return result();
        }
    }
}
