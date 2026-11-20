package org.slf4j;

import java.util.HashMap;
import java.util.Map;
import org.slf4j.helpers.BasicMarkerFactory;
import org.slf4j.helpers.NOPMDCAdapter;
import org.slf4j.spi.MDCAdapter;
import org.slf4j.spi.SLF4JServiceProvider;

/*
 * mctick's TeaVM arm: slf4j's LoggerFactory without ServiceLoader discovery (and so without log4j). Shadows the
 * class in slf4j-api on the TeaVM class path only; the JVM arms log through log4j as the game does.
 */
public final class LoggerFactory {
    private static final Map<String, Logger> LOGGERS = new HashMap<>();
    private static final ILoggerFactory FACTORY = LoggerFactory::make;
    private static final SLF4JServiceProvider PROVIDER = new SLF4JServiceProvider() {
        private final IMarkerFactory markers = new BasicMarkerFactory();
        private final MDCAdapter mdc = new NOPMDCAdapter();

        @Override
        public ILoggerFactory getLoggerFactory() {
            return FACTORY;
        }

        @Override
        public IMarkerFactory getMarkerFactory() {
            return markers;
        }

        @Override
        public MDCAdapter getMDCAdapter() {
            return mdc;
        }

        @Override
        public String getRequestedApiVersion() {
            return "2.0.99";
        }

        @Override
        public void initialize() {
        }
    };

    private LoggerFactory() {
    }

    private static Logger make(String name) {
        return LOGGERS.computeIfAbsent(name, n -> wjbench.mctick.Platform.logger(n));
    }

    public static Logger getLogger(String name) {
        return make(name);
    }

    public static Logger getLogger(Class<?> clazz) {
        return make(clazz.getName());
    }

    public static ILoggerFactory getILoggerFactory() {
        return FACTORY;
    }

    static SLF4JServiceProvider getProvider() {
        return PROVIDER;
    }
}
