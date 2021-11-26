/*
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

  Product name: redemption, a FLOSS RDP proxy
  Copyright (C) Wallix 2010 - 2012
  Author(s): Christophe Grosjean, Raphael Zhou

  Main loop
*/

#include "core/listen.hpp"
#include "core/mainloop.hpp"
#include "core/session.hpp"
#include "core/pid_file.hpp"
#include "core/font.hpp"
#include "main/version.hpp"
#include "utils/log.hpp"
#include "utils/log_siem.hpp"
#include "utils/netutils.hpp"
#include "utils/strutils.hpp"
#include "utils/ip.hpp"
#include "utils/monotonic_clock.hpp"
#include "utils/sugar/chars_to_int.hpp"

#include "configs/config.hpp"

#include <charconv>

#include <cerrno>
#include <csignal>
#include <cstring>

#include <unistd.h>

#include <sys/un.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <sys/stat.h>


namespace {
/*****************************************************************************/
[[noreturn]]
void shutdown(int sig)
{
    LOG(LOG_INFO, "shutting down : signal %d pid=%d", sig, getpid());
    exit(1);
}

/*****************************************************************************/
void sigpipe(int sig)
{
    LOG(LOG_INFO, "got SIGPIPE(%d) : ignoring", sig);
}

/*****************************************************************************/
//void sigsegv(int sig)
//{
//    LOG(LOG_INFO, "Ignoring SIGSEGV : signal %d", sig);
//}

void sighup(int sig)
{
    LOG(LOG_INFO, "Ignoring SIGHUP : signal %d", sig);
}

//void sigchld(int sig)
//{
//    // triggered when a child close. For now we will just ignore this signal
//    // because there is no child termination management yet.
//    // When there will be child management code, we will have to setup
//    // some communication protocol to discuss with childs.
//    LOG(LOG_INFO, "Ignoring SIGCHLD : signal %d pid %d", sig, getpid());
//}

void init_signals()
{
    struct sigaction sa;

    sa.sa_flags = 0;

    sigemptyset(&sa.sa_mask);
#ifdef NDEBUG
    sigaddset(&sa.sa_mask, SIGSEGV);
#endif
    sigaddset(&sa.sa_mask, SIGBUS);
    sigaddset(&sa.sa_mask, SIGTERM);
    sigaddset(&sa.sa_mask, SIGHUP);
    sigaddset(&sa.sa_mask, SIGINT);
    sigaddset(&sa.sa_mask, SIGPIPE);
    sigaddset(&sa.sa_mask, SIGCHLD);
    sigaddset(&sa.sa_mask, SIGALRM);
    sigaddset(&sa.sa_mask, SIGUSR1);
    sigaddset(&sa.sa_mask, SIGUSR2);

REDEMPTION_DIAGNOSTIC_PUSH()
REDEMPTION_DIAGNOSTIC_GCC_IGNORE("-Wold-style-cast")
REDEMPTION_DIAGNOSTIC_GCC_ONLY_IGNORE("-Wzero-as-null-pointer-constant")
#if REDEMPTION_COMP_CLANG_VERSION >= REDEMPTION_COMP_VERSION_NUMBER(5, 0, 0)
    REDEMPTION_DIAGNOSTIC_CLANG_IGNORE("-Wzero-as-null-pointer-constant")
#endif

#ifdef NDEBUG
    sa.sa_handler = SIG_IGN; /*NOLINT*/
    sigaction(SIGSEGV, &sa, nullptr);
#endif

    sa.sa_handler = SIG_DFL;
    sigaction(SIGBUS, &sa, nullptr);

    sa.sa_handler = shutdown;
    sigaction(SIGTERM, &sa, nullptr);

    sa.sa_handler = sighup;
    sigaction(SIGHUP, &sa, nullptr);

    sa.sa_handler = shutdown;
    sigaction(SIGINT, &sa, nullptr);

    sa.sa_handler = sigpipe;
    sigaction(SIGPIPE, &sa, nullptr);

    sa.sa_handler = SIG_IGN; /*NOLINT*/
    sigaction(SIGCHLD, &sa, nullptr);

    sa.sa_handler = SIG_DFL;
    sigaction(SIGALRM, &sa, nullptr);

    sa.sa_handler = SIG_IGN; /*NOLINT*/
    sigaction(SIGUSR1, &sa, nullptr);

    sa.sa_handler = SIG_IGN; /*NOLINT*/
    sigaction(SIGUSR2, &sa, nullptr);
REDEMPTION_DIAGNOSTIC_POP()
}

}  // namespace

