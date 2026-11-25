#!/usr/bin/env python3
import os
import sys
import subprocess

# Extensions to look for
EXTENSIONS = ('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp')

def get_files_to_format(src_dir):
    """Recursively collects all C/C++ source files, following symlinks."""
    matched_files = []
    # os.walk(..., followlinks=True) behaves like `find -L`
    for root, _, files in os.walk(src_dir, followlinks=True):
        for file in files:
            if file.lower().endswith(EXTENSIONS):
                matched_files.append(os.path.join(root, file))
    return matched_files

def find_comment_index(line):
    """Finds the index of the first '//' comment outside string or char literals."""
    in_string = False
    in_char = False
    escaped = False
    i = 0
    n = len(line)
    while i < n - 1:
        ch = line[i]
        if escaped:
            escaped = False
            i += 1
            continue
        if in_string:
            if ch == '\\':
                escaped = True
            elif ch == '"':
                in_string = False
        elif in_char:
            if ch == '\\':
                escaped = True
            elif ch == '\'':
                in_char = False
        else:
            if ch == '"':
                in_string = True
            elif ch == '\'':
                in_char = True
            elif ch == '/' and line[i + 1] == '/':
                return i
        i += 1
    return -1


def transform_comments(file_path):
    """Applies the comment-style conversions line by line."""
    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
            lines = f.readlines()
    except Exception as e:
        print(f"ERROR: Could not read {file_path}: {e}", file=sys.stderr)
        return False

    new_lines = []
    for line in lines:
        # Find the first occurrence of '//' outside string/char literals
        comment_idx = find_comment_index(line)

        if comment_idx != -1:
            code_part = line[:comment_idx]
            comment_part = line[comment_idx:]

            # Step A: Globally swap ///< to //< in comment portion only
            if '///<' in comment_part:
                comment_part = comment_part.replace('///<', '//<')

            code_before = code_part.strip()
            if code_before:
                # Step B: Inline comment on a line with code
                if comment_part.startswith('///') and not comment_part.startswith('///<'):
                    comment_part = '//<' + comment_part[3:]
                elif comment_part.startswith('//') and not comment_part.startswith('//<'):
                    comment_part = '//<' + comment_part[2:]
            else:
                # Step C: Pure comment line (only whitespace before '//')
                if comment_part.startswith('//') and not comment_part.startswith('///') and not comment_part.startswith('//<'):
                    comment_part = '///' + comment_part[2:]

            line = code_part + comment_part

        new_lines.append(line)

    try:
        with open(file_path, 'w', encoding='utf-8') as f:
            f.writelines(new_lines)
        return True
    except Exception as e:
        print(f"ERROR: Could not write {file_path}: {e}", file=sys.stderr)
        return False

def main():
    # Target directory is the folder where this script is saved
    script_dir = os.path.dirname(os.path.abspath(__file__))
    
    # Verify clang-format exists in system path
    try:
        subprocess.run(['clang-format', '--version'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("ERROR: clang-format not found in PATH", file=sys.stderr)
        sys.exit(2)

    if not os.path.isdir(script_dir):
        print(f"ERROR: src directory not found: {script_dir}", file=sys.stderr)
        sys.exit(2)

    files = get_files_to_format(script_dir)
    total_files = len(files)
    
    print(f"Found {total_files} files to format.")
    if total_files == 0:
        sys.exit(0)

    formatted = 0
    failures = 0

    for f in files:
        if not os.access(f, os.R_OK):
            print(f"SKIP (unreadable): {f}", file=sys.stderr)
            failures += 1
            continue

        # Run clang-format command
        try:
            result = subprocess.run(
                ['clang-format', '--verbose', '-i', f],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )
            if result.returncode == 0:
                formatted += 1
            else:
                print(f"FAILED: clang-format on: {f}\n{result.stderr}", file=sys.stderr)
                failures += 1
                continue
        except Exception as e:
            print(f"FAILED: Execution error on {f}: {e}", file=sys.stderr)
            failures += 1
            continue

        # Run our custom comment parser
        if not transform_comments(f):
            failures += 1

    print(f"Result: formatted={formatted}, failed={failures}, total={total_files}")
    
    if failures > 0:
        sys.exit(1)
    sys.exit(0)

if __name__ == '__main__':
    main()


