/*
 * Copyright (C) Red Hat, Inc. 2014
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *      Michal Privoznik <mprivozn@redhat.com>
 */

#include <config.h>
#include <stdlib.h>

#include "testutils.h"
#include "domain_capabilities.h"


#define VIR_FROM_THIS VIR_FROM_NONE

typedef int (*virDomainCapsFill)(virDomainCapsPtr domCaps,
                                 void *opaque);

#define SET_ALL_BITS(x) \
    memset(&(x.values), 0xff, sizeof(x.values))

static int ATTRIBUTE_SENTINEL
fillStringValues(virDomainCapsStringValuesPtr values, ...)
{
    int ret = 0;
    va_list list;
    const char *str;

    va_start(list, values);
    while ((str = va_arg(list, const char *))) {
        if (VIR_REALLOC_N(values->values, values->nvalues + 1) < 0 ||
            VIR_STRDUP(values->values[values->nvalues], str) < 0) {
            ret = -1;
            break;
        }
        values->nvalues++;
    }
    va_end(list);

    return ret;
}

static int
fillAllCaps(virDomainCapsPtr domCaps)
{
    virDomainCapsOSPtr os = &domCaps->os;
    virDomainCapsLoaderPtr loader = &os->loader;
    virDomainCapsDeviceDiskPtr disk = &domCaps->disk;
    virDomainCapsDeviceHostdevPtr hostdev = &domCaps->hostdev;
    domCaps->maxvcpus = 255;

    os->supported = true;

    loader->supported = true;
    SET_ALL_BITS(loader->type);
    SET_ALL_BITS(loader->readonly);
    if (fillStringValues(&loader->values,
                         "/foo/bar",
                         "/tmp/my_path",
                         NULL) < 0)
        return -1;

    disk->supported = true;
    SET_ALL_BITS(disk->diskDevice);
    SET_ALL_BITS(disk->bus);

    hostdev->supported = true;
    SET_ALL_BITS(hostdev->mode);
    SET_ALL_BITS(hostdev->startupPolicy);
    SET_ALL_BITS(hostdev->subsysType);
    SET_ALL_BITS(hostdev->capsType);
    SET_ALL_BITS(hostdev->pciBackend);
    return 0;
}


#if WITH_QEMU
# include "testutilsqemu.h"

