# EOPSY Lab 5: File System

A simple filesystem stored inside a single disk file, with commands to upload, list, remove and download files and an interactive terminal menu.

Lab 5 of the Operating Systems (EOPSY) course at Warsaw University of Technology.

## Build and run

Requires Linux with gcc and make.

```bash
make
./fs_tool mydisk                          # interactive menu
./fs_tool mydisk upload notes.txt
./fs_tool mydisk ls
./fs_tool mydisk download notes.txt out.txt
./fs_tool mydisk rm notes.txt
```

The disk file is created on first use.
