package org.slf4j;

import org.slf4j.helpers.BasicMarkerFactory;

/* mctick's TeaVM arm: no provider discovery (see LoggerFactory). */
public class MarkerFactory {
    static IMarkerFactory markerFactory = new BasicMarkerFactory();

    private MarkerFactory() {
    }

    public static Marker getMarker(String name) {
        return markerFactory.getMarker(name);
    }

    public static Marker getDetachedMarker(String name) {
        return markerFactory.getDetachedMarker(name);
    }

    public static IMarkerFactory getIMarkerFactory() {
        return markerFactory;
    }
}
