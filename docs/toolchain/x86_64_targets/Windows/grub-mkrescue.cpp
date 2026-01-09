#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <sstream>
#include <array>
#include <memory>
#include <windows.h> // Windows specific

// Windows definitions
#define popen _popen
#define pclose _pclose

namespace fs = std::filesystem;

// Constants
const std::string ISO_VOLID = "GATOS_ISO";
const std::string EXE_EXT = ".exe";

// Utility: Convert Windows path to Cygwin format (e.g. C:\path -> /cygdrive/c/path)
// Required because xorriso and grub tools often expect POSIX-style paths on Windows
std::string to_posix_path(const fs::path& p) {
    std::string abs_path = fs::absolute(p).string();
    
    if (abs_path.length() >= 2 && abs_path[1] == ':') {
        char drive = std::tolower(abs_path[0]);
        std::string rest = abs_path.substr(2);
        
        // Replace backslashes with forward slashes
        for (char& c : rest) {
            if (c == '\\') c = '/';
        }
        
        return "/cygdrive/" + std::string(1, drive) + rest;
    }
    
    return abs_path;
}

// Utility: Run a command and stream output
int run_command(const std::vector<std::string>& cmd, bool check = true) {
    // Build command string
    std::ostringstream oss;
    oss << ">>> ";
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (i > 0) oss << " ";
        // Quote arguments with spaces
        if (cmd[i].find(' ') != std::string::npos) {
            oss << "\"" << cmd[i] << "\"";
        } else {
            oss << cmd[i];
        }
    }
    std::cout << oss.str() << std::endl;
    
    // Build command for execution
    std::string full_cmd;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (i > 0) full_cmd += " ";
        if (cmd[i].find(' ') != std::string::npos) {
            full_cmd += "\"" + cmd[i] + "\"";
        } else {
            full_cmd += cmd[i];
        }
    }
    
    int ret = std::system(full_cmd.c_str());
    
    if (ret != 0 && check) {
        std::cerr << "[FATAL] Command failed with code " << ret << std::endl;
        std::exit(ret);
    }
    
    return ret;
}

// Utility: Copy directory recursively
void copy_tree(const fs::path& src, const fs::path& dst) {
    if (!fs::exists(src)) {
        throw std::runtime_error("Source path does not exist: " + src.string());
    }
    
    fs::create_directories(dst);
    
    for (const auto& entry : fs::recursive_directory_iterator(src)) {
        const auto& path = entry.path();
        auto rel_path = fs::relative(path, src);
        auto dst_path = dst / rel_path;
        
        if (fs::is_directory(path)) {
            fs::create_directories(dst_path);
        } else {
            fs::copy_file(path, dst_path, fs::copy_options::overwrite_existing);
        }
    }
}

// Copy GRUB files into staging directory
void copy_grub_files(const fs::path& staging_root, 
                     const fs::path& bios_src,
                     const fs::path& efi_src,
                     const fs::path& font_src) {
    fs::path grub_target_dir = staging_root / "boot" / "grub";
    fs::create_directories(grub_target_dir);
    
    auto copy_if_exists = [&](const fs::path& src, const std::string& name) {
        if (!fs::exists(src)) {
            std::cout << "[WARN] Missing " << name << " directory, skipping: " << src << std::endl;
            return;
        }
        fs::path target = grub_target_dir / src.filename();
        std::cout << "Copying " << name << " modules to " << target << std::endl;
        copy_tree(src, target);
    };
    
    copy_if_exists(bios_src, "BIOS");
    copy_if_exists(efi_src, "UEFI");
    copy_if_exists(font_src, "Font");
}

