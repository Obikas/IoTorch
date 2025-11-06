#include "MctpBridge.hpp"
#include "MctpSerial.hpp"
#include "NullModem.hpp"
#include "LinuxMctpSerial.hpp"

#include <iostream>         // std::cout, std::cerr
#include <string>           // std::string, std::to_string
#include <cstring>          // strlen, strncpy, memset
#include <cstdlib>          // std::system, atoi, EXIT_FAILURE / EXIT_SUCCESS
#include <atomic>           // std::atomic
#include <optional>         // std::optional, std::nullopt
#include <unordered_map>    // std::unordered_map
#include <vector>           // std::vector
#include <algorithm>        // std::max, std::transform
#include <iomanip>          // std::hex, std::dec, manipulators
#include <cctype>           // std::tolower
#include <chrono>           // std::chrono
#include <sstream>
#include <thread>           // std::this_thread::sleep_for
#include <csignal>          // std::signal
#include <fstream>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/time.h>
// libnl-3 includes for low-level netlink access
#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <netlink/cache.h>
#include <netlink/route/link.h>
#include "MctpNetlink.hpp"

extern "C" {
    #include <getopt.h>
    #include <stdio.h>
    #include <termios.h>
    #include <pty.h>
    #include <fcntl.h>
    #include <linux/mctp.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <sys/un.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <sys/select.h>
    #include <sys/time.h>
    #include <unistd.h>
}

// NOTE: Netlink probe functionality removed. The project previously included
// several helper functions to capture netlink state to /tmp for forensic
// analysis and a `--probe-netlink` CLI mode. Per project request those
// testing/debugging helpers have been removed from this translation unit to
// keep the runtime binary focused on production behavior.

std::atomic<bool> interrupted{false};

/*
 * @brief Handle signals (e.g., SIGINT, SIGTERM) by setting the interrupted flag.
 *
 * @param signum  Signal number received.
 * @return void
 */
void signalHandler(int signum) {
    std::cout << "\nCaught signal " << signum << ", cleaning up...\n";
    interrupted = true;
}

// configure the mctp side of the bridge.  This should be done externally
// when configuring all of the mctp subsystem.  For now, we just make
// system calls to the CodeConstruct mctp utility.
/*
 * @brief Configure the MCTP device using the external `mctp` utility.
 *
 * This runs a small sequence of system commands to add the local address,
 * bring the link up, and add a route for the destination EID.
 *
 * @param devname    Name of the network device to configure (e.g. "mctp0").
 * @param local_eid  Local endpoint ID to add to the device.
 * @param dest_eid   Destination endpoint ID to route to via the device.
 * @return true on success, false on failure (non-zero system command result).
 */
bool configure_mctp(const std::string &devname, std::optional<uint8_t> local_eid, std::optional<uint8_t> dest_eid) {
    // Use the MctpNetlink helpers to program the local EID, bring the
    // interface up, and add a route. Follow the control rules:
    //  - Only set the local address if `local_eid` is provided.
    //  - Only bring the link up if the local address was requested and set.
    //  - Only add a route if the local address was requested/set AND
    //    `dest_eid` is provided.

    bool did_any = false;

    if (!local_eid.has_value()) {
        // Nothing to program.
        return true;
    }

    did_any = true;
    uint8_t local = *local_eid;
    std::cout << "configure_mctp: setting local EID " << int(local) << " on " << devname << "\n";
    if (!mctpnet::setMctpLocalEid(devname, local)) {
        std::cerr << "configure_mctp: failed to set local EID " << int(local) << " on " << devname << "\n";
        return false;
    }

    // Bring the link up now that we added the address.
    if (!mctpnet::setMctpInterfaceStatus(devname, true)) {
        std::cerr << "configure_mctp: failed to bring interface " << devname << " up\n";
        return false;
    }

    // If a destination EID was provided, add a route via this interface.
    if (dest_eid.has_value()) {
        uint8_t dest = *dest_eid;
        std::cout << "configure_mctp: adding route to dest EID " << int(dest) << " via " << devname << "\n";
        if (!mctpnet::addMctpRoute(devname, dest)) {
            std::cerr << "configure_mctp: failed to add route to " << int(dest) << " via " << devname << "\n";
            return false;
        }
    }
    return true;
}

