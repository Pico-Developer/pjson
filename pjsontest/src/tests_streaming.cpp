//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
// Streaming SAX parsing and direct ostream serialization tests.
//
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"

#include <new>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>

using namespace ByteDance;
using pjson_test::parse;
using pjson_test::valueInt;

namespace {

    // Captures the SAX callback stream in a compact form suitable for exact ordering checks.
    struct RecordingHandler : pjson::SaxHandler {
        std::vector<std::string> events;

        bool onNull() override {
            events.push_back("null");
            return true;
        }
        bool onBool(bool value) override {
            events.push_back(value ? "bool:true" : "bool:false");
            return true;
        }
        bool onInt(int64_t value) override {
            events.push_back("int:" + std::to_string(value));
            return true;
        }
        bool onDouble(double value) override {
            pjson tmp;
            tmp = value;
            events.push_back("double:" + tmp.toString());
            return true;
        }
        bool onString(const std::string& value) override {
            events.push_back("string:" + value);
            return true;
        }
        bool onStartArray() override {
            events.push_back("start-array");
            return true;
        }
        bool onEndArray() override {
            events.push_back("end-array");
            return true;
        }
        bool onStartObject() override {
            events.push_back("start-object");
            return true;
        }
        bool onKey(const std::string& key) override {
            events.push_back("key:" + key);
            return true;
        }
        bool onEndObject() override {
            events.push_back("end-object");
            return true;
        }
    };

    // Records callbacks normally, then asks the parser to cancel after a fixed event budget.
    struct CancelAfterNHandler : RecordingHandler {
        explicit CancelAfterNHandler(size_t limit)
                : remaining(limit) {}

        size_t remaining;

        bool allow() {
            if (remaining == 0)
                return false;
            --remaining;
            return true;
        }

        bool onNull() override {
            RecordingHandler::onNull();
            return allow();
        }
        bool onBool(bool value) override {
            RecordingHandler::onBool(value);
            return allow();
        }
        bool onInt(int64_t value) override {
            RecordingHandler::onInt(value);
            return allow();
        }
        bool onDouble(double value) override {
            RecordingHandler::onDouble(value);
            return allow();
        }
        bool onString(const std::string& value) override {
            RecordingHandler::onString(value);
            return allow();
        }
        bool onStartArray() override {
            RecordingHandler::onStartArray();
            return allow();
        }
        bool onEndArray() override {
            RecordingHandler::onEndArray();
            return allow();
        }
        bool onStartObject() override {
            RecordingHandler::onStartObject();
            return allow();
        }
        bool onKey(const std::string& key) override {
            RecordingHandler::onKey(key);
            return allow();
        }
        bool onEndObject() override {
            RecordingHandler::onEndObject();
            return allow();
        }
    };

    // Simulates a consumer exception from inside a SAX callback.
    struct ThrowingHandler : RecordingHandler {
        bool onKey(const std::string& key) override {
            RecordingHandler::onKey(key);
            throw std::runtime_error("boom");
        }
    };

    struct BadAllocHandler : RecordingHandler {
        bool onString(const std::string&) override { throw std::bad_alloc(); }
    };

    struct NumberHandler : pjson::SaxHandler {
        bool sawDouble = false;
        double value = 1.0;

        bool onDouble(double number) override {
            sawDouble = true;
            value = number;
            return true;
        }
    };

    // Exposes an input string in small refills to force tokens across stream-buffer boundaries.
    class ChunkedStreamBuf : public std::streambuf {
    public:
        ChunkedStreamBuf(const std::string& src, size_t chunk)
                : _src(src)
                , _chunk(chunk)
                , _pos(0) {
            setg(_buffer, _buffer, _buffer);
        }

    protected:
        int_type underflow() override {
            if (_pos >= _src.size())
                return traits_type::eof();
            const size_t n = std::min(_chunk, _src.size() - _pos);
            for (size_t i = 0; i < n; ++i)
                _buffer[i] = _src[_pos + i];
            _pos += n;
            setg(_buffer, _buffer, _buffer + static_cast<std::ptrdiff_t>(n));
            return traits_type::to_int_type(*gptr());
        }

    private:
        // Own fixture bytes so construction from a temporary string remains
        // valid for the stream's complete lifetime.
        const std::string _src;
        size_t _chunk;
        size_t _pos;
        char _buffer[32];
    };

    // Owns the chunking buffer for the std::istream interface consumed by parseSaxStream().
    struct ChunkedIStream : std::istream {
        ChunkedIStream(const std::string& src, size_t chunk)
                : std::istream(nullptr)
                , _buf(src, chunk) {
            rdbuf(&_buf);
        }

    private:
        ChunkedStreamBuf _buf;
    };

