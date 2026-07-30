#pragma once

// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

class prolink_djl_t;

#include "kaitai/kaitaistruct.h"
#include <stdint.h>
#include <memory>
#include <set>

#if KAITAI_STRUCT_VERSION < 11000L
#error "Incompatible Kaitai Struct C++/STL API: version 0.11 or later is required"
#endif

/**
 * The broadcast announcement protocol Pioneer players and mixers use to find
 * each other and to agree on device numbers, as observed on two CDJ-2000NXS
 * running firmware 1.44.
 * 
 * Written from `docs/PROTOCOL.md` §2 and the evidence in `docs/FINDINGS.md`,
 * both of which come from our own captures of our own hardware. It is not
 * derived from any other project's schema.
 * 
 * **This describes UDP 50000 only.** Port 50002 carries a different header --
 * the name starts at 0x0b rather than 0x0c and byte 0x1f is a structural 0x01
 * (C14) -- and lives in `prolink_status.ksy`. Sharing one parser between them
 * yields plausible nonsense rather than an error, which is worse.
 * 
 * The handshake is:
 * 
 *     3x hello -> 3x claim_mac -> 3x claim_ip -> Nx claim_number -> keep_alive forever
 * 
 * ~300 ms apart, all broadcast. N is 3 into an empty network and 1 into a
 * populated one (C13) -- it is *not* governed by the auto/manual setting, which
 * is what `research/02` §1.0 claims.
 * \sa docs/PROTOCOL.md
 */

class prolink_djl_t : public kaitai::kstruct {

public:
    class claim_ip_body_t;
    class claim_mac_body_t;
    class hello_body_t;
    class keep_alive_body_t;
    class number_body_t;
    class number_conflict_body_t;
    class unknown_body_t;

    enum assignment_mode_t {
        ASSIGNMENT_MODE_AUTO = 1,
        ASSIGNMENT_MODE_MANUAL = 2
    };
    static bool _is_defined_assignment_mode_t(assignment_mode_t v);

private:
    static const std::set<assignment_mode_t> _values_assignment_mode_t;

public:

    enum device_kind_t {
        DEVICE_KIND_MIXER = 1,
        DEVICE_KIND_CDJ = 2,
        DEVICE_KIND_REKORDBOX_OR_CDJ3000 = 3,
        DEVICE_KIND_CDJ3000_HELLO = 4
    };
    static bool _is_defined_device_kind_t(device_kind_t v);

private:
    static const std::set<device_kind_t> _values_device_kind_t;

public:

    enum packet_type_t {
        PACKET_TYPE_CLAIM_MAC = 0,
        PACKET_TYPE_MIXER_ASSIGN_INTENT = 1,
        PACKET_TYPE_CLAIM_IP = 2,
        PACKET_TYPE_MIXER_ASSIGN = 3,
        PACKET_TYPE_CLAIM_NUMBER = 4,
        PACKET_TYPE_NUMBER_IN_USE = 5,
        PACKET_TYPE_KEEP_ALIVE = 6,
        PACKET_TYPE_NUMBER_CONFLICT = 8,
        PACKET_TYPE_HELLO = 10
    };
    static bool _is_defined_packet_type_t(packet_type_t v);

private:
    static const std::set<packet_type_t> _values_packet_type_t;

public:

    prolink_djl_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, prolink_djl_t* p__root = nullptr);

private:
    void _read();
    void _clean_up();

