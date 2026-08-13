#!/usr/bin/env python3

import argparse
import json
import os
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


# ANSI colors
RESET = "\033[0m"
RED = "\033[31m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
CYAN = "\033[36m"
BOLD = "\033[1m"


def color(text, ansi):
    return f"{ansi}{text}{RESET}"


def print_diagnostic(text):
    for line in text.splitlines(keepends=True):
        if re.search(r"\berror:", line):
            print(color(line, RED), end="")
        elif re.search(r"\bwarning:", line):
            print(color(line, YELLOW), end="")
        elif re.search(r"\bnote:", line):
            print(color(line, CYAN), end="")
        else:
            print(line, end="")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run clang-tidy in parallel."
    )

    parser.add_argument(
        "-p",
        "--build-dir",
        default="build",
        help="Build directory containing compile_commands.json.",
    )

    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=os.cpu_count() or 1,
        help="Number of clang-tidy processes to run in parallel.",
    )

    parser.add_argument(
        "--only-diff",
        action="store_true",
        help="Only lint files changed compared with origin/master.",
    )

    parser.add_argument(
        "--fix",
        action="store_true",
        help="Apply clang-tidy fixes.",
    )

    parser.add_argument(
        "--clang-tidy",
        default="clang-tidy",
        help="Path to clang-tidy executable.",
    )

    return parser.parse_args()


def load_files(build_dir):
    path = Path(build_dir) / "compile_commands.json"

    if not path.exists():
        raise FileNotFoundError(
            f"{path} not found. Configure the project first."
        )

    with path.open() as file:
        database = json.load(file)

    project_root = Path(__file__).resolve().parent.parent

    source_dirs = {
        project_root / "src",
        project_root / "tests",
    }

    return sorted(
        {
            Path(entry["file"]).resolve()
            for entry in database
            if Path(entry["file"]).suffix in {".cc", ".cpp", ".cxx"}
            and any(
                source_dir in Path(entry["file"]).resolve().parents
                for source_dir in source_dirs
            )
        }
    )


def get_changed_files():
    result = subprocess.run(
        [
            "git",
            "--no-pager",
            "diff",
            "--name-only",
            "@{upstream}",
        ],
        check=True,
        capture_output=True,
        text=True,
    )

    return {
        Path(path).resolve()
        for path in result.stdout.splitlines()
        if path
    }


def run_clang_tidy(file, build_dir, clang_tidy, fix):
    command = [
        clang_tidy,
        "-quiet",
        f"-p={build_dir}",
        str(file),
    ]

    if fix:
        command.insert(2, "-fix")

    result = subprocess.run(
        command,
        capture_output=True,
        text=True,
    )

    return file, result.returncode, result.stdout, result.stderr


def main():
    args = parse_args()

    files = load_files(args.build_dir)

    if args.only_diff:
        changed = get_changed_files()
        files = [file for file in files if file in changed]

    if not files:
        print("No files to lint.")
        return 0

    print(
        f"{BOLD}Running clang-tidy on {len(files)} files "
    )

    failed = []

    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(
                run_clang_tidy,
                file,
                args.build_dir,
                args.clang_tidy,
                args.fix,
            ): file
            for file in files
        }

        for future in as_completed(futures):
            file, returncode, stdout, stderr = future.result()

            if returncode != 0:
                print(f"\n{color('[FAIL]', RED)} {color(str(file), BOLD + CYAN)}")
            else:
                print(f"\n{color('[OK]', GREEN)} {color(str(file), BOLD + CYAN)}")

            if stdout:
                print_diagnostic(stdout)

            if stderr:
                print_diagnostic(stderr)

            if returncode != 0:
                failed.append(file)

    if failed:
        print(f"\n{color('clang-tidy failed for:', RED + BOLD)}")
        for file in failed:
            print(f"  {color(str(file), RED)}")
        return 1

    print(f"\n{color('clang-tidy passed.', GREEN + BOLD)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())