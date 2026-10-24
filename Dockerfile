FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends gcc make libc6-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src/ ./src/
RUN make

FROM debian:bookworm-slim
RUN groupadd --gid 10001 aicache && useradd --uid 10001 --gid 10001 --no-create-home aicache && mkdir /data && chown 10001:10001 /data
COPY --from=build /src/bin/kvstore /usr/local/bin/kvstore
COPY docker.conf /etc/aicachedb.conf
USER 10001:10001
VOLUME ["/data"]
EXPOSE 8888
ENTRYPOINT ["kvstore", "-c", "/etc/aicachedb.conf"]
