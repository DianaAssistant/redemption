/*
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

   Product name: redemption, a FLOSS RDP proxy
   Copyright (C) Wallix 2013
   Author(s): Christophe Grosjean

   Network related utility functions

*/

#pragma once

#include "utils/sugar/unique_fd.hpp"
#include "utils/sugar/bytes_view.hpp"
#include "utils/sugar/zstring_view.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>

class in_addr;
class addrinfo;

struct IpAddress
{
    char ip_addr[46] {};
};

struct AddrInfoDeleter
{
    void operator()(addrinfo *addr_info) noexcept;
};

using AddrInfoPtrWithDel_t = std::unique_ptr<addrinfo, AddrInfoDeleter>;

bool try_again(int errnum);

/// std::expected
/// \return nullptr if ok, view string if error
char const* resolve_ipv4_address(const char* ip, in_addr & s4_sin_addr);

unique_fd ip_connect(const char* ip, int port, char const** error_result = nullptr);

[[nodiscard]]
AddrInfoPtrWithDel_t resolve_both_ipv4_and_ipv6_address
(const char *ip, int port, const char **error_result = nullptr) noexcept;

unique_fd ip_connect_both_ipv4_and_ipv6
(const char* ip, int port, const char **error_result = nullptr) noexcept;

unique_fd local_connect(const char* sck_name, bool no_log);

unique_fd addr_connect(zstring_view addr, bool no_log_for_unix_socket);

unique_fd addr_connect_blocking(zstring_view addr, bool no_log_for_unix_socket);

bool compare_binary_ipv4(const char *s1, const char *s2);
bool compare_binary_ipv6(const char *s1, const char *s2);

/// \return ip found or empty view whether not found or error
zstring_view parse_ip_conntrack(
    int fd, const char * source, const char * dest, int sport, int dport,
    writable_bytes_view transparent_dest,
    bool (*ip_compare)(const char *s1, const char *s2), uint32_t verbose);

FILE* popen_conntrack(const char* source_ip, int source_port, int target_port);

[[nodiscard]]
bool get_local_ip_address(IpAddress& client_address, int fd) noexcept;
