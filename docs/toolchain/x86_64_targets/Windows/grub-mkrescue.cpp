// grub-mkrescue.cpp
//
// A faithful Windows port of upstream `grub-mkrescue` (grub-2.14/util/grub-mkrescue.c).
//
// Upstream `grub-mkrescue` is a C program that is deliberately NOT shipped in the
// prebuilt grub-for-windows binaries. It is an *orchestrator*: it lays out a
// temporary ISO 9660 tree, generates a BIOS El Torito core image and per-platform
// UEFI core images with grub-mkimage, authors a FAT EFI System Partition image
// (efi.img) with mtools, writes the marker files the cores search for, and finally
// invokes `xorriso -as mkisofs` with the exact set of El Torito / isohybrid MBR /
// GPT / HFS+ options that make the result bootable on real BIOS and UEFI firmware
// and when written block-for-block to a USB stick.
//
// Every tool upstream drives is already vendored in the Windows toolchain, so this
// port reproduces grub-mkrescue.c step for step by shelling out to them:
//
//   grub/grub-mkimage.exe        core images          (native Windows paths)
//   grub/grub-glue-efi.exe       fat 32/64 EFI binary (native Windows paths)
//   grub/grub-render-label.exe   Apple .disk_label    (native Windows paths)
//   grub/unicode.pf2             label font
//   ../mtools/mformat.exe        create FAT efi.img    (native Windows paths)
//   ../mtools/mcopy.exe          populate efi.img      (native Windows paths)
//   ../xorriso/xorriso.exe       assemble the ISO      (CYGWIN build -> /cygdrive paths)
//
// xorriso.exe is a Cygwin build (ships cygwin1.dll), so every *host* path handed to
// it must be converted to a POSIX /cygdrive/<drive>/... path (see to_posix_path).
// grub-mkimage and mtools are native builds and take ordinary Windows paths.
//
// Usage mirrors how run.py invokes it:
//   grub-mkrescue.exe -d <GRUB_PKGLIBDIR> -o <out.iso> <ISO_ROOT>
// where <GRUB_PKGLIBDIR> is the base dir containing the platform subdirs
// (i386-pc/, i386-efi/, x86_64-efi/) and <ISO_ROOT> is the user's staged tree
// (boot/kernel.bin, boot/grub/grub.cfg, ...). <ISO_ROOT> is graft-merged into the
// final image exactly as upstream treats trailing SOURCE arguments.

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <filesystem>
#include <sstream>
#include <array>
#include <windows.h>

namespace fs = std::filesystem;

static const std::string EXE_EXT = ".exe";

// Upstream defaults product_name/product_version to PACKAGE_NAME/PACKAGE_VERSION.
static const std::string PRODUCT_NAME = "GRUB";
static const std::string PRODUCT_VERSION = "2.14";

// Platforms we support (those present in the Windows grub dir). Order matters only
// for readability; behavior matches grub-mkrescue.c for each.
static const char* PLAT_I386_PC     = "i386-pc";
static const char* PLAT_I386_EFI    = "i386-efi";
static const char* PLAT_X86_64_EFI  = "x86_64-efi";

// Path helpers

// Convert a Windows path to a Cygwin POSIX path (C:\path -> /cygdrive/c/path).
// Required for xorriso.exe, which is a Cygwin build.
static std::string to_posix_path(const fs::path& p) {
    std::string abs_path = fs::absolute(p).string();

    if (abs_path.length() >= 2 && abs_path[1] == ':') {
        char drive = static_cast<char>(std::tolower(abs_path[0]));
        std::string rest = abs_path.substr(2);
        for (char& c : rest) {
            if (c == '\\') c = '/';
        }
        return "/cygdrive/" + std::string(1, drive) + rest;
    }
    return abs_path;
}

// Command execution

static std::string quote_arg(const std::string& a) {
    if (a.find(' ') != std::string::npos || a.empty())
        return "\"" + a + "\"";
    return a;
}

static std::string join_cmd(const std::vector<std::string>& cmd) {
    std::string full;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (i > 0) full += " ";
        full += quote_arg(cmd[i]);
    }
    return full;
}

