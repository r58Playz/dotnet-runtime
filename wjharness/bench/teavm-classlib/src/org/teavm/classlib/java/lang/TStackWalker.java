package org.teavm.classlib.java.lang;

import java.util.Set;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.stream.Stream;

/* No stack walking on TeaVM: an empty stack; getCallerClass() answers Object (logger names, Tracy zone names). */
public final class TStackWalker {
    private static final TStackWalker INSTANCE = new TStackWalker();

    public enum Option {
        RETAIN_CLASS_REFERENCE,
        SHOW_REFLECT_FRAMES,
        SHOW_HIDDEN_FRAMES
    }

    public interface StackFrame {
        String getClassName();

        String getMethodName();

        Class<?> getDeclaringClass();

        int getLineNumber();

        String getFileName();
    }

    private TStackWalker() {
    }

    public static TStackWalker getInstance() {
        return INSTANCE;
    }

    public static TStackWalker getInstance(Option option) {
        return INSTANCE;
    }

    public static TStackWalker getInstance(Set<Option> options) {
        return INSTANCE;
    }

    public static TStackWalker getInstance(Set<Option> options, int estimateDepth) {
        return INSTANCE;
    }

    public <T> T walk(Function<? super Stream<StackFrame>, ? extends T> function) {
        return function.apply(Stream.empty());
    }

    public void forEach(Consumer<? super StackFrame> action) {
    }

    public Class<?> getCallerClass() {
        return Object.class;
    }
}
