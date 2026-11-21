package java.util.concurrent;

@FunctionalInterface
public interface Executor {
    void execute(Runnable command);
}
