/* boxrig - Under MIT. Optional consumer test using elder-terms' own codec. */
#include <boxrig/terminal.h>
#include "terminal-text-stream.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

using namespace elder_terms;

struct Connection {
    std::string name;
    TerminalTextStreamPtr body;
    TerminalTextStreamPtr title;
    TerminalTextStreamPtr send;
    std::string raw;
    std::string cooked;
    std::string macro_pending;
    bool send_replacement = false;

    explicit Connection(const std::string &encoding)
        : name(encoding), body(create_terminal_text_decoder(name)),
          title(create_terminal_text_decoder(name)), send(create_terminal_text_encoder(name)) {}
};

static boxrig_status convert(void *context, boxrig_encoding_channel channel, const uint8_t *input,
    size_t length, bool finish, boxrig_encoding_buffer *buffer, size_t *written)
{
    auto &c = *static_cast<Connection *>(context);
    auto &stream = channel == BOXRIG_ENCODING_SEND ? c.send : channel == BOXRIG_ENCODING_TITLE ? c.title : c.body;
    /* The encodings in this fixture need at most eight bytes per scalar and a
     * final shift sequence. Reserve before invoking a non-rewindable iconv. */
    uint8_t *bytes;
    if (length > (SIZE_MAX - 16) / 8) return BOXRIG_STATUS_LIMIT_EXCEEDED;
    const size_t bound = length * 8 + 16;
    auto status = boxrig_encoding_reserve(buffer, bound, &bytes);
    if (status != BOXRIG_STATUS_OK) return status;
    try {
        if (!stream) stream = channel == BOXRIG_ENCODING_SEND ?
            create_terminal_text_encoder(c.name) : create_terminal_text_decoder(c.name);
        auto result = finish ? finish_terminal_text(stream.get()) : convert_terminal_text(stream.get(), {input,length});
        assert(result.bytes.size() <= bound);
        std::copy(result.bytes.begin(), result.bytes.end(), bytes);
        if (channel == BOXRIG_ENCODING_SEND && result.used_replacement) c.send_replacement = true;
        *written = result.bytes.size();
        if (finish) stream.reset();
        return BOXRIG_STATUS_OK;
    } catch (...) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
}

static void observe(void *context, const uint8_t *bytes, size_t length)
{
    auto &connection = *static_cast<Connection *>(context);
    connection.cooked.append(reinterpret_cast<const char *>(bytes), length);
    connection.macro_pending.append(reinterpret_cast<const char *>(bytes), length);
}

static void attach(boxrig_terminal *terminal, Connection &c, boxrig_encoding_mode mode)
{
    const boxrig_encoding_hooks hooks{mode, &c, convert, observe};
    assert(boxrig_terminal_set_encoding_hooks(terminal, &hooks) == BOXRIG_STATUS_OK);
}

static void feed(boxrig_terminal *terminal, Connection &c, const std::string &bytes)
{
    c.raw += bytes;
    for (unsigned char byte : bytes) {
        size_t consumed;
        assert(boxrig_terminal_feed(terminal, &byte, 1, &consumed) == BOXRIG_STATUS_OK && consumed == 1);
    }
}

