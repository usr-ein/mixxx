// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

#include "prolink_status.h"
#include "kaitai/exceptions.h"
const std::set<prolink_status_t::media_slot_t> prolink_status_t::_values_media_slot_t{
    prolink_status_t::MEDIA_SLOT_NONE,
    prolink_status_t::MEDIA_SLOT_CD,
    prolink_status_t::MEDIA_SLOT_SD,
    prolink_status_t::MEDIA_SLOT_USB,
    prolink_status_t::MEDIA_SLOT_REKORDBOX,
};
bool prolink_status_t::_is_defined_media_slot_t(prolink_status_t::media_slot_t v) {
    return prolink_status_t::_values_media_slot_t.find(v) != prolink_status_t::_values_media_slot_t.end();
}
const std::set<prolink_status_t::media_state_t> prolink_status_t::_values_media_state_t{
    prolink_status_t::MEDIA_STATE_LOADED,
    prolink_status_t::MEDIA_STATE_UNMOUNTING,
    prolink_status_t::MEDIA_STATE_UNMOUNTING_ALT,
    prolink_status_t::MEDIA_STATE_EMPTY,
};
bool prolink_status_t::_is_defined_media_state_t(prolink_status_t::media_state_t v) {
    return prolink_status_t::_values_media_state_t.find(v) != prolink_status_t::_values_media_state_t.end();
}
const std::set<prolink_status_t::packet_type_t> prolink_status_t::_values_packet_type_t{
    prolink_status_t::PACKET_TYPE_MEDIA_QUERY,
    prolink_status_t::PACKET_TYPE_MEDIA_RESPONSE,
    prolink_status_t::PACKET_TYPE_CDJ_STATUS,
    prolink_status_t::PACKET_TYPE_MIXER_STATUS,
    prolink_status_t::PACKET_TYPE_SETTINGS_QUERY,
    prolink_status_t::PACKET_TYPE_SETTINGS_RESPONSE,
};
bool prolink_status_t::_is_defined_packet_type_t(prolink_status_t::packet_type_t v) {
    return prolink_status_t::_values_packet_type_t.find(v) != prolink_status_t::_values_packet_type_t.end();
}

prolink_status_t::prolink_status_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent, prolink_status_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root ? p__root : this;
    f_body_length = false;
    f_device_name_raw = false;
    f_query_requester_ip = false;
    f_query_slot = false;
    f_query_target_device = false;
    f_response_device = false;
    f_response_playlist_count = false;
    f_response_slot = false;
    f_response_track_count = false;
    f_response_volume_name = false;
    f_sender_device = false;
    f_settings_magic = false;
    f_settings_payload = false;
    f_settings_requester = false;
    f_settings_slot = false;
    f_status_bpm_100 = false;
    f_status_firmware = false;
    f_status_link_available = false;
    f_status_master_meaningful = false;
    f_status_packet_counter = false;
    f_status_play_state = false;
    f_status_sd_state = false;
    f_status_source_player = false;
    f_status_source_slot = false;
    f_status_track_id = false;
    f_status_track_type = false;
    f_status_usb_state = false;
    f_subtype = false;
    _read();
}

void prolink_status_t::_read() {
    m_magic = m__io->read_bytes(10);
    if (!(m_magic == std::string("\x51\x73\x70\x74\x31\x57\x6D\x4A\x4F\x4C", 10))) {
        throw kaitai::validation_not_equal_error<std::string>(std::string("\x51\x73\x70\x74\x31\x57\x6D\x4A\x4F\x4C", 10), m_magic, m__io, std::string("/seq/0"));
    }
    m_packet_type = static_cast<prolink_status_t::packet_type_t>(m__io->read_u1());
    m_device_name = kaitai::kstream::bytes_to_str(kaitai::kstream::bytes_terminate(m__io->read_bytes(20), 0, false), "ASCII");
    m_const_one = m__io->read_bytes(1);
    if (!(m_const_one == std::string("\x01", 1))) {
        throw kaitai::validation_not_equal_error<std::string>(std::string("\x01", 1), m_const_one, m__io, std::string("/seq/3"));
    }
}

prolink_status_t::~prolink_status_t() {
    _clean_up();
}