//void reset_signals(void)
//{
//    struct sigaction sa;
//
//    sa.sa_flags = 0;
//    sa.sa_handler = SIG_DFL;
//
//    sigemptyset(&sa.sa_mask);
//    sigaddset(&sa.sa_mask, SIGSEGV);
//    sigaddset(&sa.sa_mask, SIGBUS);
//    sigaddset(&sa.sa_mask, SIGTERM);
//    sigaddset(&sa.sa_mask, SIGHUP);
//    sigaddset(&sa.sa_mask, SIGINT);
//    sigaddset(&sa.sa_mask, SIGPIPE);
//    sigaddset(&sa.sa_mask, SIGCHLD);
//    sigaddset(&sa.sa_mask, SIGALRM);
//    sigaddset(&sa.sa_mask, SIGUSR1);
//    sigaddset(&sa.sa_mask, SIGUSR2);
//
//    sigaction(SIGSEGV, &sa, nullptr);
//    sigaction(SIGBUS, &sa, nullptr);
//    sigaction(SIGTERM, &sa, nullptr);
//    sigaction(SIGHUP, &sa, nullptr);
//    sigaction(SIGINT, &sa, nullptr);
//    sigaction(SIGPIPE, &sa, nullptr);
//    sigaction(SIGCHLD, &sa, nullptr);
//    sigaction(SIGALRM, &sa, nullptr);
//    sigaction(SIGUSR1, &sa, nullptr);
//    sigaction(SIGUSR2, &sa, nullptr);
//}

namespace
{
    enum SocketType : char
    {
        Ws,
        Wss,
        Tls,
    };

