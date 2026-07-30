#pragma once

// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

class prolink_status_t;

#include "kaitai/kaitaistruct.h"
#include <stdint.h>
#include <memory>
#include <set>

#if KAITAI_STRUCT_VERSION < 11000L
#error "Incompatible Kaitai Struct C++/STL API: version 0.11 or later is required"
#endif

/**
 * The unicast half of Pro DJ Link. Players publish what they are doing and what
 * is in their slots here, and ask each other the two questions that a browse
 * depends on: "what is in your slot N?" and "give me the settings on it".
 * 
 * **This port is invisible until you announce.** A player unicasts on 50002 to
 * peers that have announced themselves and to nobody else -- 1507 status packets
 * in one session all went deck-to-deck, and not one reached a host that had been
 * on the network the whole time without announcing (F21). Slot occupancy is
 * published here and *nowhere else* (F20), which is why the virtual CDJ is a
 * hard prerequisite for both browsing a deck and being browsed by one.
 * 
 * **The header is not the one on port 50000.** The device name occupies
 * 0x0b-0x1e -- one byte earlier and one byte shorter -- and byte 0x1f is a
 * structural 0x01 where the discovery header has its name's last byte (C14).
 * Reusing `prolink_djl.ksy` here yields plausible nonsense rather than an error,
 * so the two schemas are deliberately kept apart.
 * 
 * **Why the type-specific fields are `instances` and not a `body` switch.**
 * These packets are sparse: a 284-byte status packet has about a dozen fields we
 * can name and 260 bytes we cannot, and of 749 consecutive packets from an idle
 * CDJ-2000nexus only six bytes ever changed. Declaring the unknown runs as
 * padding would be inventing structure. Instances carry the one thing that is
 * actually known -- an absolute offset -- they read exactly like the offset
 * tables in `docs/PROTOCOL.md`, and being lazy they cost nothing for the fields a
 * given packet type does not have.
 * \sa docs/PROTOCOL.md
 */

class prolink_status_t : public kaitai::kstruct {

public:

    enum media_slot_t {
        MEDIA_SLOT_NONE = 0,
        MEDIA_SLOT_CD = 1,
        MEDIA_SLOT_SD = 2,
        MEDIA_SLOT_USB = 3,
        MEDIA_SLOT_REKORDBOX = 4
    };
    static bool _is_defined_media_slot_t(media_slot_t v);

private:
    static const std::set<media_slot_t> _values_media_slot_t;

public:

    enum media_state_t {
        MEDIA_STATE_LOADED = 0,
        MEDIA_STATE_UNMOUNTING = 2,
        MEDIA_STATE_UNMOUNTING_ALT = 3,
        MEDIA_STATE_EMPTY = 4
    };
    static bool _is_defined_media_state_t(media_state_t v);

private:
    static const std::set<media_state_t> _values_media_state_t;

public:

    enum packet_type_t {
        PACKET_TYPE_MEDIA_QUERY = 5,
        PACKET_TYPE_MEDIA_RESPONSE = 6,
        PACKET_TYPE_CDJ_STATUS = 10,
        PACKET_TYPE_MIXER_STATUS = 41,
        PACKET_TYPE_SETTINGS_QUERY = 53,
        PACKET_TYPE_SETTINGS_RESPONSE = 54
    };
    static bool _is_defined_packet_type_t(packet_type_t v);

private:
    static const std::set<packet_type_t> _values_packet_type_t;

public:

    prolink_status_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, prolink_status_t* p__root = nullptr);

private:
    void _read();
    void _clean_up();

public:
    ~prolink_status_t();

private:
    bool f_body_length;
    uint16_t m_body_length;

public:

    /**
     * Bytes following 0x24.
     */
    uint16_t body_length();

private:
    bool f_device_name_raw;
    std::string m_device_name_raw;

public:

    /**
     * The literal bytes, padding included, for byte-diffing against hardware.
     */
    std::string device_name_raw();

private:
    bool f_query_requester_ip;
    std::string m_query_requester_ip;

public:
    std::string query_requester_ip();

private:
    bool f_query_slot;
    media_slot_t m_query_slot;

public:
    media_slot_t query_slot();

private:
    bool f_query_target_device;
    uint32_t m_query_target_device;

public:
    uint32_t query_target_device();

private:
    bool f_response_device;
    uint32_t m_response_device;

public:
    uint32_t response_device();

private:
    bool f_response_playlist_count;
    uint32_t m_response_playlist_count;

public:
    uint32_t response_playlist_count();

private:
    bool f_response_slot;
    media_slot_t m_response_slot;

public:
    media_slot_t response_slot();

private:
    bool f_response_track_count;
    uint32_t m_response_track_count;

public:
    uint32_t response_track_count();

private:
    bool f_response_volume_name;
    std::string m_response_volume_name;