void prolink_status_t::_clean_up() {
    if (f_body_length) {
    }
    if (f_device_name_raw) {
    }
    if (f_query_requester_ip) {
    }
    if (f_query_slot) {
    }
    if (f_query_target_device) {
    }
    if (f_response_device) {
    }
    if (f_response_playlist_count) {
    }
    if (f_response_slot) {
    }
    if (f_response_track_count) {
    }
    if (f_response_volume_name) {
    }
    if (f_sender_device) {
    }
    if (f_settings_magic) {
    }
    if (f_settings_payload) {
    }
    if (f_settings_requester) {
    }
    if (f_settings_slot) {
    }
    if (f_status_bpm_100) {
    }
    if (f_status_firmware) {
    }
    if (f_status_link_available) {
    }
    if (f_status_master_meaningful) {
    }
    if (f_status_packet_counter) {
    }
    if (f_status_play_state) {
    }
    if (f_status_sd_state) {
    }
    if (f_status_source_player) {
    }
    if (f_status_source_slot) {
    }
    if (f_status_track_id) {
    }
    if (f_status_track_type) {
    }
    if (f_status_usb_state) {
    }
    if (f_subtype) {
    }
}

uint16_t prolink_status_t::body_length() {
    if (f_body_length)
        return m_body_length;
    f_body_length = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(34);
    m_body_length = m__io->read_u2be();
    m__io->seek(_pos);
    return m_body_length;
}

std::string prolink_status_t::device_name_raw() {
    if (f_device_name_raw)
        return m_device_name_raw;
    f_device_name_raw = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(11);
    m_device_name_raw = m__io->read_bytes(20);
    m__io->seek(_pos);
    return m_device_name_raw;
}

std::string prolink_status_t::query_requester_ip() {
    if (f_query_requester_ip)
        return m_query_requester_ip;
    f_query_requester_ip = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(36);
    m_query_requester_ip = m__io->read_bytes(4);
    m__io->seek(_pos);
    return m_query_requester_ip;
}

prolink_status_t::media_slot_t prolink_status_t::query_slot() {
    if (f_query_slot)
        return m_query_slot;
    f_query_slot = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(44);
    m_query_slot = static_cast<prolink_status_t::media_slot_t>(m__io->read_u4be());
    m__io->seek(_pos);
    return m_query_slot;
}

uint32_t prolink_status_t::query_target_device() {
    if (f_query_target_device)
        return m_query_target_device;
    f_query_target_device = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(40);
    m_query_target_device = m__io->read_u4be();
    m__io->seek(_pos);
    return m_query_target_device;
}

uint32_t prolink_status_t::response_device() {
    if (f_response_device)
        return m_response_device;
    f_response_device = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(36);
    m_response_device = m__io->read_u4be();
    m__io->seek(_pos);
    return m_response_device;
}

uint32_t prolink_status_t::response_playlist_count() {
    if (f_response_playlist_count)
        return m_response_playlist_count;
    f_response_playlist_count = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(172);
    m_response_playlist_count = m__io->read_u4be();
    m__io->seek(_pos);
    return m_response_playlist_count;
}

prolink_status_t::media_slot_t prolink_status_t::response_slot() {
    if (f_response_slot)
        return m_response_slot;
    f_response_slot = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(40);
    m_response_slot = static_cast<prolink_status_t::media_slot_t>(m__io->read_u4be());
    m__io->seek(_pos);
    return m_response_slot;
}

uint32_t prolink_status_t::response_track_count() {
    if (f_response_track_count)
        return m_response_track_count;
    f_response_track_count = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(164);
    m_response_track_count = m__io->read_u4be();
    m__io->seek(_pos);
    return m_response_track_count;
}

std::string prolink_status_t::response_volume_name() {
    if (f_response_volume_name)
        return m_response_volume_name;
    f_response_volume_name = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(44);
    m_response_volume_name = m__io->read_bytes(64);
    m__io->seek(_pos);
    return m_response_volume_name;
}

uint8_t prolink_status_t::sender_device() {
    if (f_sender_device)
        return m_sender_device;
    f_sender_device = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(33);
    m_sender_device = m__io->read_u1();
    m__io->seek(_pos);
    return m_sender_device;
}

uint32_t prolink_status_t::settings_magic() {
    if (f_settings_magic)
        return m_settings_magic;
    f_settings_magic = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(40);
    m_settings_magic = m__io->read_u4be();
    m__io->seek(_pos);
    return m_settings_magic;
}

std::string prolink_status_t::settings_payload() {
    if (f_settings_payload)
        return m_settings_payload;
    f_settings_payload = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(48);
    m_settings_payload = m__io->read_bytes(32);
    m__io->seek(_pos);
    return m_settings_payload;
}

