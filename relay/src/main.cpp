// The relay daemon.
//
// Prints the pairing fingerprint on startup, because that is what a controller and an agent are
// paired against. Refuses a non-loopback bind unless explicitly told otherwise -- see
// Relay::start, which is where that decision is enforced and why.
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "rw/relay.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) {
    g_stop = 1;
}

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [options]\n"
              << "  --bind <addr>              interface to listen on (default 127.0.0.1)\n"
              << "  --port <n>                 port, 0 to let the OS choose (default 0)\n"
              << "  --allow-unauthenticated-lan permit a non-loopback bind before TLS is wired in\n"
              << "  --quiet                    suppress per-event logging\n"
              << "  --once                     serve a single pass and exit (used by tests)\n";
}

}  // namespace

int main(int argc, char** argv) {
    rw::RelayOptions options;
    bool once = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "error: " << what << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--bind") {
            options.bindAddress = next("--bind");
        } else if (arg == "--port") {
            options.port = static_cast<uint16_t>(std::stoi(next("--port")));
        } else if (arg == "--allow-unauthenticated-lan") {
            options.allowUnauthenticatedLan = true;
        } else if (arg == "--quiet") {
            options.verbose = false;
        } else if (arg == "--once") {
            once = true;
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "error: unknown option " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    rw::Relay relay(options);
    std::string error;
    if (!relay.start(&error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    std::cout << "remote-worker relay listening on " << options.bindAddress << ":" << relay.port()
              << "\n";
    if (options.allowUnauthenticatedLan && options.bindAddress != "127.0.0.1") {
        std::cerr << "WARNING: serving " << options.bindAddress
                  << " with no authentication and no transport encryption.\n"
                  << "         Any host on this network may pair as a controller and be routed to\n"
                  << "         any endpoint it can name, and anything watching the wire can read\n"
                  << "         the frames. Acceptable only on a network you control.\n";
    }

    if (once) {
        relay.run_once(0);
        relay.stop();
        return 0;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (g_stop == 0) {
        relay.run_once(50);
    }
    std::cout << "\nshutting down\n";
    relay.stop();
    return 0;
}