// Run a command, echoing it, streaming its output. Optionally set an extra env var
// for the duration of the call (used for MTOOLS_SKIP_CHECK). Aborts on failure when
// check is true, mirroring grub_util_error() behavior upstream.
static int run_command(const std::vector<std::string>& cmd,
                       bool check = true,
                       const char* env_name = nullptr,
                       const char* env_val = nullptr,
                       const fs::path& cwd = fs::path()) {
    std::string line = join_cmd(cmd);

    // Run in a specific working directory when requested. Used for mtools, which
    // parses a leading "X:" in any bare pathspec as an MS-DOS drive letter rather
    // than a Windows path; running from the ISO tree lets us pass drive-letterless
    // relative names (efi.img, efi) that mtools treats as host files.
    std::string full = cwd.empty()
        ? line
        : ("cd /d " + quote_arg(cwd.string()) + " && " + line);
    std::cout << ">>> " << (cwd.empty() ? "" : ("[" + cwd.string() + "] ")) << line << std::endl;

    std::string saved;
    bool had_saved = false;
    if (env_name) {
        char buf[32768];
        DWORD n = GetEnvironmentVariableA(env_name, buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) { saved.assign(buf, n); had_saved = true; }
        SetEnvironmentVariableA(env_name, env_val);
    }

    int ret = std::system(full.c_str());

    if (env_name) {
        SetEnvironmentVariableA(env_name, had_saved ? saved.c_str() : nullptr);
    }

    if (ret != 0 && check) {
        std::cerr << "[FATAL] Command failed with code " << ret << ": " << cmd[0] << std::endl;
        std::exit(ret ? ret : 1);
    }
    return ret;
}

// Run a command and capture its combined stdout+stderr. Used by check_xorriso().
static std::string capture_command(const std::string& cmdline) {
    std::string full = cmdline + " 2>&1";
    std::string out;
    FILE* pipe = _popen(full.c_str(), "r");
    if (!pipe) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0)
        out.append(buf, n);
    _pclose(pipe);
    return out;
}

// Filesystem helpers

static void copy_tree(const fs::path& src, const fs::path& dst) {
    if (!fs::exists(src))
        throw std::runtime_error("Source path does not exist: " + src.string());

    fs::create_directories(dst);
    for (const auto& entry : fs::recursive_directory_iterator(src)) {
        const auto& path = entry.path();
        auto rel = fs::relative(path, src);
        auto target = dst / rel;
        if (fs::is_directory(path))
            fs::create_directories(target);
        else
            fs::copy_file(path, target, fs::copy_options::overwrite_existing);
    }
}

static void touch_empty(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
}

// Reproduce grub-mkrescue.c:write_part(): emit `insmod <name>` for each partmap
// listed in <platdir>/partmap.lst. Silently no-op if the list is absent.
static void write_part(std::ostream& cfg, const fs::path& platdir) {
    std::ifstream in(platdir / "partmap.lst");
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (!line.empty())
            cfg << "insmod " << line << "\n";
    }
}

// Tool locations (resolved once, relative to this executable)

struct Tools {
    fs::path grub_mkimage;
    fs::path grub_glue_efi;
    fs::path grub_render_label;
    fs::path unicode_pf2;
    fs::path mformat;
    fs::path mcopy;
    fs::path mmd;
    fs::path xorriso;
};

static void require(const fs::path& p, const std::string& what) {
    if (!fs::exists(p)) {
        std::cerr << "[FATAL] Missing required tool (" << what << "): " << p << std::endl;
        std::exit(1);
    }
}

// Resolve the helper tools relative to this executable, which lives in the GRUB
// tools folder (the same layout as an upstream install: grub-mkimage, unicode.pf2
// and the i386-pc/ x86_64-efi/ module dirs are siblings here). mtools/ and xorriso/
// are siblings of that folder. `xorriso_override` mirrors upstream's --xorriso=FILE;
// when empty we use the vendored sibling, then fall back to "xorriso" on PATH.
static Tools resolve_tools(const fs::path& exe_dir, const std::string& xorriso_override) {
    fs::path tool_root = exe_dir.parent_path();
    Tools t;
    t.grub_mkimage      = exe_dir / ("grub-mkimage" + EXE_EXT);
    t.grub_glue_efi     = exe_dir / ("grub-glue-efi" + EXE_EXT);
    t.grub_render_label = exe_dir / ("grub-render-label" + EXE_EXT);
    t.unicode_pf2       = exe_dir / "unicode.pf2";
    t.mformat           = tool_root / "mtools" / ("mformat" + EXE_EXT);
    t.mcopy             = tool_root / "mtools" / ("mcopy" + EXE_EXT);
    t.mmd               = tool_root / "mtools" / ("mmd" + EXE_EXT);

    if (!xorriso_override.empty())
        t.xorriso = fs::absolute(xorriso_override);
    else if (fs::exists(tool_root / "xorriso" / ("xorriso" + EXE_EXT)))
        t.xorriso = tool_root / "xorriso" / ("xorriso" + EXE_EXT);
    else
        t.xorriso = fs::path("xorriso" + EXE_EXT);   // rely on PATH, like upstream

    require(t.grub_mkimage,      "grub-mkimage");
    require(t.mformat,           "mformat (mtools)");
    require(t.mcopy,             "mcopy (mtools)");
    require(t.mmd,               "mmd (mtools)");
    // xorriso: if an explicit path was given it must exist; a bare "xorriso" is
    // resolved via PATH by the shell at run time, so don't stat it here.
    if (!xorriso_override.empty() || t.xorriso.is_absolute())
        require(t.xorriso, "xorriso");
    // glue/render/font are only needed for the Apple path; checked when used.
    return t;
}

