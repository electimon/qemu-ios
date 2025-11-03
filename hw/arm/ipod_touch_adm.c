#include "hw/arm/ipod_touch_adm.h"
#include "hw/qdev-properties.h"
#include "hw/arm/ipod_touch_nand.h"
#include "qapi/error.h"

static uint16_t byte_swap_16(uint16_t v)
{
    return (v << 8) | (v >> 8);
}

static uint32_t byte_swap_32(uint32_t v)
{
    return (((v >> 24) & 0xFF) |
            ((v << 8) & 0xFF0000) |
            ((v >> 8) & 0x00FF00) |
            ((v << 24) & 0xFF000000));
}

static void set_bank(ITNandState *s, uint8_t activate_bank) {
    for(int bank = 0; bank < 8; bank++) {
        // clear bit, toggle if it is active
        s->fmctrl0 &= ~(1 << (bank + 1));
        if(bank == activate_bank) {
            s->fmctrl0 ^= 1 << (bank + 1);
        }
    }
}

static uint64_t ipod_touch_adm_read(void *opaque, hwaddr offset, unsigned size)
{
    //IPodTouchADMState *s = (IPodTouchADMState *)opaque;

    //fprintf(stderr, "s5l8900_adm_read(): offset = 0x%08x\n", offset);

    switch (offset) {
        case ADM_CTRL: // this seems to be the control register
            return 0x2; // this seems to indicate that the device is ready
        case ADM_CTRL2:
            return 0x10; // indicates that a upload event has been finished
        default:
            break;
    }

    return 0;
}

// After setting up nand_state->pages_to_read and nand_state->banks_to_read, call this to initiate the I/O
static void itadm_setup_multiple_page_io(IPodTouchADMState* s, uint16_t page_count)
{
    s->nand_state->reading_multiple_pages = true;
    s->nand_state->fmdnum = 0x800 * page_count;
    s->nand_state->cur_bank_reading = -1;
    
    uint8_t sbuf[12] = {};
    sbuf[10] = 0xFF;
    
    for (int i = 0; i < page_count; i++)
        address_space_rw(&s->downstream_as, s->data3_sec_addr + i * 0xC, MEMTXATTRS_UNSPECIFIED, sbuf, 0xC, 1);
}