static void drain(boxrig_terminal *terminal, const std::string &expected)
{
    const uint8_t *wire;
    size_t length = boxrig_terminal_output_peek(terminal, &wire);
    assert(length == expected.size() && !std::memcmp(wire, expected.data(), length));
    assert(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
}

static void check_cell(boxrig_terminal *terminal, size_t row, size_t column, const std::string &text)
{
    boxrig_cell_view cell{};
    assert(boxrig_terminal_get_cell(terminal,row,column,&cell) == BOXRIG_STATUS_OK);
    assert(cell.text_length == text.size() && !std::memcmp(cell.text, text.data(), text.size()));
}

int main()
{
    auto config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = nullptr;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    Connection cp("CP932"), utf("UTF-8"), iso("ISO-2022-JP"), wide("UTF-16LE");
    attach(terminal, cp, BOXRIG_ENCODING_FIELDS);
    feed(terminal, cp, "\x93\xfa\x82");
    assert(boxrig_terminal_send_text(terminal,"日",3) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_finish_input(terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_finish_encoding_output(terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_set_encoding_hooks(terminal,nullptr) == BOXRIG_STATUS_BUSY);
    drain(terminal, "\x93\xfa");
    attach(terminal, utf, BOXRIG_ENCODING_FIELDS);
    feed(terminal, utf, "語");
    assert(cp.cooked + utf.cooked == "日�語" && cp.raw + utf.raw == "\x93\xfa\x82語");
    assert(cp.macro_pending + utf.macro_pending == "日�語");
    assert(boxrig_terminal_send_text(terminal,"語",3) == BOXRIG_STATUS_OK);
    drain(terminal, "語");
    assert(boxrig_terminal_finish_encoding_output(terminal) == BOXRIG_STATUS_OK);
    check_cell(terminal,0,0,"日");
    check_cell(terminal,0,2,"�");
    check_cell(terminal,0,3,"語");
    assert(boxrig_terminal_finish_input(terminal) == BOXRIG_STATUS_OK);
    attach(terminal, iso, BOXRIG_ENCODING_STREAM);
    feed(terminal, iso, "\033$BF|\033(B\033[2;3H\033$BK\\\033(B");
    assert(iso.cooked == "日\033[2;3H本");
    check_cell(terminal,1,2,"本");
    assert(boxrig_terminal_send_text(terminal,"日本",6) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_finish_encoding_output(terminal) == BOXRIG_STATUS_OK);
    drain(terminal, "\033$BF|K\\\033(B");
    assert(boxrig_terminal_finish_input(terminal) == BOXRIG_STATUS_OK);
    attach(terminal, wide, BOXRIG_ENCODING_STREAM);
    const char query[] = "\033\0[\0" "5\0n\0";
    feed(terminal, wide, std::string(query,sizeof(query)-1));
    const char reply[] = "\033\0[\0" "0\0n\0";
    drain(terminal, std::string(reply,sizeof(reply)-1));
    std::vector<uint8_t> binary(256);
    for (size_t i=0; i<binary.size(); ++i) binary[i]=static_cast<uint8_t>(i);
    assert(boxrig_terminal_send_bytes(terminal,binary.data(),binary.size()) == BOXRIG_STATUS_OK);
    drain(terminal,std::string(reinterpret_cast<const char *>(binary.data()),binary.size()));
    assert(boxrig_terminal_finish_input(terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_finish_encoding_output(terminal) == BOXRIG_STATUS_OK);
    Connection paced("CP932");
    attach(terminal, paced, BOXRIG_ENCODING_FIELDS);
    feed(terminal, paced, "\033[?2004h");
    feed(terminal, paced, "\x93");
    /* A dedicated paced sender can borrow the send channel while the body
     * decoder retains a lead byte. Capacity and pacing count encoded bytes. */
    auto ordinary_sender = std::move(paced.send);
    paced.send = create_terminal_text_encoder("CP932");
    boxrig_paste_event event{BOXRIG_PASTE_BEGIN, nullptr, 0};
    assert(boxrig_terminal_send_paste(terminal, &event) == BOXRIG_STATUS_OK);
    event = {BOXRIG_PASTE_CHUNK, "日🙂\n", 8};
    assert(boxrig_terminal_send_paste(terminal, &event) == BOXRIG_STATUS_OK);
    event = {BOXRIG_PASTE_END, nullptr, 0};
    assert(boxrig_terminal_send_paste(terminal, &event) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_finish_encoding_output(terminal) == BOXRIG_STATUS_OK);
    const uint8_t *wire;
    std::string transcript;
    while (size_t available = boxrig_terminal_output_peek(terminal, &wire)) {
        size_t allowance = std::min<size_t>(2, available);
        transcript.append(reinterpret_cast<const char *>(wire), allowance);
        assert(boxrig_terminal_output_consume(terminal, allowance) == BOXRIG_STATUS_OK);
    }
    assert(transcript == "\033[200~\x93\xfa?\n\033[201~" && paced.send_replacement);
    paced.send = std::move(ordinary_sender);
    feed(terminal, paced, "\xfa");
    assert(paced.macro_pending == "\033[?2004h日");
    boxrig_terminal_destroy(terminal);
}