// Build BIOS boot image
void build_bios_image(const fs::path& grub_mkimage,
                      const fs::path& bios_src_dir,
                      const fs::path& bios_img_path) {
    if (!fs::exists(bios_src_dir)) {
        std::cout << "[WARN] Missing BIOS modules, skipping BIOS image build: " 
                  << bios_src_dir << std::endl;
        return;
    }
    
    fs::create_directories(bios_img_path.parent_path());
    
    // Create temporary config file
    fs::path temp_cfg = fs::temp_directory_path() / ("grub_cfg_" + std::to_string(std::time(nullptr)) + ".txt");
    {
        std::ofstream cfg_file(temp_cfg);
        if (!cfg_file) {
            throw std::runtime_error("Failed to create temporary config file");
        }
        cfg_file << "search --label --set=root " << ISO_VOLID << "\n";
        cfg_file << "set prefix=(${root})/boot/grub\n";
        cfg_file << "configfile (${prefix})/grub.cfg\n";
    }
    
    std::cout << "Using temporary BIOS config: " << temp_cfg << std::endl;
    
    try {
        std::vector<std::string> cmd = {
            grub_mkimage.string(),
            "-O", "i386-pc-eltorito",
            "-d", bios_src_dir.string(),
            "-c", temp_cfg.string(),
            "--prefix=/boot/grub",
            "-o", bios_img_path.string(),
            "biosdisk", "iso9660", "part_msdos",
            "configfile", "search", "search_label", "normal"
        };
        run_command(cmd);
    } catch (...) {
        fs::remove(temp_cfg);
        throw;
    }
    
    fs::remove(temp_cfg);
}

// Build the final ISO
void build_iso(const fs::path& xorriso,
               const fs::path& output_iso,
               const fs::path& staging_root,
               const fs::path& bios_src_dir,
               const fs::path& bios_img_path,
               const fs::path& efi_img_path) {
    if (!fs::exists(xorriso)) {
        std::cerr << "[FATAL] Missing xorriso, cannot create ISO: " << xorriso << std::endl;
        std::exit(1);
    }
    
    bool bios_bootable = fs::exists(bios_img_path);
    bool efi_bootable = fs::exists(efi_img_path);
    
    if (!bios_bootable && !efi_bootable) {
        std::cerr << "[FATAL] No bootable images were found in staging dir. Aborting." << std::endl;
        std::exit(1);
    }
    
    std::vector<std::string> cmd = {
        xorriso.string(), "-as", "mkisofs",
        "-R", "-J",
        "-V", ISO_VOLID
    };
    
    if (bios_bootable) {
        std::cout << "Adding BIOS boot options to ISO." << std::endl;
        cmd.insert(cmd.end(), {
            "-b", "boot/grub/i386-pc/eltorito.img",
            "-no-emul-boot",
            "-boot-load-size", "4",
            "-boot-info-table",
            "-isohybrid-mbr", to_posix_path(bios_src_dir / "boot_hybrid.img"),
            "--grub2-boot-info",
            "--grub2-mbr", to_posix_path(bios_src_dir / "boot.img")
        });
    }
    
    if (efi_bootable) {
        std::cout << "Adding UEFI boot options to ISO." << std::endl;
        cmd.insert(cmd.end(), {
            "-eltorito-alt-boot",
            "-e", "EFI/BOOT/BOOTX64.EFI",
            "-no-emul-boot",
            "-isohybrid-gpt-basdat"
        });
    }
    
    cmd.insert(cmd.end(), {
        "-o", to_posix_path(output_iso),
        to_posix_path(staging_root)
    });
    
    run_command(cmd);
}

// Print usage
void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [OPTIONS] ISO_ROOT\n"
              << "\nOptions:\n"
              << "  -o, --output FILE    Output ISO file (required)\n"
              << "  -d, --directory DIR  GRUB module base directory (override)\n"
              << "  -h, --help           Show this help message\n"
              << "\nArguments:\n"
              << "  ISO_ROOT             Path to ISO tree (must contain /boot/grub/grub.cfg)\n";
}

