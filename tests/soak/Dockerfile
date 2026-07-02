# Concurrency soak of the REAL mod_audio_fork AudioPipe under ASan + TSan.
# Build context MUST be the repo root so the live module sources are used:
#
#   docker build -f tests/soak/Dockerfile -t audiopipe-soak .
#   docker run --rm --security-opt seccomp=unconfined audiopipe-soak
#
# AudioPipe depends only on libwebsockets + std (no FreeSWITCH), so this drives
# the real lws service thread, a media-thread writer, and the teardown/reaper
# concurrently against a mock ws server. seccomp=unconfined lets setarch/TSan run.
FROM debian:trixie
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ libwebsockets-dev pkg-config ca-certificates util-linux \
    python3 python3-websockets && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/soak
# the REAL module source (build context = repo root)
COPY modules/mod_audio_fork/audio_pipe.cpp modules/mod_audio_fork/audio_pipe.hpp ./
COPY tests/soak/soak_audiopipe.cpp tests/soak/ws_mock.py tests/soak/run.sh ./
RUN g++ -std=c++11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
        soak_audiopipe.cpp audio_pipe.cpp $(pkg-config --cflags --libs libwebsockets) -lpthread -o soak_asan
RUN g++ -std=c++11 -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
        soak_audiopipe.cpp audio_pipe.cpp $(pkg-config --cflags --libs libwebsockets) -lpthread -o soak_tsan
# deepgram AudioPipe variant (same lws transport lineage; TLS-only dialer, so
# every connect terminates via CONNECT_FAIL against the plain-ws mock -- see
# soak_deepgram.cpp for exactly what that does and does not exercise)
COPY modules/mod_deepgram_transcribe/audio_pipe.cpp modules/mod_deepgram_transcribe/audio_pipe.hpp tests/soak/soak_deepgram.cpp dg/
COPY tests/soak/tsan.supp ./
RUN g++ -std=c++11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
        dg/soak_deepgram.cpp dg/audio_pipe.cpp $(pkg-config --cflags --libs libwebsockets) -lpthread -o soak_dg_asan
RUN g++ -std=c++11 -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
        dg/soak_deepgram.cpp dg/audio_pipe.cpp $(pkg-config --cflags --libs libwebsockets) -lpthread -o soak_dg_tsan
RUN chmod +x run.sh
CMD ["./run.sh"]