static void ipod_touch_adm_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchADMState *s = (IPodTouchADMState *)opaque;

    //fprintf(stderr, "s5l8900_adm_write: offset = 0x%08x, val = 0x%08x\n", (unsigned)offset, (unsigned)value);

    switch(offset) {
        case ADM_CTRL:
            if(value == 0x3) {
                // some kind of start-up command?
                // write some bits to data2_sec_addr to indicate that the device is started
                uint32_t *data = (uint32_t *) malloc(sizeof(uint32_t));
                data[0] = 0x50;
                address_space_rw(&s->downstream_as, s->data2_sec_addr, MEMTXATTRS_UNSPECIFIED, (uint8_t *)data, 4, 1);

                // dunno, write some bytes to data4_sec_addr to indicate that the NAND banks are ready
                data = (uint32_t *) malloc(sizeof(uint32_t) * 8);
                for(int i = 0; i < 8; i++) {
                    data[i] = NAND_CHIP_ID;
                }
                
                address_space_rw(&s->downstream_as, s->data3_sec_addr, MEMTXATTRS_UNSPECIFIED, (uint8_t *)data, 8 * sizeof(uint32_t), 1);
            }
            break;
        case ADM_CTRL2:
        {
            if (value == 0x2)
            {
                uint32_t cmd;
                
                uint32_t buffer[20];
                address_space_read(&s->downstream_as, s->data2_sec_addr + 0x1104 + 0x24, MEMTXATTRS_UNSPECIFIED, buffer, 80);
                fprintf(stderr, "DUMPING BUFFER: ");
                for(int i = 0; i < 20; i++) {
                    fprintf(stderr, "0x%08x ", buffer[i]);
                }
                fprintf(stderr, "\n");
                
                uintptr_t my_addr = s->data2_sec_addr + 0x1104;
                address_space_read(&s->downstream_as, my_addr + 0x24, MEMTXATTRS_UNSPECIFIED, &cmd, 4);

                fprintf(stderr, "ADM processing command %d\n", cmd);
                switch (cmd)
                {
                    case 0x200:
                    {
                        // Read Multiple Pages (contiguous)
                        uint16_t page_count;
                        uint32_t page;
                        
                        address_space_read(&s->downstream_as, my_addr + 0x28, MEMTXATTRS_UNSPECIFIED, &page_count, 2);
                        address_space_read(&s->downstream_as, my_addr + 0x244, MEMTXATTRS_UNSPECIFIED, &page, 4);
                        
                        page_count = byte_swap_16(page_count);
                        page = byte_swap_32(page);
                        //fprintf(stderr, "starting with page %u\n", page);
                        
                        uint16_t ops = page_count / 8;
                        for (int op = 0; op < ops; op++)
                        {
                            for (int i = 0; i < 8; i++)
                            {
                                s->nand_state->pages_to_read[op * 8 + i] = page;
                                s->nand_state->banks_to_read[op * 8 + i] = i;
                            }
                            page++;
                        }
                        
                        itadm_setup_multiple_page_io(s, page_count);
                        break;
                    }
                    case 0x300:
                    {
                        // Read Pages (one or scattered)
                        uint16_t page_count;
                        
                        s->nand_state->reading_multiple_pages = false;
                        
                        address_space_read(&s->downstream_as, my_addr + 0x28, MEMTXATTRS_UNSPECIFIED, &page_count, 2);
                        page_count = (page_count >> 8) | (page_count << 8);
                        
                        if (page_count == 1)
                        {
                            // TODO: This can probably be refactored to re-use the logic to read multiple pages.
                            uint8_t bank;
                            uint32_t page;
                            
                            address_space_read(&s->downstream_as, my_addr + 0x44, MEMTXATTRS_UNSPECIFIED, &bank, 1);
                            address_space_read(&s->downstream_as, my_addr + 0x244, MEMTXATTRS_UNSPECIFIED, &page, 4);
                            
                            page = byte_swap_32(page);
                            
                            // set the bank, page, and operation.
                            set_bank(s->nand_state, bank);
                            memory_region_dispatch_write(&s->nand_state->iomem, NAND_FMDNUM, 0x800 - 1, MO_32, MEMTXATTRS_UNSPECIFIED);
                            memory_region_dispatch_write(&s->nand_state->iomem, NAND_FMADDR0, page << 16, MO_32, MEMTXATTRS_UNSPECIFIED);
                            memory_region_dispatch_write(&s->nand_state->iomem, NAND_FMADDR1, (page >> 16) & 0xFF, MO_32, MEMTXATTRS_UNSPECIFIED);
                            memory_region_dispatch_write(&s->nand_state->iomem, NAND_CMD, NAND_CMD_READ, MO_32, MEMTXATTRS_UNSPECIFIED);
                            
                            // write the spare of the page to the 3rd data section
                            nand_set_buffered_page(s->nand_state, page);
                            address_space_rw(&s->downstream_as, s->data3_sec_addr, MEMTXATTRS_UNSPECIFIED, (uint8_t*) s->nand_state->page_spare_buffer, NAND_BYTES_PER_SPARE, 1);
                        }
                        else if (page_count > 1)
                        {
                            // Scattered Mode
                            s->nand_state->reading_multiple_pages = true;
                            
                            for (int i = 0; i < page_count; i++)
                            {
                                uint8_t bank;
                                uint32_t page;
                                address_space_read(&s->downstream_as, my_addr + 0x244 + 4 * i, MEMTXATTRS_UNSPECIFIED, &page, 4);
                                
                                page = byte_swap_32(page);
                                
                                address_space_read(&s->downstream_as, my_addr + 0x44 + i, MEMTXATTRS_UNSPECIFIED, &bank, 1);
                                
                                s->nand_state->pages_to_read[i] = page;
                                s->nand_state->banks_to_read[i] = bank;
                            }
                            
                            itadm_setup_multiple_page_io(s, page_count);
                        }
                        
                        break;
                    }
                    case 0x400:
                    {
                        // Write Multiple Pages (contiguous)
                        uint16_t page_count;
                        uint32_t page;
                        
                        address_space_read(&s->downstream_as, my_addr + 0x28, MEMTXATTRS_UNSPECIFIED, &page_count, 2);
                        address_space_read(&s->downstream_as, my_addr + 0x244, MEMTXATTRS_UNSPECIFIED, &page, 4);
                        
                        page_count = byte_swap_16(page_count);
                        page = byte_swap_32(page);
                        fprintf(stderr, "write starting with page %u\n", page);
                        
                        uint16_t ops = page_count / 8;
                        for (int op = 0; op < ops; op++)
                        {
                            for (int i = 0; i < 8; i++)
                            {
                                s->nand_state->pages_to_read[op * 8 + i] = page;
                                s->nand_state->banks_to_read[op * 8 + i] = i;
                            }
                            page++;
                        }
                        
                        s->nand_state->is_writing = true;
                        s->nand_state->ignore_write_count = 0;
                        itadm_setup_multiple_page_io(s, page_count);
                        break;
                    }
                    case 0x500:
                    {
                        // Write Pages (one or scattered, but current code only supports one)
                        uint16_t page_count;
                        
                        s->nand_state->reading_multiple_pages = false;
                        
                        address_space_read(&s->downstream_as, my_addr + 0x28, MEMTXATTRS_UNSPECIFIED, &page_count, 2);
                        page_count = (page_count >> 8) | (page_count << 8);
                        
                        if (page_count != 1)
                        {
                            fprintf(stderr, "%s: WARNING: Scattered writes to more than one page (%d pages) are currently not supported.\n", __func__, (int) page_count);
                            break;
                        }
                        
                        uint8_t bank;
                        uint32_t page;
                        
                        address_space_read(&s->downstream_as, my_addr + 0x44, MEMTXATTRS_UNSPECIFIED, &bank, 1);
                        address_space_read(&s->downstream_as, my_addr + 0x244, MEMTXATTRS_UNSPECIFIED, &page, 4);
                        
                        page = byte_swap_32(page);
                        
                        // set the bank, page, and operation.
                        set_bank(s->nand_state, bank);
                        nand_set_buffered_page(s->nand_state, page);
                        s->nand_state->fmdnum = NAND_BYTES_PER_PAGE;
                        s->nand_state->is_writing = true;
                        s->nand_state->ignore_write_count = 0;
                        
                        fprintf(stderr, "ADM commencing single page write to page %u, bank %u, VPN %u\n", page, bank, page * 8 + bank);
                        break;
                    }
                    default:
                    {
                        fprintf(stderr, "%s: WARNING: Unsupported ADM command %d.\n", __func__, cmd);
                        break;
                    }
                }
                
                qemu_irq_raise(s->irq);
            }
            
            if ((value & 0x2) == 0)
                qemu_irq_lower(s->irq);
            
            break;
        }
        
        case ADM_CODE_SEC_ADDR:
            s->code_sec_addr = value;
            break;
        case ADM_DATA1_SEC_ADDR:
            s->data1_sec_addr = value;
            break;
        case ADM_DATA2_SEC_ADDR:
            s->data2_sec_addr = value;
            break;
        case ADM_DATA3_SEC_ADDR:
            s->data3_sec_addr = value;
            break;
        default:
            break;
    }
}

