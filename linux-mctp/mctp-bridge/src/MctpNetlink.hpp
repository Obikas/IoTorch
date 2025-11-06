/**
 * @file MctpNetlink.hpp
 * @brief Minimal NETLINK helper API to manage MCTP link/address/route operations.
 *
 * This header declares a small, explicit set of helpers that construct and send
 * NETLINK_ROUTE messages for common MCTP administrative tasks used by the
 * test tooling in this repository. The helpers are intentionally low-level and
 * send raw RTM_* messages suitable for programmatic control or tests.
 *
 * Exposed helpers:
 *  - setMctpInterfaceName(oldname, newname)  : rename an existing interface
 *    from `oldname` to `newname`. The kernel typically creates `mctpserial`
 *    devices when the MCTP line discipline is attached to a TTY; this
 *    helper only renames an interface that already exists.
 *  - setMctpLocalEid(ifname, eid)  : add a local MCTP EID (RTM_NEWADDR -> IFA_LOCAL)
 *  - addMctpRoute(ifname, dest_eid) : add an MCTP route (RTM_NEWROUTE RTA_DST / RTA_OIF)
 *  - removeMctpRoute(ifname, dest_eid)
 *  - setMctpInterfaceStatus(ifname, up) : bring interface up/down (RTM_NEWLINK)
 *
 * These helpers return true on success and false on failure and are intended
 * for diagnostic / control use in this userland toolset. They do not attempt
 * to implement a full rtnetlink client — use libnl for production code.
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

#pragma once

#include <string>
#include <cstdint>

namespace mctpnet {

/**
 * @brief Ensure/create a link of kind "mctpserial" with the given name.
 *
 * This attempts to send an RTM_NEWLINK/IFLA_LINKINFO message with
 * IFLA_INFO_KIND="mctpserial". On many systems the kernel will create
 * mctpserial devices automatically when a line discipline opens; this call
 * is a best-effort request to create the link from userspace.
 */
bool setMctpInterfaceName(const std::string &oldname, const std::string &newname);

/**
 * @brief Add a local MCTP EID to the specified interface.
 *
 * Sends RTM_NEWADDR with family AF_MCTP and IFA_LOCAL (u8) containing the
 * endpoint ID. Returns true on success.
 */
bool setMctpLocalEid(const std::string &ifname, uint8_t eid);

/**
 * @brief Add an MCTP route mapping dest_eid -> output interface.
 *
 * Sends RTM_NEWROUTE with rtm_family=AF_MCTP, RTA_DST (u8) and RTA_OIF
 * (u32) specifying the destination endpoint and output interface.
 */
bool addMctpRoute(const std::string &ifname, uint8_t dest_eid);

/**
 * @brief Remove an MCTP route previously added with addMctpRoute().
 */
bool removeMctpRoute(const std::string &ifname, uint8_t dest_eid);

/**
 * @brief Set interface administrative status up (true) or down (false).
 *
 * Uses RTM_NEWLINK/ifinfomsg to set IFF_UP.
 */
bool setMctpInterfaceStatus(const std::string &ifname, bool up);

} // namespace mctpnet
