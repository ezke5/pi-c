PKGBUILD-anrix(5)

# NAME

PKGBUILD-anrix - package build metadata supported by pi

# DESCRIPTION

A PKGBUILD is executed in the namespace sandbox. Standard metadata includes
*pkgname*, *pkgver*, *pkgrel*, *pkgdesc*, *url*, *license*, *arch*, *source*,
*sha256sums*, *depends*, and *makedepends*.

# DEPENDENCIES

Dependencies may use conditional syntax such as *wayland?wayland-protocols*
or *!systemd?eudev*. Version constraints use *pkg>=1.2* and *pkg=2.0*.

# FUNCTIONS

*prepare()*, *build()*, and *package()* populate the staged package directory.
Split packages use *package_<subpkg>()*. An optional *.install* file may
define *pre_install()*, *post_install()*, *pre_upgrade()*, *post_upgrade()*,
*pre_remove()*, and *post_remove()*.
