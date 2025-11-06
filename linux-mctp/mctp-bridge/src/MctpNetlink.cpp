/**
 * @file MctpNetlink.cpp
 * @brief Raw NETLINK_ROUTE helper implementations for MCTP administrative tasks.
 *
 * This translation unit implements a small collection of helpers that send
 * explicit RTM_* netlink messages used to manage MCTP test interfaces. The
 * implementations are intentionally minimal and report errors to stderr.
 *
 * These routines are used by the test harness and diagnostic tooling in this
 * repository. For production-grade control, prefer libnl's higher-level APIs.
 *
 * @author Doug Sandy
 * @license MIT No Attribution (MIT-0)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED.
 */

#include "MctpNetlink.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>

/*
 * Avoid pulling in the C library's <net/if_arp.h> which can conflict with
 * the kernel headers (linux/if_arp.h) when both are seen in the same TU.
 * Instead of including <net/if.h>, declare the small symbol we need
 * (if_nametoindex) here. This keeps the translation unit free of the
 * glibc ARP struct definitions while still linking to the libc function.
 */
extern "C" unsigned int if_nametoindex(const char *);

/* Some platforms may not define ARPHRD_NONE; provide a local fallback. */
#ifndef ARPHRD_NONE
#define ARPHRD_NONE 0
#endif

/* Minimal fallback for interface flags we use; real values come from <net/if.h>. */
#ifndef IFF_UP
#define IFF_UP 0x1
#endif

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <errno.h>
#include <cstdint>

// Prefer libnl for link manipulations when available; it produces the same
// netlink messages as iproute2 and avoids subtle attribute/flag mismatches.
#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <netlink/route/link.h>

namespace mctpnet {

/**
 * @brief Open a NETLINK_ROUTE socket and bind to the local address.
 *
 * Returns a socket file descriptor on success or -1 on failure. Errors are
 * reported to stderr.
 */
static int openNetlink()
{
    int sock = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (sock < 0) {
        std::cerr << "netlink socket open failed: " << strerror(errno) << "\n";
        return -1;
    }
    sockaddr_nl local;
    memset(&local, 0, sizeof(local));
    local.nl_family = AF_NETLINK;
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        std::cerr << "netlink bind failed: " << strerror(errno) << "\n";
        close(sock);
        return -1;
    }
    return sock;
}

/**
 * @brief Send a single netlink message and wait for an ACK/error reply.
 *
 * This helper sends the provided nlmsghdr (which must have nlmsg_len set)
 * and waits for one reply. It returns true on success and false on error.
 */
