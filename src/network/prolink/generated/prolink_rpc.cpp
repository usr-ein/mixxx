// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

#include "prolink_rpc.h"
#include "kaitai/exceptions.h"
const std::set<prolink_rpc_t::auth_flavor_t> prolink_rpc_t::_values_auth_flavor_t{
    prolink_rpc_t::AUTH_FLAVOR_AUTH_NULL,
    prolink_rpc_t::AUTH_FLAVOR_AUTH_UNIX,
    prolink_rpc_t::AUTH_FLAVOR_AUTH_SHORT,
};
bool prolink_rpc_t::_is_defined_auth_flavor_t(prolink_rpc_t::auth_flavor_t v) {
    return prolink_rpc_t::_values_auth_flavor_t.find(v) != prolink_rpc_t::_values_auth_flavor_t.end();
}
const std::set<prolink_rpc_t::ip_protocol_t> prolink_rpc_t::_values_ip_protocol_t{
    prolink_rpc_t::IP_PROTOCOL_TCP,
    prolink_rpc_t::IP_PROTOCOL_UDP,
};
bool prolink_rpc_t::_is_defined_ip_protocol_t(prolink_rpc_t::ip_protocol_t v) {
    return prolink_rpc_t::_values_ip_protocol_t.find(v) != prolink_rpc_t::_values_ip_protocol_t.end();
}
const std::set<prolink_rpc_t::mount_proc_t> prolink_rpc_t::_values_mount_proc_t{
    prolink_rpc_t::MOUNT_PROC_NULL_PROC,
    prolink_rpc_t::MOUNT_PROC_MNT,
    prolink_rpc_t::MOUNT_PROC_DUMP,
    prolink_rpc_t::MOUNT_PROC_UMNT,
    prolink_rpc_t::MOUNT_PROC_UMNT_ALL,
    prolink_rpc_t::MOUNT_PROC_EXPORT,
};
bool prolink_rpc_t::_is_defined_mount_proc_t(prolink_rpc_t::mount_proc_t v) {
    return prolink_rpc_t::_values_mount_proc_t.find(v) != prolink_rpc_t::_values_mount_proc_t.end();
}
const std::set<prolink_rpc_t::nfs_proc_t> prolink_rpc_t::_values_nfs_proc_t{
    prolink_rpc_t::NFS_PROC_NULL_PROC,
    prolink_rpc_t::NFS_PROC_GETATTR,
    prolink_rpc_t::NFS_PROC_SETATTR,
    prolink_rpc_t::NFS_PROC_LOOKUP,
    prolink_rpc_t::NFS_PROC_READLINK,
    prolink_rpc_t::NFS_PROC_READ,
    prolink_rpc_t::NFS_PROC_WRITE,
    prolink_rpc_t::NFS_PROC_CREATE,
    prolink_rpc_t::NFS_PROC_REMOVE,
    prolink_rpc_t::NFS_PROC_RENAME,
    prolink_rpc_t::NFS_PROC_LINK,
    prolink_rpc_t::NFS_PROC_SYMLINK,
    prolink_rpc_t::NFS_PROC_MKDIR,
    prolink_rpc_t::NFS_PROC_RMDIR,
    prolink_rpc_t::NFS_PROC_READDIR,
    prolink_rpc_t::NFS_PROC_STATFS,
};
bool prolink_rpc_t::_is_defined_nfs_proc_t(prolink_rpc_t::nfs_proc_t v) {
    return prolink_rpc_t::_values_nfs_proc_t.find(v) != prolink_rpc_t::_values_nfs_proc_t.end();
}
const std::set<prolink_rpc_t::portmap_proc_t> prolink_rpc_t::_values_portmap_proc_t{
    prolink_rpc_t::PORTMAP_PROC_NULL_PROC,
    prolink_rpc_t::PORTMAP_PROC_SET,
    prolink_rpc_t::PORTMAP_PROC_UNSET,
    prolink_rpc_t::PORTMAP_PROC_GETPORT,
    prolink_rpc_t::PORTMAP_PROC_DUMP,
};
bool prolink_rpc_t::_is_defined_portmap_proc_t(prolink_rpc_t::portmap_proc_t v) {
    return prolink_rpc_t::_values_portmap_proc_t.find(v) != prolink_rpc_t::_values_portmap_proc_t.end();
}
const std::set<prolink_rpc_t::program_t> prolink_rpc_t::_values_program_t{
    prolink_rpc_t::PROGRAM_PORTMAP,
    prolink_rpc_t::PROGRAM_NFS,
    prolink_rpc_t::PROGRAM_MOUNT,
};
bool prolink_rpc_t::_is_defined_program_t(prolink_rpc_t::program_t v) {
    return prolink_rpc_t::_values_program_t.find(v) != prolink_rpc_t::_values_program_t.end();
}

