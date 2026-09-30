#include "../psxrecomp/runtime/src/cdrom.c"
/* Exercise the actual cadence/IRQ implementation without linking the rest of
 * the emulator. Unused CD device sections are discarded by the test target. */
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "failed at line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)
static int dma_active, queries;
uint64_t psx_cycle_count;
int dma_cdrom_transfer_active(void) { return dma_active; }
static CDROMDataReadRange policy(int lba) {
    queries++;
    return lba >= 100 && lba < 104
        ? (CDROMDataReadRange){100,104,32}
        : (CDROMDataReadRange){lba,lba+1,0};
}
int main(void) {
    cdrom_set_data_read_policy(policy);
    read_min=0;read_sec=3;read_sect=25; /* LBA 100 */
    mode_reg=0xA0;
    CHECK(apply_read_speed(225792)==VBLANK_CYCLES_NTSC/32);
    CHECK(queries==1);
    cdrom_set_data_read_policy_enabled(0);
    CHECK(cdrom_data_read_policy_available());
    CHECK(!cdrom_data_read_policy_enabled());
    CHECK(apply_read_speed(225792)==225792);
    cdrom_set_data_read_policy_enabled(1);
    CHECK(apply_read_speed(225792)==VBLANK_CYCLES_NTSC/32);
    CHECK(queries==2); /* reclassified after toggling */
    read_sect=28;
    CHECK(apply_read_speed(225792)==VBLANK_CYCLES_NTSC/32);
    CHECK(queries==2); /* same immutable resource, no repeated FAT scan */
    irq_flag=1; dma_active=0;
    CHECK(bounded_read_consumer_blocked()==1); /* no sector overwrite */
    reading=1; read_delay=123; irq_enable=0;
    CHECK(cdrom_cycles_to_irq(4)==UINT32_MAX); /* blocked producer is not an event */
    dma_active=1;
    CHECK(bounded_read_consumer_blocked()==0); /* multi-sector DMA drains */
    CHECK(cdrom_cycles_to_irq(4)==123);
    pending_dataready=1;
    CHECK(bounded_read_consumer_blocked()==1); /* retain pending INT1 */
    pending_dataready=0;
    mode_reg=0xE0;
    CHECK(apply_read_speed(225792)==225792); /* XA */
    mode_reg=0xA8;
    CHECK(apply_read_speed(225792)==225792); /* filter */
    mode_reg=0xA0;xa_stream_active=1;
    CHECK(apply_read_speed(225792)==225792);
    xa_stream_active=0;read_sect=29;
    CHECK(apply_read_speed(225792)==225792); /* next asset excluded */
    CHECK(queries==3);
    CHECK(bounded_read_consumer_blocked()==0);
    puts("CD policy cadence/IRQ/media guards: PASS");
}
