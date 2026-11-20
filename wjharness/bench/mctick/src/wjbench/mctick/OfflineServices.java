package wjbench.mctick;

import com.mojang.authlib.GameProfile;
import java.util.HashSet;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import net.minecraft.server.Services;
import net.minecraft.server.players.NameAndId;
import net.minecraft.server.players.ProfileResolver;
import net.minecraft.server.players.UserNameToIdResolver;
import com.mojang.authlib.yggdrasil.ServicesKeySet;

/* No Mojang services: GameTestServer's NO_SERVICES (offline names, no session service, no profile lookups). */
final class OfflineServices {
    private OfflineServices() {
    }

    static Services create() {
        return new Services(null, ServicesKeySet.EMPTY, null, new Names(), new Profiles());
    }

    private static final class Names implements UserNameToIdResolver {
        private final Set<NameAndId> known = new HashSet<>();

        @Override
        public void add(NameAndId nameAndId) {
            known.add(nameAndId);
        }

        @Override
        public Optional<NameAndId> get(String name) {
            for (NameAndId n : known) {
                if (n.name().equals(name)) {
                    return Optional.of(n);
                }
            }
            return Optional.of(NameAndId.createOffline(name));
        }

        @Override
        public Optional<NameAndId> get(UUID id) {
            for (NameAndId n : known) {
                if (n.id().equals(id)) {
                    return Optional.of(n);
                }
            }
            return Optional.empty();
        }

        @Override
        public void resolveOfflineUsers(boolean value) {
        }

        @Override
        public void save() {
        }
    }

    private static final class Profiles implements ProfileResolver {
        @Override
        public Optional<GameProfile> fetchByName(String name) {
            return Optional.empty();
        }

        @Override
        public Optional<GameProfile> fetchById(UUID id) {
            return Optional.empty();
        }
    }
}
