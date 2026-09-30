import os
import pyperclip

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    files = []
    out = []  # what gets copied to the clipboard

    print("Scanning for files in:", here)
    for root, _, names in os.walk(here):
        for name in names:
            path = os.path.join(root, name)
            if "toolchain" in path.lower() or path == os.path.abspath(__file__):
                continue
            files.append(path)

    if not files:
        print("No files found to scan (excluding the script itself).")
        return

    exts = sorted({os.path.splitext(f)[1][1:].lower() for f in files if os.path.splitext(f)[1]})

    print("\nAvailable file extensions found:")
    if exts:
        print(", ".join(exts))
    else:
        print("No specific file extensions found.")

    while True:
        query = input("\nEnter desired file extensions (e.g., py, json, txt), specific filenames (e.g., main.py, config.json), or 'all': ").strip().lower()

        if not query:
            print("Please enter at least one extension, filename, or 'all'.")
            continue

        if query == 'all':
            want_ext, want_name = None, set()
        else:
            items = [i.strip() for i in query.split(',')]
            want_name = {i for i in items if '.' in i}
            want_ext = {i for i in items if '.' not in i}
            print(f"Looking for: extensions={want_ext} OR filenames={want_name}")
        break

    print("\n--- File Contents ---")
    found = False

    for path in files:
        name = os.path.basename(path)
        ext = os.path.splitext(path)[1][1:].lower()

        if want_ext is None or ext in want_ext or name.lower() in want_name:
            found = True
            header = f"\n{name}:"
            print(header)
            out.append(header)

            try:
                with open(path, 'r', encoding='utf-8') as f:
                    text = f.read()
                print(text)
                out.append(text)
            except UnicodeDecodeError:
                msg = f"Error: Could not decode {name} with UTF-8. Skipping content display."
                print(msg)
                out.append(msg)
            except Exception as e:
                msg = f"Error reading {name}: {e}"
                print(msg)
                out.append(msg)

    if not found:
        msg = f"No files found matching: {query}"
        print(msg)
        out.append(msg)

    try:
        pyperclip.copy("\n".join(out))
        print("\nAll output copied to clipboard!")
    except Exception as e:
        print(f"\nFailed to copy to clipboard: {e}")

if __name__ == "__main__":
    main()
