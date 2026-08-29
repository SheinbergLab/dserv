#!/bin/sh
# Regenerate the JLCPCB upload zip for photomarker rev B.
#
# Only fabrication layers + drill go in. The *-drl_map.gbr files KiCad also
# emits are DOCUMENTATION drawings (drill symbols and a legend table) -- putting
# them in the zip risks JLCPCB's layer auto-detection treating one as a
# mechanical layer, which can produce spurious DFM findings.
set -e
CLI=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
D=$(cd "$(dirname "$0")" && pwd)
PCB=$D/photomarker-tht.kicad_pcb

rm -rf "$D/fab/gerbers" "$D/fab/photomarker-tht-gerbers.zip"
mkdir -p "$D/fab/gerbers"

"$CLI" pcb export gerbers \
  --layers "F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts" \
  --no-protel-ext -o "$D/fab/gerbers/" "$PCB"
"$CLI" pcb export drill --format excellon --drill-origin absolute \
  --excellon-separate-th --generate-map --map-format gerberx2 \
  -o "$D/fab/gerbers/" "$PCB"

cd "$D/fab/gerbers"
zip -q ../photomarker-tht-gerbers.zip \
  photomarker-tht-F_Cu.gbr photomarker-tht-B_Cu.gbr \
  photomarker-tht-F_Mask.gbr photomarker-tht-B_Mask.gbr \
  photomarker-tht-F_Silkscreen.gbr photomarker-tht-B_Silkscreen.gbr \
  photomarker-tht-F_Paste.gbr photomarker-tht-B_Paste.gbr \
  photomarker-tht-Edge_Cuts.gbr \
  photomarker-tht-PTH.drl photomarker-tht-NPTH.drl
echo "wrote $D/fab/photomarker-tht-gerbers.zip ($(unzip -l ../photomarker-tht-gerbers.zip | tail -1 | awk '{print $2}') files)"
