# wjharness build/ikvm-java-linux.sh. IKVM.Java (the Java 25 class library IKVM compiles from OpenJDK's own build) needs a LINUX host: IKVM.OpenJdk.Make
# configures OpenJDK for the host, and a macOS host builds the macOS flavour of java.base. linux/amd64, matching the
# Linux box's x86_64 build (its generated platform constants), run under Rosetta on the M4.
FROM --platform=linux/amd64 mcr.microsoft.com/dotnet/sdk:10.0
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      autoconf make zip unzip file cpio gcc g++ git curl ca-certificates \
      libx11-dev libxext-dev libxrender-dev libxrandr-dev libxtst-dev libxt-dev libcups2-dev \
      libfontconfig1-dev libasound2-dev libfreetype-dev \
  && rm -rf /var/lib/apt/lists/*
RUN mkdir -p /opt/jdk25 && curl -fsSL "https://api.adoptium.net/v3/binary/latest/25/ga/linux/x64/jdk/hotspot/normal/eclipse" \
      | tar xz -C /opt/jdk25 --strip-components=1 && /opt/jdk25/bin/java -version
ENV JAVA_HOME=/opt/jdk25 OpenJdkBootJdk=/opt/jdk25
