# Legal Notes

## Windows ABI Compatibility

Brook implements a Windows ABI compatibility layer ("bring your own DLLs" model,
similar to Wine) to allow running Windows PE/COFF executables. This implementation:

- Is written entirely from scratch with no reference to proprietary Microsoft source code
- References only publicly available documentation and open-source projects:
  - Microsoft public documentation (MSDN / learn.microsoft.com)
  - *Windows Internals* by Mark Russinovich et al. (public book)
  - [ReactOS](https://reactos.org/) — open source, clean-room Windows-compatible OS
  - [Wine](https://www.winehq.org/) — open source Windows compatibility layer
  - Published academic research on NT internals

The author is employed by Microsoft. This project is a personal hobby project
developed entirely outside of work hours, on personal equipment, referencing
only the public sources listed above. No proprietary or confidential Microsoft
information has been used.
