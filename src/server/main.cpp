// freepend: the freePen sync server and its administration commands.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include <httplib.h>  // before anything that may include <windows.h>

#ifdef _WIN32
#include <io.h>
#include <shellapi.h>
#else
#include <grp.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "core/config.h"
#include "freepen/version.h"
#include "sync/server_api.h"
#include "sync/store.h"

using namespace fp;
using namespace fp::sync;

namespace {

class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// --- argument parsing ----------------------------------------------------------------

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::vector<std::string>> options;

    bool has(const std::string& name) const { return options.count(name) > 0; }
    std::string get(const std::string& name, const std::string& def = {}) const
    {
        auto it = options.find(name);
        return it == options.end() ? def : it->second.back();
    }
    std::vector<std::string> all(const std::string& name) const
    {
        auto it = options.find(name);
        return it == options.end() ? std::vector<std::string>() : it->second;
    }
};

struct Command {
    const char* name;
    const char* usage;
    const char* help;
    std::set<std::string> valued;  // options taking a value
    std::set<std::string> flags;   // options without a value
    size_t minArgs;
    size_t maxArgs;
    int (*run)(const Args&);
};

Args parseArgs(const std::vector<std::string>& argv, const Command& cmd)
{
    Args a;
    for (size_t i = 0; i < argv.size(); ++i) {
        const auto& arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            a.options["help"].push_back("");
        } else if (arg == "--") {
            a.positional.insert(a.positional.end(), argv.begin() + (long)i + 1, argv.end());
            break;
        } else if (startsWith(arg, "--")) {
            auto name = arg.substr(2);
            std::optional<std::string> value;
            if (auto eq = name.find('='); eq != std::string::npos) {
                value = name.substr(eq + 1);
                name.resize(eq);
            }
            if (cmd.valued.count(name) || name == "data") {
                if (!value) {
                    if (i + 1 >= argv.size())
                        throw UsageError("option --" + name + " needs a value");
                    value = argv[++i];
                }
                a.options[name].push_back(*value);
            } else if (cmd.flags.count(name) && !value) {
                a.options[name].push_back("");
            } else {
                throw UsageError("unknown option " + arg);
            }
        } else if (arg.size() > 1 && arg[0] == '-') {
            throw UsageError("unknown option " + arg);
        } else {
            a.positional.push_back(arg);
        }
    }
    if (!a.has("help") && (a.positional.size() < cmd.minArgs || a.positional.size() > cmd.maxArgs))
        throw UsageError(a.positional.size() < cmd.minArgs ? "missing argument" : "too many arguments");
    return a;
}

int64_t parseInt(const std::string& s, const char* what, int64_t min, int64_t max)
{
    try {
        size_t used = 0;
        auto v = std::stoll(s, &used);
        if (used == s.size() && v >= min && v <= max)
            return v;
    } catch (const std::exception&) {
    }
    throw UsageError(std::string("invalid ") + what + " '" + s + "'");
}

// --- environment ------------------------------------------------------------------------

fs::path defaultServerDataDir()
{
#ifdef _WIN32
    if (const wchar_t* v = _wgetenv(L"FREEPEND_DATA"); v && *v)
        return fs::path(v);
    if (const wchar_t* v = _wgetenv(L"LOCALAPPDATA"); v && *v)
        return fs::path(v) / "freepend";
    return homeDir() / "AppData" / "Local" / "freepend";
#else
    if (const char* v = std::getenv("FREEPEND_DATA"); v && *v)
        return u8path(v);
    if (geteuid() == 0)
        return "/var/lib/freepend";
#ifdef __APPLE__
    return homeDir() / "Library" / "Application Support" / "freepend";
#else
    if (const char* v = std::getenv("XDG_DATA_HOME"); v && *v)
        return u8path(v) / "freepend";
    return homeDir() / ".local" / "share" / "freepend";
#endif
#endif
}

// The data directory. Run as root on a directory owned by a service account
// (/var/lib/freepend owned by "freepend"), freepend first switches to that
// account, so files it creates stay accessible to the daemon.
fs::path dataDir(const Args& a)
{
    auto dir = a.has("data") ? u8path(a.get("data")) : defaultServerDataDir();
#ifndef _WIN32
    struct stat st {};
    if (geteuid() == 0 && ::stat(pathString(dir).c_str(), &st) == 0 && st.st_uid != 0) {
        if (setgroups(0, nullptr) != 0 || setgid(st.st_gid) != 0 || setuid(st.st_uid) != 0)
            throw std::runtime_error("cannot switch to the owner of " + pathString(dir));
    }
#endif
    return dir;
}

