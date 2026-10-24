/* The Bootlin binutils predates the SFrame format in Axon's glibc startup
 * objects. Drop optional SFrame metadata before the linker tries to merge it.
 * Keep .eh_frame and .eh_frame_hdr for normal unwinding. */
SECTIONS
{
    /DISCARD/ : { *(.sframe) }
}
INSERT AFTER .text;
