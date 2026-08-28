# SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
# SPDX-License-Identifier: Apache-2.0

# Use OSS-Fuzz's standard compiler, sanitizer, and helper environment.
FROM gcr.io/oss-fuzz-base/base-builder

# A shallow checkout keeps the builder image small while providing the source
# tree that OSS-Fuzz compiles through build.sh.
RUN git clone --depth 1 https://github.com/Pico-Developer/pjson.git pjson
WORKDIR pjson

# OSS-Fuzz discovers the project build entry point at $SRC/build.sh.
COPY build.sh $SRC/