bool stdinIsTerminal()
{
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

// Reads one line from stdin without echoing it when stdin is a terminal.
std::string readSecretLine()
{
    std::string line;
    bool ok = false;
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode & ~(DWORD)ENABLE_ECHO_INPUT);
        std::wstring wline;
        wchar_t buf[256];
        DWORD n = 0;
        while (ReadConsoleW(h, buf, 256, &n, nullptr) && n > 0) {
            wline.append(buf, n);
            if (wline.back() == L'\n')
                break;
        }
        SetConsoleMode(h, mode);
        while (!wline.empty() && (wline.back() == L'\n' || wline.back() == L'\r'))
            wline.pop_back();
        ok = true;
        int len = WideCharToMultiByte(CP_UTF8, 0, wline.data(), (int)wline.size(), nullptr, 0, nullptr, nullptr);
        line.resize((size_t)len);
        WideCharToMultiByte(CP_UTF8, 0, wline.data(), (int)wline.size(), line.data(), len, nullptr, nullptr);
        std::cerr << "\n";
    } else {
        ok = (bool)std::getline(std::cin, line);
    }
#else
    termios old{};
    bool tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        termios quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    ok = (bool)std::getline(std::cin, line);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        std::cerr << "\n";
    }
#endif
    if (!ok)
        throw UsageError("no password given");
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    return line;
}

std::string passwordFrom(const Args& a)
{
    if (a.has("password"))
        return a.get("password");
    if (!stdinIsTerminal())
        return readSecretLine();  // piped in: a single line
    std::cerr << "Password: " << std::flush;
    auto pw = readSecretLine();
    std::cerr << "Repeat password: " << std::flush;
    if (readSecretLine() != pw)
        throw UsageError("passwords do not match");
    return pw;
}

// --- serve ----------------------------------------------------------------------------------

std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free, "needed in signal handlers");

extern "C" void onSignal(int) { g_stop = true; }

#ifdef _WIN32
BOOL WINAPI onConsoleEvent(DWORD)
{
    g_stop = true;
    return TRUE;
}
#endif

void installSignalHandlers()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifdef _WIN32
    SetConsoleCtrlHandler(onConsoleEvent, TRUE);
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

std::string printable(std::string s)
{
    for (auto& c : s)
        if ((unsigned char)c < 0x20 || c == 0x7f)
            c = '?';
    return s;
}

void logRequest(const httplib::Request& req, const httplib::Response& res)
{
    static std::mutex mutex;
    // start_time_ is set by httplib when it starts reading the request.
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - req.start_time_).count();
    auto user = ApiServer::requestUser(res);
    std::lock_guard lock(mutex);
    std::printf("%s %s %s %s %d %s %.1fms\n", nowIso().c_str(), req.remote_addr.c_str(), req.method.c_str(),
                printable(req.path).c_str(), res.status, user.empty() ? "-" : user.c_str(), ms);
    std::fflush(stdout);
}

