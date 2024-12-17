#include "qemu/osdep.h"
#include "sysemu/sysemu.h"
#include "hw/pci/pci.h"
#include "ui/console.h"
#include "hw/qdev-properties.h"
#include "hw/display/bochs-vbe.h" /* for limits */
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/loader.h"
#include "hw/display/virtio-ramfb.h"
#include "qemu/units.h"

static DisplaySurface *ramfb_create_display_surface(int width, int height,
                                                    pixman_format_code_t format,
                                                    hwaddr stride,
                                                    void *data, hwaddr maxsize)
{
    DisplaySurface *surface;
    hwaddr size, mapsize, linesize;

    if (width < 16 || width > VBE_DISPI_MAX_XRES ||
        height < 16 || height > VBE_DISPI_MAX_YRES ||
        format == 0 /* unknown format */)
        return NULL;

    linesize = width * PIXMAN_FORMAT_BPP(format) / 8;
    if (stride == 0) {
        stride = linesize;
    }

    mapsize = size = stride * (height - 1) + linesize;
    if (mapsize > maxsize) {
        return NULL;
    }

    surface = qemu_create_displaysurface_from(width, height,
                                              format, stride, data);

    return surface;
}

static void ramfb_fw_cfg_write(void *dev, off_t offset, size_t len)
{
    VirtIORAMFBBase *s = dev;
    DisplaySurface *surface;
    uint32_t fourcc, format, width, height;
    hwaddr stride;

    width  = be32_to_cpu(s->cfg.width);
    height = be32_to_cpu(s->cfg.height);
    stride = be32_to_cpu(s->cfg.stride);
    fourcc = be32_to_cpu(s->cfg.fourcc);
    format = qemu_drm_format_to_pixman(fourcc);

    surface = ramfb_create_display_surface(width, height,
                                           format, stride,
                                           s->vram_ptr, s->vram_size);
    if (!surface) {
        return;
    }

    s->width = width;
    s->height = height;
    qemu_free_displaysurface(s->ds);
    s->ds = surface;
}

static void ramfb_display_update(QemuConsole *con, VirtIORAMFBBase *s)
{
    if (!s->width || !s->height) {
        return;
    }

    if (s->ds) {
        dpy_gfx_replace_surface(con, s->ds);
        s->ds = NULL;
    }

    /* simple full screen update */
    dpy_gfx_update_full(con);
}

static int ramfb_post_load(void *opaque, int version_id)
{
    ramfb_fw_cfg_write(opaque, 0, 0);
    return 0;
}

const VMStateDescription virtio_ramfb_vmstate = {
    .name = "virtio-ramfb",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ramfb_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER_UNSAFE(cfg, VirtIORAMFBBase, 0, sizeof(VirtIORAMFBCfg)),
        VMSTATE_END_OF_LIST()
    }
};

static bool ramfb_setup(VirtIORAMFBBase *s, Error **errp)
{
    FWCfgState *fw_cfg = fw_cfg_find();

    if (!fw_cfg || !fw_cfg->dma_enabled) {
        error_setg(errp, "ramfb device requires fw_cfg with DMA");
        return false;
    }

    rom_add_vga("vgabios-ramfb.bin");
    fw_cfg_add_file_callback(fw_cfg, "etc/virtio-ramfb",
                             NULL, ramfb_fw_cfg_write, s,
                             &s->cfg, sizeof(s->cfg), false);
    return true;
}

static int virtio_ramfb_get_flags(void *opaque)
{
    VirtIORAMFBBase *vramfb = opaque;
    VirtIOGPUBase *g = vramfb->vgpu;

    if (g->hw_ops->get_flags) {
        return g->hw_ops->get_flags(g);
    } else {
        return 0;
    }
}

static void virtio_ramfb_invalidate_display(void *opaque)
{
    VirtIORAMFBBase *vramfb = opaque;
    VirtIOGPUBase *g = vramfb->vgpu;

    if (g->enable) {
        g->hw_ops->invalidate(g);
    }
}

static void virtio_ramfb_text_update(void *opaque, console_ch_t *chardata)
{
    VirtIORAMFBBase *vramfb = opaque;
    VirtIOGPUBase *g = vramfb->vgpu;

    if (g->hw_ops->text_update) {
        g->hw_ops->text_update(g, chardata);
    }
}

static void virtio_ramfb_update_display(void *opaque)
{
    VirtIORAMFBBase *vramfb = opaque;
    VirtIOGPUBase *g = vramfb->vgpu;

    if (g->enable) {
        g->hw_ops->gfx_update(g);
    } else {
        ramfb_display_update(g->scanout[0].con, vramfb);
    }
}

static void virtio_ramfb_ui_info(void *opaque, uint32_t idx, QemuUIInfo *info)
{
    VirtIORAMFBBase *vramfb = opaque;
    VirtIOGPUBase *g = vramfb->vgpu;

    if (g->hw_ops->ui_info) {
        g->hw_ops->ui_info(g, idx, info);
    }
}

