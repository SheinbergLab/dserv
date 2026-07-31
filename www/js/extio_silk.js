/* Silkscreen (Arduino header) names for extio boxes, shared by extio.html and
 * extio-config.html so the two pages cannot drift.
 *
 * DISPLAY ONLY. The pin/channel NUMBER is the contract everywhere -- cmd/do/<n>,
 * config/pin/<n>, group bitmasks, persisted config -- and everything the box
 * echoes back is numeric. These names are the board's silkscreen, shown beside
 * the number so nobody carries the map in their head; they are never parsed
 * and never sent.
 *
 * Keyed on the announced `board` (CONFIG_BOARD, e.g. "frdm_mcxn947"): an
 * unknown board gets NO names rather than confidently wrong ones -- the same
 * reasoning that drives the config page's pin table from pins/all instead of
 * a per-board list. If a second board grows a map (the RW612 has the same UNO
 * header), prefer announcing the names from box_announce_manifest() over
 * extending this table: the BOARD should declare what its pins are called.
 *
 * frdm_mcxn947 (extio-zephyr/PINMAP.md is the authority):
 *   box pin n IS Arduino Dn (0-13, D5 reserved to the PHY); pins 14-17 are
 *   the A2-A5 positions. ain channel k IS Arduino Ak: ch0/1 on dedicated
 *   analog-only pads, ch2-5 riding pins 14-17 (analog only while that pin is
 *   in `ain` mode).
 */
const ExtioSilk = (() => {
    const MAPS = {
        frdm_mcxn947: {
            pin:    p => (p >= 0 && p <= 13) ? 'D' + p
                       : (p >= 14 && p <= 17) ? 'A' + (p - 12) : null,
            ain:    k => (k >= 0 && k <= 5) ? 'A' + k : null,
            ainPin: k => (k >= 2 && k <= 5) ? 12 + k : null,
        },
    };
    /* CONFIG_BOARD carries no qualifiers, but split anyway so a future
     * "board/soc/core" string still matches its family. */
    return { of: board => MAPS[String(board || '').split('/')[0]] || null };
})();