    // Accepts at most `limit` output bytes, then reports a short write to its ostream.
    class FailingStreamBuf : public std::stringbuf {
    public:
        explicit FailingStreamBuf(size_t limit)
                : _limit(limit)
                , _written(0) {}

    protected:
        std::streamsize xsputn(const char* s, std::streamsize count) override {
            const std::streamsize room =
                static_cast<std::streamsize>(_limit > _written ? _limit - _written : 0);
            const std::streamsize n = room < count ? room : count;
            if (n > 0) {
                _written += static_cast<size_t>(n);
                return std::stringbuf::xsputn(s, n);
            }
            return 0;
        }

        int_type overflow(int_type ch) override {
            if (traits_type::eq_int_type(ch, traits_type::eof()))
                return traits_type::not_eof(ch);
            if (_written >= _limit)
                return traits_type::eof();
            ++_written;
            return std::stringbuf::overflow(ch);
        }

    private:
        size_t _limit;
        size_t _written;
    };

    // Owns FailingStreamBuf so writer failure propagation can be tested through std::ostream.
    struct FailingOStream : std::ostream {
        explicit FailingOStream(size_t limit)
                : std::ostream(nullptr)
                , _buf(limit) {
            rdbuf(&_buf);
        }

    private:
        FailingStreamBuf _buf;
    };
} // namespace

//===----------------------------------------------------------------------===//
// SAX event ordering, incremental input, cancellation, and diagnostics
//===----------------------------------------------------------------------===//

TEST(streaming_sax_scalar_events) {
    RecordingHandler h;
    CHECK(pjson::parseSax(" [null,true,false,1,2.5,\"x\"] ", h));
    CHECK_EQ(h.events.size(), size_t(8));
    CHECK_EQ(h.events[0], std::string("start-array"));
    CHECK_EQ(h.events[1], std::string("null"));
    CHECK_EQ(h.events[2], std::string("bool:true"));
    CHECK_EQ(h.events[3], std::string("bool:false"));
    CHECK_EQ(h.events[4], std::string("int:1"));
    CHECK_EQ(h.events[5], std::string("double:2.5"));
    CHECK_EQ(h.events[6], std::string("string:x"));
    CHECK_EQ(h.events[7], std::string("end-array"));
}

TEST(streaming_sax_object_order_and_empty_containers) {
    RecordingHandler h;
    CHECK(pjson::parseSax("{\"a\":{},\"b\":[],\"c\":{\"d\":[1]}}", h));
    const std::vector<std::string> want = {
        "start-object", "key:a",     "start-object", "end-object",   "key:b",
        "start-array",  "end-array", "key:c",        "start-object", "key:d",
        "start-array",  "int:1",     "end-array",    "end-object",   "end-object"};
    CHECK_EQ(h.events.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i)
        CHECK_EQ(h.events[i], want[i]);
}

TEST(streaming_sax_chunked_stream_boundaries) {
    const std::string doc = "{\"msg\":\"hello\",\"arr\":[1,2,3],\"nested\":{\"ok\":true}}";
    ChunkedIStream in(doc, 1);
    RecordingHandler h;
    pjson::ParseError err;
    CHECK(pjson::parseSaxStream(in, h, err));
    CHECK(err.ok);
    CHECK_EQ(h.events.front(), std::string("start-object"));
    CHECK_EQ(h.events.back(), std::string("end-object"));
    CHECK(std::find(h.events.begin(), h.events.end(), std::string("string:hello")) !=
          h.events.end());
    CHECK(std::find(h.events.begin(), h.events.end(), std::string("bool:true")) != h.events.end());
}

TEST(streaming_sax_utf8_escape_and_number_chunk_boundaries) {
    const std::string doc = "{\"utf8\":\"\xC3\xA9\",\"escaped\":\"\\uD83D\\uDE00\","
                            "\"number\":-12.5e+3}";
    for (size_t chunk = 1; chunk <= 4; ++chunk) {
        ChunkedIStream in(doc, chunk);
        RecordingHandler h;
        pjson::ParseError err;
        CHECK(pjson::parseSaxStream(in, h, err));
        CHECK(err.ok);
        CHECK(std::find(h.events.begin(), h.events.end(), std::string("string:\xC3\xA9")) !=
              h.events.end());
        CHECK(std::find(h.events.begin(), h.events.end(), std::string("string:\xF0\x9F\x98\x80")) !=
              h.events.end());
        CHECK(std::find(h.events.begin(), h.events.end(), std::string("double:-12500.0")) !=
              h.events.end());
    }
}

