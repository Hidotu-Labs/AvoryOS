# Security Policy

## Supported Versions

| Version | Supported          | Known Issues                                           |
| ------- | ------------------ | ------------------------------------------------------ |
| 2.x     | :warning:          | Unbalanced CPU usage, user faults in certain programs, AMD GPU renderer not working |
| 1.x     | :x:                | End of support (PTY stopping, memory leaks)           |
| < 1.0   | :x:                | End of support                                         |

## Known Issues

### Version 2.x
- **Unbalanced CPU Usage**: Workload scheduling across SMP cores can become unbalanced under certain multitasking conditions, resulting in uneven core utilization.
- **User Faults in Some Programs**: Certain userland programs may trigger unexpected user-space faults / crashes due to incomplete syscall corner cases or edge behaviors.
- **AMD GPU Renderer Not Working**: LinuxKPI AMDGPU hardware 3D acceleration via radeonsi is currently non-functional / broken. Use VirtIO-GPU or software rendering (pixman / llvmpipe) for stable graphics sessions.

### Version 1.x (Legacy)
- **PTY Stopping Issue**: PTY (pseudo-terminal) processes may stop unexpectedly under certain conditions. Workaround: restart the terminal session.
- **Memory Leak**: Approximately 300KB memory leak detected during normal system operation.

## Reporting a Vulnerability

To report a security vulnerability in AvoryOS:

1. **Do not** open a public GitHub issue
2. Email security concerns to the maintainers with:
   - Description of the vulnerability
   - Steps to reproduce (if applicable)
   - Affected version(s)
   - Suggested fix (if you have one)

We will acknowledge your report within 7 days and provide updates on progress every 2 weeks until resolved or declined.

## Security Expectations

AvoryOS is an experimental operating system. Users should understand:
- This is not a production-ready OS
- Security patches may not be available for all issues
- Users are encouraged to report issues to help improve stability
- Kernel and core components may have unidentified security issues