// grub-mkimage wrapper

static void make_image(const Tools& t,
                       const std::string& format,
                       const fs::path& platdir,
                       const fs::path& load_cfg,
                       const fs::path& output,
                       const std::vector<std::string>& modules) {
    fs::create_directories(output.parent_path());
    std::vector<std::string> cmd = {
        t.grub_mkimage.string(),
        "-O", format,
        "-d", platdir.string(),
        "-c", load_cfg.string(),
        "-p", "/boot/grub",
        "-o", output.string()
    };
    for (const auto& m : modules) cmd.push_back(m);
    run_command(cmd);
}

// xorriso capability probe

static bool check_xorriso(const Tools& t, const std::string& needle) {
    std::vector<std::string> cmd = { t.xorriso.string(), "-as", "mkisofs", "-help" };
    std::string out = capture_command(join_cmd(cmd));
    return out.find(needle) != std::string::npos;
}

// main

static void print_usage(const char* prog) {
    std::cout <<
"Usage: " << prog << " [OPTION] SOURCE...\n"
"Make GRUB CD-ROM, disk, pendrive and floppy bootable image.\n"
"\n"
"  -o, --output=FILE      save output in FILE [required]\n"
"  -d, --directory=DIR    use a single platform's images/modules from DIR\n"
"                         (default: scan this program's folder for i386-pc,\n"
"                         i386-efi, x86_64-efi)\n"
"      --xorriso=FILE     use FILE as xorriso [optional]\n"
"      --fonts=FONTS      install FONTS (space separated) [e.g. unicode]\n"
"      --themes=THEMES    install THEMES [optional]\n"
"      --locales=LOCALES  install LOCALES [optional]\n"
"  -h, --help             show this help\n"
"\n"
"Arguments other than options are passed to xorriso, and indicate source files\n"
"or directories grafted into the image (e.g. the staged ISO tree containing\n"
"boot/grub/grub.cfg).\n";
}

// Is `name` a long option (without leading dashes) that upstream grub-mkrescue
// consumes a value for? Used only to correctly skip the value in the space-
// separated form of options we don't otherwise act on, so it isn't mistaken for a
// SOURCE argument. Options we DO act on are handled explicitly before this.
static bool is_value_option(const std::string& name) {
    static const char* v[] = {
        "compress", "modules", "install-modules", "pubkey", "sbat", "dtb",
        "config", "memdisk", "format", "prefix", "compression", "sbat-generation",
        "product-name", "product-version", "label-font", "label-color",
        "label-bgcolor", "rom-directory", "grub-mkimage", "grub-glue-efi",
        "grub-render-label", "core-compress", "verbose-modules"
    };
    for (const char* s : v) if (name == s) return true;
    return false;
}

