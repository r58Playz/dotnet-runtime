/*
 * Copyright 2013 The Netty Project
 *
 * The Netty Project licenses this file to you under the Apache License,
 * version 2.0 (the "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at:
 *
 *   https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

package io.netty.util.internal;

import java.util.HashMap;
import java.util.Map;

/*
 * wjharness mctick (TeaVM arm only): netty 4.2.15's TypeParameterMatcher without generic reflection. find() resolves a
 * handler's type parameter through Class.getTypeParameters / getGenericSuperclass, which TeaVM's linear-memory target
 * has no metadata for; here it answers the match-everything matcher. Minecraft's Connection (a
 * SimpleChannelInboundHandler<Packet<?>>) calls it from its constructor; mctick's NullConnection has no pipeline, so the
 * matcher is never consulted. Modified from the original; Apache License 2.0.
 */
public abstract class TypeParameterMatcher {

    private static final TypeParameterMatcher NOOP = new TypeParameterMatcher() {
        @Override
        public boolean match(Object msg) {
            return true;
        }
    };

    public static TypeParameterMatcher get(final Class<?> parameterType) {
        return parameterType == Object.class ? NOOP : new ReflectiveMatcher(parameterType);
    }

    public static TypeParameterMatcher find(
            final Object object, final Class<?> parametrizedSuperclass, final String typeParamName) {
        return NOOP;
    }

    public abstract boolean match(Object msg);

    private static final class ReflectiveMatcher extends TypeParameterMatcher {
        private final Class<?> type;

        ReflectiveMatcher(Class<?> type) {
            this.type = type;
        }

        @Override
        public boolean match(Object msg) {
            return type.isInstance(msg);
        }
    }

    TypeParameterMatcher() { }
}