static const MemoryRegionOps ipod_touch_adm_ops = {
    .read = ipod_touch_adm_read,
    .write = ipod_touch_adm_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_adm_realize(DeviceState *dev, Error **errp)
{
    IPodTouchADMState *s = IPOD_TOUCH_ADM(dev);

    if (!s->downstream) {
        error_setg(errp, "ADM 'downstream' link not set");
        return;
    }

    address_space_init(&s->downstream_as, s->downstream, "adm-downstream");
}

static Property adm_properties[] = {
    DEFINE_PROP_LINK("downstream", IPodTouchADMState, downstream,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_END_OF_LIST(),
};

static void ipod_touch_adm_init(Object *obj)
{
    IPodTouchADMState *s = IPOD_TOUCH_ADM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_adm_ops, s, TYPE_IPOD_TOUCH_ADM, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void ipod_touch_adm_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = ipod_touch_adm_realize;
    device_class_set_props(dc, adm_properties);
}

static const TypeInfo ipod_touch_adm_type_info = {
    .name = TYPE_IPOD_TOUCH_ADM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchADMState),
    .instance_init = ipod_touch_adm_init,
    .class_init = ipod_touch_adm_class_init,
};

static void ipod_touch_adm_register_types(void)
{
    type_register_static(&ipod_touch_adm_type_info);
}

type_init(ipod_touch_adm_register_types)