    void session_server_start(
        int incoming_sck, bool forkable, unsigned uid, unsigned gid,
        std::string const& config_filename, bool debug_config,
        SocketType socket_type, Font const& font)
    {
        union
        {
            struct sockaddr s;
            struct sockaddr_storage ss;
            struct sockaddr_in s4;
            struct sockaddr_in6 s6;
        } u;
        unsigned int sin_size = sizeof(u);
        memset(&u, 0, sin_size);

        int const sck = accept(incoming_sck, &u.s, &sin_size);
        if (-1 == sck) {
            LOG(LOG_ERR, "Accept failed on socket %d (%s)", incoming_sck, strerror(errno));
            _exit(1);
        }

        /* start new process */
        const pid_t pid = forkable ? fork() : 0;
        switch (pid) {
        case 0: /* child */ {
        // TODO: see exit status of child, we could use it to diagnose session behaviours
        // TODO: we could probably use some session launcher object here. Something like
        // an abstraction layer that would manage either forking of threading behavior
        // this would also likely have some effect on network ressources management
        // (that means the select() on ressources could be managed by that layer)
            close(incoming_sck);

            const MonotonicTimePoint start_time = MonotonicTimePoint::clock::now();

            IpPort source_ip_port;

            if (auto error = source_ip_port.extract_of(u.s, sizeof(u.ss)).error) {
                LOG(LOG_ERR, "Cannot get ip and port of source : %s", error);
                _exit(1);
            }

            const auto source_ip = source_ip_port.ip_address();
            const auto source_port = source_ip_port.port();
            bool is_ipv6 = (u.s.sa_family == AF_INET6
                            && !source_ip_port.is_ipv4_mapped());

            using namespace std::string_view_literals;
            const bool source_is_localhost = (source_ip.to_sv() == "127.0.0.1"sv || source_ip.to_sv() == "::1"sv);
            Inifile ini;

            ini.set<cfg::debug::config>(debug_config);
            configuration_load(ini.configuration_holder(), config_filename.c_str());

            if (ini.get<cfg::debug::session>()){
                LOG(LOG_INFO, "Setting new session socket to %d", sck);
            }

            {
                long long const sec = std::chrono::duration_cast<std::chrono::seconds>(
                    MonotonicTimePoint::clock::now().time_since_epoch()).count();
                int const pid = getpid();
                char psid[128];
                char* p = psid;
                p = std::to_chars(p, std::end(psid), sec).ptr;
                p = std::to_chars(p, std::end(psid), pid).ptr;
                ini.set_acl<cfg::context::psid>(std::string_view(psid, std::size_t(p - psid)));
                log_proxy::set_psid(ini.get<cfg::context::psid>());
            }

            if (!source_is_localhost) {
                log_proxy::incoming_connection(source_ip.to_sv(), source_port);
            }

            union
            {
                struct sockaddr s;
                struct sockaddr_storage ss;
                struct sockaddr_in s4;
                struct sockaddr_in6 s6;
            } localAddress;
            socklen_t addressLength = sizeof(localAddress);

            if (-1 == getsockname(sck, &localAddress.s, &addressLength)){
                LOG(LOG_ERR, "getsockname failed error=%s", strerror(errno));
                _exit(1);
            }

            IpPort target_ip_port;

            if (auto error = target_ip_port.extract_of(
                localAddress.s, sizeof(localAddress.ss)).error
            ) {
                LOG(LOG_ERR, "Cannot get ip and port of target : %s", error);
                _exit(1);
            }

            const auto target_port = target_ip_port.port();
            zstring_view target_ip;

            if (!ini.get<cfg::debug::fake_target_ip>().empty()) {
                target_ip = ini.get<cfg::debug::fake_target_ip>();
                LOG(LOG_INFO, "fake_target_ip='%s'", target_ip);
            }
            else {
                target_ip = target_ip_port.ip_address();
            }

            if (!source_is_localhost) {
                // do not log early messages for localhost (to avoid tracing in watchdog)
                LOG(LOG_INFO, "Redemption " VERSION);
                LOG(LOG_INFO, "src=%s sport=%d dst=%s dport=%d",
                    source_ip, source_port, target_ip, target_port);
            }

            char real_target_ip_buff[256];
            zstring_view real_target_ip;
            auto ip_compare = (is_ipv6) ?
                compare_binary_ipv6 : compare_binary_ipv4;

            if (ini.get<cfg::globals::enable_transparent_mode>() && !source_is_localhost) {
                bool use_conntrack = false;
                FILE* fs = nullptr;
                int fd = open("/proc/net/nf_conntrack", O_RDONLY);
                if (fd < 0) {
                    int errno_nf = errno;
                    fd = open("/proc/net/ip_conntrack", O_RDONLY);
                    if (fd < 0) {
                        int errno_ip = errno;
                        fs = popen_conntrack(source_ip, source_port, target_port);
                        int errno_conntrack = errno;
                        if (fs == nullptr) {
                            LOG(LOG_WARNING, "Failed to read conntrack file /proc/net/nf_conntrack: %d", errno_nf);
                            LOG(LOG_WARNING, "Failed to read conntrack file /proc/net/ip_conntrack: %d", errno_ip);
                            LOG(LOG_WARNING, "Failed to run conntrack: %d", errno_conntrack);
                        }
                        else {
                            use_conntrack = true;
                            fd = fileno(fs);
                        }
                    }
                    else {
                        LOG(LOG_WARNING, "Reading /proc/net/ip_conntrack");
                    }
                }
                else {
                    LOG(LOG_WARNING, "Reading /proc/net/nf_conntrack");
                }

                uint32_t verbose = ini.get<cfg::debug::auth>();
                // source and dest are inverted because we get the information we want from reply path rule
                LOG(LOG_INFO, "transparent proxy: looking for real target for src=%s:%d dst=%s:%d",
                    source_ip, source_port, target_ip, target_port);

                real_target_ip = parse_ip_conntrack(
                    fd, target_ip, source_ip, target_port, source_port,
                    make_writable_array_view(real_target_ip_buff),
                    ip_compare, verbose);
                if (real_target_ip.empty()) {
                    LOG(LOG_WARNING, "Failed to get transparent proxy target from ip_conntrack: %d", fd);
                }

                if (use_conntrack) {
                    pclose(fs);
                }
                else {
                    close(fd);
                }

                if (setgid(gid) != 0) {
                    LOG(LOG_ERR, "Changing process group to %u failed with error: %s", gid, strerror(errno));
                    _exit(1);
                }

                if (setuid(uid) != 0) {
                    LOG(LOG_ERR, "Changing process user to %u failed with error: %s", uid, strerror(errno));
                    _exit(1);
                }

                LOG(LOG_INFO, "src=%s sport=%d dst=%s dport=%d",
                    source_ip, source_port, real_target_ip, target_port);
            }

            int nodelay = 1;
            if (0 == setsockopt(sck, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay))) {
                // Create session file
                int const child_pid = getpid();
                PidFile pid_file(child_pid);
                if (!pid_file.is_open()) {
                    _exit(1);
                }

                // Launch session
                if (!source_is_localhost){
                    // do not log early messages for localhost (to avoid tracing in watchdog)
                    LOG(LOG_INFO,
                        "New session on %d (pid=%d) from %s to %s",
                        sck, child_pid, source_ip,
                        real_target_ip.empty() ? target_ip : real_target_ip);
                }

                ini.set_acl<cfg::globals::host>(source_ip);
                ini.set_acl<cfg::globals::target>(target_ip);
                if (ini.get<cfg::globals::enable_transparent_mode>()
                    && !ip_compare(target_ip, real_target_ip))
                {
                    ini.set_acl<cfg::context::real_target_device>(
                                                         real_target_ip);
                }

                switch (socket_type) {
                    case SocketType::Ws:
                        session_start_ws(unique_fd{sck}, start_time, ini, pid_file, font);
                        break;
                    case SocketType::Wss:
                        // disable rdp tls
                        ini.set<cfg::client::tls_support>(false);
                        ini.set<cfg::client::tls_fallback_legacy>(true);
                        session_start_wss(unique_fd{sck}, start_time, ini, pid_file, font);
                        break;
                    case SocketType::Tls:
                        session_start_tls(unique_fd{sck}, start_time, ini, pid_file, font);
                        break;
                }

                if (ini.get<cfg::debug::session>()){
                    LOG(LOG_INFO, "Session::end of Session(%d)", sck);
                }
            }
            else {
                LOG(LOG_ERR, "Failed to set socket TCP_NODELAY option on client socket: %d: %s", errno, strerror(errno));
            }

            _exit(0);
        }
        default: /* father */
            close(sck);
            break;
        case -1:
            // error forking
            LOG(LOG_ERR, "Error creating process for new session : %s", strerror(errno));
            break;
        }
    }

    unique_fd create_ws_server(
        uint32_t s_addr, zstring_view ws_addr,
        EnableTransparentMode enable_transparent_mode,
        bool enable_ipv6)
    {
        // "[:]port"
        {
            const std::ptrdiff_t pos = (ws_addr[0] == ':' ? 1 : 0);
            if (auto port = parse_decimal_chars<int>(ws_addr + pos)) {
                return interface_create_server(enable_ipv6,
                                               s_addr,
                                               port.value,
                                               enable_transparent_mode);
            }
        }

        // "addr:port"
        const char* ws_port = strchr(ws_addr, ':');
        if (ws_port) {
            std::string listen_addr(ws_addr.data(), ws_port);
            uint32_t ws_iaddr = inet_addr(listen_addr.c_str());
            REDEMPTION_DIAGNOSTIC_PUSH()
            REDEMPTION_DIAGNOSTIC_GCC_IGNORE("-Wold-style-cast")
            REDEMPTION_DIAGNOSTIC_GCC_ONLY_IGNORE("-Wuseless-cast")
            if (ws_iaddr == INADDR_NONE) { ws_iaddr = INADDR_ANY; }
            REDEMPTION_DIAGNOSTIC_POP()

            if (auto port = parse_decimal_chars<int>(ws_port + 1)) {
                return interface_create_server(enable_ipv6,
                                               ws_iaddr,
                                               port.value,
                                               enable_transparent_mode);
            }
        }

        // removed previous websocket
        if (struct stat buf; 0 == stat(ws_addr, &buf) && S_ISSOCK(buf.st_mode)) {
            unlink(ws_addr);
        }
        return create_unix_server(ws_addr, enable_transparent_mode);
    }
} // anonymous namespace

