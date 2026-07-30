#pragma once

// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

class prolink_dbserver_t;

#include "kaitai/kaitaistruct.h"
#include <stdint.h>
#include <memory>
#include <vector>

#if KAITAI_STRUCT_VERSION < 11000L
#error "Incompatible Kaitai Struct C++/STL API: version 0.11 or later is required"
#endif

/**
 * One message of the "remotedb" protocol -- the one the LINK button drives, and
 * the only way to get album art out of a player: a real CDJ never asks NFS for an
 * image (docs/FINDINGS.md F49).
 * 
 * Written from docs/PROTOCOL.md section 5. The format round-trips byte-exactly
 * against 1957 messages captured from two CDJ-2000NXS (F7).
 * 
 * **This schema parses exactly one message**, not a stream of them. Messages
 * carry no length prefix and are framed by nothing but their own contents, so the
 * only way to know whether a TCP buffer holds a whole one is to try: running off
 * the end is the *expected* outcome of trying too early, and the caller
 * distinguishes that (an EOF from the runtime) from a structural error (a
 * validation failure) to decide between "wait for more" and "drop the
 * connection". The parser's final stream position is how many bytes it consumed.
 * 
 * Kaitai cannot generate C++ serializers, so this is the read direction only.
 * The writers are hand-written in
 * `mixxx/src/network/prolink/dbserver/dbservermessage.cpp` and their unit tests
 * compare against vectors produced by the Python proof-of-concept.
 */

class prolink_dbserver_t : public kaitai::kstruct {

public:
    class argument_t;
    class field_t;

    prolink_dbserver_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, prolink_dbserver_t* p__root = nullptr);

private:
    void _read();
    void _clean_up();

