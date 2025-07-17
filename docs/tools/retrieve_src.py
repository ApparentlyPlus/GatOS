import os
import pyperclip

def map_and_display_by_extension():
    """
    Maps all files recursively in the current directory and its subfolders.
    Asks the user for desired file extensions (without the '.'),
    outputs the contents of matching files, and copies everything to clipboard.
    """
    script_dir = os.path.dirname(os.path.abspath(__file__))
    all_files = []
    output_content = []  # Store all output for clipboard

    print("Scanning for files in:", script_dir)
    for root, _, files in os.walk(script_dir):
        for filename in files:
            file_path = os.path.join(root, filename)
            if file_path == os.path.abspath(__file__):
                continue  # Skip the script itself
            all_files.append(file_path)

    if not all_files:
        print("No files found to scan (excluding the script itself).")
        return

    # Get unique extensions (lowercase, without dot)
    unique_extensions = sorted({os.path.splitext(f)[1][1:].lower() for f in all_files if os.path.splitext(f)[1]})
    
    print("\nAvailable file extensions found:")
    if unique_extensions:
        print(", ".join(unique_extensions))
    else:
        print("No specific file extensions found.")

    while True:
        extensions_input = input("\nEnter desired file extensions (e.g., py, json, txt) or 'all': ").strip().lower()
        
        if not extensions_input:
            print("Please enter at least one extension or 'all'.")
            continue
            
        desired_extensions = set(ext.strip() for ext in extensions_input.split(',')) if extensions_input != 'all' else 'all'
        break

    print("\n--- File Contents ---")
    files_found = False

    for file_path in all_files:
        filename = os.path.basename(file_path)
        extension = os.path.splitext(file_path)[1][1:].lower()
        
        if desired_extensions == 'all' or extension in desired_extensions:
            files_found = True
            file_header = f"\n{filename}:"
            print(file_header)
            output_content.append(file_header)
            
            try:
                with open(file_path, 'r', encoding='utf-8') as f:
                    content = f.read()
                    print(content)
                    output_content.append(content)
            except UnicodeDecodeError:
                error_msg = f"Error: Could not decode {filename} with UTF-8. Skipping content display."
                print(error_msg)
                output_content.append(error_msg)
            except Exception as e:
                error_msg = f"Error reading {filename}: {e}"
                print(error_msg)
                output_content.append(error_msg)

    if not files_found:
        msg = f"No files found with extensions: {extensions_input}"
        print(msg)
        output_content.append(msg)
    
    # Copy everything to clipboard
    try:
        pyperclip.copy("\n".join(output_content))
        print("\nAll output copied to clipboard!")
    except Exception as e:
        print(f"\nFailed to copy to clipboard: {e}")

if __name__ == "__main__":
    map_and_display_by_extension()