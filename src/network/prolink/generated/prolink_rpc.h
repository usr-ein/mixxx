#pragma once

// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

class prolink_rpc_t;

#include "kaitai/kaitaistruct.h"
#include <stdint.h>
#include <memory>
#include <set>

#if KAITAI_STRUCT_VERSION < 11000L
#error "Incompatible Kaitai Struct C++/STL API: version 0.11 or later is required"
#endif

/**
 * The **call** direction of ONC RPC v2 (RFC 1057), plus the argument bodies of
 * the eleven procedures a CDJ actually invokes across the three programs it
 * expects a peer to run: the portmapper, MOUNT and NFS v2.
 * 
 * This is the serve side. Mixxx already speaks the client half by hand
 * (`src/network/prolink/rpc/`), and that half only ever *builds* calls and
 * *parses* replies; this schema is what lets it parse the calls a real player
 * makes to us. The reply direction stays hand-written -- Kaitai generates no C++
 * serializers -- and its unit tests round-trip through the client parsers, so
 * both halves of every procedure are checked against each other.
 * 
 * **Only calls.** `msg_type` and `rpc_version` are validated rather than
 * switched on, so a reply or a v1/v3 call fails to parse instead of decoding
 * into something plausible. On our ports that traffic belongs to somebody else,
 * and dropping it is the correct answer.
 * 
 * Three details of Pioneer's usage that this schema pins down:
 * 
 * * **Path and file names are UTF-16LE**, not the ASCII that standard NFS and
 *   MOUNT use, still length-prefixed, and the prefix counts **bytes** -- so an
 *   n-character ASCII name announces 2n. This is the single most important
 *   non-standard fact about the file-access path, and it is why libnfs cannot
 *   simply be linked: its wire encoder emits ASCII.
 * * **Credentials are not enforced.** A player exports to the whole link-local
 *   subnet. They are parsed here for the record and ignored by the server; being
 *   stricter than the hardware we are impersonating would only make us the
 *   reason a real deck fails.
 * * **Offsets and sizes are 32-bit**, so NFSv2 cannot address past 4 GiB. Fine
 *   for audio, but the ceiling must be asserted rather than silently wrapped.
 * \sa docs/PROTOCOL.md
 */

class prolink_rpc_t : public kaitai::kstruct {

public:
    class fhandle_args_t;
    class getport_args_t;
    class lookup_args_t;
    class opaque_auth_t;
    class path_args_t;
    class read_args_t;
    class readdir_args_t;
    class void_args_t;
    class xdr_string_t;

    enum auth_flavor_t {
        AUTH_FLAVOR_AUTH_NULL = 0,
        AUTH_FLAVOR_AUTH_UNIX = 1,
        AUTH_FLAVOR_AUTH_SHORT = 2
    };
    static bool _is_defined_auth_flavor_t(auth_flavor_t v);

private:
    static const std::set<auth_flavor_t> _values_auth_flavor_t;

public:

    enum ip_protocol_t {
        IP_PROTOCOL_TCP = 6,
        IP_PROTOCOL_UDP = 17
    };
    static bool _is_defined_ip_protocol_t(ip_protocol_t v);

private:
    static const std::set<ip_protocol_t> _values_ip_protocol_t;

public:

    enum mount_proc_t {
        MOUNT_PROC_NULL_PROC = 0,
        MOUNT_PROC_MNT = 1,
        MOUNT_PROC_DUMP = 2,
        MOUNT_PROC_UMNT = 3,
        MOUNT_PROC_UMNT_ALL = 4,
        MOUNT_PROC_EXPORT = 5
    };
    static bool _is_defined_mount_proc_t(mount_proc_t v);

private:
    static const std::set<mount_proc_t> _values_mount_proc_t;

public:

    enum nfs_proc_t {
        NFS_PROC_NULL_PROC = 0,
        NFS_PROC_GETATTR = 1,
        NFS_PROC_SETATTR = 2,
        NFS_PROC_LOOKUP = 4,
        NFS_PROC_READLINK = 5,
        NFS_PROC_READ = 6,
        NFS_PROC_WRITE = 8,
        NFS_PROC_CREATE = 9,
        NFS_PROC_REMOVE = 10,
        NFS_PROC_RENAME = 11,
        NFS_PROC_LINK = 12,
        NFS_PROC_SYMLINK = 13,
        NFS_PROC_MKDIR = 14,
        NFS_PROC_RMDIR = 15,
        NFS_PROC_READDIR = 16,
        NFS_PROC_STATFS = 17
    };
    static bool _is_defined_nfs_proc_t(nfs_proc_t v);

private:
    static const std::set<nfs_proc_t> _values_nfs_proc_t;

public:

    enum portmap_proc_t {
        PORTMAP_PROC_NULL_PROC = 0,
        PORTMAP_PROC_SET = 1,
        PORTMAP_PROC_UNSET = 2,
        PORTMAP_PROC_GETPORT = 3,
        PORTMAP_PROC_DUMP = 4
    };
    static bool _is_defined_portmap_proc_t(portmap_proc_t v);

private:
    static const std::set<portmap_proc_t> _values_portmap_proc_t;

public:

