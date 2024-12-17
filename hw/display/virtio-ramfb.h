#ifndef VIRTIO_RAMFB_H
#define VIRTIO_RAMFB_H

#include "hw/virtio/virtio-gpu-pci.h"
#include "qom/object.h"
#include "ui/surface.h"

struct QEMU_PACKED VirtIORAMFBCfg {
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

typedef struct VirtIORAMFBCfg VirtIORAMFBCfg;

/*
 * virtio-ramfb-base: This extends VirtioPCIProxy.
 */
#define TYPE_VIRTIO_RAMFB_BASE "virtio-ramfb-base"
OBJECT_DECLARE_TYPE(VirtIORAMFBBase, VirtIORAMFBBaseClass,
                    VIRTIO_RAMFB_BASE)

struct VirtIORAMFBBase {
    VirtIOPCIProxy parent_obj;

    VirtIOGPUBase *vgpu;

    DisplaySurface *ds;
    uint32_t width, height;
    struct VirtIORAMFBCfg cfg;

    uint32_t vram_size;
    uint32_t vram_size_mb; /* property */
    MemoryRegion vram;
    uint8_t *vram_ptr;
};

struct VirtIORAMFBBaseClass {
    VirtioPCIClass parent_class;

    DeviceReset parent_reset;
};

#endif /* VIRTIO_RAMFB_H */
