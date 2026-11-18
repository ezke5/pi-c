// SPDX-License-Identifier: GPL-3.0-or-later
#define _DEFAULT_SOURCE
#include "build.h"
#include "net.h"
#include "pi_archive.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void write_file(const char *path, const char *content)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    assert(fputs(content, file) >= 0);
    assert(fclose(file) == 0);
}

int main(void)
{
    char root[128];
    char port[256];
    char pkgbuild[256];
    char package[256];
    char archive[256];
    char manifest[256];
    char extracted[256];
    char path[512];
    struct stat status;
    int result;

    assert(pi_net_clone("not-a-git-url", "/tmp/never-cloned") == EINVAL);

    assert(snprintf(root, sizeof(root), "/tmp/pi-pkgbuild-%ld-%ld", (long)getpid(),
                    (long)time(NULL)) > 0);
    assert(snprintf(port, sizeof(port), "%s/port", root) > 0);
    assert(mkdir(root, 0755) == 0);
    assert(mkdir(port, 0755) == 0);
    assert(snprintf(pkgbuild, sizeof(pkgbuild), "%s/PKGBUILD", port) > 0);
    write_file(pkgbuild,
               "pkgname=mockpkg\npkgver=1.0\npkgrel=1\npkgdesc='mock package'\n"
               "arch=('x86_64')\nlicense=('MIT')\nsource=()\nsha256sums=()\n"
               "package() { mkdir -p \"$pkgdir/usr/share/mock\"; "
               "echo built > \"$pkgdir/usr/share/mock/file\"; }\n");
    assert(snprintf(path, sizeof(path), "%s/etc", root) > 0);
    assert(mkdir(path, 0755) == 0);
    assert(snprintf(path, sizeof(path), "%s/etc/anrix", root) > 0);
    assert(mkdir(path, 0755) == 0);
    assert(snprintf(path, sizeof(path), "%s/etc/anrix/anrix.conf", root) > 0);
    write_file(path, "USE=test\nMAX_JOBS=1\n");

    result = pi_build_run(&(struct pi_build_spec){
        .root = root, .pkgbuild_path = port, .network = false, .nodeps = true});
    if (result == EPERM || result == EACCES || result == ENOSYS) {
        return 0;
    }
    assert(result == 0);
    assert(snprintf(package, sizeof(package), "%s/var/cache/pi/pkg/mockpkg/.PKGINFO", root) > 0);
    assert(stat(package, &status) == 0 && S_ISREG(status.st_mode));
    assert(snprintf(archive, sizeof(archive),
                    "%s/var/cache/pi/binpkgs/mockpkg-1.0-1-x86_64.pkg.tar.zst", root) > 0);
    assert(snprintf(manifest, sizeof(manifest), "%s.manifest", archive) > 0);
    assert(stat(manifest, &status) == 0 && S_ISREG(status.st_mode));
    assert(snprintf(extracted, sizeof(extracted), "%s/extracted", root) > 0);
    result = pi_archive_extract(archive, extracted);
    assert(result == 0);
    assert(snprintf(path, sizeof(path), "%s/.PKGINFO", extracted) > 0);
    assert(stat(path, &status) == 0 && S_ISREG(status.st_mode));
    assert(snprintf(path, sizeof(path), "%s/.MANIFEST", extracted) > 0);
    assert(stat(path, &status) == 0 && S_ISREG(status.st_mode));
    return 0;
}
