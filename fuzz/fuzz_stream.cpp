// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");

#include "fuzz_util.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <streambuf>
#include <string>

using ByteDance::pjson;
using ByteDance::pJsonParser;

namespace {

    // Chunk-limited stream adapter.

    // Exposes an in-memory string through refills no larger than the requested chunk size.
    class ChunkedBuffer : public std::streambuf {
    public:
        // Starts with an empty get area; the harness keeps chunkSize within the 32-byte buffer.
        ChunkedBuffer(const std::string& input, size_t chunkSize)
                : _input(input)
                , _chunkSize(chunkSize)
                , _position(0) {
            setg(_buffer, _buffer, _buffer);
        }

    protected:
        // Refills the get area with the next bounded chunk, or reports end of input.
        int_type underflow() override {
            if (_position >= _input.size())
                return traits_type::eof();
            const size_t count = std::min(_chunkSize, _input.size() - _position);
            for (size_t i = 0; i < count; ++i)
                _buffer[i] = _input[_position + i];
            _position += count;
            setg(_buffer, _buffer, _buffer + static_cast<std::ptrdiff_t>(count));
            return traits_type::to_int_type(*gptr());
        }

    private:
        const std::string& _input;
        size_t _chunkSize;
        size_t _position;
        char _buffer[32];
    };

    // Owns the chunking stream buffer and installs it on an input-stream facade.
    class ChunkedStream : public std::istream {
    public:
        // Attaches the fully constructed chunking buffer to the otherwise bufferless base stream.
        ChunkedStream(const std::string& input, size_t chunkSize)
                : std::istream(nullptr)
                , _buffer(input, chunkSize) {
            rdbuf(&_buffer);
        }

    private:
        ChunkedBuffer _buffer;
    };

    // Order-sensitive SAX event fingerprinting.

    // Records both event count and content so buffered and streamed SAX traces can be compared.
    // The small methods below all fold a distinct event tag plus any payload
    // into the same order-sensitive digest and always continue parsing.
    struct DigestHandler : pJsonParser::SaxHandler {
        DigestHandler()
                : digest(1469598103934665603ULL)
                , events(0) {}

        uint64_t digest;
        size_t events;

        void mix(uint64_t value) {
            digest ^= value;
            digest *= 1099511628211ULL;
        }

        // Include length so different adjacent strings cannot produce the same byte stream.
        void mixString(const std::string& value) {
            for (size_t i = 0; i < value.size(); ++i)
                mix(static_cast<unsigned char>(value[i]));
            mix(value.size());
        }

        bool mark(uint64_t tag) {
            ++events;
            mix(tag);
            return true;
        }

        bool onNull() override { return mark(1); }

        bool onBool(bool value) override { return mark(value ? 3 : 2); }

        bool onInt(int64_t value) override {
            mark(4);
            mix(static_cast<uint64_t>(value));
            return true;
        }

        bool onUInt(uint64_t value) override {
            mark(7);
            mix(value);
            return true;
        }

        // Canonical serialization gives floating-point values a stable byte representation.
        bool onDouble(double value) override {
            mark(5);
            pjson number;
            number = value;
            mixString(number.toString());
            return true;
        }

        bool onString(const std::string& value) override {
            mark(6);
            mixString(value);
            return true;
        }

        bool onStartArray() override { return mark(7); }

        bool onEndArray() override { return mark(8); }

        bool onStartObject() override { return mark(9); }

        // Keys use their own tag so they cannot collide with ordinary strings.
        bool onKey(const std::string& value) override {
            mark(10);
            mixString(value);
            return true;
        }

        bool onEndObject() override { return mark(11); }
    };

    // Cross-interface parser consistency.

    // Compares contiguous and chunked DOM/SAX parsing for one
    // option variant.
    void exerciseStreams(const uint8_t* data, size_t size, const std::string& input,
                         size_t chunkSize, size_t variantOffset) {
        const pJsonParser::Options options =
            pjson_fuzz::parseOptionsVariant(data, size, variantOffset);

        // Contiguous DOM parsing provides the baseline status and value.
        pJsonParser::Error bufferError;
        pjson buffered = pJsonParser(options).parse(input.c_str(), input.size(), bufferError);

        // Chunk boundaries must not affect DOM acceptance or represented value.
        ChunkedStream domInput(input, chunkSize);
        pJsonParser::Error streamError;
        pjson streamed = pJsonParser(options).parseStream(domInput, streamError);
        pjson_fuzz::require(bufferError.ok == streamError.ok);
        if (bufferError.ok)
            pjson_fuzz::require(buffered == streamed);

        // Capture the SAX trace from the same contiguous baseline input.
        DigestHandler bufferHandler;
        pJsonParser::Error saxBufferError;
        const bool saxBuffer = pJsonParser(options).parseSax(input.c_str(), input.size(),
                                                             bufferHandler, saxBufferError);
        pjson_fuzz::require(saxBuffer == saxBufferError.ok);

        // Streamed SAX parsing must agree on status, event count, order, and payloads.
        ChunkedStream saxInput(input, chunkSize);
        DigestHandler streamHandler;
        pJsonParser::Error saxStreamError;
        const bool saxStream =
            pJsonParser(options).parseSaxStream(saxInput, streamHandler, saxStreamError);
        pjson_fuzz::require(saxStream == saxStreamError.ok);
        pjson_fuzz::require(saxBuffer == saxStream);
        pjson_fuzz::require(bufferError.ok == saxBuffer);
        // Failure can be detected before any callbacks for a bounded in-memory
        // input but only after prefix callbacks for a stream, so compare traces
        // only when both parsers consumed the complete document successfully.
        if (saxBuffer) {
            pjson_fuzz::require(bufferHandler.events == streamHandler.events);
            pjson_fuzz::require(bufferHandler.digest == streamHandler.digest);
        }
    }

} // namespace

// libFuzzer entry point.

// Derives a safe 1..32-byte chunk size and tests several
// duplicate-policy/resource-budget variants.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > pjson_fuzz::kMaxInputBytes)
        return 0;
    const std::string input(pjson_fuzz::bytes(data, size), size);
    const size_t chunkSize = size == 0 ? 1U : static_cast<size_t>(data[0] % 32U) + 1U;
    exerciseStreams(data, size, input, chunkSize, 0U);
    exerciseStreams(data, size, input, chunkSize, 4U);
    exerciseStreams(data, size, input, chunkSize, 8U);
    return 0;
}