    enum program_t {
        PROGRAM_PORTMAP = 100000,
        PROGRAM_NFS = 100003,
        PROGRAM_MOUNT = 100005
    };
    static bool _is_defined_program_t(program_t v);

private:
    static const std::set<program_t> _values_program_t;

public:

    prolink_rpc_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, prolink_rpc_t* p__root = nullptr);

private:
    void _read();
    void _clean_up();

public:
    ~prolink_rpc_t();

    /**
     * NFS `getattr` and `statfs`.
     */

    class fhandle_args_t : public kaitai::kstruct {

    public:

        fhandle_args_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent = nullptr, prolink_rpc_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~fhandle_args_t();

    private:
        std::string m_fhandle;
        prolink_rpc_t* m__root;
        prolink_rpc_t* m__parent;

    public:
        std::string fhandle() const { return m_fhandle; }
        prolink_rpc_t* _root() const { return m__root; }
        prolink_rpc_t* _parent() const { return m__parent; }
    };

    /**
     * "Which port serves this program?" The gate on everything: a deck asks the
     * portmapper for mountd and nfsd *before* it opens dbserver, retries once a
     * second indefinitely if nothing answers, and never falls back to the
     * well-known ports even when those are bound and idle (F46).
     */

    class getport_args_t : public kaitai::kstruct {

    public:

        getport_args_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent = nullptr, prolink_rpc_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~getport_args_t();

    private:
        program_t m_program;
        uint32_t m_program_version;
        ip_protocol_t m_protocol;
        uint32_t m_port;
        prolink_rpc_t* m__root;
        prolink_rpc_t* m__parent;

    public:
        program_t program() const { return m_program; }
        uint32_t program_version() const { return m_program_version; }
        ip_protocol_t protocol() const { return m_protocol; }

        /**
         * Ignored in a GETPORT; the reply carries the answer.
         */
        uint32_t port() const { return m_port; }
        prolink_rpc_t* _root() const { return m__root; }
        prolink_rpc_t* _parent() const { return m__parent; }
    };

    /**
     * Walk one path component. A player resolves a track's path one `lookup` per
     * directory from the mount root, so this is by far the most frequent call.
     */

    class lookup_args_t : public kaitai::kstruct {

    public:

        lookup_args_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent = nullptr, prolink_rpc_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~lookup_args_t();

    private:
        std::string m_dir_fhandle;
        std::unique_ptr<xdr_string_t> m_name;
        prolink_rpc_t* m__root;
        prolink_rpc_t* m__parent;

    public:
        std::string dir_fhandle() const { return m_dir_fhandle; }

        /**
         * UTF-16LE. See the top-level doc.
         */
        xdr_string_t* name() const { return m_name.get(); }
        prolink_rpc_t* _root() const { return m__root; }
        prolink_rpc_t* _parent() const { return m__parent; }
    };

    /**
     * RFC 1057 §7.2: a flavour and a length-prefixed body, padded to four bytes.
     * Real players send AUTH_UNIX with a **fresh stamp on every call** -- it is a
     * nonce, not the magic constant that documentation and one reference client
     * both took it for (C8).
     */

    class opaque_auth_t : public kaitai::kstruct {

    public:

        opaque_auth_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent = nullptr, prolink_rpc_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~opaque_auth_t();

    private:
        auth_flavor_t m_flavor;
        uint32_t m_len_body;
        std::string m_body;
        std::string m_padding;
        prolink_rpc_t* m__root;
        prolink_rpc_t* m__parent;

    public:
        auth_flavor_t flavor() const { return m_flavor; }

        /**
         * RFC 1057's own ceiling on an opaque_auth body.
         */
        uint32_t len_body() const { return m_len_body; }
        std::string body() const { return m_body; }
        std::string padding() const { return m_padding; }
        prolink_rpc_t* _root() const { return m__root; }
        prolink_rpc_t* _parent() const { return m__parent; }
    };

    /**
     * MOUNT `mnt` and `umnt`. The export path, UTF-16LE.
     */

    class path_args_t : public kaitai::kstruct {

    public:

        path_args_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent = nullptr, prolink_rpc_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~path_args_t();

    private:
        std::unique_ptr<xdr_string_t> m_path;
        prolink_rpc_t* m__root;
        prolink_rpc_t* m__parent;

    public:
        xdr_string_t* path() const { return m_path.get(); }
        prolink_rpc_t* _root() const { return m__root; }
        prolink_rpc_t* _parent() const { return m__parent; }
    };

    class read_args_t : public kaitai::kstruct {

    public:

        read_args_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent = nullptr, prolink_rpc_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~read_args_t();

    private:
        std::string m_fhandle;
        uint32_t m_offset;
        uint32_t m_count;
        uint32_t m_total_count;
        prolink_rpc_t* m__root;
        prolink_rpc_t* m__parent;

    public:
        std::string fhandle() const { return m_fhandle; }
        uint32_t offset() const { return m_offset; }
        uint32_t count() const { return m_count; }

        /**
         * Deprecated already in RFC 1094 and ignored by every server, including
         * this one. Parsed so the argument block is fully accounted for.
         */
        uint32_t total_count() const { return m_total_count; }
        prolink_rpc_t* _root() const { return m__root; }
        prolink_rpc_t* _parent() const { return m__parent; }
    };

    class readdir_args_t : public kaitai::kstruct {

    public:

        readdir_args_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent = nullptr, prolink_rpc_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~readdir_args_t();

    private:
        std::string m_fhandle;
        std::string m_cookie;
        uint32_t m_count;
        prolink_rpc_t* m__root;
        prolink_rpc_t* m__parent;

    public:
        std::string fhandle() const { return m_fhandle; }

        /**
         * Opaque position in the listing; all zeroes means "from the start".
         * Opaque to the *client*, that is -- the server mints it, so we are free
         * to make it an index.
         */
        std::string cookie() const { return m_cookie; }

        /**
         * Maximum reply size in bytes, not a number of entries.
         */
        uint32_t count() const { return m_count; }
        prolink_rpc_t* _root() const { return m__root; }
        prolink_rpc_t* _parent() const { return m__parent; }
    };

    class void_args_t : public kaitai::kstruct {

    public:

        void_args_t(kaitai::kstream* p__io, prolink_rpc_t* p__parent = nullptr, prolink_rpc_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~void_args_t();

    private:
        std::string m_rest;
        prolink_rpc_t* m__root;
        prolink_rpc_t* m__parent;

    public:
        std::string rest() const { return m_rest; }
        prolink_rpc_t* _root() const { return m__root; }
        prolink_rpc_t* _parent() const { return m__parent; }
    };

    /**
     * A length-prefixed, four-byte-padded byte run. Both the ASCII strings of
     * standard XDR and Pioneer's UTF-16LE names travel in this shape; which one
     * it is depends on the field, so the bytes are handed back undecoded.
     */

    class xdr_string_t : public kaitai::kstruct {

    public:

        xdr_string_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, prolink_rpc_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~xdr_string_t();

    private:
        uint32_t m_len_value;
        std::string m_value;
        std::string m_padding;
        prolink_rpc_t* m__root;
        kaitai::kstruct* m__parent;

    public:

        /**
         * Capped so a corrupt or hostile datagram claiming a 4 GiB name costs a
         * parse failure rather than an allocation. The longest real path on a
         * rekordbox medium is a few hundred bytes.
         */
        uint32_t len_value() const { return m_len_value; }
        std::string value() const { return m_value; }
        std::string padding() const { return m_padding; }
        prolink_rpc_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

private:
    bool f_call_key;
    int32_t m_call_key;

public:

    /**
     * Program and procedure flattened into one switch key. The three program
     * numbers are five- and six-digit, so multiplying by 1000 cannot collide
     * with any procedure number -- NFS has 18 of them and MOUNT 6.
     * 
     * Both operands are reduced first because neither is validated and Kaitai
     * evaluates this into a **signed** 32-bit expression. A datagram naming
     * program `0xffffffff` would otherwise overflow it, which is undefined
     * behaviour rather than the harmless fall-through to `void_args` that it
     * looks like. After reduction the maximum is 999,999,999, and for the three
     * real programs the reduction is the identity, so the keys below are
     * unaffected.
     */
    int32_t call_key();

private:
    uint32_t m_xid;
    uint32_t m_msg_type;
    uint32_t m_rpc_version;
    program_t m_program;
    uint32_t m_program_version;
    uint32_t m_procedure;
    std::unique_ptr<opaque_auth_t> m_credential;
    std::unique_ptr<opaque_auth_t> m_verifier;
    std::unique_ptr<kaitai::kstruct> m_arguments;
    prolink_rpc_t* m__root;
    kaitai::kstruct* m__parent;

public:

    /**
     * The caller's correlation token. Echoed verbatim in the reply and otherwise
     * meaningless to us -- notably it is *not* a sequence number we may validate.
     */
    uint32_t xid() const { return m_xid; }

    /**
     * 0 is CALL. A reply on our port is not ours to answer.
     */
    uint32_t msg_type() const { return m_msg_type; }
    uint32_t rpc_version() const { return m_rpc_version; }
    program_t program() const { return m_program; }
    uint32_t program_version() const { return m_program_version; }
    uint32_t procedure() const { return m_procedure; }
    opaque_auth_t* credential() const { return m_credential.get(); }

    /**
     * AUTH_NULL with an empty body on every call we have seen.
     */
    opaque_auth_t* verifier() const { return m_verifier.get(); }

    /**
     * Dispatched on program and procedure together, because the procedure
     * numbers collide across programs -- MOUNT's `mnt` and NFS's `getattr` are
     * both 1.
     * 
     * Everything unrecognised parses as `void_args` rather than failing. The
     * procedures with no arguments (`null`, portmap `dump`, MOUNT `export`) are
     * genuinely empty, and for anything else a server that can at least read the
     * header can answer PROC_UNAVAIL -- which is a real answer, and what a
     * player expects when it probes for a procedure that is not implemented.
     */
    kaitai::kstruct* arguments() const { return m_arguments.get(); }
    prolink_rpc_t* _root() const { return m__root; }
    kaitai::kstruct* _parent() const { return m__parent; }
};
