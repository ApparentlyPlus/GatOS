import os
import pyperclip

def map_and_display_files():
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
        user_input = input("\nEnter desired file extensions (e.g., py, json, txt), specific filenames (e.g., main.py, config.json), or 'all': ").strip().lower()
        
        if not user_input:
            print("Please enter at least one extension, filename, or 'all'.")
            continue
            
        if user_input == 'all':
            desired_extensions = 'all'
            desired_filenames = set()
        else:
            # Split input and process each item
            items = [item.strip() for item in user_input.split(',')]
            desired_extensions = set()
            desired_filenames = set()
            
            for item in items:
                if '.' in item:
                    # This looks like a filename with extension
                    desired_filenames.add(item)
                else:
                    # This looks like an extension
                    desired_extensions.add(item)
            
            print(f"Looking for: extensions={desired_extensions} OR filenames={desired_filenames}")
        
        break

    print("\n--- File Contents ---")
    files_found = False

    for file_path in all_files:
        filename = os.path.basename(file_path)
        extension = os.path.splitext(file_path)[1][1:].lower()
        
        # Check if file matches our criteria
        file_matches = False
        if desired_extensions == 'all':
            file_matches = True
        else:
            # Match by extension OR by filename
            if extension in desired_extensions or filename.lower() in desired_filenames:
                file_matches = True
        
        if file_matches:
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
        msg = f"No files found matching: {user_input}"
        print(msg)
        output_content.append(msg)
    
    # Copy everything to clipboard
    try:
        pyperclip.copy("\n".join(output_content))
        print("\nAll output copied to clipboard!")
    except Exception as e:
        print(f"\nFailed to copy to clipboard: {e}")

if __name__ == "__main__":
    map_and_display_files()