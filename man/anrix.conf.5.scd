anrix.conf(5)

# NAME

anrix.conf - ANRIX-Linux build and repository configuration

# DESCRIPTION

*pi* reads key-value assignments from */etc/anrix/anrix.conf*. The root can be
redirected with *--root* for isolated administration.

# OPTIONS

*USE*="wayland -X clang"

: Enables or disables package feature flags. Per-package files in
*/etc/anrix/package.use/* override site policy.

*CFLAGS*, *CXXFLAGS*, *LDFLAGS*, *MAKEFLAGS*
: Compiler and make flags passed to sandboxed builds.

*CCACHE*=0|1
: Enables the shared compiler cache when set to *1*.

*MAX_JOBS*=N
: Caps parallel build jobs.

# FILES

*/etc/anrix/anrix.conf*
: Global configuration.

*/etc/anrix/package.use/<pkg>*
: Package-specific USE flags.

*/etc/anrix/mirrors.conf*
: Repository mirror URLs.