prolink_rpc_t::prolink_rpc_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent, prolink_rpc_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root ? p__root : this;
    m_credential = nullptr;
    m_verifier = nullptr;
    f_call_key = false;
    _read();
}

void prolink_rpc_t::_read() {
    m_xid = m__io->read_u4be();
    m_msg_type = m__io->read_u4be();
    if (!(m_msg_type == 0)) {
        throw kaitai::validation_not_equal_error<uint32_t>(0, m_msg_type, m__io, std::string("/seq/1"));
    }
    m_rpc_version = m__io->read_u4be();
    if (!(m_rpc_version == 2)) {
        throw kaitai::validation_not_equal_error<uint32_t>(2, m_rpc_version, m__io, std::string("/seq/2"));
    }
    m_program = static_cast<prolink_rpc_t::program_t>(m__io->read_u4be());
    m_program_version = m__io->read_u4be();
    m_procedure = m__io->read_u4be();
    m_credential = std::unique_ptr<opaque_auth_t>(new opaque_auth_t(m__io, this, m__root));
    m_verifier = std::unique_ptr<opaque_auth_t>(new opaque_auth_t(m__io, this, m__root));
    switch (call_key()) {
    case 100000003: {
        m_arguments = std::unique_ptr<getport_args_t>(new getport_args_t(m__io, this, m__root));
        break;
    }
    case 100003001: {
        m_arguments = std::unique_ptr<fhandle_args_t>(new fhandle_args_t(m__io, this, m__root));
        break;
    }
    case 100003004: {
        m_arguments = std::unique_ptr<lookup_args_t>(new lookup_args_t(m__io, this, m__root));
        break;
    }
    case 100003006: {
        m_arguments = std::unique_ptr<read_args_t>(new read_args_t(m__io, this, m__root));
        break;
    }
    case 100003016: {
        m_arguments = std::unique_ptr<readdir_args_t>(new readdir_args_t(m__io, this, m__root));
        break;
    }
    case 100003017: {
        m_arguments = std::unique_ptr<fhandle_args_t>(new fhandle_args_t(m__io, this, m__root));
        break;
    }
    case 100005001: {
        m_arguments = std::unique_ptr<path_args_t>(new path_args_t(m__io, this, m__root));
        break;
    }
    case 100005003: {
        m_arguments = std::unique_ptr<path_args_t>(new path_args_t(m__io, this, m__root));
        break;
    }
    default: {
        m_arguments = std::unique_ptr<void_args_t>(new void_args_t(m__io, this, m__root));
        break;
    }
    }
}

prolink_rpc_t::~prolink_rpc_t() {
    _clean_up();
}

void prolink_rpc_t::_clean_up() {
}

prolink_rpc_t::fhandle_args_t::fhandle_args_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent, prolink_rpc_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void prolink_rpc_t::fhandle_args_t::_read() {
    m_fhandle = m__io->read_bytes(32);
}

prolink_rpc_t::fhandle_args_t::~fhandle_args_t() {
    _clean_up();
}

void prolink_rpc_t::fhandle_args_t::_clean_up() {
}

prolink_rpc_t::getport_args_t::getport_args_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent, prolink_rpc_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void prolink_rpc_t::getport_args_t::_read() {
    m_program = static_cast<prolink_rpc_t::program_t>(m__io->read_u4be());
    m_program_version = m__io->read_u4be();
    m_protocol = static_cast<prolink_rpc_t::ip_protocol_t>(m__io->read_u4be());
    m_port = m__io->read_u4be();
}

prolink_rpc_t::getport_args_t::~getport_args_t() {
    _clean_up();
}

void prolink_rpc_t::getport_args_t::_clean_up() {
}