/*
 * @brief Open MCTP listener and sender sockets bound to the local EID.
 *
 * @param if_name      Name of the MCTP interface (used for informational purposes).
 * @param local_eid    Local endpoint ID to bind sockets to.
 * @param dest_eid     Destination endpoint ID (not used for binding here).
 * @param listener_fd  Output parameter: on success set to the listener socket FD.
 * @param sender_fd    Output parameter: on success set to the sender socket FD.
 * @return true on success, false on failure.
 */
bool open_mctp_sockets(std::string if_name, int local_eid, int dest_eid, int &listener_fd, int &sender_fd) {
    // open a listener socket on the linux mctp network
    int sock_rx = socket(AF_MCTP, SOCK_DGRAM, 0);
    if (sock_rx < 0) {
        return false;
    }
    struct sockaddr_mctp local_addr = {};
    local_addr.smctp_family = AF_MCTP;
    local_addr.smctp_network = 0;         // Local network address
    local_addr.smctp_addr.s_addr = local_eid;     // Local endpoint ID
    if (bind(sock_rx, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {  
        close(sock_rx);
        return false;
    }

    // sender socket
    int sock_tx = socket(AF_MCTP, SOCK_DGRAM, 0);
    if (sock_tx < 0) {
        perror("socket");
        close(sock_rx);
        return false;
    }
    struct sockaddr_mctp addr = {};
    addr.smctp_family = AF_MCTP;   
    addr.smctp_network = 0;  // Use 0 for default network
    addr.smctp_addr.s_addr = local_eid;  // Your local endpoint ID
    if (bind(sock_tx, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock_rx);
        close(sock_tx);
        return false;
    }
    listener_fd = sock_rx;
    sender_fd = sock_tx;
    return true;
}

/*
 * @brief Open and connect a Unix domain SOCK_SEQPACKET socket to the bcast messenger.
 *
 * @param if_name  Base name used to construct the socket path under /tmp (e.g. "mctp0").
 * @return socket file descriptor on success, -1 on failure.
 */
int open_bcast_socket(std::string if_name) {
    std::string socketPath = "/tmp/"+if_name;

    // Create a Unix domain SOCK_SEQPACKET socket
    int sockfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    // Set up the server address
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    std::cout << "Connecting to BcastMessenger server.\n";

    // Connect to the server
    if (connect(sockfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cout << socketPath<<std::endl;
        perror("connect");
        close(sockfd);
        return -1;
    }

    std::cout << "Connected to BcastMessenger server.\n";
    return sockfd;
}

/*
 * @brief Run a test bridge loop using a null-modem connection and MCTP sockets.
 *
 * This function opens the serial bridge, configures MCTP, opens sockets and
 * periodically sends a test message while listening for replies and broadcasts.
 *
 * @param dev        Path to serial device (C-string), e.g. "/dev/ttyUSB0".
 * @param ifname     Name to assign to the MCTP interface (e.g. "mctp0") - if empty, use default assigned by system.
 * @param local_eid  Local endpoint ID used for binding and addressing.
 * @param dest_eid   Destination endpoint ID to send test messages to.
 * @return true on successful run, false on error.
 */
bool testBridge(char *dev, std::string ifname, uint8_t local_eid, uint8_t dest_eid, BaudRate baud, bool hw_flow_control) {
    MctpBridge bridge;
    std::string devname = ifname;
    bridge.open(dev, devname, baud, hw_flow_control);

    int sock_rx, sock_tx;
    if (!configure_mctp(devname, std::optional<uint8_t>(local_eid), std::optional<uint8_t>(dest_eid))) return false;
    if (!open_mctp_sockets(devname, local_eid, dest_eid, sock_rx, sock_tx)) return false;
    int bcast = open_bcast_socket("bcast_"+devname);
    if (bcast<0) return false;
    while (!interrupted) {
        struct sockaddr_mctp dest = {};
        dest.smctp_family = AF_MCTP;
        dest.smctp_network = 0;
        dest.smctp_addr.s_addr = dest_eid;  // Destination endpoint ID

        const char* msg = "Hello MCTP";
        ssize_t sent = sendto(sock_tx, msg, strlen(msg), 0,
                            (struct sockaddr*)&dest, sizeof(dest));
        if (sent < 0) {
            close(sock_rx);
            close(sock_tx);
            close(bcast);
            perror("sendto");
            return false;
        }
        std::vector<uint8_t> msg_bcast ={0x01, 0xff, 0x00, 0xC0, 0x00, 't', 'e', 's', 't'};
        if (send(bcast, msg_bcast.data(), msg_bcast.size(), 0) < 0) {
            perror("send bcast");
            close(sock_rx);
            close(sock_tx);
            close(bcast);
            return false;
        }
        
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock_rx, &readfds);
        FD_SET(bcast, &readfds);  // Add bcast to monitored set

        int maxfd = std::max(sock_rx, bcast);  // Update maxfd for select()

        timeval timeout = {};
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;

        int ready = select(maxfd + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            continue;  // Timeout or error
        }

        // Check sock_rx
        if (FD_ISSET(sock_rx, &readfds)) {
            char buffer[256];
            sockaddr_mctp from_addr = {};
            socklen_t from_len = sizeof(from_addr);

            ssize_t received = recvfrom(sock_rx, buffer, sizeof(buffer) - 1, 0,
                                        (sockaddr*)&from_addr, &from_len);
            if (received < 0) {
                std::cerr << std::flush << "Error receiving data.\n";
                close(sock_rx);
                close(sock_tx);
                close(bcast);
                return false;
            }
            buffer[received] = '\0';
            std::cout << std::flush << "Received from EID " << from_addr.smctp_addr.s_addr
                    << ": " << buffer << "\n";
        }

        // Check bcast
        if (FD_ISSET(bcast, &readfds)) {
            uint8_t bcast_buf[256];
            ssize_t len = recv(bcast, bcast_buf, sizeof(bcast_buf), 0);
            if (len > 0) {
                std::cout << "Received broadcast message: ";
                for (ssize_t i = 0; i < len; ++i) {
                    std::cout << std::hex << static_cast<int>(bcast_buf[i]) << " ";
                }
                std::cout << std::dec << "\n";
            } else {
                std::cerr << "Error receiving broadcast message.\n";
                close(sock_rx);
                close(sock_tx);
                close(bcast);
                return false;
            }
        }
    }
    close(sock_rx);
    close(sock_tx);
    close(bcast);
    std::cout<<std::flush<<"   passed."<<std::endl;
    return true;
}

/**
 * @brief Maps a string like "B115200" to a BaudRate enum value.
 * @param str The baud rate string (e.g., "B9600", "B115200").
 * @return An optional BaudRate value if the string is valid; std::nullopt otherwise.
 */
std::optional<BaudRate> baudRateFromString(const std::string& str) {
    static const std::unordered_map<std::string, BaudRate> baudMap = {
        {"B0", BaudRate::BR_0},
        {"B50", BaudRate::BR_50},
        {"B75", BaudRate::BR_75},
        {"B110", BaudRate::BR_110},
        {"B134", BaudRate::BR_134},
        {"B150", BaudRate::BR_150},
        {"B200", BaudRate::BR_200},
        {"B300", BaudRate::BR_300},
        {"B600", BaudRate::BR_600},
        {"B1200", BaudRate::BR_1200},
        {"B1800", BaudRate::BR_1800},
        {"B2400", BaudRate::BR_2400},
        {"B4800", BaudRate::BR_4800},
        {"B9600", BaudRate::BR_9600},
        {"B19200", BaudRate::BR_19200},
        {"B38400", BaudRate::BR_38400},
        {"B57600", BaudRate::BR_57600},
        {"B115200", BaudRate::BR_115200},
        {"B230400", BaudRate::BR_230400}
    };

    auto it = baudMap.find(str);
    if (it != baudMap.end()) {
        return it->second;
    }
    return std::nullopt;
}

/* 
 * @brief Print command-line usage for the program.
 *
 * @param progName  Program name (typically argv[0]) used in usage and examples.
 * @return void     Prints usage to stdout and returns.
 */
void printUsage(const char* progName) {
    std::cout << "Usage: " << progName << " --mode <BRIDGE|TEST> --tty <tty-path> [options]\n\n";

    std::cout << "Required:\n";
    std::cout << "  --mode <BRIDGE|TEST>    Operation mode. BRIDGE: run as bridge; TEST: run in test mode.\n";
    std::cout << "  --tty  <tty-path>       Path to serial device (e.g. /dev/ttyS0, /dev/ttyUSB0).\n\n";

    std::cout << "Optional:\n";
    std::cout << "  --baud <baud-string>    Baud rate string (e.g. B9600, B115200). If omitted, default B115200 is used\n";
    std::cout << "  --hwflow <TRUE|FALSE>   Hardware flow control. TRUE to enable RTS/CTS, FALSE (default) to disable.\n";
    std::cout << "  --ifname <name>         Name of MCTP interface for socket connections (e.g. mctp0). Default: none.\n";
    std::cout << "  --local-eid <n>         Local endpoint ID (1-254). Specify with this option for TEST or BRIDGE modes.\n";
    std::cout << "  --dest-eid  <n>         Destination endpoint ID (1-254). Specify with this option for TEST or BRIDGE modes.\n";
    std::cout << "  --help                  Show this help message and exit.\n\n";

    std::cout << "Examples:\n";
    std::cout << "  " << progName << " --mode BRIDGE --tty /dev/ttyUSB0 --baud B115200 --hwflow TRUE --ifname mctp0\n";
    std::cout << "  " << progName << " --mode TEST --tty /dev/ttyS1\n\n";

    std::cout << "Notes:\n";
    std::cout << "  - Mode and tty are required. Other options may be omitted.\n";
    std::cout << "  - The code is blocking and will run until iterrupted with SIGINT.\n";
    std::cout << std::flush;
}



// Assumes BaudRate and baudRateFromString(const std::string&) -> std::optional<BaudRate> exist.

struct CmdOptions {
    enum class Mode { BRIDGE, TEST } mode;
    std::string tty;
    std::optional<BaudRate> baud;   // std::nullopt if not provided
    bool hwflow = false;            // default false
    std::optional<std::string> ifname;
    std::optional<int> local_eid;
    std::optional<int> dest_eid;
};

/*
 * @brief Convert a string to lowercase.
 *
 * @param s  Input string to convert (modified copy).
 * @return std::string  Lowercased copy of the input.
 */
static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

/*
 * @brief Parse a boolean-like string into a boolean value.
 *
 * Accepts (case-insensitive): true/1/yes and false/0/no.
 *
 * @param s  Input string (e.g. "TRUE", "false", "1", "0").
 * @return std::optional<bool>  true/false on recognized values, std::nullopt otherwise.
 */
static std::optional<bool> parseBool(const std::string& s) {
    auto low = toLower(s);
    if (low == "true" || low == "1" || low == "yes") return true;
    if (low == "false" || low == "0" || low == "no") return false;
    return std::nullopt;
}

/*
 * @brief Parse the mode string into a CmdOptions::Mode.
 *
 * Accepts (case-insensitive) "BRIDGE" or "TEST".
 *
 * @param s  Mode string.
 * @return std::optional<CmdOptions::Mode>  Mode on success, std::nullopt on failure.
 */
static std::optional<CmdOptions::Mode> parseMode(const std::string& s) {
    auto low = toLower(s);
    if (low == "bridge") return CmdOptions::Mode::BRIDGE;
    if (low == "test")   return CmdOptions::Mode::TEST;
    return std::nullopt;
}

/*
 * @brief Parse and validate command-line arguments.
 *
 * Uses getopt_long to accept:
 *   --mode <BRIDGE|TEST>  (required)
 *   --tty  <tty-path>     (required)
 *   --baud <baud-string>  (optional)
 *   --hwflow <TRUE|FALSE> (optional)
 *   --ifname <name>       (optional)
 *   --help                (prints usage and returns std::nullopt)
 *
 * On parse/validation error this function prints usage (via printUsage)
 * and returns std::nullopt.
 *
 * @param argc  Argument count.
 * @param argv  Argument vector.
 * @return std::optional<CmdOptions>  Validated options or std::nullopt on error.
 */
std::optional<CmdOptions> parseArgs(int argc, char** argv) {
    static struct option longOpts[] = {
        {"mode",    required_argument, nullptr, 'm'},
        {"tty",     required_argument, nullptr, 't'},
        {"baud",    required_argument, nullptr, 'b'},
        {"hwflow",  required_argument, nullptr, 'f'},
        {"ifname",  required_argument, nullptr, 'i'},
        {"local-eid", required_argument, nullptr, 'L'},
        {"dest-eid",  required_argument, nullptr, 'D'},
        {"help",    no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    CmdOptions opts;
    bool seenMode = false;
    bool seenTty  = false;

    int opt;
    int longIndex = 0;
    while ((opt = getopt_long(argc, argv, "m:t:b:f:i:L:D:h", longOpts, &longIndex)) != -1) {
        switch (opt) {
        case 'm': {
            auto m = parseMode(optarg);
            if (!m) {
                std::cerr << "Invalid --mode value: " << optarg << "\n";
                printUsage(argv[0]);
                return std::nullopt;
            }
            opts.mode = *m;
            seenMode = true;
            break;
        }
        case 't':
            opts.tty = optarg;
            seenTty = true;
            break;
        case 'b': {
            auto b = baudRateFromString(optarg); // returns std::optional<BaudRate>
            if (!b) {
                std::cerr << "Invalid --baud value: " << optarg << "\n";
                printUsage(argv[0]);
                return std::nullopt;
            }
            opts.baud = *b;
            break;
        }
        case 'f': {
            auto pb = parseBool(optarg);
            if (!pb) {
                std::cerr << "Invalid --hwflow value: " << optarg << " (expect TRUE/FALSE)\n";
                printUsage(argv[0]);
                return std::nullopt;
            }
            opts.hwflow = *pb;
            break;
        }
        case 'i':
            opts.ifname = std::string(optarg);
            break;
        case 'L': {
            int v = atoi(optarg);
            if (v < 1 || v > 254) {
                std::cerr << "Invalid --local-eid value: " << optarg << "\n";
                printUsage(argv[0]);
                return std::nullopt;
            }
            opts.local_eid = v;
            break;
        }
        case 'D': {
            int v = atoi(optarg);
            if (v < 1 || v > 254) {
                std::cerr << "Invalid --dest-eid value: " << optarg << "\n";
                printUsage(argv[0]);
                return std::nullopt;
            }
            opts.dest_eid = v;
            break;
        }
        case 'h':
        default:
            printUsage(argv[0]);
            return std::nullopt;
        }
    }

    // Validate required options

    if (!seenMode) {
        std::cerr << "Missing required option: --mode\n";
        printUsage(argv[0]);
        return std::nullopt;
    }
    if (!seenTty) {
        std::cerr << "Missing required option: --tty\n";
        printUsage(argv[0]);
        return std::nullopt;
    }

    // If dest is supplied, local must also be supplied (per requirements)
    if (opts.dest_eid.has_value() && !opts.local_eid.has_value()) {
        std::cerr << "--dest-eid requires --local-eid to be specified\n";
        printUsage(argv[0]);
        return std::nullopt;
    }

    return opts;
}

// (parseArgs example main removed; using original program main below)
/*
 * @brief Program entry point: parse arguments, validate, and start bridge or test mode.
 *
 * Uses the long-option parser implemented in parseArgs(). In TEST mode this
 * requires both --local-eid and --dest-eid to be provided as named options.
 *
 * @param argc  Argument count.
 * @param argv  Argument vector.
 * @return int EXIT_SUCCESS on success, EXIT_FAILURE on error.
 */
int main(int argc, char *argv[]) {
    auto maybeOpts = parseArgs(argc, argv);
    if (!maybeOpts) return EXIT_FAILURE; // parse error or --help

    CmdOptions opts = *maybeOpts;

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    if (opts.mode == CmdOptions::Mode::TEST) {
        // In TEST mode both local and destination EIDs must be provided as
        // named options (--local-eid and --dest-eid). Positional args are
        // not supported.
        std::optional<int> local_val = opts.local_eid;
        std::optional<int> dest_val = opts.dest_eid;

        if (!local_val || !dest_val) {
            std::cerr << "TEST mode requires both --local-eid and --dest-eid to be specified\n";
            printUsage(argv[0]);
            return EXIT_FAILURE;
        }

        // Validate ranges
        if (*local_val < 1 || *local_val > 254) {
            std::cerr << "Invalid local EID: " << *local_val << "\n";
            return EXIT_FAILURE;
        }
        if (*dest_val < 1 || *dest_val > 254) {
            std::cerr << "Invalid destination EID: " << *dest_val << "\n";
            return EXIT_FAILURE;
        }

        // Convert to uint8_t and call testBridge (pass baud & hwflow from opts)
        uint8_t local_eid_u8 = static_cast<uint8_t>(*local_val);
        uint8_t dest_eid_u8  = static_cast<uint8_t>(*dest_val);
        BaudRate test_baud = BaudRate::BR_115200;
        if (opts.baud) test_baud = *opts.baud;
        std::string ifname;
        if (opts.ifname) ifname = *opts.ifname;
        bool test_hwflow = opts.hwflow;

        if (!testBridge(const_cast<char*>(opts.tty.c_str()), ifname, local_eid_u8, dest_eid_u8, test_baud, test_hwflow)) {
            return EXIT_FAILURE;
        }
    } else {
        // BRIDGE mode: open the serial bridge and run until interrupted.
        MctpBridge mctpbridge;
        std::string ifname;
        if (opts.ifname) ifname = *opts.ifname;
        BaudRate baud = BaudRate::BR_115200;
        if (opts.baud) baud = *opts.baud;

        if (!mctpbridge.open(opts.tty.c_str(), ifname, baud, opts.hwflow)) {
            std::cerr << "Failed to open serial bridge on " << opts.tty << "\n";
            return EXIT_FAILURE;
        }

        // If both local and dest EIDs were provided via options, attempt to configure MCTP
        if (opts.local_eid) {
            uint8_t local_val = static_cast<uint8_t>(*opts.local_eid);
            std::optional<uint8_t> dest_val = std::nullopt;
            if (opts.dest_eid) dest_val = static_cast<uint8_t>(*opts.dest_eid);
            if (!configure_mctp(ifname, std::optional<uint8_t>(local_val), dest_val)) {
                std::cerr << "Warning: configure_mctp failed for " << ifname << "\n";
            }
        }

        // Run until signalled
        while (!interrupted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return 0;
}
