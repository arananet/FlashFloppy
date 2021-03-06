/*
 * scp.c
 * 
 * SuperCard Pro (SCP) flux files.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

struct scp_header {
    uint8_t sig[3]; /* 'SCP' */
    uint8_t version;
    uint8_t disk_type;
    uint8_t nr_revs;
    uint8_t start_trk;
    uint8_t end_trk;
    uint8_t flags;
    uint8_t bc_enc;
    uint8_t heads;
    uint8_t rsvd;
    uint32_t csum;
};

struct trk_header {
    uint8_t sig[3]; /* 'TRK' */
    uint8_t track;
    struct {
        uint32_t duration;
        uint32_t nr_flux;
        uint32_t dat_off;
    } rev[5];
};

#define FF_MHZ (SYSCLK_MHZ*16u)
#define SCP_MHZ 40u

static bool_t scp_open(struct image *im)
{
    struct scp_header header;

    F_read(&im->fp, &header, sizeof(header), NULL);
    if (strncmp((char *)header.sig, "SCP", 3)) {
        printk("Not a SCP file\n");
        return FALSE;
    }

    if (header.nr_revs == 0) {
        printk("Invalid revolution count (%u)\n", header.nr_revs);
        return FALSE;
    }

    if (header.bc_enc != 0 && header.bc_enc != 16) {
        printk("Unsupported bit cell time width (%u)\n", header.bc_enc);
        return FALSE;
    }

    im->scp.nr_revs = header.nr_revs;
    im->nr_tracks = header.end_trk - header.start_trk + 1;

    return TRUE;
}

static bool_t scp_seek_track(
    struct image *im, uint16_t track, stk_time_t *start_pos)
{
    struct image_buf *rd = &im->bufs.read_data;
    uint32_t sys_ticks = start_pos ? *start_pos : 0;
    struct trk_header header;
    uint32_t hdr_offset, i, j, nr_flux;

    /* TODO: Fake out unformatted tracks. */
    track = min_t(uint16_t, track, im->nr_tracks-1);

    hdr_offset = 0x10 + track*4;
    F_lseek(&im->fp, hdr_offset);
    F_read(&im->fp, &hdr_offset, 4, NULL);

    hdr_offset = le32toh(hdr_offset);
    F_lseek(&im->fp, hdr_offset);
    F_read(&im->fp, &header, sizeof(header), NULL);

    if (strncmp((char *)header.sig, "TRK", 3) || header.track != track)
        return TRUE;

    for (i = 0; i < ARRAY_SIZE(im->scp.rev); i++) {
        j = i % im->scp.nr_revs;
        im->scp.rev[i].dat_off = hdr_offset + le32toh(header.rev[j].dat_off);
        im->scp.rev[i].nr_dat = le32toh(header.rev[j].nr_flux);
    }

    im->scp.pf_rev = im->scp.ld_rev = 0;
    im->ticks_since_flux = 0;
    im->cur_track = track;

    im->cur_ticks = sys_ticks * 16;

    nr_flux = im->scp.rev[0].nr_dat;
    im->scp.pf_pos = im->cur_ticks / ((sysclk_ms(DRIVE_MS_PER_REV) * 16)
                                      / nr_flux);
    if (im->scp.pf_pos >= nr_flux)
        im->scp.pf_pos = 0;
    im->scp.ld_pos = im->scp.pf_pos;

    rd->prod = rd->cons = 0;

    if (start_pos)
        image_read_track(im);

    return FALSE;
}

static bool_t scp_read_track(struct image *im)
{
    struct image_buf *rd = &im->bufs.read_data;
    UINT nr, nr_flux = im->scp.rev[im->scp.pf_rev].nr_dat;
    uint16_t *buf = im->bufs.read_data.p;
    unsigned int buflen = im->bufs.read_data.len & ~511;
    uint32_t off;

    /* At least 2kB buffer space available to fill? */
    if ((uint32_t)(rd->prod - rd->cons) > (buflen-2048)/2)
        return FALSE;

    off = im->scp.rev[im->scp.pf_rev].dat_off + im->scp.pf_pos*2;
    F_lseek(&im->fp, off);

    /* Up to 2kB, further limited by end of buffer and end of stream. */
    nr = min_t(UINT, 2048, (nr_flux - im->scp.pf_pos) * 2);
    nr = min_t(UINT, nr, buflen - ((rd->prod*2) % buflen));
    /* Partial sector is dealt with separately, so that following read is 
     * aligned and can occur directly into the ring buffer (and also as 
     * a multi-sector read at the flash device). */
    if (off & 511)
        nr = min_t(UINT, nr, (-off)&511);

    F_read(&im->fp, &buf[rd->prod % (buflen/2)], nr, NULL);
    rd->prod += nr/2;
    im->scp.pf_pos += nr/2;
    if (im->scp.pf_pos >= nr_flux) {
        ASSERT(im->scp.pf_pos == nr_flux);
        im->scp.pf_pos = 0;
        im->scp.pf_rev = (im->scp.pf_rev + 1) % ARRAY_SIZE(im->scp.rev);
    }

    return TRUE;
}

static uint16_t scp_rdata_flux(struct image *im, uint16_t *tbuf, uint16_t nr)
{
    struct image_buf *rd = &im->bufs.read_data;
    uint32_t x, ticks = im->ticks_since_flux, todo = nr;
    uint32_t nr_flux = im->scp.rev[im->scp.ld_rev].nr_dat;
    uint16_t *buf = im->bufs.read_data.p;
    unsigned int buflen = im->bufs.read_data.len & ~511;

    while (rd->cons != rd->prod) {
        if (im->scp.ld_pos == nr_flux) {
            im->tracklen_ticks = im->cur_ticks;
            im->cur_ticks = 0;
            im->scp.ld_pos = 0;
            im->scp.ld_rev = (im->scp.ld_rev + 1) % ARRAY_SIZE(im->scp.rev);
            nr_flux = im->scp.rev[im->scp.ld_rev].nr_dat;
        }
        im->scp.ld_pos++;
        x = be16toh(buf[rd->cons++ % (buflen/2)]) ?: 0x10000;
        x *= (FF_MHZ << 8) / SCP_MHZ;
        x >>= 8;
        if (x >= 0x10000)
            x = 0xffff; /* clamp */
        im->cur_ticks += x;
        ticks += x;
        *tbuf++ = (ticks >> 4) - 1;
        ticks &= 15;
        if (!--todo)
            goto out;
    }

out:
    im->ticks_since_flux = ticks;
    return nr - todo;
}

const struct image_handler scp_image_handler = {
    .open = scp_open,
    .seek_track = scp_seek_track,
    .read_track = scp_read_track,
    .rdata_flux = scp_rdata_flux,
    .syncword = 0xffffffff
};

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
