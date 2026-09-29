// remote-worker: the endpoint agent and the key tool, in one executable.
//
// One file to copy to a machine, so the thing that runs on an endpoint is a single binary, and the
// agent and the tools that provision it cannot drift apart in version.
//
//   remote-worker agent    run the endpoint agent (this is what goes on the remote machine)
//   remote-worker keygen   mint a key for a machine and print it, along with its machine id
//
// The agent is the only thing that runs on a remote system. A controller holds a list of endpoints
// and connects to each agent directly, so there is no separate program in the middle.
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "rw/agent.hpp"
#include "rw/credential.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) {
    g_stop = 1;
}

void usage() {
    std::cerr << "usage: remote-worker <command> [options]\n"
              << "  agent  [--bind <addr>] [--port <n>] [--key-file <path>]\n"
              << "         Serve one controller, on the machine being watched.\n"
              << "  keygen [--key-file <path>] [--rotate]\n"
              << "         Mint a key for this machine and print it once, with its machine id.\n";
}

// Matches "--flag value" and "--flag=value".
//
// `consume_next` comes back true when the value is the following argument, so the caller knows to
// read argv[i + 1]. That distinction is the whole point: reading a value only for the "=" form
// silently ignored the space-separated form and fell back to the default, which for --key-file
// would have written the machine's real key somewhere nobody asked for.
bool take(const std::string& arg, const std::string& name, std::string* value,
          bool* consume_next) {
    *consume_next = false;
    if (arg == name) {
        *consume_next = true;
        return true;
    }
    if (arg.rfind(name + "=", 0) == 0) {
        *value = arg.substr(name.size() + 1);
        return true;
    }
    return false;
}

// The value for a flag already known to be present: from argv[i + 1] for "--flag value", or the
// `parsed` value for "--flag=value".
std::string require_value(const std::string& arg, const std::string& name, const std::string& parsed,
                          int* i, int argc, char** argv) {
    if (arg == name) {
        if (*i + 1 >= argc) {
            throw std::runtime_error(std::string(name) + " needs a value");
        }
        return argv[++(*i)];
    }
    return parsed;
}

int command_keygen(int argc, char** argv) {
    std::string keyFile = rw::KeyStore::default_path();
    bool rotate = false;
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        std::string value;
        bool consume_next = false;
        if (take(arg, "--key-file", &value, &consume_next)) {
            keyFile = consume_next ? require_value(arg, "--key-file", value, &i, argc, argv) : value;
        } else if (arg == "--rotate") {
            rotate = true;
        } else {
            std::cerr << "error: keygen does not take " << arg << "\n";
            return 2;
        }
    }
    rw::KeyStore store(keyFile);
    std::string existing;
    std::string error;
    if (store.load(&existing, &error)) {
        if (!rotate) {
            // Printing the key again would undo the point of showing it once.
            std::cout << "This machine already has a key. It is not shown again.\n"
                      << "  machine id: " << rw::machine_id(existing) << "\n"
                      << "  key file:   " << keyFile << "\n"
                      << "Regenerate with: remote-worker keygen --rotate\n"
                      << "Regenerating changes the machine id, so every controller must be paired\n"
                      << "again -- which is the point of rotating a key.\n";
            return 0;
        }
        std::cout << "Replacing the existing key. The machine id will change.\n";
    } else if (!error.empty()) {
        // Never overwrite a key we could not read: that would unpair the machine for no reason.
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    std::string key = rw::generate_token();
    if (!store.store(key, &error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    std::cout << "Key for this machine. Shown once -- put it in the mod.\n\n"
              << "  machine id:  " << rw::machine_id(key) << "\n"
              << "  key:         " << key << "\n"
              << "  key file:    " << keyFile << "\n\n"
              << "The machine id is derived from the key, so this machine is known by that id until\n"
              << "the key is regenerated. Anyone who can see the id on the network learns nothing:\n"
              << "it is not the key and cannot be replayed as one.\n";
    return 0;
}

int command_agent(int argc, char** argv) {
    rw::AgentOptions options;
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        std::string value;
        bool consume_next = false;
        if (take(arg, "--bind", &value, &consume_next) && consume_next) {
            options.bindAddress = require_value(arg, "--bind", value, &i, argc, argv);
        } else if (take(arg, "--port", &value, &consume_next) && consume_next) {
            options.port = static_cast<uint16_t>(std::stoi(
                require_value(arg, "--port", value, &i, argc, argv)));
        } else if (take(arg, "--key-file", &value, &consume_next) && consume_next) {
            options.keyFile = require_value(arg, "--key-file", value, &i, argc, argv);
        } else {
            std::cerr << "error: agent does not take " << arg << "\n";
            return 2;
        }
    }
    rw::AgentServer server(options);
    std::string error;
    if (!server.start(&error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (g_stop == 0) {
        server.run_once(50);
    }
    std::cout << "\nstopping\n";
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string command = argv[1];
    try {
        if (command == "keygen") return command_keygen(argc, argv);
        if (command == "agent") return command_agent(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    if (command == "-h" || command == "--help" || command == "help") {
        usage();
        return 0;
    }
    std::cerr << "error: unknown command '" << command << "'\n";
    usage();
    return 2;
}