void redemption_main_loop(Inifile & ini, unsigned uid, unsigned gid, std::string config_filename, bool forkable)
{
    init_signals();

    uint32_t s_addr = inet_addr(ini.get<cfg::globals::listen_address>().c_str());
    REDEMPTION_DIAGNOSTIC_PUSH()
    REDEMPTION_DIAGNOSTIC_GCC_IGNORE("-Wold-style-cast")
    REDEMPTION_DIAGNOSTIC_GCC_ONLY_IGNORE("-Wuseless-cast")
    if (s_addr == INADDR_NONE) { s_addr = INADDR_ANY; }
    REDEMPTION_DIAGNOSTIC_POP()

    const bool debug_config = (ini.get<cfg::debug::config>() == Inifile::ENABLE_DEBUG_CONFIG);
    const EnableTransparentMode enable_transparent_mode
      = EnableTransparentMode(ini.get<cfg::globals::enable_transparent_mode>());
    bool enable_ipv6 = ini.get<cfg::globals::enable_ipv6>();
    unique_fd sck1 = interface_create_server(enable_ipv6,
                                             s_addr,
                                             ini.get<cfg::globals::port>(),
                                             enable_transparent_mode);
    const Font font(app_path(AppPath::DefaultFontFile));

    if (ini.get<cfg::websocket::enable_websocket>())
    {
        unique_fd sck2 = create_ws_server(
            s_addr,
            ini.get<cfg::websocket::listen_address>(),
            enable_transparent_mode,
            enable_ipv6);
        const auto ws_sck = sck2.fd();
        const bool use_tls = ini.get<cfg::websocket::use_tls>();
        two_server_loop(std::move(sck1), std::move(sck2), [&](int sck)
        {
            auto const socket_type = (ws_sck == sck)
                ? use_tls
                    ? SocketType::Wss
                    : SocketType::Ws
                : SocketType::Tls;
            session_server_start(sck, forkable, uid, gid, config_filename,
                                 debug_config, socket_type, font);
            return true;
        });
    }
    else
    {
        unique_server_loop(std::move(sck1), [&](int sck)
        {
            session_server_start(sck, forkable, uid, gid, config_filename,
                                 debug_config, SocketType::Tls, font);
            return true;
        });
    }
}
