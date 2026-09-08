# Welcome to fscomp!

```text
   __                                
  / _|___  ___ ___  _ __ ___  _ __   
 | |_/ __|/ __/ _ \| '_ ` _ \| '_ \ 
 |  _\__ \ (_| (_) | | | | | | |_) |
 |_| |___/\___\___/|_| |_| |_| .__/ 
                             |_|     
```

fscomp is a simple tool for transparent filesystem compression on macOS (with support for Linux filesystems planned for the future). 

macOS supports transparent APFS compression natively. Files take up less disk space, but programs can still open and read them normally. Under the hood, `afsctool` handles the actual compression, but it does not keep an index, so it has to re-read every file whenever you run it. `fscomp` wraps it with a lightweight SQLite database to track file sizes, timestamps, and hashes so subsequent runs finish in seconds without re-doing work.

# Installation & Setup

#### Homebrew (recommended):
This repo doubles as a Homebrew tap. Install with:

```bash
brew tap ltmerletti/fscomp https://github.com/ltmerletti/fscomp
brew install fscomp
```

#### Prerequisites (manual build):
- macOS 11 or newer
- `afsctool` (install with `brew install afsctool`)
- Xcode command line tools (install with `xcode-select --install`)

#### Build and Install:
To build and install fscomp, just run:

```bash
make
make install
```

This installs `fscomp` to `~/.local/bin` (just make sure that directory is in your `PATH`).

If you want to update and reinstall:

```bash
make upgrade
```

Or to run the test suites:

```bash
make test
make asan
```

# Usage

#### Tracking Folders:
To track folders and update them together with one command, you can do the following:

```bash
# Add folders to track
fscomp add ~/Documents ~/Developer/logs

# Check what is tracked
fscomp list

# Compress all tracked folders
fscomp update

# Remove a folder from tracking
fscomp remove ~/Developer/logs
```

#### Running One-Off Commands:
If you want to run fscomp directly on a folder without adding it to the tracked list, you can do:

```bash
# Preview what would compress without writing to disk
fscomp scan ~/Documents

# Compress a directory directly
fscomp run ~/Documents
```

If you want to run it without verbose output or file notices:

```bash
# Print summary only without live progress or file notices
fscomp run -q ~/Documents

# Run silently with zero standard output
fscomp run -s ~/Documents
```

Set `worker_count` in `~/.config/fscomp/config.json` to process files in parallel:

```json
{
  "worker_count": 4
}
```

Use `--jobs <count>` to override that setting for one command.

And to check your space savings:

```bash
# View total space saved
fscomp status

# View savings breakdown by file extension
fscomp report
```

# How It Works

#### How files are skipped:
- Skips formats that are already compressed (like zip, mp4, png, etc.)
- Checks file size and modification time against SQLite first, skipping the file with zero disk reads if neither changed
- Verifies SHA-256 before invoking `afsctool` if timestamps or sizes change
- Reads `st_blocks * 512` to track actual disk blocks saved

Please note that you should not run fscomp on `/` (the root macOS volume is sealed, read-only, and already compressed). Point it at source code repositories, document folders, logs, or build caches instead.