TEST(streaming_sax_crlf_split_reports_coordinates) {
    const std::string doc = "{\r\n\"a\": [1,\r\n]}";
    ChunkedIStream in(doc, 1);
    RecordingHandler h;
    pjson::ParseError err;
    CHECK(!pjson::parseSaxStream(in, h, err));
    CHECK_EQ(err.line, size_t(3));
    CHECK_EQ(err.column, size_t(1));
}

TEST(streaming_sax_duplicate_key_policies) {
    const std::string doc = "{\"a\":1,\"a\":2}";

    RecordingHandler keepFirst;
    pjson::ParseOptions first;
    first.duplicateKeys = pjson::ParseOptions::KeepFirstDuplicate;
    CHECK(pjson::parseSax(doc, keepFirst, first));
    const std::vector<std::string> wantFirst = {"start-object", "key:a", "int:1", "end-object"};
    CHECK_EQ(keepFirst.events.size(), wantFirst.size());
    for (size_t i = 0; i < wantFirst.size(); ++i)
        CHECK_EQ(keepFirst.events[i], wantFirst[i]);

    RecordingHandler keepLast;
    pjson::ParseOptions last;
    last.duplicateKeys = pjson::ParseOptions::KeepLastDuplicate;
    CHECK(pjson::parseSax(doc, keepLast, last));
    CHECK_EQ(keepLast.events.size(), size_t(6));
    CHECK_EQ(keepLast.events[1], std::string("key:a"));
    CHECK_EQ(keepLast.events[2], std::string("int:1"));
    CHECK_EQ(keepLast.events[3], std::string("key:a"));
    CHECK_EQ(keepLast.events[4], std::string("int:2"));

    RecordingHandler reject;
    pjson::ParseError err;
    CHECK(!pjson::parseSax(doc, reject, err));
    CHECK(!err.ok);
    CHECK(err.message.find("duplicate") != std::string::npos);

    ChunkedIStream streamed(doc, 1);
    RecordingHandler streamReject;
    CHECK(!pjson::parseSaxStream(streamed, streamReject, err));
    CHECK_EQ(err.offset, size_t(7));
    CHECK_EQ(err.line, size_t(1));
    CHECK_EQ(err.column, size_t(8));
}

TEST(streaming_sax_errors_report_line_and_column) {
    RecordingHandler h;
    pjson::ParseError err;
    CHECK(!pjson::parseSax("{\r\n  \"a\": [1,\r\n}", h, err));
    CHECK(!err.ok);
    CHECK_EQ(err.line, size_t(3));
    CHECK_EQ(err.column, size_t(1));
    CHECK(!err.message.empty());
}

TEST(streaming_sax_cancel_and_throw_become_parse_error) {
    CancelAfterNHandler cancel(3);
    pjson::ParseError err;
    CHECK(!pjson::parseSax("[1,2,3]", cancel, err));
    CHECK(!err.ok);
    CHECK(err.message.find("aborted") != std::string::npos);

    ThrowingHandler throwing;
    CHECK(!pjson::parseSax("{\"a\":1}", throwing, err));
    CHECK(!err.ok);
    CHECK(err.message.find("exception") != std::string::npos);

    ChunkedIStream throwingStream("{\"a\":1}", 1);
    ThrowingHandler streamThrowing;
    CHECK(!pjson::parseSaxStream(throwingStream, streamThrowing, err));
    CHECK(!err.ok);
    CHECK(err.message.empty() || err.message.find("exception") != std::string::npos);

    BadAllocHandler allocationFailure;
    CHECK(!pjson::parseSax("\"value\"", allocationFailure, err));
    CHECK(!err.ok);
    CHECK(err.message.empty() || err.message.find("memory") != std::string::npos);

    ChunkedIStream stream("\"value\"", 1);
    BadAllocHandler streamedAllocationFailure;
    CHECK(!pjson::parseSaxStream(stream, streamedAllocationFailure, err));
    CHECK(!err.ok);
    CHECK(err.message.empty() || err.message.find("memory") != std::string::npos);
}

TEST(streaming_sax_null_stream_buffer_reports_read_failure) {
    std::istream input(nullptr);
    RecordingHandler handler;
    pjson::ParseError error;

    CHECK(!pjson::parseSaxStream(input, handler, error));
    CHECK(!error.ok);
    CHECK(error.message.find("stream read failed") != std::string::npos);
    CHECK(handler.events.empty());
}