int main(int argc, char* argv[]) {
    try {
        // -------------------------------------------------------------------
        // Path Configuration (Adjusted for Executable inside 'grub' folder)
        // -------------------------------------------------------------------
        
        // Assumption: Executable is inside the 'grub' folder (e.g., C:/tools/grub/maker.exe)
        fs::path grub_bin_dir = fs::absolute(argv[0]).parent_path();
        
        // Assumption: 'xorriso' folder is a sibling of 'grub' (e.g., C:/tools/xorriso/)
        fs::path tool_root = grub_bin_dir.parent_path(); 
        
        // Default paths based on the provided tree structure
        // Since we are inside 'grub', these are direct children
        fs::path grub_i386_dir = grub_bin_dir / "i386-pc";
        fs::path grub_x64_dir = grub_bin_dir / "x86_64-efi";
        fs::path grub_font_dir = grub_bin_dir / "fonts"; 
        
        // The mkimage tool is likely in the same folder as this executable
        fs::path grub_mkimage = grub_bin_dir / ("grub-mkimage" + EXE_EXT);
        
        // Locate xorriso in the sibling directory
        fs::path xorriso = tool_root / "xorriso" / ("xorriso" + EXE_EXT);
        
        // -------------------------------------------------------------------

        // Parse arguments
        std::string output_iso_str;
        std::string iso_root_str;
        std::string directory_override;
        
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            
            if (arg == "-h" || arg == "--help") {
                print_usage(argv[0]);
                return 0;
            } else if (arg == "-o" || arg == "--output") {
                if (i + 1 >= argc) {
                    std::cerr << "Error: " << arg << " requires an argument" << std::endl;
                    return 1;
                }
                output_iso_str = argv[++i];
            } else if (arg == "-d" || arg == "--directory") {
                if (i + 1 >= argc) {
                    std::cerr << "Error: " << arg << " requires an argument" << std::endl;
                    return 1;
                }
                directory_override = argv[++i];
            } else if (arg[0] == '-') {
                std::cerr << "Error: Unknown option " << arg << std::endl;
                print_usage(argv[0]);
                return 1;
            } else {
                if (iso_root_str.empty()) {
                    iso_root_str = arg;
                } else {
                    std::cerr << "Error: Multiple positional arguments provided" << std::endl;
                    return 1;
                }
            }
        }
        
        // Validate required arguments
        if (output_iso_str.empty() || iso_root_str.empty()) {
            std::cerr << "Error: Missing required arguments" << std::endl;
            print_usage(argv[0]);
            return 1;
        }
        
        // Setup paths
        fs::path iso_root_source = fs::absolute(iso_root_str);
        fs::path output_iso = fs::absolute(output_iso_str);
        
        if (!directory_override.empty()) {
            fs::path module_dir = fs::absolute(directory_override);
            grub_i386_dir = module_dir / "i386-pc";
            grub_x64_dir = module_dir / "x86_64-efi";
            grub_font_dir = module_dir / "fonts";
        }
        
        // Check for required files in source tree
        fs::path grub_cfg_source = iso_root_source / "boot" / "grub" / "grub.cfg";
        fs::path efi_img_source = iso_root_source / "EFI" / "BOOT" / "BOOTX64.EFI";
        
        if (!fs::exists(grub_cfg_source)) {
            std::cerr << "[FATAL] Missing source grub.cfg at: " << grub_cfg_source << std::endl;
            return 1;
        }
        
        if (!fs::exists(efi_img_source)) {
            std::cout << "[WARN] Missing source UEFI image: " << efi_img_source 
                      << ". ISO may not be UEFI-bootable." << std::endl;
        }
        
        // Create temporary staging directory
        fs::path temp_dir = fs::temp_directory_path() / ("grub_mkrescue_" + std::to_string(std::time(nullptr)));
        fs::path staging_root = temp_dir / "iso_tree";
        
        std::cout << "Created temporary staging directory: " << staging_root << std::endl;
        fs::create_directories(staging_root);
        
        try {
            // 1. Copy user's ISO tree to staging dir
            std::cout << "Copying " << iso_root_source << " to staging area..." << std::endl;
            copy_tree(iso_root_source, staging_root);
            
            // Define paths within staging directory
            fs::path bios_img_path_stage = staging_root / "boot" / "grub" / "i386-pc" / "eltorito.img";
            fs::path efi_img_path_stage = staging_root / "EFI" / "BOOT" / "BOOTX64.EFI";
            
            // 2. Copy all GRUB modules/fonts into staging tree
            copy_grub_files(staging_root, grub_i386_dir, grub_x64_dir, grub_font_dir);
            
            // 3. Build the BIOS bootloader into staging tree
            build_bios_image(grub_mkimage, grub_i386_dir, bios_img_path_stage);
            
            // 4. Assemble the final ISO from staging tree
            build_iso(xorriso, output_iso, staging_root, grub_i386_dir, 
                      bios_img_path_stage, efi_img_path_stage);
            
            std::cout << "[DONE] Hybrid ISO created: " << output_iso << std::endl;
            
            // Cleanup
            fs::remove_all(temp_dir);
            
        } catch (...) {
            // Cleanup on error
            if (fs::exists(temp_dir)) {
                fs::remove_all(temp_dir);
            }
            throw;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "\nAn error occurred: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}