public:
    ~prolink_djl_t();

    /**
     * Stage 2, 0x32 bytes. Publishes the IP and proposes a device number.
     * `research/02` calls this IdUseRequest.
     */

    class claim_ip_body_t : public kaitai::kstruct {

    public:

        claim_ip_body_t(kaitai::kstream* p__io, prolink_djl_t* p__parent = nullptr, prolink_djl_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~claim_ip_body_t();

    private:
        std::string m_ip;
        std::string m_mac;
        uint8_t m_device_number;
        uint8_t m_iteration;
        uint8_t m_role;
        assignment_mode_t m_assignment_mode;
        prolink_djl_t* m__root;
        prolink_djl_t* m__parent;

    public:
        std::string ip() const { return m_ip; }
        std::string mac() const { return m_mac; }

        /**
         * Byte 0x2e. The number being proposed, not yet held.
         */
        uint8_t device_number() const { return m_device_number; }
        uint8_t iteration() const { return m_iteration; }

        /**
         * Byte 0x30. A CDJ/mixer role, **not** a constant: a DJM-2000nexus
         * sends 0x02 where a CDJ sends 0x01 (C1), and `research/02` documents
         * it as invariant.
         */
        uint8_t role() const { return m_role; }

        /**
         * Byte 0x31 (F36). Every capture before F36 had both decks numbered
         * manually, so only `manual` had ever been seen and `research/02`
         * marked this settled on documentation alone.
         */
        assignment_mode_t assignment_mode() const { return m_assignment_mode; }
        prolink_djl_t* _root() const { return m__root; }
        prolink_djl_t* _parent() const { return m__parent; }
    };

    /**
     * Stage 1 of the claim chain, 0x2c bytes. Publishes the MAC.
     */

    class claim_mac_body_t : public kaitai::kstruct {

    public:

        claim_mac_body_t(kaitai::kstream* p__io, prolink_djl_t* p__parent = nullptr, prolink_djl_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~claim_mac_body_t();

    private:
        uint8_t m_iteration;
        uint8_t m_flags;
        std::string m_mac;
        prolink_djl_t* m__root;
        prolink_djl_t* m__parent;

    public:

        /**
         * 1, 2, 3 -- the packet's position in its 3-packet burst.
         */
        uint8_t iteration() const { return m_iteration; }
        uint8_t flags() const { return m_flags; }
        std::string mac() const { return m_mac; }
        prolink_djl_t* _root() const { return m__root; }
        prolink_djl_t* _parent() const { return m__parent; }
    };

    /**
     * 0x25 bytes total. "I am here", the first thing a device broadcasts.
     */

    class hello_body_t : public kaitai::kstruct {

    public:

        hello_body_t(kaitai::kstream* p__io, prolink_djl_t* p__parent = nullptr, prolink_djl_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~hello_body_t();

    private:
        uint8_t m_payload;
        prolink_djl_t* m__root;
        prolink_djl_t* m__parent;

    public:
        uint8_t payload() const { return m_payload; }
        prolink_djl_t* _root() const { return m__root; }
        prolink_djl_t* _parent() const { return m__parent; }
    };

    /**
     * Steady state, 0x36 bytes, broadcast every **2.0026 s** -- a tight
     * hardware timer, not the 1.5 s `research/02` gives, which traces back to
     * what reference *tools* chose (C12). The 10 s device timeout is therefore
     * five missed keep-alives, not six or seven.
     */

    class keep_alive_body_t : public kaitai::kstruct {

    public:

        keep_alive_body_t(kaitai::kstream* p__io, prolink_djl_t* p__parent = nullptr, prolink_djl_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~keep_alive_body_t();

    private:
        uint8_t m_device_number;
        uint8_t m_was_first_on_network;
        std::string m_mac;
        std::string m_ip;
        uint8_t m_peer_count;
        std::string m_pad_31;
        uint8_t m_flags;
        uint8_t m_trailing;
        prolink_djl_t* m__root;
        prolink_djl_t* m__parent;

    public:
        uint8_t device_number() const { return m_device_number; }

        /**
         * Byte 0x25: 0x02 if this device was first onto the network, 0x01 if
         * peers were already present. Latched at boot and never re-evaluated --
         * a deck held 0x02 while its peer count went 1 to 2 (F9). It is not a
         * CDJ/mixer role byte as documented, and not the peer count.
         */
        uint8_t was_first_on_network() const { return m_was_first_on_network; }
        std::string mac() const { return m_mac; }
        std::string ip() const { return m_ip; }

        /**
         * Byte 0x30.
         */
        uint8_t peer_count() const { return m_peer_count; }
        std::string pad_31() const { return m_pad_31; }

        /**
         * Byte 0x34. Tracks the device kind, like `claim_ip`'s role byte.
         */
        uint8_t flags() const { return m_flags; }

        /**
         * Byte 0x35. **0x00 on nexus hardware, not 0x01** as `research/02` has
         * it (C3). 0x64 is required for CDJ-3000 coexistence.
         */
        uint8_t trailing() const { return m_trailing; }
        prolink_djl_t* _root() const { return m__root; }
        prolink_djl_t* _parent() const { return m__parent; }
    };

    /**
     * Stage 3 (`claim_number`) and `number_in_use`, both 0x26 bytes and
     * identical but for the type byte.
     * 
     * `number_in_use` is the surprise. `research/02` §1.7 files type 0x05 under
     * mixer channel assignment. What we saw instead: in the same instant a
     * joining deck sent its stage-3 claim, an **auto-numbered** deck *unicast*
     * one of these back carrying its own number (F36). Reading it as "this
     * number is taken" fits what an auto-assigning device must publish, though
     * that is inference from a single occurrence.
     */

    class number_body_t : public kaitai::kstruct {

    public:

        number_body_t(kaitai::kstream* p__io, prolink_djl_t* p__parent = nullptr, prolink_djl_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~number_body_t();

    private:
        uint8_t m_device_number;
        uint8_t m_iteration;
        prolink_djl_t* m__root;
        prolink_djl_t* m__parent;

    public:
        uint8_t device_number() const { return m_device_number; }
        uint8_t iteration() const { return m_iteration; }
        prolink_djl_t* _root() const { return m__root; }
        prolink_djl_t* _parent() const { return m__parent; }
    };

    /**
     * 0x29 bytes, **unicast** by the device that already holds the number.
     * Sent in reply to someone else's claim.
     * 
     * Note that silence is not evidence a number is free: XDJ-XZ and Opus Quad
     * do not defend their numbers with these at all, so only having watched the
     * network is.
     */

    class number_conflict_body_t : public kaitai::kstruct {

    public:

        number_conflict_body_t(kaitai::kstream* p__io, prolink_djl_t* p__parent = nullptr, prolink_djl_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~number_conflict_body_t();

    private:
        uint8_t m_device_number;
        std::string m_ip;
        prolink_djl_t* m__root;
        prolink_djl_t* m__parent;

    public:
        uint8_t device_number() const { return m_device_number; }
        std::string ip() const { return m_ip; }
        prolink_djl_t* _root() const { return m__root; }
        prolink_djl_t* _parent() const { return m__parent; }
    };

    class unknown_body_t : public kaitai::kstruct {

    public:

        unknown_body_t(kaitai::kstream* p__io, prolink_djl_t* p__parent = nullptr, prolink_djl_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~unknown_body_t();

    private:
        std::string m_rest;
        prolink_djl_t* m__root;
        prolink_djl_t* m__parent;

    public:
        std::string rest() const { return m_rest; }
        prolink_djl_t* _root() const { return m__root; }
        prolink_djl_t* _parent() const { return m__parent; }
    };

private:
    bool f_device_name_raw;
    std::string m_device_name_raw;

public:

    /**
     * The literal 20 bytes, alongside the decoded string. Needed because the
     * padding is part of what makes an announcement indistinguishable from a
     * real one, and `strz` discards it.
     */
    std::string device_name_raw();

private:
    std::string m_magic;
    packet_type_t m_packet_type;
    uint8_t m_subtype;
    std::string m_device_name;
    std::string m_const_one;
    device_kind_t m_device_kind;
    std::string m_pad_22;
    uint8_t m_stype;
    std::unique_ptr<kaitai::kstruct> m_body;
    prolink_djl_t* m__root;
    kaitai::kstruct* m__parent;

public:

    /**
     * Present on every Pro DJ Link datagram, on all three UDP ports.
     */
    std::string magic() const { return m_magic; }

    /**
     * Byte 0x0a, the discriminator. The values are not ordered by handshake
     * position.
     */
    packet_type_t packet_type() const { return m_packet_type; }

    /**
     * Byte 0x0b. Zero on everything we have observed; kept distinct from
     * `stype` below because the two are different bytes that both look like
     * length or variant markers.
     */
    uint8_t subtype() const { return m_subtype; }

    /**
     * NUL-padded. `CDJ-2000nexus` is the exact casing, captured literally
     * rather than inferred (F1) -- `research/02` §4.1 guessed at it and a
     * mis-cased name is the kind of thing a peer could plausibly reject.
     */
    std::string device_name() const { return m_device_name; }

    /**
     * Byte 0x20. Invariant across every packet in every capture.
     */
    std::string const_one() const { return m_const_one; }

    /**
     * Byte 0x21. Critical for impersonation.
     */
    device_kind_t device_kind() const { return m_device_kind; }
    std::string pad_22() const { return m_pad_22; }

    /**
     * Byte 0x23. **Equals the total datagram length** for every type we have
     * seen. `research/02` §0.1 gives claim_number a length of 0x2a against an
     * stype of 0x26; six real type-0x04 packets are 0x26 bytes long, so the
     * document's length column is simply wrong there (C2).
     */
    uint8_t stype() const { return m_stype; }

    /**
     * Everything from 0x24 on. Unknown types decode to `unknown_body` rather
     * than failing: a mixer, a CDJ-3000 or a newer firmware may send types we
     * have never seen, and a parser that throws would take out discovery
     * entirely for the devices we *do* understand.
     */
    kaitai::kstruct* body() const { return m_body.get(); }
    prolink_djl_t* _root() const { return m__root; }
    kaitai::kstruct* _parent() const { return m__parent; }
};