TEST(streaming_sax_number_range_matches_dom_parser) {
    const char* overflows[] = {"1e400", "-1e400"};
    for (size_t i = 0; i < sizeof(overflows) / sizeof(overflows[0]); ++i) {
        CHECK(pjson::parse(overflows[i]) == nullptr);
        NumberHandler handler;
        pjson::ParseError err;
        CHECK(!pjson::parseSax(overflows[i], handler, err));
        CHECK(!err.ok);
        CHECK(!handler.sawDouble);

        ChunkedIStream stream(overflows[i], 1);
        NumberHandler streamHandler;
        CHECK(!pjson::parseSaxStream(stream, streamHandler, err));
        CHECK(!err.ok);
        CHECK(!streamHandler.sawDouble);
    }

    const char* accepted[] = {"1e-400", "4.9406564584124654e-324"};
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        pjson::unique_ptr dom = pjson::parse(accepted[i]);
        CHECK(dom != nullptr);
        NumberHandler handler;
        pjson::ParseError err;
        CHECK(pjson::parseSax(accepted[i], handler, err));
        CHECK(err.ok);
        CHECK(handler.sawDouble);
        double domValue = 1.0;
        CHECK(dom->tryGet(domValue));
        CHECK_EQ(handler.value, domValue);

        ChunkedIStream stream(accepted[i], 1);
        NumberHandler streamHandler;
        CHECK(pjson::parseSaxStream(stream, streamHandler, err));
        CHECK_EQ(err.message, std::string());
        CHECK(streamHandler.sawDouble);
        CHECK_EQ(streamHandler.value, domValue);
    }
}

TEST(streaming_sax_max_input_bytes_and_max_nodes_on_stream) {
    const std::string doc = "[1,2,3,4]";
    ChunkedIStream in1(doc, 2);
    RecordingHandler h1;
    pjson::ParseOptions bytes;
    bytes.maxInputBytes = 4;
    pjson::ParseError err;
    CHECK(!pjson::parseSaxStream(in1, h1, err, bytes));
    CHECK(!err.ok);
    CHECK_EQ(err.offset, size_t(4));
    CHECK(err.message.find("maxInputBytes") != std::string::npos);

    ChunkedIStream in2(doc, 2);
    RecordingHandler h2;
    pjson::ParseOptions nodes;
    nodes.maxNodes = 3;
    CHECK(!pjson::parseSaxStream(in2, h2, err, nodes));
    CHECK(!err.ok);
    CHECK(err.message.find("node budget") != std::string::npos);

    const std::string nested = "[[1]]";
    ChunkedIStream in3(nested, 1);
    RecordingHandler h3;
    pjson::ParseOptions depth;
    depth.maxDepth = 0; // same effective minimum limit as the DOM parser
    CHECK(!pjson::parseSaxStream(in3, h3, err, depth));
    CHECK(err.message.find("depth") != std::string::npos);
}

//===----------------------------------------------------------------------===//
// Large incremental input stays streaming rather than requiring a contiguous buffer
//===----------------------------------------------------------------------===//

TEST(streaming_sax_large_stream_does_not_need_full_buffer) {
    std::string doc = "[";
    for (int i = 0; i < 2000; ++i) {
        if (i != 0)
            doc += ',';
        doc += std::to_string(i);
    }
    doc += "]";

    ChunkedIStream in(doc, 7);
    RecordingHandler h;
    CHECK(pjson::parseSaxStream(in, h));
    CHECK_EQ(h.events.front(), std::string("start-array"));
    CHECK_EQ(h.events.back(), std::string("end-array"));
    CHECK_EQ(h.events.size(), size_t(2002));
    CHECK_EQ(h.events[1], std::string("int:0"));
    CHECK_EQ(h.events[h.events.size() - 2], std::string("int:1999"));
}

//===----------------------------------------------------------------------===//
// Direct ostream serialization and output-failure propagation
//===----------------------------------------------------------------------===//

TEST(streaming_writer_matches_to_string_and_pretty) {
    auto p = parse("{\"a\":1,\"b\":[2,3],\"c\":{\"x\":\"y\"}}");
    CHECK(p != nullptr);

    std::ostringstream compact;
    p->write(compact);
    CHECK_EQ(compact.str(), p->toString());

    pjson::SerializeOptions prettyOpts = pjson::SerializeOptions::prettyPrinted();
    std::ostringstream pretty;
    p->write(pretty, prettyOpts);
    CHECK_EQ(pretty.str(), p->toString(prettyOpts));
}

TEST(streaming_writer_sets_failbit_on_write_failure) {
    pjson doc;
    doc["a"] = std::vector<int64_t>({1, 2, 3, 4, 5});
    FailingOStream out(5);
    doc.write(out);
    CHECK(out.fail());
}

TEST(streaming_writer_handles_deep_documents_iteratively) {
    const int depth = 20000;
    pjson root;
    pjson* cur = &root;
    for (int i = 0; i < depth; ++i)
        cur = &((*cur)["a"]);
    *cur = int64_t(1);

    std::ostringstream out;
    root.write(out);
    CHECK_EQ(out.str(), root.toString());
}
