// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

#include "prolink_djl.h"
#include "kaitai/exceptions.h"
const std::set<prolink_djl_t::assignment_mode_t> prolink_djl_t::_values_assignment_mode_t{
    prolink_djl_t::ASSIGNMENT_MODE_AUTO,
    prolink_djl_t::ASSIGNMENT_MODE_MANUAL,
};
bool prolink_djl_t::_is_defined_assignment_mode_t(prolink_djl_t::assignment_mode_t v) {
    return prolink_djl_t::_values_assignment_mode_t.find(v) != prolink_djl_t::_values_assignment_mode_t.end();
}
const std::set<prolink_djl_t::device_kind_t> prolink_djl_t::_values_device_kind_t{
    prolink_djl_t::DEVICE_KIND_MIXER,
    prolink_djl_t::DEVICE_KIND_CDJ,
    prolink_djl_t::DEVICE_KIND_REKORDBOX_OR_CDJ3000,
    prolink_djl_t::DEVICE_KIND_CDJ3000_HELLO,
};
bool prolink_djl_t::_is_defined_device_kind_t(prolink_djl_t::device_kind_t v) {
    return prolink_djl_t::_values_device_kind_t.find(v) != prolink_djl_t::_values_device_kind_t.end();
}
const std::set<prolink_djl_t::packet_type_t> prolink_djl_t::_values_packet_type_t{
    prolink_djl_t::PACKET_TYPE_CLAIM_MAC,
    prolink_djl_t::PACKET_TYPE_MIXER_ASSIGN_INTENT,
    prolink_djl_t::PACKET_TYPE_CLAIM_IP,
    prolink_djl_t::PACKET_TYPE_MIXER_ASSIGN,
    prolink_djl_t::PACKET_TYPE_CLAIM_NUMBER,
    prolink_djl_t::PACKET_TYPE_NUMBER_IN_USE,
    prolink_djl_t::PACKET_TYPE_KEEP_ALIVE,
    prolink_djl_t::PACKET_TYPE_NUMBER_CONFLICT,
    prolink_djl_t::PACKET_TYPE_HELLO,
};
bool prolink_djl_t::_is_defined_packet_type_t(prolink_djl_t::packet_type_t v) {
    return prolink_djl_t::_values_packet_type_t.find(v) != prolink_djl_t::_values_packet_type_t.end();
}

prolink_djl_t::prolink_djl_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent, prolink_djl_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root ? p__root : this;
    f_device_name_raw = false;
    _read();
}

void prolink_djl_t::_read() {
    m_magic = m__io->read_bytes(10);
    if (!(m_magic == std::string("\x51\x73\x70\x74\x31\x57\x6D\x4A\x4F\x4C", 10))) {
        throw kaitai::validation_not_equal_error<std::string>(std::string("\x51\x73\x70\x74\x31\x57\x6D\x4A\x4F\x4C", 10), m_magic, m__io, std::string("/seq/0"));
    }
    m_packet_type = static_cast<prolink_djl_t::packet_type_t>(m__io->read_u1());
    m_subtype = m__io->read_u1();
    m_device_name = kaitai::kstream::bytes_to_str(kaitai::kstream::bytes_terminate(m__io->read_bytes(20), 0, false), "ASCII");
    m_const_one = m__io->read_bytes(1);
    if (!(m_const_one == std::string("\x01", 1))) {
        throw kaitai::validation_not_equal_error<std::string>(std::string("\x01", 1), m_const_one, m__io, std::string("/seq/4"));
    }
    m_device_kind = static_cast<prolink_djl_t::device_kind_t>(m__io->read_u1());
    m_pad_22 = m__io->read_bytes(1);
    if (!(m_pad_22 == std::string("\x00", 1))) {
        throw kaitai::validation_not_equal_error<std::string>(std::string("\x00", 1), m_pad_22, m__io, std::string("/seq/6"));
    }
    m_stype = m__io->read_u1();
    switch (packet_type()) {
    case prolink_djl_t::PACKET_TYPE_CLAIM_IP: {
        m_body = std::unique_ptr<claim_ip_body_t>(new claim_ip_body_t(m__io, this, m__root));
        break;
    }
    case prolink_djl_t::PACKET_TYPE_CLAIM_MAC: {
        m_body = std::unique_ptr<claim_mac_body_t>(new claim_mac_body_t(m__io, this, m__root));
        break;
    }
    case prolink_djl_t::PACKET_TYPE_CLAIM_NUMBER: {
        m_body = std::unique_ptr<number_body_t>(new number_body_t(m__io, this, m__root));
        break;
    }
    case prolink_djl_t::PACKET_TYPE_HELLO: {
        m_body = std::unique_ptr<hello_body_t>(new hello_body_t(m__io, this, m__root));
        break;
    }
    case prolink_djl_t::PACKET_TYPE_KEEP_ALIVE: {
        m_body = std::unique_ptr<keep_alive_body_t>(new keep_alive_body_t(m__io, this, m__root));
        break;
    }
    case prolink_djl_t::PACKET_TYPE_NUMBER_CONFLICT: {
        m_body = std::unique_ptr<number_conflict_body_t>(new number_conflict_body_t(m__io, this, m__root));
        break;
    }
    case prolink_djl_t::PACKET_TYPE_NUMBER_IN_USE: {
        m_body = std::unique_ptr<number_body_t>(new number_body_t(m__io, this, m__root));
        break;
    }
    default: {
        m_body = std::unique_ptr<unknown_body_t>(new unknown_body_t(m__io, this, m__root));
        break;
    }
    }
}

