// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

#include "prolink_dbserver.h"
#include "kaitai/exceptions.h"

prolink_dbserver_t::prolink_dbserver_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent, prolink_dbserver_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root ? p__root : this;
    m_args = nullptr;
    _read();
}

void prolink_dbserver_t::_read() {
    m_magic_tag = m__io->read_u1();
    if (!(m_magic_tag == 17)) {
        throw kaitai::validation_not_equal_error<uint8_t>(17, m_magic_tag, m__io, std::string("/seq/0"));
    }
    m_magic = m__io->read_u4be();
    if (!(m_magic == 2267236782UL)) {
        throw kaitai::validation_not_equal_error<uint32_t>(2267236782UL, m_magic, m__io, std::string("/seq/1"));
    }
    m_transaction_id_tag = m__io->read_u1();
    if (!(m_transaction_id_tag == 17)) {
        throw kaitai::validation_not_equal_error<uint8_t>(17, m_transaction_id_tag, m__io, std::string("/seq/2"));
    }
    m_transaction_id = m__io->read_u4be();
    m_message_type_tag = m__io->read_u1();
    if (!(m_message_type_tag == 16)) {
        throw kaitai::validation_not_equal_error<uint8_t>(16, m_message_type_tag, m__io, std::string("/seq/4"));
    }
    m_message_type = m__io->read_u2be();
    m_num_args_tag = m__io->read_u1();
    if (!(m_num_args_tag == 15)) {
        throw kaitai::validation_not_equal_error<uint8_t>(15, m_num_args_tag, m__io, std::string("/seq/6"));
    }
    m_num_args = m__io->read_u1();
    if (!(m_num_args <= 12)) {
        throw kaitai::validation_greater_than_error<uint8_t>(12, m_num_args, m__io, std::string("/seq/7"));
    }
    m_arg_tags_tag = m__io->read_u1();
    if (!(m_arg_tags_tag == 20)) {
        throw kaitai::validation_not_equal_error<uint8_t>(20, m_arg_tags_tag, m__io, std::string("/seq/8"));
    }
    m_len_arg_tags = m__io->read_u4be();
    if (!(m_len_arg_tags == 12)) {
        throw kaitai::validation_not_equal_error<uint32_t>(12, m_len_arg_tags, m__io, std::string("/seq/9"));
    }
    m_arg_tags = m__io->read_bytes(len_arg_tags());
    m_args = std::unique_ptr<std::vector<std::unique_ptr<argument_t>>>(new std::vector<std::unique_ptr<argument_t>>());
    const int l_args = num_args();
    for (int i = 0; i < l_args; i++) {
        m_args->push_back(std::move(std::unique_ptr<argument_t>(new argument_t(arg_tags().at(i), ((i == 0) ? (1) : (args()->at(i - 1)->num_value())), m__io, this, m__root))));
    }
}

prolink_dbserver_t::~prolink_dbserver_t() {
    _clean_up();
}

void prolink_dbserver_t::_clean_up() {
}

prolink_dbserver_t::argument_t::argument_t(uint8_t p_arg_tag, uint32_t p_prev_value, kaitai::kstream* p__io, prolink_dbserver_t* p__parent, prolink_dbserver_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_arg_tag = p_arg_tag;
    m_prev_value = p_prev_value;
    m_field = nullptr;
    f_is_present = false;
    f_num_value = false;
    _read();
}

void prolink_dbserver_t::argument_t::_read() {
    n_field = true;
    if (is_present()) {
        n_field = false;
        m_field = std::unique_ptr<field_t>(new field_t(m__io, this, m__root));
    }
}

prolink_dbserver_t::argument_t::~argument_t() {
    _clean_up();
}

void prolink_dbserver_t::argument_t::_clean_up() {
    if (!n_field) {
    }
}

bool prolink_dbserver_t::argument_t::is_present() {
    if (f_is_present)
        return m_is_present;
    f_is_present = true;
    m_is_present = !( ((arg_tag() == 3) && (prev_value() == 0)) );
    return m_is_present;
}

uint32_t prolink_dbserver_t::argument_t::num_value() {
    if (f_num_value)
        return m_num_value;
    f_num_value = true;
    m_num_value = ((is_present()) ? (field()->num_value()) : (1));
    return m_num_value;
}

prolink_dbserver_t::field_t::field_t(kaitai::kstream* p__io, prolink_dbserver_t::argument_t* p__parent, prolink_dbserver_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    f_num_value = false;
    _read();
}

void prolink_dbserver_t::field_t::_read() {
    m_field_type = m__io->read_u1();
    if (!( ((m_field_type == 15) || (m_field_type == 16) || (m_field_type == 17) || (m_field_type == 20) || (m_field_type == 38)) )) {
        throw kaitai::validation_not_any_of_error<uint8_t>(m_field_type, m__io, std::string("/types/field/seq/0"));
    }
    n_value_u8 = true;
    if (field_type() == 15) {
        n_value_u8 = false;
        m_value_u8 = m__io->read_u1();
    }
    n_value_u16 = true;
    if (field_type() == 16) {
        n_value_u16 = false;
        m_value_u16 = m__io->read_u2be();
    }
    n_value_u32 = true;
    if (field_type() == 17) {
        n_value_u32 = false;
        m_value_u32 = m__io->read_u4be();
    }
    n_len_blob = true;
    if (field_type() == 20) {
        n_len_blob = false;
        m_len_blob = m__io->read_u4be();
        if (!(m_len_blob <= 16777216)) {
            throw kaitai::validation_greater_than_error<uint32_t>(16777216, m_len_blob, m__io, std::string("/types/field/seq/4"));
        }
    }
    n_blob = true;
    if (field_type() == 20) {
        n_blob = false;
        m_blob = m__io->read_bytes(len_blob());
    }
    n_num_chars = true;
    if (field_type() == 38) {
        n_num_chars = false;
        m_num_chars = m__io->read_u4be();
        if (!(m_num_chars <= 8388608)) {
            throw kaitai::validation_greater_than_error<uint32_t>(8388608, m_num_chars, m__io, std::string("/types/field/seq/6"));
        }
    }
    n_text_raw = true;
    if (field_type() == 38) {
        n_text_raw = false;
        m_text_raw = m__io->read_bytes(num_chars() * 2);
    }
}

prolink_dbserver_t::field_t::~field_t() {
    _clean_up();
}

void prolink_dbserver_t::field_t::_clean_up() {
    if (!n_value_u8) {
    }
    if (!n_value_u16) {
    }
    if (!n_value_u32) {
    }
    if (!n_len_blob) {
    }
    if (!n_blob) {
    }
    if (!n_num_chars) {
    }
    if (!n_text_raw) {
    }
}

uint32_t prolink_dbserver_t::field_t::num_value() {
    if (f_num_value)
        return m_num_value;
    f_num_value = true;
    m_num_value = ((field_type() == 15) ? (value_u8()) : (((field_type() == 16) ? (value_u16()) : (((field_type() == 17) ? (value_u32()) : (1))))));
    return m_num_value;
}