int cmdServe(const Args& a)
{
    auto dir = dataDir(a);
    auto host = a.get("host", "0.0.0.0");
    int port = (int)parseInt(a.get("port", "8642"), "port", 1, 65535);
    auto cert = a.get("cert");
    auto key = a.get("key");
    if (cert.empty() != key.empty())
        throw UsageError("--cert and --key must be given together");
    ServerOptions opts;
    if (a.has("max-upload-mb"))
        opts.maxUploadBytes = parseInt(a.get("max-upload-mb"), "upload limit", 1, 1 << 24) << 20;

    Store store(dir);
    std::unique_ptr<httplib::Server> svr;
    if (!cert.empty()) {
#ifdef CPPHTTPLIB_SSL_ENABLED
        svr = std::make_unique<httplib::SSLServer>(cert.c_str(), key.c_str());
        if (!svr->is_valid()) {
            std::cerr << "freepend: cannot load TLS certificate " << cert << " / key " << key << "\n";
            return 1;
        }
#else
        std::cerr << "freepend: built without TLS support; run it behind a TLS reverse proxy instead\n";
        return 1;
#endif
    } else {
        svr = std::make_unique<httplib::Server>();
    }

    ApiServer api(store, opts);
    api.mount(*svr);
    svr->set_read_timeout(60);
    svr->set_write_timeout(60);
    if (a.has("trusted-proxy"))
        svr->set_trusted_proxies(a.all("trusted-proxy"));
    svr->set_logger(logRequest);

    if (!svr->bind_to_port(host, port)) {
        std::cerr << "freepend: cannot listen on " << host << ":" << port << "\n";
        return 1;
    }
    installSignalHandlers();
    std::atomic<bool> finished{false};
    std::thread watcher([&] {
        while (!g_stop)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        while (!finished) {  // stop() is a no-op until listen_after_bind() is running
            svr->stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    std::printf("freepend %s listening on %s://%s:%d (data: %s)\n", FREEPEN_VERSION, cert.empty() ? "http" : "https",
                host.c_str(), port, pathString(dir).c_str());
    if (store.users().empty())
        std::printf("no users yet: create one with 'freepend useradd NAME --data %s'\n", pathString(dir).c_str());
    std::fflush(stdout);

    bool ok = svr->listen_after_bind();
    finished = true;
    g_stop = true;
    watcher.join();
    std::printf("freepend stopped\n");
    return ok ? 0 : 1;
}

// --- administration -------------------------------------------------------------------------

int cmdUseradd(const Args& a)
{
    Store store(dataDir(a));
    auto& name = a.positional[0];
    if (!isValidUsername(name))
        throw UsageError("invalid user name '" + name + "' (use 1-64 of A-Z a-z 0-9 _ . -)");
    if (store.userExists(name))
        throw SyncError("user '" + name + "' already exists", 409);
    store.createUser(name, passwordFrom(a), a.has("admin"));
    std::printf("created user %s%s\n", name.c_str(), a.has("admin") ? " (admin)" : "");
    return 0;
}

int cmdPasswd(const Args& a)
{
    Store store(dataDir(a));
    auto& name = a.positional[0];
    if (!store.userExists(name))
        throw SyncError("no such user '" + name + "'", 404);
    store.setPassword(name, passwordFrom(a));
    std::printf("password changed for %s; existing tokens were revoked\n", name.c_str());
    return 0;
}

int cmdUserdel(const Args& a)
{
    Store store(dataDir(a));
    store.deleteUser(a.positional[0], a.has("delete-libraries"));
    std::printf("deleted user %s\n", a.positional[0].c_str());
    return 0;
}

int cmdUsers(const Args& a)
{
    Store store(dataDir(a));
    auto users = store.users();
    if (users.empty()) {
        std::printf("no users\n");
        return 0;
    }
    std::printf("%-24s %-6s %-7s %s\n", "USER", "ADMIN", "TOKENS", "CREATED");
    for (auto& u : users)
        std::printf("%-24s %-6s %-7lld %s\n", jstr(u, "username").c_str(), jbool(u, "admin") ? "yes" : "no",
                    jint(u, "tokens"), jstr(u, "created").c_str());
    return 0;
}

int cmdToken(const Args& a)
{
    Store store(dataDir(a));
    std::printf("%s\n", store.createToken(a.positional[0], a.get("device", "freepend token")).c_str());
    return 0;
}

int cmdLibraries(const Args& a)
{
    Store store(dataDir(a));
    bool perUser = a.has("user");
    auto libs = perUser ? store.libraries(a.get("user")) : store.allLibraries();
    if (libs.empty()) {
        std::printf("no libraries\n");
        return 0;
    }
    std::printf("%-20s %-24s %-6s %-16s %-8s %s\n", "UID", "NAME", "TYPE", "OWNER", "VERSION",
                perUser ? "ROLE" : "MEMBERS");
    for (auto& l : libs) {
        std::string extra = l.role;
        if (!perUser) {
            std::vector<std::string> members;
            for (auto& m : store.members(l.uid))
                members.push_back(jstr(m, "username") + ":" + jstr(m, "role"));
            extra = members.empty() ? "-" : join(members, ", ");
        }
        std::printf("%-20s %-24s %-6s %-16s %-8lld %s\n", l.uid.c_str(), l.name.c_str(), l.type.c_str(),
                    l.owner.c_str(), (long long)l.version, extra.c_str());
    }
    return 0;
}

int cmdShare(const Args& a)
{
    Store store(dataDir(a));
    auto role = a.get("role", "write");
    store.share(a.positional[0], a.positional[1], role);
    std::printf("shared %s with %s (%s)\n", a.positional[0].c_str(), a.positional[1].c_str(), role.c_str());
    return 0;
}

int cmdUnshare(const Args& a)
{
    Store store(dataDir(a));
    bool removed = store.unshare(a.positional[0], a.positional[1]);
    std::printf(removed ? "%s is no longer shared with %s\n" : "%s was not shared with %s\n", a.positional[0].c_str(),
                a.positional[1].c_str());
    return 0;
}

int cmdVersion(const Args&)
{
#ifdef CPPHTTPLIB_SSL_ENABLED
    const char* tls = "yes";
#else
    const char* tls = "no";
#endif
    std::printf("freepend %s (sync API %d, TLS support: %s)\n", FREEPEN_VERSION, kApiVersion, tls);
    return 0;
}

const std::vector<Command>& commands()
{
    static const std::vector<Command> cmds = {
        {"serve",
         "serve [--data DIR] [--host ADDR] [--port N] [--cert FILE --key FILE] [--max-upload-mb N]\n"
         "                [--trusted-proxy ADDR]...",
         "Run the sync server (default 0.0.0.0:8642). --cert/--key enable HTTPS. --trusted-proxy\n"
         "names a reverse proxy whose X-Forwarded-For header is believed (repeatable).",
         {"host", "port", "cert", "key", "max-upload-mb", "trusted-proxy"}, {}, 0, 0, cmdServe},
        {"useradd", "useradd NAME [--admin] [--password PW]",
         "Create a user. Prompts for the password unless --password is given\n"
         "(or reads one line from stdin when it is not a terminal).",
         {"password"}, {"admin"}, 1, 1, cmdUseradd},
        {"passwd", "passwd NAME [--password PW]", "Change a user's password and revoke the user's tokens.",
         {"password"}, {}, 1, 1, cmdPasswd},
        {"userdel", "userdel NAME [--delete-libraries]",
         "Delete a user with their tokens and memberships. Libraries the user owns\n"
         "must be deleted explicitly with --delete-libraries.",
         {}, {"delete-libraries"}, 1, 1, cmdUserdel},
        {"users", "users", "List users.", {}, {}, 0, 0, cmdUsers},
        {"token", "token NAME [--device D]", "Create an access token for a user and print it.", {"device"}, {}, 1, 1,
         cmdToken},
        {"libraries", "libraries [--user NAME]", "List all libraries, or those a user can access.", {"user"}, {}, 0, 0,
         cmdLibraries},
        {"share", "share UID USER [--role read|write]", "Give a user access to a library (default role: write).",
         {"role"}, {}, 2, 2, cmdShare},
        {"unshare", "unshare UID USER", "Remove a user's access to a library.", {}, {}, 2, 2, cmdUnshare},
        {"version", "version", "Print the version.", {}, {}, 0, 0, cmdVersion},
    };
    return cmds;
}

void printUsage(std::FILE* out)
{
    std::fprintf(out, "freepend %s - freePen sync server\n\nUsage: freepend COMMAND [OPTIONS]\n\nCommands:\n",
                 FREEPEN_VERSION);
    for (auto& c : commands())
        std::fprintf(out, "  %s\n", c.usage);
    std::fprintf(out,
                 "\nEvery command accepts --data DIR (default: $FREEPEND_DATA, /var/lib/freepend\n"
                 "as root, else a per-user data directory: %s).\n"
                 "Run 'freepend COMMAND --help' for details.\n",
                 pathString(defaultServerDataDir()).c_str());
}

std::vector<std::string> utf8Args(int argc, char** argv)
{
#ifdef _WIN32
    // argv is in the ANSI code page on Windows; rebuild it as UTF-8.
    (void)argv;
    std::vector<std::string> out;
    int n = 0;
    if (wchar_t** w = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 0; i < n; ++i)
            out.push_back(pathString(fs::path(w[i])));
        LocalFree(w);
        return out;
    }
#endif
    return std::vector<std::string>(argv, argv + argc);
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#else
    umask(077);  // the data directory holds password and token hashes
#endif
    auto args = utf8Args(argc, argv);
    if (args.size() < 2 || args[1] == "help" || args[1] == "--help" || args[1] == "-h") {
        printUsage(stdout);
        return args.size() < 2 ? 2 : 0;
    }
    if (args[1] == "--version")
        return cmdVersion({});

    const Command* cmd = nullptr;
    for (auto& c : commands())
        if (args[1] == c.name)
            cmd = &c;
    if (!cmd) {
        std::fprintf(stderr, "freepend: unknown command '%s'\n\n", args[1].c_str());
        printUsage(stderr);
        return 2;
    }

    try {
        auto parsed = parseArgs(std::vector<std::string>(args.begin() + 2, args.end()), *cmd);
        if (parsed.has("help")) {
            std::printf("Usage: freepend %s\n\n%s\n", cmd->usage, cmd->help);
            if (std::string(cmd->name) != "version")
                std::printf("\n--data DIR   data directory (default: %s)\n",
                            pathString(defaultServerDataDir()).c_str());
            return 0;
        }
        return cmd->run(parsed);
    } catch (const UsageError& e) {
        std::fprintf(stderr, "freepend %s: %s\nUsage: freepend %s\n", cmd->name, e.what(), cmd->usage);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "freepend: %s\n", e.what());
        return 1;
    }
}
