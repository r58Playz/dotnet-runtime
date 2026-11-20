package io.netty.util.internal.logging;

/* mctick's TeaVM arm: netty logs through slf4j (mctick's console logger) without probing for log4j by class name. */
public abstract class InternalLoggerFactory {
    private static volatile InternalLoggerFactory defaultFactory;

    public InternalLoggerFactory() {
    }

    @SuppressWarnings("deprecation")
    public static InternalLoggerFactory getDefaultFactory() {
        if (defaultFactory == null) {
            defaultFactory = Slf4JLoggerFactory.INSTANCE;
        }
        return defaultFactory;
    }

    public static void setDefaultFactory(InternalLoggerFactory factory) {
        defaultFactory = factory;
    }

    public static InternalLogger getInstance(Class<?> clazz) {
        return getInstance(clazz.getName());
    }

    public static InternalLogger getInstance(String name) {
        return getDefaultFactory().newInstance(name);
    }

    protected abstract InternalLogger newInstance(String name);
}