public:

    /**
     * The volume label the DJ formatted the medium with, UTF-16 **big**-endian
     * -- like the dbserver strings and unlike the NFS layer's UTF-16LE.
     * 
     * Raw bytes on purpose. Mixxx compiles the Kaitai runtime with
     * `KS_STR_ENCODING_NONE`, under which an `encoding: UTF-16BE` is silently a
     * no-op: it would hand back these same bytes in a string that claimed to be
     * decoded, correct for ASCII and mojibake for everything else. Decode in the
     * caller.
     * 
     * Fixed 64 bytes, NUL-padded, and the padding is not a terminator -- the
     * field is always this long and the name simply stops. **Often empty and
     * legitimately so**: an unlabelled stick reports no name while carrying a
     * full library, so emptiness here is not emptiness of the slot.
     */
    std::string response_volume_name();

private:
    bool f_sender_device;
    uint8_t m_sender_device;

public:

    /**
     * Who sent this. Present on every type here, which is what lets a receiver
     * attribute a packet without looking at the source address.
     */
    uint8_t sender_device();

private:
    bool f_settings_magic;
    uint32_t m_settings_magic;

public:

    /**
     * Response only. Constant `0x12345678` in the one exchange captured -- the
     * same value that leads the payload of `PIONEER/MYSETTING.DAT`, which is
     * what ties the file to the wire.
     */
    uint32_t settings_magic();

private:
    bool f_settings_payload;
    std::string m_settings_payload;

public:

    /**
     * Response only. Deliberately not interpreted: the observed bytes look like
     * 0x80-based enumerations but nothing maps them to the named options on the
     * deck's screen, and a server only has to hand over what the medium holds.
     */
    std::string settings_payload();

private:
    bool f_settings_requester;
    uint8_t m_settings_requester;

public:

    /**
     * The device that wants the settings. In a query this equals
     * `sender_device`; in a response it does not, which is how the two are told
     * apart without looking at the type byte.
     */
    uint8_t settings_requester();

private:
    bool f_settings_slot;
    media_slot_t m_settings_slot;

public:
    media_slot_t settings_slot();

private:
    bool f_status_bpm_100;
    uint16_t m_status_bpm_100;

public:

    /**
     * Tempo in centi-BPM, before the pitch fader is applied.
     */
    uint16_t status_bpm_100();

private:
    bool f_status_firmware;
    std::string m_status_firmware;

public:
    std::string status_firmware();

private:
    bool f_status_link_available;
    uint8_t m_status_link_available;

public:

    /**
     * Set when any media is available anywhere on the network.
     */
    uint8_t status_link_available();

private:
    bool f_status_master_meaningful;
    uint8_t m_status_master_meaningful;

public:

    /**
     * Non-zero on whichever player currently holds tempo master: 1 on a
     * rekordbox track, 2 on a track with no usable tempo. This is the only place
     * mastership is published, so a device that never announces can never know
     * who the master is.
     */
    uint8_t status_master_meaningful();

private:
    bool f_status_packet_counter;
    uint32_t m_status_packet_counter;

public:
    uint32_t status_packet_counter();

private:
    bool f_status_play_state;
    uint8_t m_status_play_state;

public:
    uint8_t status_play_state();

private:
    bool f_status_sd_state;
    media_state_t m_status_sd_state;

public:
    media_state_t status_sd_state();

private:
    bool f_status_source_player;
    uint8_t m_status_source_player;

public:

    /**
     * Which player the loaded track came from; 0 when nothing is loaded.
     */
    uint8_t status_source_player();

private:
    bool f_status_source_slot;
    media_slot_t m_status_source_slot;

public:
    media_slot_t status_source_slot();

private:
    bool f_status_track_id;
    uint32_t m_status_track_id;

public:
    uint32_t status_track_id();

private:
    bool f_status_track_type;
    uint8_t m_status_track_type;

public:
    uint8_t status_track_type();

private:
    bool f_status_usb_state;
    media_state_t m_status_usb_state;

public:
    media_state_t status_usb_state();

private:
    bool f_subtype;
    uint8_t m_subtype;

public:
    uint8_t subtype();

private:
    std::string m_magic;
    packet_type_t m_packet_type;
    std::string m_device_name;
    std::string m_const_one;
    prolink_status_t* m__root;
    kaitai::kstruct* m__parent;

public:
    std::string magic() const { return m_magic; }

    /**
     * Byte 0x0a, the discriminator, on the same byte as on port 50000.
     */
    packet_type_t packet_type() const { return m_packet_type; }

    /**
     * 0x0b-0x1e. Twenty bytes, not the twenty-one `research/03` §0 states: byte
     * 0x1f was a constant 0x01 in all 1503 captured packets, which is the same
     * shape as the keep-alive where the name runs 0x0c-0x1f and the constant
     * sits at 0x20 (C14).
     */
    std::string device_name() const { return m_device_name; }

    /**
     * Byte 0x1f.
     */
    std::string const_one() const { return m_const_one; }
    prolink_status_t* _root() const { return m__root; }
    kaitai::kstruct* _parent() const { return m__parent; }
};