static void virtio_ramfb_gl_block(void *opaque, bool block)
{
    VirtIORAMFBBase *vramfb = opaque;
    VirtIOGPUBase *g = vramfb->vgpu;

    if (g->hw_ops->gl_block) {
        g->hw_ops->gl_block(g, block);
    }
}

static const GraphicHwOps virtio_ramfb_ops = {
    .get_flags = virtio_ramfb_get_flags,
    .invalidate = virtio_ramfb_invalidate_display,
    .gfx_update = virtio_ramfb_update_display,
    .text_update = virtio_ramfb_text_update,
    .ui_info = virtio_ramfb_ui_info,
    .gl_block = virtio_ramfb_gl_block,
};

/* RAMFB device wrapper around PCI device around virtio GPU */
static void virtio_ramfb_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VirtIORAMFBBase *s = VIRTIO_RAMFB_BASE(vpci_dev);
    VirtIOGPUBase *g = s->vgpu;
    int i;

    /* init ramfb */
    if (!ramfb_setup(s, errp)) {
        return;
    }

    /* init vram */
    s->vram_size_mb = pow2ceil(s->vram_size_mb);
    s->vram_size = s->vram_size_mb * MiB;
    if (!memory_region_init_ram(&s->vram, OBJECT(vpci_dev), "ramfb.vram",
                                s->vram_size, errp)) {
        return;
    }
    s->vram_ptr = memory_region_get_ram_ptr(&s->vram);
    pci_register_bar(&vpci_dev->pci_dev, 0,
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vram);

    /* init virtio bits */
    virtio_pci_force_virtio_1(vpci_dev);
    if (!qdev_realize(DEVICE(g), BUS(&vpci_dev->bus), errp)) {
        return;
    }
    graphic_console_set_hwops(g->scanout[0].con, &virtio_ramfb_ops, s);

    for (i = 0; i < g->conf.max_outputs; i++) {
        object_property_set_link(OBJECT(g->scanout[i].con), "device",
                                 OBJECT(vpci_dev), &error_abort);
    }
}

static void virtio_ramfb_reset(DeviceState *dev)
{
    VirtIORAMFBBaseClass *klass = VIRTIO_RAMFB_BASE_GET_CLASS(dev);

    /* reset virtio-gpu */
    klass->parent_reset(dev);
}

static Property virtio_ramfb_base_properties[] = {
    DEFINE_VIRTIO_GPU_PCI_PROPERTIES(VirtIOPCIProxy),
    DEFINE_PROP_UINT32("vgamem_mb", VirtIORAMFBBase, vram_size_mb, 8),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_ramfb_base_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    VirtIORAMFBBaseClass *v = VIRTIO_RAMFB_BASE_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_props(dc, virtio_ramfb_base_properties);
    dc->vmsd = &virtio_ramfb_vmstate;
    dc->hotpluggable = false;
    device_class_set_parent_reset(dc, virtio_ramfb_reset,
                                  &v->parent_reset);

    k->realize = virtio_ramfb_realize;
    pcidev_k->class_id = PCI_CLASS_DISPLAY_OTHER;
}

static const TypeInfo virtio_ramfb_base_info = {
    .name          = TYPE_VIRTIO_RAMFB_BASE,
    .parent        = TYPE_VIRTIO_PCI,
    .instance_size = sizeof(VirtIORAMFBBase),
    .class_size    = sizeof(VirtIORAMFBBaseClass),
    .class_init    = virtio_ramfb_base_class_init,
    .abstract      = true,
};

#define TYPE_VIRTIO_RAMFB "virtio-ramfb"

typedef struct VirtIORAMFB VirtIORAMFB;
DECLARE_INSTANCE_CHECKER(VirtIORAMFB, VIRTIO_RAMFB,
                         TYPE_VIRTIO_RAMFB)

struct VirtIORAMFB {
    VirtIORAMFBBase parent_obj;

    VirtIOGPU     vdev;
};

static void virtio_ramfb_inst_initfn(Object *obj)
{
    VirtIORAMFB *dev = VIRTIO_RAMFB(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_GPU);
    VIRTIO_RAMFB_BASE(dev)->vgpu = VIRTIO_GPU_BASE(&dev->vdev);
}

static VirtioPCIDeviceTypeInfo virtio_ramfb_info = {
    .generic_name  = TYPE_VIRTIO_RAMFB,
    .parent        = TYPE_VIRTIO_RAMFB_BASE,
    .instance_size = sizeof(VirtIORAMFB),
    .instance_init = virtio_ramfb_inst_initfn,
};

static void virtio_ramfb_register_types(void)
{
    type_register_static(&virtio_ramfb_base_info);
    virtio_pci_types_register(&virtio_ramfb_info);
}

type_init(virtio_ramfb_register_types)