public:
    ~prolink_dbserver_t();

    class argument_t : public kaitai::kstruct {

    public:

        argument_t(uint8_t p_arg_tag, uint32_t p_prev_value, kaitai::kstream* p__io, prolink_dbserver_t* p__parent = nullptr, prolink_dbserver_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~argument_t();

    private:
        bool f_is_present;
        bool m_is_present;

    public:

        /**
         * **A zero-length binary argument is omitted from the wire entirely.**
         * Not sent as an empty blob: simply absent, with the preceding UInt32
         * length argument the only thing that says so. It is the rule that
         * desynchronises a naive parser -- a reader that expects the blob
         * consumes the next message's magic as a field, and every argument after
         * that is one position out with no error to show for it.
         * 
         * A player answers GetArtwork for a track with no art exactly this way,
         * so it is the common case rather than an exotic one.
         */
        bool is_present();

    private:
        bool f_num_value;
        uint32_t m_num_value;

    public:

        /**
         * For the next argument's is_present. 1 means "not a zero length".
         */
        uint32_t num_value();

    private:
        std::unique_ptr<field_t> m_field;
        bool n_field;

    public:
        bool _is_null_field() { field(); return n_field; };

    private:
        uint8_t m_arg_tag;
        uint32_t m_prev_value;
        prolink_dbserver_t* m__root;
        prolink_dbserver_t* m__parent;

    public:
        field_t* field() const { return m_field.get(); }

        /**
         * This argument's entry in the header's 12-byte tag blob.
         */
        uint8_t arg_tag() const { return m_arg_tag; }

        /**
         * The preceding argument's numeric value, or 1 for the first argument and
         * for any predecessor that was not a number. Only used to decide whether
         * this argument is on the wire at all -- see is_present.
         */
        uint32_t prev_value() const { return m_prev_value; }
        prolink_dbserver_t* _root() const { return m__root; }
        prolink_dbserver_t* _parent() const { return m__parent; }
    };

    /**
     * One tagged value. The tag byte is the first numbering (see arg_tags).
     */

    class field_t : public kaitai::kstruct {

    public:

        field_t(kaitai::kstream* p__io, prolink_dbserver_t::argument_t* p__parent = nullptr, prolink_dbserver_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~field_t();

    private:
        bool f_num_value;
        uint32_t m_num_value;

    public:

        /**
         * The integer this field carries, or 1 if it is not an integer -- which
         * is what argument::is_present needs, since only a genuine zero length
         * means the next binary argument was omitted.
         */
        uint32_t num_value();

    private:
        uint8_t m_field_type;
        uint8_t m_value_u8;
        bool n_value_u8;

    public:
        bool _is_null_value_u8() { value_u8(); return n_value_u8; };

    private:
        uint16_t m_value_u16;
        bool n_value_u16;

    public:
        bool _is_null_value_u16() { value_u16(); return n_value_u16; };

    private:
        uint32_t m_value_u32;
        bool n_value_u32;

    public:
        bool _is_null_value_u32() { value_u32(); return n_value_u32; };

    private:
        uint32_t m_len_blob;
        bool n_len_blob;

    public:
        bool _is_null_len_blob() { len_blob(); return n_len_blob; };

    private:
        std::string m_blob;
        bool n_blob;

    public:
        bool _is_null_blob() { blob(); return n_blob; };

    private:
        uint32_t m_num_chars;
        bool n_num_chars;

    public:
        bool _is_null_num_chars() { num_chars(); return n_num_chars; };

    private:
        std::string m_text_raw;
        bool n_text_raw;

    public:
        bool _is_null_text_raw() { text_raw(); return n_text_raw; };

    private:
        prolink_dbserver_t* m__root;
        prolink_dbserver_t::argument_t* m__parent;

    public:
        uint8_t field_type() const { return m_field_type; }
        uint8_t value_u8() const { return m_value_u8; }
        uint16_t value_u16() const { return m_value_u16; }
        uint32_t value_u32() const { return m_value_u32; }

        /**
         * Capped, because the runtime allocates the buffer before it discovers
         * the stream is shorter than the length claims: without this one corrupt
         * word asks for four gigabytes. Well above any real payload -- the
         * largest thing this protocol carries is a cover image.
         */
        uint32_t len_blob() const { return m_len_blob; }
        std::string blob() const { return m_blob; }

        /**
         * Capped for the same reason as len_blob, and doubled below.
         */
        uint32_t num_chars() const { return m_num_chars; }

        /**
         * UTF-16 **big**-endian, and the prefix counts *characters* including the
         * terminating NUL -- so a three-character string announces 4 and carries
         * 8 bytes.
         * 
         * Left as raw bytes rather than given an `encoding:`, deliberately:
         * Mixxx compiles the Kaitai runtime with KS_STR_ENCODING_NONE, under
         * which bytes_to_str returns its input unchanged. An encoding would
         * therefore be silently ignored and the caller would get UTF-16 bytes in
         * a std::string it believed was decoded. The caller converts.
         * 
         * Note also that this is the opposite convention to the NFS half of the
         * same protocol, which sends UTF-16 *little*-endian counted in *bytes*.
         * The two must never share a helper.
         */
        std::string text_raw() const { return m_text_raw; }
        prolink_dbserver_t* _root() const { return m__root; }
        prolink_dbserver_t::argument_t* _parent() const { return m__parent; }
    };

private:
    uint8_t m_magic_tag;
    uint32_t m_magic;
    uint8_t m_transaction_id_tag;
    uint32_t m_transaction_id;
    uint8_t m_message_type_tag;
    uint16_t m_message_type;
    uint8_t m_num_args_tag;
    uint8_t m_num_args;
    uint8_t m_arg_tags_tag;
    uint32_t m_len_arg_tags;
    std::string m_arg_tags;
    std::unique_ptr<std::vector<std::unique_ptr<argument_t>>> m_args;
    prolink_dbserver_t* m__root;
    kaitai::kstruct* m__parent;

public:

    /**
     * The magic is itself a tagged UInt32, tag included.
     */
    uint8_t magic_tag() const { return m_magic_tag; }
    uint32_t magic() const { return m_magic; }
    uint8_t transaction_id_tag() const { return m_transaction_id_tag; }

    /**
     * Echoed in the reply, and the only way to pair one with its request. A
     * player uses 0xfffffffe for Introduce and Disconnect and counts up from
     * about 0x03800001 for everything else.
     */
    uint32_t transaction_id() const { return m_transaction_id; }
    uint8_t message_type_tag() const { return m_message_type_tag; }

    /**
     * Requests are 0x0nnn-0x3nnn, replies 0x4nnn. 0x2003 GetArtwork is answered
     * by 0x4002 Artwork; 0x4003 is a refusal.
     */
    uint16_t message_type() const { return m_message_type; }
    uint8_t num_args_tag() const { return m_num_args_tag; }
    uint8_t num_args() const { return m_num_args; }
    uint8_t arg_tags_tag() const { return m_arg_tags_tag; }
    uint32_t len_arg_tags() const { return m_len_arg_tags; }

    /**
     * The *other* numbering. Twelve bytes, one per possible argument, describing
     * the same arguments the tag bytes in the stream describe -- but with
     * different values for the same five types (02/03/06 here against
     * 0f/10/11/14/26 there). Both must agree or the message is rejected, so a
     * writer has to fill in two unrelated tables consistently.
     */
    std::string arg_tags() const { return m_arg_tags; }
    std::vector<std::unique_ptr<argument_t>>* args() const { return m_args.get(); }
    prolink_dbserver_t* _root() const { return m__root; }
    kaitai::kstruct* _parent() const { return m__parent; }
};
