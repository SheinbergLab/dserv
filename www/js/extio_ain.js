/* Analog-group block decoder, shared by extio.html and extio-config.html --
 * the JS twin of lib/extio-1.0.tm ::extio::ain_decode. The wire authority is
 * extio-zephyr/src/core/box_ain_group.h; keep the three in step.
 *
 * A box publishes state/ain/<label> as a DSERV_BYTE datapoint, and dserv's
 * websocket JSON delivers that as an ARRAY OF BYTE VALUES in `data`
 * (src/Datapoint.c dpoint_to_json) -- which is exactly what decode() takes.
 * Layout: 12-byte self-describing header, then scan-major int16 LE samples in
 * ASCENDING channel order (the same decode-from-the-announced-string contract
 * the DI groups use; here mask travels IN the block, so it stands alone):
 *
 *   off size field
 *   0   1    ver (0x01)
 *   1   1    mask         channels present, bit c = ch c
 *   2   1    nchan        popcount(mask)
 *   3   1    count        scans in this block (1 = on-change / per-scan)
 *   4   4    interval_us  spacing between scans (0 if count == 1)
 *   8   2    flags        bit0 = boxcar-averaged
 *   10  2    reserved
 *   12  ...  int16[count*nchan]
 *
 * Returns null for anything that is not a decodable block (wrong version,
 * short payload, empty mask) -- a monitor shows nothing rather than garbage.
 * `last` is the NEWEST scan's values, one per chans[k]: what a dashboard
 * wants. Frame timestamp carries t0; scan k is at t0 + k*interval_us.
 */
const ExtioAinBlock = (() => {
    function decode(bytes) {
        if (!Array.isArray(bytes) || bytes.length < 12) return null;
        if (bytes[0] !== 0x01) return null;
        const mask = bytes[1], nchan = bytes[2], count = bytes[3];
        const interval_us =
            (bytes[4] | (bytes[5] << 8) | (bytes[6] << 16) | (bytes[7] << 24)) >>> 0;
        const flags = bytes[8] | (bytes[9] << 8);
        const chans = [];
        for (let c = 0; c < 8; c++) if ((mask >> c) & 1) chans.push(c);
        if (!nchan || nchan !== chans.length) return null;
        if (bytes.length < 12 + count * nchan * 2) return null;
        const off = 12 + (count - 1) * nchan * 2;      /* newest scan */
        const last = [];
        for (let i = 0; i < nchan; i++) {
            let v = bytes[off + 2 * i] | (bytes[off + 2 * i + 1] << 8);
            if (v & 0x8000) v -= 0x10000;              /* int16 */
            last.push(v);
        }
        return { mask, chans, nchan, count, interval_us,
                 avg: (flags & 1) !== 0, last };
    }
    return { decode };
})();