prolink_rpc_t::lookup_args_t::lookup_args_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent, prolink_rpc_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_name = nullptr;
    _read();
}

void prolink_rpc_t::lookup_args_t::_read() {
    m_dir_fhandle = m__io->read_bytes(32);
    m_name = std::unique_ptr<xdr_string_t>(new xdr_string_t(m__io, this, m__root));
}

prolink_rpc_t::lookup_args_t::~lookup_args_t() {
    _clean_up();
}

void prolink_rpc_t::lookup_args_t::_clean_up() {
}

prolink_rpc_t::opaque_auth_t::opaque_auth_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent, prolink_rpc_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void prolink_rpc_t::opaque_auth_t::_read() {
    m_flavor = static_cast<prolink_rpc_t::auth_flavor_t>(m__io->read_u4be());
    m_len_body = m__io->read_u4be();
    if (!(m_len_body <= 400)) {
        throw kaitai::validation_greater_than_error<uint32_t>(400, m_len_body, m__io, std::string("/types/opaque_auth/seq/1"));
    }
    m_body = m__io->read_bytes(len_body());
    m_padding = m__io->read_bytes(kaitai::kstream::mod(4 - kaitai::kstream::mod(len_body(), 4), 4));
}

prolink_rpc_t::opaque_auth_t::~opaque_auth_t() {
    _clean_up();
}

void prolink_rpc_t::opaque_auth_t::_clean_up() {
}

prolink_rpc_t::path_args_t::path_args_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent, prolink_rpc_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_path = nullptr;
    _read();
}

void prolink_rpc_t::path_args_t::_read() {
    m_path = std::unique_ptr<xdr_string_t>(new xdr_string_t(m__io, this, m__root));
}

prolink_rpc_t::path_args_t::~path_args_t() {
    _clean_up();
}

void prolink_rpc_t::path_args_t::_clean_up() {
}

prolink_rpc_t::read_args_t::read_args_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent, prolink_rpc_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void prolink_rpc_t::read_args_t::_read() {
    m_fhandle = m__io->read_bytes(32);
    m_offset = m__io->read_u4be();
    m_count = m__io->read_u4be();
    m_total_count = m__io->read_u4be();
}

prolink_rpc_t::read_args_t::~read_args_t() {
    _clean_up();
}

void prolink_rpc_t::read_args_t::_clean_up() {
}

prolink_rpc_t::readdir_args_t::readdir_args_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent, prolink_rpc_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void prolink_rpc_t::readdir_args_t::_read() {
    m_fhandle = m__io->read_bytes(32);
    m_cookie = m__io->read_bytes(4);
    m_count = m__io->read_u4be();
}

prolink_rpc_t::readdir_args_t::~readdir_args_t() {
    _clean_up();
}

void prolink_rpc_t::readdir_args_t::_clean_up() {
}

prolink_rpc_t::void_args_t::void_args_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent, prolink_rpc_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void prolink_rpc_t::void_args_t::_read() {
    m_rest = m__io->read_bytes_full();
}

prolink_rpc_t::void_args_t::~void_args_t() {
    _clean_up();
}

void prolink_rpc_t::void_args_t::_clean_up() {
}

prolink_rpc_t::xdr_string_t::xdr_string_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent, prolink_rpc_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void prolink_rpc_t::xdr_string_t::_read() {
    m_len_value = m__io->read_u4be();
    if (!(m_len_value <= 1024)) {
        throw kaitai::validation_greater_than_error<uint32_t>(1024, m_len_value, m__io, std::string("/types/xdr_string/seq/0"));
    }
    m_value = m__io->read_bytes(len_value());
    m_padding = m__io->read_bytes(kaitai::kstream::mod(4 - kaitai::kstream::mod(len_value(), 4), 4));
}

prolink_rpc_t::xdr_string_t::~xdr_string_t() {
    _clean_up();
}

void prolink_rpc_t::xdr_string_t::_clean_up() {
}

int32_t prolink_rpc_t::call_key() {
    if (f_call_key)
        return m_call_key;
    f_call_key = true;
    m_call_key = kaitai::kstream::mod(program(), 1000000) * 1000 + kaitai::kstream::mod(procedure(), 1000);
    return m_call_key;
}