int main(int argc, char* argv[]) {
    try {
        fs::path exe_dir = fs::absolute(argv[0]).parent_path();
        std::string output_str, xorriso_str, dir_str, fonts_str;
        std::vector<std::string> sources;   // xorriso tail (SOURCE dirs/files, passthrough)

        auto need_value = [&](int& i, const std::string& opt) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "[FATAL] option requires an argument -- '" << opt << "'\n";
                std::exit(1);
            }
            return argv[++i];
        };

        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];

            if (a == "-h" || a == "--help" || a == "-?") { print_usage(argv[0]); return 0; }
            if (a == "--usage" || a == "--version" || a == "-V" || a == "-v"
                || a == "--verbose" || a == "--sparc-boot" || a == "--arcs-boot"
                || a == "--disable-shim-lock") {
                continue;   // accepted, no-op for this port
            }

            if (a == "-o") { output_str = need_value(i, "output"); continue; }
            if (a == "-d") { dir_str    = need_value(i, "directory"); continue; }

            if (a.rfind("--", 0) == 0) {
                std::string body = a.substr(2);
                std::string key = body, val;
                bool has_val = false;
                auto eq = body.find('=');
                if (eq != std::string::npos) { key = body.substr(0, eq); val = body.substr(eq + 1); has_val = true; }

                if (key == "output")   { output_str  = has_val ? val : need_value(i, "output");   continue; }
                if (key == "directory"){ dir_str     = has_val ? val : need_value(i, "directory");continue; }
                if (key == "xorriso")  { xorriso_str = has_val ? val : need_value(i, "xorriso");  continue; }
                if (key == "fonts")    { fonts_str   = has_val ? val : need_value(i, "fonts");    continue; }
                if (key == "themes" || key == "locales") {
                    if (!has_val) need_value(i, key);   // accept & ignore (like empty upstream)
                    continue;
                }
                if (is_value_option(key)) { if (!has_val) need_value(i, key); continue; }
                // Unknown long option -> forward to xorriso, upstream-style.
                sources.push_back(a);
                continue;
            }

            if (!a.empty() && a[0] == '-') {
                // Unknown short option -> forward to xorriso.
                sources.push_back(a);
                continue;
            }

            // Bare argument: a SOURCE for xorriso.
            sources.push_back(a);
        }

        if (output_str.empty()) {
            std::cerr << "[FATAL] output file must be specified (-o FILE)\n";
            return 1;
        }
        if (sources.empty()) {
            std::cerr << "[FATAL] no SOURCE given (e.g. the staged ISO tree)\n";
            return 1;
        }

        fs::path output_iso = fs::absolute(output_str);

        Tools t = resolve_tools(exe_dir, xorriso_str);

        std::map<std::string, fs::path> plat;
        if (!dir_str.empty()) {
            fs::path d = fs::absolute(dir_str);
            plat[d.filename().string()] = d;
        } else {
            for (const char* name : {PLAT_I386_PC, PLAT_I386_EFI, PLAT_X86_64_EFI}) {
                fs::path d = exe_dir / name;
                if (fs::is_directory(d)) plat[name] = d;
            }
        }
        auto plat_dir = [&](const char* name) { return plat.at(name); };
        auto has_plat = [&](const char* name) { return plat.count(name) != 0; };

        bool have_bios    = has_plat(PLAT_I386_PC);
        bool have_efi32   = has_plat(PLAT_I386_EFI);
        bool have_efi64   = has_plat(PLAT_X86_64_EFI);
        bool have_any_efi = have_efi32 || have_efi64;

        if (!have_bios && !have_any_efi) {
            std::cerr << "[FATAL] No usable platform dirs (i386-pc/i386-efi/x86_64-efi) found "
                      << (dir_str.empty() ? ("next to " + exe_dir.string()) : ("in " + dir_str))
                      << std::endl;
            return 1;
        }

        // xorriso capability checks (grub-mkrescue.c:509)
        if (!check_xorriso(t, "graft-points")) {
            std::cerr << "[FATAL] xorriso does not support -graft-points\n";
            return 1;
        }
        bool have_grub2_boot_info = check_xorriso(t, "grub2-boot-info");

        // system_area == COMMON since we always have x86 BIOS/EFI platforms.
        // (grub-mkrescue.c enables HFS+/APM/protective-MBR in this mode.)

        // temp iso tree
        fs::path temp_root = fs::temp_directory_path() /
            ("grub_mkrescue_" + std::to_string(static_cast<long long>(std::time(nullptr))));
        fs::path iso_dir = temp_root / "iso9660";
        fs::path boot_grub = iso_dir / "boot" / "grub";
        fs::create_directories(boot_grub);
        std::cout << "[INFO] Staging ISO tree at: " << iso_dir << std::endl;

        std::vector<fs::path> temp_files;   // load.cfg / sysarea temp files to clean up
        auto make_tmp = [&](const std::string& tag) {
            fs::path p = temp_root / (tag + "_" + std::to_string(temp_files.size()) + ".cfg");
            temp_files.push_back(p);
            return p;
        };

        try {
            // time-based UUID (grub-mkrescue.c:580)
            std::time_t tim = std::time(nullptr);
            std::tm* g = std::gmtime(&tim);
            char uuid[64];
            std::snprintf(uuid, sizeof(uuid), "%04d-%02d-%02d-%02d-%02d-%02d-00",
                          g->tm_year + 1900, g->tm_mon + 1, g->tm_mday,
                          g->tm_hour, g->tm_min, g->tm_sec);
            std::string iso_uuid = uuid;
            std::string mod_date;   // uuid without dashes, for --modification-date
            for (char c : iso_uuid) if (c != '-') mod_date.push_back(c);

            // xorriso argument vector
            std::vector<std::string> xa;
            auto push = [&](const std::string& s) { xa.push_back(s); };

            push(t.xorriso.string());
            push("-as");
            push("mkisofs");
            push("-graft-points");
            push("--modification-date=" + mod_date);

            // copy platform module trees into boot/grub/<plat>
            auto copy_plat = [&](const char* name, bool present) {
                if (present) copy_tree(plat_dir(name), boot_grub / name);
            };
            copy_plat(PLAT_I386_PC, have_bios);
            copy_plat(PLAT_I386_EFI, have_efi32);
            copy_plat(PLAT_X86_64_EFI, have_efi64);

            if (!fonts_str.empty()) {
                std::istringstream fs_in(fonts_str);
                std::string font;
                while (fs_in >> font) {
                    fs::path src = exe_dir / (font + ".pf2");
                    if (fs::exists(src)) {
                        fs::path dst = boot_grub / "fonts" / (font + ".pf2");
                        fs::create_directories(dst.parent_path());
                        fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
                    } else {
                        std::cerr << "[WARN] font '" << font << "' not found at " << src
                                  << "; skipping." << std::endl;
                    }
                }
            }

            // BIOS core.img
            if (have_bios) {
                std::cout << "[INFO] enabling BIOS support ..." << std::endl;
                fs::path src = plat_dir(PLAT_I386_PC);
                fs::path load_cfg = make_tmp("bios");
                {
                    std::ofstream f(load_cfg, std::ios::binary);
                    write_part(f, src);   // BIOS config is just the partmap insmods
                }
                fs::path eltorito = boot_grub / PLAT_I386_PC / "eltorito.img";
                make_image(t, "i386-pc-eltorito", src, load_cfg, eltorito,
                           {"biosdisk", "iso9660"});

                push("-b");
                push("boot/grub/i386-pc/eltorito.img");
                push("-no-emul-boot");
                push("-boot-load-size");
                push("4");
                push("-boot-info-table");

                if (have_grub2_boot_info) {
                    push("--grub2-boot-info");
                    push("--grub2-mbr");
                    push(to_posix_path(src / "boot_hybrid.img"));
                } else {
                    // Fallback for xorriso < 1.2.9: build a legacy -G system area
                    // from boot.img + a plain i386-pc core (grub-mkrescue.c:645-685).
                    std::cerr << "[WARN] xorriso lacks --grub2-boot-info; "
                                 "using legacy -G system area (boot-as-disk may be limited)." << std::endl;
                    fs::path sysarea = temp_root / "sysarea.img";
                    temp_files.push_back(sysarea);
                    fs::path core = temp_root / "core_i386pc.img";
                    temp_files.push_back(core);

                    // core image (non-eltorito) for the system area
                    make_image(t, "i386-pc", src, load_cfg, core, {"biosdisk", "iso9660"});

                    // prepend boot.img's 512-byte sector, then the core
                    std::ifstream bi(src / "boot.img", std::ios::binary);
                    std::ifstream ci(core, std::ios::binary);
                    std::ofstream so(sysarea, std::ios::binary);
                    char sect[512] = {0};
                    bi.read(sect, 512);
                    so.write(sect, 512);
                    so << ci.rdbuf();
                    so.flush();
                    auto sz = fs::exists(sysarea) ? fs::file_size(sysarea) : 0;
                    if (sz > 32768) {
                        std::cerr << "[WARN] core image too big for boot-as-disk; skipping -G." << std::endl;
                    } else {
                        push("-G");
                        push(to_posix_path(sysarea));
                    }
                }
            }

            // Apple CoreServices / HFS+
            fs::path core_services;
            if (have_any_efi) {
                core_services = iso_dir / "System" / "Library" / "CoreServices";
                fs::create_directories(core_services);

                touch_empty(iso_dir / "mach_kernel");

                {
                    std::ofstream f(core_services / "SystemVersion.plist", std::ios::binary);
                    f << "<plist version=\"1.0\">\n<dict>\n"
                      << "        <key>ProductBuildVersion</key>\n        <string></string>\n"
                      << "        <key>ProductName</key>\n        <string>" << PRODUCT_NAME << "</string>\n"
                      << "        <key>ProductVersion</key>\n        <string>" << PRODUCT_VERSION << "</string>\n"
                      << "</dict>\n</plist>\n";
                }

                std::string label_string = PRODUCT_NAME + " " + PRODUCT_VERSION;
                fs::path disk_label = core_services / ".disk_label";

                // The vendored grub-render-label.exe loads its PF2 font through GRUB's
                // internal file layer, which cannot open host paths in this Windows
                // distribution (it mangles any path to "/C:\..."). The rendered label is
                // purely cosmetic (the boot-picker image shown when an old Intel Mac boots
                // this disc from CD); it is irrelevant to BIOS/UEFI/USB boot. So render it
                // best-effort and fall back to an empty placeholder, keeping the rest of
                // the Apple/HFS+ structure faithful. Requires grub-render-label + unicode.pf2.
                bool rendered = false;
                if (fs::exists(t.grub_render_label) && fs::exists(t.unicode_pf2)) {
                    int rc = run_command({
                        t.grub_render_label.string(),
                        "-f", t.unicode_pf2.string(),
                        "-b", "white",
                        "-c", "black",
                        "-t", label_string,
                        "-o", disk_label.string()
                    }, /*check=*/false);
                    rendered = (rc == 0) && fs::exists(disk_label) && fs::file_size(disk_label) > 0;
                }
                if (!rendered) {
                    std::cerr << "[WARN] grub-render-label unavailable/failed; writing empty "
                                 ".disk_label (Apple CD boot-picker label will be blank)." << std::endl;
                    touch_empty(disk_label);
                }
                {
                    std::ofstream f(core_services / ".disk_label.contentDetails", std::ios::binary);
                    f << label_string << "\n";
                }

                // system_area == COMMON -> HFS+ blessing
                push("-hfsplus");
                push("-apm-block-size");
                push("2048");
                push("-hfsplus-file-creator-type");
                push("chrp");
                push("tbxj");
                push("/System/Library/CoreServices/.disk_label");
                push("-hfs-bless-by");
                push("i");
                push("/System/Library/CoreServices/boot.efi");
            }

            // EFI cores + FAT ESP (efi.img)
            if (have_any_efi) {
                fs::path efi_dir = iso_dir / "efi";
                fs::path efi_boot = efi_dir / "boot";
                fs::create_directories(efi_boot);

                // .disk/<uuid>.uuid marker searched for by the EFI cores.
                fs::path disk_dir = iso_dir / ".disk";
                fs::create_directories(disk_dir);
                touch_empty(disk_dir / (iso_uuid + ".uuid"));

                // Shared EFI module set embedded into each EFI core (grub-mkrescue.c
                // pushes exactly these around make_image_abs): the media-discovery modules
                // needed for the embedded core to find its root and prefix. Everything else
                // (normal, multiboot2, all_video, ...) is loaded from /boot/grub at runtime,
                // exactly as upstream intends. This is the faithful set: the GatOS Windows
                // grub EFI platform dirs are built from source (see docs/.../windows-get-grub
                // .txt) with the modules NOT stack-protector-compiled, so runtime module
                // loading works and no guard shim / boot-path embedding is required.
                std::vector<std::string> efi_mods =
                    {"part_gpt", "part_msdos", "fat", "ntfs", "part_apple", "search", "iso9660"};

                // Per-platform early config: locate the media by the .disk uuid file
                // (file-based search survives FAT32 transposition), set prefix.
                auto efi_load_cfg = [&](const fs::path& platdir) {
                    fs::path lc = make_tmp("efi");
                    std::ofstream f(lc, std::ios::binary);
                    f << "search --set=root --file /.disk/" << iso_uuid << ".uuid\n";
                    f << "set prefix=(${root})/boot/grub\n";
                    write_part(f, platdir);
                    return lc;
                };

                fs::path img64 = efi_boot / "bootx64.efi";
                fs::path img32 = efi_boot / "bootia32.efi";

                if (have_efi64) {
                    fs::path src = plat_dir(PLAT_X86_64_EFI);
                    make_image(t, "x86_64-efi", src, efi_load_cfg(src), img64, efi_mods);
                }
                if (have_efi32) {
                    fs::path src = plat_dir(PLAT_I386_EFI);
                    make_image(t, "i386-efi", src, efi_load_cfg(src), img32, efi_mods);
                    // For old Macs: efi/boot/boot.efi is a copy of the ia32 loader.
                    fs::copy_file(img32, efi_boot / "boot.efi", fs::copy_options::overwrite_existing);
                }

                // System/Library/CoreServices/boot.efi (Apple path). Upstream glues the
                // ia32+x64 loaders into one universal binary when both exist. The vendored
                // grub-glue-efi.exe cannot read its inputs through GRUB's file layer in this
                // Windows distribution, so glue best-effort and fall back to the x86_64
                // loader alone (upstream's own single-arch branch). Consequence: on old
                // 32-bit-EFI Intel Macs the disc won't boot; every 64-bit Mac still does.
                if (!core_services.empty()) {
                    fs::path img_mac = core_services / "boot.efi";
                    bool glued = false;
                    if (have_efi32 && have_efi64 && fs::exists(t.grub_glue_efi)) {
                        int rc = run_command({
                            t.grub_glue_efi.string(),
                            "-3", img32.string(),
                            "-6", img64.string(),
                            "-o", img_mac.string()
                        }, /*check=*/false);
                        glued = (rc == 0) && fs::exists(img_mac) && fs::file_size(img_mac) > 0;
                        if (!glued)
                            std::cerr << "[WARN] grub-glue-efi unavailable/failed; Apple boot.efi "
                                         "will be x86_64-only (not a universal binary)." << std::endl;
                    }
                    if (!glued) {
                        if (have_efi64)
                            fs::copy_file(img64, img_mac, fs::copy_options::overwrite_existing);
                        else if (have_efi32)
                            fs::copy_file(img32, img_mac, fs::copy_options::overwrite_existing);
                    }
                }

                // -- author the FAT EFI System Partition (grub-mkrescue.c:857-864) --
                // Upstream does `mformat ... efi.img` then `mcopy -s efi <dir> ::/`.
                // This mtools build cannot open a host *directory* as an mcopy source on
                // Windows (fopen on a dir -> "Permission denied"), so we create the ESP
                // tree with mmd and copy each file individually. All mtools calls run from
                // the ISO tree with drive-letterless relative paths, otherwise mtools reads
                // a leading "C:" as an MS-DOS drive letter.
                run_command({
                    t.mformat.string(),
                    "-C", "-f", "2880", "-L", "16",
                    "-i", "efi.img", "::"
                }, true, "MTOOLS_SKIP_CHECK", "1", iso_dir);
                run_command({
                    t.mmd.string(), "-i", "efi.img", "::/efi", "::/efi/boot"
                }, true, "MTOOLS_SKIP_CHECK", "1", iso_dir);
                for (const auto& e : fs::directory_iterator(efi_boot)) {
                    if (!e.is_regular_file()) continue;
                    std::string name = e.path().filename().string();
                    run_command({
                        t.mcopy.string(), "-i", "efi.img",
                        "efi/boot/" + name, "::/efi/boot/"
                    }, true, "MTOOLS_SKIP_CHECK", "1", iso_dir);
                }

                // Register the ESP as the El Torito EFI boot image + GPT partition.
                push("--efi-boot");
                push("efi.img");
                push("-efi-boot-part");
                push("--efi-boot-image");
            }

            // Final xorriso tail
            push("--protective-msdos-label");
            push("-o");
            push(to_posix_path(output_iso));
            push("-r");
            push(to_posix_path(iso_dir));
            push("--sort-weight");
            push("0");
            push("/");
            push("--sort-weight");
            push("1");
            push("/boot");

            for (const auto& s : sources) {
                bool is_host_path = !s.empty() && s[0] != '-' && fs::exists(fs::path(s));
                push(is_host_path ? to_posix_path(fs::path(s)) : s);
            }

            fs::create_directories(output_iso.parent_path());
            run_command(xa);

            std::cout << "[DONE] Hybrid ISO created: " << output_iso << std::endl;
            std::error_code ec;
            fs::remove_all(temp_root, ec);

        } catch (...) {
            std::error_code ec;
            fs::remove_all(temp_root, ec);
            throw;
        }

    } catch (const std::exception& e) {
        std::cerr << "\n[FATAL] " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