prolink_djl_t::~prolink_djl_t() {
    _clean_up();
}

void prolink_djl_t::_clean_up() {
    if (f_device_name_raw) {
    }
}

prolink_djl_t::claim_ip_body_t::claim_ip_body_t(kaitai::kstream* p__io, prolink_djl_t* p__parent, prolink_djl_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void prolink_djl_t::claim_ip_body_t::_read() {
    m_ip = m__io->read_bytes(4);
    m_mac = m__io->read_bytes(6);
    m_device_number = m__io->read_u1();
    m_iteration = m__io->read_u1();
    m_role = m__io->read_u1();
    m_assignment_mode = static_cast<prolink_djl_t::assignment_mode_t>(m__io->read_u1());
}

prolink_djl_t::claim_ip_body_t::~claim_ip_body_t() {
    _clean_up();
}

void prolink_djl_t::claim_ip_body_t::_clean_up() {
}

prolink_djl_t::claim_mac_body_t::claim_mac_body_t(kaitai::kstream* p__io, prolink_djl_t* p__parent, prolink_djl_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void prolink_djl_t::claim_mac_body_t::_read() {
    m_iteration = m__io->read_u1();
    m_flags = m__io->read_u1();
    m_mac = m__io->read_bytes(6);
}

prolink_djl_t::claim_mac_body_t::~claim_mac_body_t() {
    _clean_up();
}

void prolink_djl_t::claim_mac_body_t::_clean_up() {
}

prolink_djl_t::hello_body_t::hello_body_t(kaitai::kstream* p__io, prolink_djl_t* p__parent, prolink_djl_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void prolink_djl_t::hello_body_t::_read() {
    m_payload = m__io->read_u1();
}

prolink_djl_t::hello_body_t::~hello_body_t() {
    _clean_up();
}

void prolink_djl_t::hello_body_t::_clean_up() {
}

prolink_djl_t::keep_alive_body_t::keep_alive_body_t(kaitai::kstream* p__io, prolink_djl_t* p__parent, prolink_djl_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void prolink_djl_t::keep_alive_body_t::_read() {
    m_device_number = m__io->read_u1();
    m_was_first_on_network = m__io->read_u1();
    m_mac = m__io->read_bytes(6);
    m_ip = m__io->read_bytes(4);
    m_peer_count = m__io->read_u1();
    m_pad_31 = m__io->read_bytes(3);
    m_flags = m__io->read_u1();
    m_trailing = m__io->read_u1();
}

prolink_djl_t::keep_alive_body_t::~keep_alive_body_t() {
    _clean_up();
}

void prolink_djl_t::keep_alive_body_t::_clean_up() {
}

prolink_djl_t::number_body_t::number_body_t(kaitai::kstream* p__io, prolink_djl_t* p__parent, prolink_djl_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void prolink_djl_t::number_body_t::_read() {
    m_device_number = m__io->read_u1();
    m_iteration = m__io->read_u1();
}

prolink_djl_t::number_body_t::~number_body_t() {
    _clean_up();
}

void prolink_djl_t::number_body_t::_clean_up() {
}

prolink_djl_t::number_conflict_body_t::number_conflict_body_t(kaitai::kstream* p__io, prolink_djl_t* p__parent, prolink_djl_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void prolink_djl_t::number_conflict_body_t::_read() {
    m_device_number = m__io->read_u1();
    m_ip = m__io->read_bytes(4);
}

prolink_djl_t::number_conflict_body_t::~number_conflict_body_t() {
    _clean_up();
}

void prolink_djl_t::number_conflict_body_t::_clean_up() {
}

prolink_djl_t::unknown_body_t::unknown_body_t(kaitai::kstream* p__io, prolink_djl_t* p__parent, prolink_djl_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void prolink_djl_t::unknown_body_t::_read() {
    m_rest = m__io->read_bytes_full();
}

prolink_djl_t::unknown_body_t::~unknown_body_t() {
    _clean_up();
}

void prolink_djl_t::unknown_body_t::_clean_up() {
}

std::string prolink_djl_t::device_name_raw() {
    if (f_device_name_raw)
        return m_device_name_raw;
    f_device_name_raw = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(12);
    m_device_name_raw = m__io->read_bytes(20);
    m__io->seek(_pos);
    return m_device_name_raw;
}
