# The toolchain the shipped binaries are built with.
#
# musl rather than glibc because the agent is copied to someone else's host and
# has to run there: statically linked glibc still resolves names through NSS,
# which needs a matching glibc present at runtime. musl has no such split.
FROM alpine:3.22

RUN apk add --no-cache \
      clang20 clang20-extra-tools lld llvm20 compiler-rt \
      libc++-static libc++-dev llvm-libunwind-static \
      musl-dev cmake ninja ccache bash git