static bool sendNetlinkMessage(int sock, struct nlmsghdr *nlh)
{
    sockaddr_nl nladdr;
    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;

    iovec iov = { nlh, nlh->nlmsg_len };
    msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &nladdr;
    msg.msg_namelen = sizeof(nladdr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    ssize_t ret = sendmsg(sock, &msg, 0);
    if (ret < 0) {
        std::cerr << "sendmsg failed: " << strerror(errno) << "\n";
        return false;
    }

    // Wait for ACK (simple)
    std::vector<char> buf(8192);
    ssize_t len = recv(sock, buf.data(), buf.size(), 0);
    if (len < 0) {
        std::cerr << "recv ack failed: " << strerror(errno) << "\n";
        return false;
    }
    struct nlmsghdr *hdr = (struct nlmsghdr *)buf.data();
    if (hdr->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(hdr);
        if (err->error != 0) {
            std::cerr << "netlink error: " << strerror(-err->error) << "\n";
            return false;
        }
    }
    return true;
}

/**
 * @brief Append an RT attribute to the provided nlmsghdr buffer.
 *
 * Caller is responsible for ensuring the buffer has sufficient space.
 */
static void addAttribute(struct nlmsghdr *nlh, int attrtype, const void *data, int datalen)
{
    int cur_len = nlh->nlmsg_len;
    struct rtattr *rta = (struct rtattr *)(((char *)nlh) + NLMSG_ALIGN(cur_len));
    rta->rta_type = attrtype;
    rta->rta_len = RTA_LENGTH(datalen);
    memcpy(RTA_DATA(rta), data, datalen);
    nlh->nlmsg_len = NLMSG_ALIGN(cur_len) + RTA_LENGTH(datalen);
}

/**
 * @brief Helper: resolve interface name to index. Returns 0 if not found.
 */
static uint32_t ifindexByName(const std::string &ifname)
{
    return if_nametoindex(ifname.c_str());
}

/**
 * @brief See `MctpNetlink.hpp` for documentation.
 */
bool setMctpInterfaceName(const std::string &oldname, const std::string &newname)
{
    // Rename an existing interface from `oldname` to `newname` by sending
    // RTM_NEWLINK with the target ifindex and an IFLA_IFNAME attribute.
    uint32_t ifindex = ifindexByName(oldname);
    if (ifindex == 0) {
        std::cerr << "interface not found: " << oldname << "\n";
        return false;
    }

    // Perform a raw netlink RTM_NEWLINK update that mirrors the fields
    // iproute2 sends when renaming an interface. We avoid libnl here to
    // reduce runtime dependencies and prevent libnl usage issues.
    (void)0;

    // Fallback: attempt a raw netlink RTM_NEWLINK update (best-effort).
    int sock = openNetlink();
    if (sock < 0) return false;

    std::vector<char> buffer(512);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    // Send an RTM_NEWLINK request with ACK; avoid NLM_F_REPLACE to mimic
    // the sequence iproute2 uses (which omits REPLACE in practice here).
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_type = RTM_NEWLINK;

    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;
    // do not set a wide change mask here; let the kernel interpret the
    // IFLA_IFNAME attribute as a rename request.
    ifi->ifi_change = 0;

    // IFLA_IFNAME -> new name
    addAttribute(nlh, IFLA_IFNAME, newname.c_str(), (int)newname.size() + 1);

    bool ok = sendNetlinkMessage(sock, nlh);
    close(sock);
    return ok;
}

/**
 * @brief See `MctpNetlink.hpp` for documentation.
 */
bool setMctpInterfaceStatus(const std::string &ifname, bool up)
{
    uint32_t ifindex = ifindexByName(ifname);
    if (ifindex == 0) {
        std::cerr << "interface not found: " << ifname << "\n";
        return false;
    }

    int sock = openNetlink();
    if (sock < 0) return false;

    std::vector<char> buffer(256);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    nlh->nlmsg_type = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;
    ifi->ifi_flags = up ? IFF_UP : 0;
    ifi->ifi_change = IFF_UP; // indicate which flags change

    bool ok = sendNetlinkMessage(sock, nlh);
    close(sock);
    return ok;
}

/**
 * @brief See `MctpNetlink.hpp` for documentation.
 */
bool setMctpLocalEid(const std::string &ifname, uint8_t eid)
{
    uint32_t ifindex = ifindexByName(ifname);
    if (ifindex == 0) {
        std::cerr << "interface not found: " << ifname << "\n";
        return false;
    }

#ifndef AF_MCTP
#define AF_MCTP 40
#endif

    int sock = openNetlink();
    if (sock < 0) return false;

    std::vector<char> buffer(256);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_type = RTM_NEWADDR;

    struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
    ifa->ifa_family = AF_MCTP;
    ifa->ifa_prefixlen = 0;
    ifa->ifa_index = ifindex;
    ifa->ifa_scope = 0;

    // IFA_LOCAL -> single byte EID
    addAttribute(nlh, IFA_LOCAL, &eid, 1);
    // Also add IFA_ADDRESS for compatibility
    addAttribute(nlh, IFA_ADDRESS, &eid, 1);

    bool ok = sendNetlinkMessage(sock, nlh);
    close(sock);
    return ok;
}

/**
 * @brief See `MctpNetlink.hpp` for documentation.
 */
bool addMctpRoute(const std::string &ifname, uint8_t dest_eid)
{
    uint32_t ifindex = ifindexByName(ifname);
    if (ifindex == 0) {
        std::cerr << "interface not found: " << ifname << "\n";
        return false;
    }

#ifndef AF_MCTP
#define AF_MCTP 40
#endif

    int sock = openNetlink();
    if (sock < 0) return false;

    std::vector<char> buffer(512);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    nlh->nlmsg_type = RTM_NEWROUTE;

    struct rtmsg *rt = (struct rtmsg *)NLMSG_DATA(nlh);
    rt->rtm_family = AF_MCTP;
    rt->rtm_table = RT_TABLE_MAIN;
    rt->rtm_protocol = RTPROT_BOOT;
    rt->rtm_scope = RT_SCOPE_UNIVERSE;
    rt->rtm_type = RTN_UNICAST;
    rt->rtm_dst_len = 0; // module treats this as an extent; adjust if needed

    // RTA_DST (single byte)
    addAttribute(nlh, RTA_DST, &dest_eid, 1);
    // RTA_OIF (u32)
    uint32_t oif = ifindex;
    addAttribute(nlh, RTA_OIF, &oif, sizeof(oif));

    bool ok = sendNetlinkMessage(sock, nlh);
    close(sock);
    return ok;
}

/**
 * @brief See `MctpNetlink.hpp` for documentation.
 */
bool removeMctpRoute(const std::string &ifname, uint8_t dest_eid)
{
    uint32_t ifindex = ifindexByName(ifname);
    if (ifindex == 0) {
        std::cerr << "interface not found: " << ifname << "\n";
        return false;
    }

#ifndef AF_MCTP
#define AF_MCTP 40
#endif

    int sock = openNetlink();
    if (sock < 0) return false;

    std::vector<char> buffer(512);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_type = RTM_DELROUTE;

    struct rtmsg *rt = (struct rtmsg *)NLMSG_DATA(nlh);
    rt->rtm_family = AF_MCTP;
    rt->rtm_table = RT_TABLE_MAIN;
    rt->rtm_protocol = RTPROT_BOOT;
    rt->rtm_scope = RT_SCOPE_UNIVERSE;
    rt->rtm_type = RTN_UNICAST;
    rt->rtm_dst_len = 0;

    addAttribute(nlh, RTA_DST, &dest_eid, 1);
    uint32_t oif = ifindex;
    addAttribute(nlh, RTA_OIF, &oif, sizeof(oif));

    bool ok = sendNetlinkMessage(sock, nlh);
    close(sock);
    return ok;
}

} // namespace mctpnet