uint8_t prolink_status_t::settings_requester() {
    if (f_settings_requester)
        return m_settings_requester;
    f_settings_requester = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(36);
    m_settings_requester = m__io->read_u1();
    m__io->seek(_pos);
    return m_settings_requester;
}

prolink_status_t::media_slot_t prolink_status_t::settings_slot() {
    if (f_settings_slot)
        return m_settings_slot;
    f_settings_slot = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(37);
    m_settings_slot = static_cast<prolink_status_t::media_slot_t>(m__io->read_u1());
    m__io->seek(_pos);
    return m_settings_slot;
}

uint16_t prolink_status_t::status_bpm_100() {
    if (f_status_bpm_100)
        return m_status_bpm_100;
    f_status_bpm_100 = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(146);
    m_status_bpm_100 = m__io->read_u2be();
    m__io->seek(_pos);
    return m_status_bpm_100;
}

std::string prolink_status_t::status_firmware() {
    if (f_status_firmware)
        return m_status_firmware;
    f_status_firmware = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(124);
    m_status_firmware = kaitai::kstream::bytes_to_str(kaitai::kstream::bytes_terminate(m__io->read_bytes(4), 0, false), "ASCII");
    m__io->seek(_pos);
    return m_status_firmware;
}

uint8_t prolink_status_t::status_link_available() {
    if (f_status_link_available)
        return m_status_link_available;
    f_status_link_available = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(117);
    m_status_link_available = m__io->read_u1();
    m__io->seek(_pos);
    return m_status_link_available;
}

uint8_t prolink_status_t::status_master_meaningful() {
    if (f_status_master_meaningful)
        return m_status_master_meaningful;
    f_status_master_meaningful = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(158);
    m_status_master_meaningful = m__io->read_u1();
    m__io->seek(_pos);
    return m_status_master_meaningful;
}

uint32_t prolink_status_t::status_packet_counter() {
    if (f_status_packet_counter)
        return m_status_packet_counter;
    f_status_packet_counter = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(200);
    m_status_packet_counter = m__io->read_u4be();
    m__io->seek(_pos);
    return m_status_packet_counter;
}

uint8_t prolink_status_t::status_play_state() {
    if (f_status_play_state)
        return m_status_play_state;
    f_status_play_state = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(123);
    m_status_play_state = m__io->read_u1();
    m__io->seek(_pos);
    return m_status_play_state;
}

prolink_status_t::media_state_t prolink_status_t::status_sd_state() {
    if (f_status_sd_state)
        return m_status_sd_state;
    f_status_sd_state = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(115);
    m_status_sd_state = static_cast<prolink_status_t::media_state_t>(m__io->read_u1());
    m__io->seek(_pos);
    return m_status_sd_state;
}

uint8_t prolink_status_t::status_source_player() {
    if (f_status_source_player)
        return m_status_source_player;
    f_status_source_player = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(40);
    m_status_source_player = m__io->read_u1();
    m__io->seek(_pos);
    return m_status_source_player;
}

prolink_status_t::media_slot_t prolink_status_t::status_source_slot() {
    if (f_status_source_slot)
        return m_status_source_slot;
    f_status_source_slot = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(41);
    m_status_source_slot = static_cast<prolink_status_t::media_slot_t>(m__io->read_u1());
    m__io->seek(_pos);
    return m_status_source_slot;
}

uint32_t prolink_status_t::status_track_id() {
    if (f_status_track_id)
        return m_status_track_id;
    f_status_track_id = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(44);
    m_status_track_id = m__io->read_u4be();
    m__io->seek(_pos);
    return m_status_track_id;
}

uint8_t prolink_status_t::status_track_type() {
    if (f_status_track_type)
        return m_status_track_type;
    f_status_track_type = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(42);
    m_status_track_type = m__io->read_u1();
    m__io->seek(_pos);
    return m_status_track_type;
}

prolink_status_t::media_state_t prolink_status_t::status_usb_state() {
    if (f_status_usb_state)
        return m_status_usb_state;
    f_status_usb_state = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(111);
    m_status_usb_state = static_cast<prolink_status_t::media_state_t>(m__io->read_u1());
    m__io->seek(_pos);
    return m_status_usb_state;
}

uint8_t prolink_status_t::subtype() {
    if (f_subtype)
        return m_subtype;
    f_subtype = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(32);
    m_subtype = m__io->read_u1();
    m__io->seek(_pos);
    return m_subtype;
}