static int
fillQemuCaps(virDomainCapsPtr domCaps,
             const char *name,
             virArch arch,
             const char *machine,
             virQEMUDriverConfigPtr cfg)
{
    int ret = -1;
    char *path = NULL;
    virQEMUCapsPtr qemuCaps = NULL;
    virDomainCapsLoaderPtr loader = &domCaps->os.loader;

    if (virAsprintf(&path, "%s/qemucapabilitiesdata/%s.%s.xml",
                    abs_srcdir, name, virArchToString(arch)) < 0 ||
        !(qemuCaps = qemuTestParseCapabilities(path)))
        goto cleanup;

    if (machine &&
        VIR_STRDUP(domCaps->machine,
                   virQEMUCapsGetCanonicalMachine(qemuCaps, machine)) < 0)
        goto cleanup;

    if (!domCaps->machine &&
        VIR_STRDUP(domCaps->machine,
                   virQEMUCapsGetDefaultMachine(qemuCaps)) < 0)
        goto cleanup;

    if (virQEMUCapsFillDomainCaps(domCaps, qemuCaps,
                                  cfg->loader, cfg->nloader) < 0)
        goto cleanup;

    /* The function above tries to query host's KVM & VFIO capabilities by
     * calling qemuHostdevHostSupportsPassthroughLegacy() and
     * qemuHostdevHostSupportsPassthroughVFIO() which, however, can't be
     * successfully mocked as they are not exposed as internal APIs. Therefore,
     * instead of mocking set the expected values here by hand. */
    VIR_DOMAIN_CAPS_ENUM_SET(domCaps->hostdev.pciBackend,
                             VIR_DOMAIN_HOSTDEV_PCI_BACKEND_DEFAULT,
                             VIR_DOMAIN_HOSTDEV_PCI_BACKEND_KVM,
                             VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO);

    /* As of f05b6a918e28 we are expecting to see OVMF_CODE.fd file which
     * may not exists everywhere. */
    while (loader->values.nvalues)
        VIR_FREE(loader->values.values[--loader->values.nvalues]);

    if (fillStringValues(&loader->values,
                         "/usr/share/AAVMF/AAVMF_CODE.fd",
                         "/usr/share/OVMF/OVMF_CODE.fd",
                         NULL) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virObjectUnref(qemuCaps);
    VIR_FREE(path);
    return ret;
}
#endif /* WITH_QEMU */


enum testCapsType {
    CAPS_NONE,
    CAPS_ALL,
    CAPS_QEMU,
};

struct testData {
    const char *name;
    const char *emulator;
    const char *machine;
    virArch arch;
    virDomainVirtType type;
    enum testCapsType capsType;
    const char *capsName;
    void *capsOpaque;
};

static int
test_virDomainCapsFormat(const void *opaque)
{
    const struct testData *data = opaque;
    virDomainCapsPtr domCaps = NULL;
    char *path = NULL;
    char *domCapsXML = NULL;
    int ret = -1;

    if (virAsprintf(&path, "%s/domaincapsschemadata/domaincaps-%s.xml",
                    abs_srcdir, data->name) < 0)
        goto cleanup;

    if (!(domCaps = virDomainCapsNew(data->emulator, data->machine, data->arch,
                                     data->type)))
        goto cleanup;

    switch (data->capsType) {
    case CAPS_NONE:
        break;

    case CAPS_ALL:
        if (fillAllCaps(domCaps) < 0)
            goto cleanup;
        break;

    case CAPS_QEMU:
#if WITH_QEMU
        if (fillQemuCaps(domCaps, data->capsName, data->arch, data->machine,
                         data->capsOpaque) < 0)
            goto cleanup;
#endif
        break;
    }

    if (!(domCapsXML = virDomainCapsFormat(domCaps)))
        goto cleanup;

    if (virtTestCompareToFile(domCapsXML, path) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    VIR_FREE(domCapsXML);
    VIR_FREE(path);
    virObjectUnref(domCaps);
    return ret;
}

static int
mymain(void)
{
    int ret = 0;

#if WITH_QEMU
    virQEMUDriverConfigPtr cfg = virQEMUDriverConfigNew(false);

    if (!cfg)
        return EXIT_FAILURE;
#endif

#define DO_TEST(Name, Emulator, Machine, Arch, Type, CapsType)          \
    do {                                                                \
        struct testData data = {                                        \
            .name = Name,                                               \
            .emulator = Emulator,                                       \
            .machine = Machine,                                         \
            .arch = Arch,                                               \
            .type = Type,                                               \
            .capsType = CapsType,                                       \
        };                                                              \
        if (virtTestRun(Name, test_virDomainCapsFormat, &data) < 0)     \
            ret = -1;                                                   \
    } while (0)

#define DO_TEST_QEMU(Name, CapsName, Emulator, Machine, Arch, Type)     \
    do {                                                                \
        struct testData data = {                                        \
            .name = Name,                                               \
            .emulator = Emulator,                                       \
            .machine = Machine,                                         \
            .arch = Arch,                                               \
            .type = Type,                                               \
            .capsType = CAPS_QEMU,                                      \
            .capsName = CapsName,                                       \
            .capsOpaque = cfg,                                          \
        };                                                              \
        if (virtTestRun(Name, test_virDomainCapsFormat, &data) < 0)     \
            ret = -1;                                                   \
    } while (0)

    DO_TEST("basic", "/bin/emulatorbin", "my-machine-type",
            VIR_ARCH_X86_64, VIR_DOMAIN_VIRT_UML, CAPS_NONE);
    DO_TEST("full", "/bin/emulatorbin", "my-machine-type",
            VIR_ARCH_X86_64, VIR_DOMAIN_VIRT_KVM, CAPS_ALL);

#if WITH_QEMU

    DO_TEST_QEMU("qemu_1.6.50-1", "caps_1.6.50-1",
                 "/usr/bin/qemu-system-x86_64", NULL,
                 VIR_ARCH_X86_64, VIR_DOMAIN_VIRT_KVM);

#endif /* WITH_QEMU */

    return ret;
}

VIRT_TEST_MAIN(mymain)
